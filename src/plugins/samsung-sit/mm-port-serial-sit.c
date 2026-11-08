/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-log-object.h"
#include "mm-port-serial-sit.h"

typedef struct {
    guint                            id;
    MMPortSerialSitUnsolicitedFn     callback;
    gpointer                         user_data;
    GDestroyNotify                   notify;
} UnsolicitedHandler;

struct _MMPortSerialSitPrivate {
    GList   *unsolicited_handlers;
    guint    next_handler_id;
    guint16  next_token;
    /*
     * Requests written and not yet answered, keyed by the token the CP echoes
     * back. Several at once, on purpose.
     *
     * The CP answers out of order and -- measured on hardware -- will sit on a
     * finished reply until the AP writes again. One request at a time deadlocks
     * against that: the write that would release the reply is the one we would
     * only make once we had it, so the reply arrives when our own timeout fires
     * and is then too late to use. Stock keeps up to nine requests outstanding
     * on one channel and is never silent for more than 124ms with work
     * pending, so it never meets the behaviour at all.
     */
    GHashTable *pending;
    /*
     * Frames written in submission order, held only while the transport is
     * blocked. One frame per write, never two together and never a part of
     * one: the ring is message-oriented and both have broken it before.
     */
    GQueue  *tx;
    guint    tx_id;
};

G_DEFINE_TYPE (MMPortSerialSit, mm_port_serial_sit, MM_TYPE_PORT_SERIAL)

static gboolean
peek_complete_frame (MMPortSerialSit *self,
                     GByteArray      *buffer,
                     guint16         *out_frame_len)
{
    g_autoptr(GError) error = NULL;

    if (buffer->len < MM_SIT_HEADER_SIZE)
        return FALSE;

    if (!mm_sit_frame_peek_length (buffer->data, buffer->len, out_frame_len, &error)) {
        mm_obj_warn (self, "invalid SIT receive buffer: %s", error->message);
        g_byte_array_set_size (buffer, 0);
        return FALSE;
    }

    return buffer->len >= *out_frame_len;
}

static void
log_message (MMPortSerialSit    *self,
             const gchar        *prefix,
             const MMSitMessage *message)
{
    if (message->has_status)
        mm_obj_dbg (self, "%s %s (0x%04x), token 0x%04x, length %u, status %u",
                    prefix,
                    mm_sit_message_id_get_string (message->id),
                    message->id,
                    message->token,
                    message->length,
                    message->status);
    else
        mm_obj_dbg (self, "%s %s (0x%04x), token 0x%04x, length %u",
                    prefix,
                    mm_sit_message_id_get_string (message->id),
                    message->id,
                    message->token,
                    message->length);
}

static void
dispatch_indication (MMPortSerialSit    *self,
                     const MMSitMessage *message)
{
    GList *l;

    log_message (self, "<-- indication", message);
    for (l = self->priv->unsolicited_handlers; l; l = g_list_next (l)) {
        UnsolicitedHandler *handler = l->data;

        handler->callback (self, message, handler->user_data);
    }
}

typedef struct {
    MMPortSerialSit *self;
    GTask           *task;
    guint16          token;
    guint16          expected_id;
    guint            timeout_id;
    GCancellable    *cancellable;
    gulong           cancelled_id;
} Pending;

static void
pending_free (Pending *pending)
{
    if (pending->timeout_id)
        g_source_remove (pending->timeout_id);
    if (pending->cancellable) {
        if (pending->cancelled_id)
            g_cancellable_disconnect (pending->cancellable, pending->cancelled_id);
        g_object_unref (pending->cancellable);
    }
    g_clear_object (&pending->task);
    g_free (pending);
}

/*
 * Hand the answer to its caller and retire the token. Taking it out of the
 * table frees the Pending, so nothing here may touch it afterwards -- and the
 * task is stolen first so completing it cannot run against a freed Pending if
 * the caller submits its next request from the callback.
 */
static void
pending_complete (Pending *pending, MMSitMessage *response, GError *error)
{
    MMPortSerialSit         *self = pending->self;
    g_autoptr(GTask)         task = g_steal_pointer (&pending->task);
    g_autoptr(MMSitMessage)  owned = response;

    g_hash_table_remove (self->priv->pending, GUINT_TO_POINTER ((guint) pending->token));
    if (!task) {
        if (error)
            g_error_free (error);
        return;
    }
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, g_steal_pointer (&owned), (GDestroyNotify) mm_sit_message_free);
}

static gboolean
pending_timed_out (Pending *pending)
{
    pending->timeout_id = 0;
    mm_obj_dbg (pending->self, "SIT request 0x%04x token 0x%04x went unanswered",
                pending->expected_id, pending->token);
    pending_complete (pending, NULL,
                      g_error_new (MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT,
                                   "Serial command timed out"));
    return G_SOURCE_REMOVE;
}

static void
pending_cancelled (GCancellable *cancellable, Pending *pending)
{
    /*
     * This runs inside the cancellable's own "cancelled" emission, and
     * completing the request retires the token, which frees the Pending.
     * g_cancellable_disconnect() waits for the cancelled handlers to return,
     * so pending_free() must not be allowed to reach it from here or the
     * thread deadlocks against itself -- main loop included, which takes the
     * whole daemon down with it: no D-Bus, no modems, and a live process that
     * systemd still calls active.
     *
     * Drop the handler the non-blocking way instead and clear the id so
     * pending_free() leaves it alone. The id is zero when g_cancellable_connect()
     * ran us synchronously because the cancellable was already cancelled.
     */
    if (pending->cancelled_id) {
        g_signal_handler_disconnect (cancellable, pending->cancelled_id);
        pending->cancelled_id = 0;
    }

    pending_complete (pending, NULL,
                      g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                   "SIT request cancelled"));
}

static void tx_flush (MMPortSerialSit *self);

static gboolean
tx_retry (MMPortSerialSit *self)
{
    self->priv->tx_id = 0;
    tx_flush (self);
    return G_SOURCE_REMOVE;
}

static void
tx_flush (MMPortSerialSit *self)
{
    while (!g_queue_is_empty (self->priv->tx)) {
        g_autoptr(GByteArray) frame = g_queue_peek_head (self->priv->tx);
        g_autoptr(GError)     error = NULL;

        if (mm_port_serial_send (MM_PORT_SERIAL (self), frame, &error)) {
            g_queue_pop_head (self->priv->tx);
            continue;
        }
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            /* Keep it at the head so order is preserved, and come back for it. */
            g_steal_pointer (&frame);
            if (!self->priv->tx_id)
                self->priv->tx_id = g_timeout_add (10, (GSourceFunc) tx_retry, self);
            return;
        }
        mm_obj_warn (self, "could not write SIT frame: %s", error->message);
        g_queue_pop_head (self->priv->tx);
    }
}

static void
tx_push (MMPortSerialSit *self, GByteArray *frame)
{
    g_queue_push_tail (self->priv->tx, g_byte_array_ref (frame));
    tx_flush (self);
}

/*
 * Consume every frame the buffer holds in full: indications go to their
 * handlers, a response goes to whichever outstanding request echoes its token,
 * and a response for no outstanding request is the wreckage of one that already
 * gave up. Nothing is ever left behind for MMPortSerial's own response parsing,
 * which this port does not use.
 */
static void
drain_frames (MMPortSerialSit *self, GByteArray *buffer)
{
    while (buffer->len >= MM_SIT_HEADER_SIZE) {
        g_autoptr(MMSitMessage) message = NULL;
        g_autoptr(GError)       error = NULL;
        guint16                 frame_len;
        Pending                *pending;

        if (!peek_complete_frame (self, buffer, &frame_len))
            return;

        message = mm_sit_message_parse (buffer->data, frame_len, &error);
        g_byte_array_remove_range (buffer, 0, frame_len);
        if (!message) {
            mm_obj_warn (self, "could not parse SIT message: %s", error->message);
            continue;
        }

        if (message->type == MM_SIT_MESSAGE_TYPE_INDICATION) {
            dispatch_indication (self, message);
            continue;
        }

        if (message->type != MM_SIT_MESSAGE_TYPE_RESPONSE) {
            mm_obj_warn (self, "unexpected SIT message type %u", message->type);
            continue;
        }

        pending = g_hash_table_lookup (self->priv->pending,
                                       GUINT_TO_POINTER ((guint) message->token));
        if (!pending) {
            mm_obj_dbg (self, "<-- late response %s (0x%04x), token 0x%04x: nothing is waiting for it",
                        mm_sit_message_id_get_string (message->id), message->id, message->token);
            continue;
        }

        log_message (self, "<-- response", message);
        if (message->id != pending->expected_id) {
            pending_complete (pending, NULL,
                              g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                           "Unexpected SIT response 0x%04x for token 0x%04x; expected 0x%04x",
                                           message->id, message->token, pending->expected_id));
            continue;
        }
        pending_complete (pending, g_steal_pointer (&message), NULL);
    }
}

static void
parse_unsolicited (MMPortSerial *port,
                   GByteArray   *response_buffer)
{
    drain_frames (MM_PORT_SERIAL_SIT (port), response_buffer);
}

/* Every frame is already accounted for by the time this runs; see drain_frames(). */
static MMPortSerialResponseType
parse_response (MMPortSerial  *port,
                GByteArray    *response_buffer,
                GByteArray   **parsed_response,
                GError       **error)
{
    return MM_PORT_SERIAL_RESPONSE_NONE;
}

static void
debug_log (MMPortSerial *port,
           const gchar  *prefix,
           const gchar  *buffer,
           gsize         buffer_len)
{
    g_autoptr(MMSitMessage) message = NULL;

    /* Receive messages are logged after validation, without sensitive data. */
    if (g_str_equal (prefix, "<--"))
        return;

    message = mm_sit_message_parse ((const guint8 *) buffer, buffer_len, NULL);
    if (message)
        log_message (MM_PORT_SERIAL_SIT (port), "--> request", message);
    else
        mm_obj_dbg (port, "--> malformed SIT request, length %" G_GSIZE_FORMAT,
                    buffer_len);
}

void
mm_port_serial_sit_command (MMPortSerialSit     *self,
                            GByteArray          *request,
                            guint32              timeout_seconds,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
    Pending *pending;
    guint16  token;

    g_return_if_fail (MM_IS_PORT_SERIAL_SIT (self));
    g_return_if_fail (request != NULL);
    g_return_if_fail (request->len >= MM_SIT_HEADER_SIZE);

    token = request->data[6] | (((guint16) request->data[7]) << 8);

    /*
     * Tokens come from mm_port_serial_sit_next_token() and wrap, so one still
     * outstanding would silently take over the older request's answer. Refuse
     * rather than guess whose response the next frame is.
     */
    if (g_hash_table_contains (self->priv->pending, GUINT_TO_POINTER ((guint) token))) {
        g_task_report_new_error (self, callback, user_data, mm_port_serial_sit_command,
                                 G_IO_ERROR, G_IO_ERROR_BUSY,
                                 "SIT token 0x%04x is already outstanding", token);
        return;
    }

    pending = g_new0 (Pending, 1);
    pending->self = self;
    pending->token = token;
    pending->expected_id = request->data[2] | (((guint16) request->data[3]) << 8);
    pending->task = g_task_new (self, NULL, callback, user_data);
    pending->timeout_id = g_timeout_add_seconds (timeout_seconds,
                                                 (GSourceFunc) pending_timed_out,
                                                 pending);
    g_hash_table_insert (self->priv->pending, GUINT_TO_POINTER ((guint) token), pending);

    /*
     * The cancellable is held here rather than given to the GTask, because
     * cancelling has to retire the token as well and only this knows how.
     *
     * Connected *after* the insert, and the result is checked rather than
     * stored blindly: if the cancellable is already cancelled,
     * g_cancellable_connect() runs pending_cancelled() synchronously and
     * returns 0. That completes the request and retires the token, so the
     * Pending is already freed when it returns -- assigning to it, or sending
     * the request, would be a use-after-free. Connecting before the insert
     * was worse still: the remove found nothing, so pending_free() never ran
     * and the request went out anyway with a completed task and a live
     * timeout behind it.
     */
    if (cancellable) {
        gulong cancelled_id;

        pending->cancellable = g_object_ref (cancellable);
        cancelled_id = g_cancellable_connect (cancellable,
                                              (GCallback) pending_cancelled,
                                              pending, NULL);
        if (!cancelled_id)
            return;
        pending->cancelled_id = cancelled_id;
    }

    /* mm_port_serial_send() still logs the write through the debug_log vfunc
     * below, so the request lines read exactly as they did before. */
    tx_push (self, request);
}

/*
 * How many of `ids` are written and still unanswered.
 *
 * Stock never has a registration query in flight when it asks for an IMS
 * context; see mm_sit_client_registration_queries_in_flight().
 */
guint
mm_port_serial_sit_pending_count (MMPortSerialSit *self,
                                  const guint16   *ids,
                                  gsize            n_ids)
{
    GHashTableIter iter;
    gpointer       value;
    guint          n = 0;

    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (self), 0);

    g_hash_table_iter_init (&iter, self->priv->pending);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        Pending *pending = value;
        gsize    i;

        for (i = 0; i < n_ids; i++) {
            if (pending->expected_id == ids[i]) {
                n++;
                break;
            }
        }
    }
    return n;
}

MMSitMessage *
mm_port_serial_sit_command_finish (MMPortSerialSit  *self,
                                   GAsyncResult     *res,
                                   GError          **error)
{
    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

/*
 * Send a frame the CP will not answer.
 *
 * Indications the AP raises at the CP (AIMS_MEDIA_STATE_IND is the one we
 * need) carry no token and get no response, so they take the write path
 * without an entry in `pending`: a token of zero there would swallow the
 * matching of a later real response.
 */
void
mm_port_serial_sit_send_indication (MMPortSerialSit *self,
                                    GByteArray      *frame)
{
    g_return_if_fail (MM_IS_PORT_SERIAL_SIT (self));
    g_return_if_fail (frame != NULL);
    g_return_if_fail (frame->len >= MM_SIT_HEADER_SIZE);

    tx_push (self, frame);
}

guint16
mm_port_serial_sit_next_token (MMPortSerialSit *self)
{
    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (self), 0);

    self->priv->next_token++;
    if (self->priv->next_token == 0)
        self->priv->next_token++;
    return self->priv->next_token;
}

guint
mm_port_serial_sit_add_unsolicited_handler (MMPortSerialSit                *self,
                                            MMPortSerialSitUnsolicitedFn     callback,
                                            gpointer                        user_data,
                                            GDestroyNotify                  notify)
{
    UnsolicitedHandler *handler;

    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (self), 0);
    g_return_val_if_fail (callback != NULL, 0);

    handler = g_new0 (UnsolicitedHandler, 1);
    handler->id = ++self->priv->next_handler_id;
    handler->callback = callback;
    handler->user_data = user_data;
    handler->notify = notify;
    self->priv->unsolicited_handlers = g_list_append (self->priv->unsolicited_handlers, handler);
    return handler->id;
}

void
mm_port_serial_sit_remove_unsolicited_handler (MMPortSerialSit *self,
                                               guint            handler_id)
{
    GList *l;

    g_return_if_fail (MM_IS_PORT_SERIAL_SIT (self));

    for (l = self->priv->unsolicited_handlers; l; l = g_list_next (l)) {
        UnsolicitedHandler *handler = l->data;

        if (handler->id != handler_id)
            continue;
        if (handler->notify)
            handler->notify (handler->user_data);
        self->priv->unsolicited_handlers = g_list_delete_link (self->priv->unsolicited_handlers, l);
        g_free (handler);
        return;
    }
}

MMPortSerialSit *
mm_port_serial_sit_new (const gchar *name)
{
    return MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, name,
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             NULL));
}

MMPortSerialSit *
mm_port_serial_sit_new_socket (const gchar *path)
{
    /* MM_PORT_SUBSYS_UNIX is all it takes: mm_port_serial_open() then connects
     * an AF_UNIX GSocket to MM_PORT_DEVICE instead of open()ing /dev/<device>.
     * The framing above is unchanged -- it reassembles by the header's declared
     * length rather than trusting message boundaries, which is exactly what a
     * SOCK_STREAM connection needs (and what the device itself needs anyway,
     * since the kernel splits messages over 2036 bytes across reads). */
    return MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, path,
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_UNIX,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             NULL));
}

static void
mm_port_serial_sit_init (MMPortSerialSit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_PORT_SERIAL_SIT,
                                              MMPortSerialSitPrivate);

    self->priv->pending = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                 NULL, (GDestroyNotify) pending_free);
    self->priv->tx = g_queue_new ();
}

static void
finalize (GObject *object)
{
    MMPortSerialSit *self = MM_PORT_SERIAL_SIT (object);

    while (self->priv->unsolicited_handlers) {
        UnsolicitedHandler *handler = self->priv->unsolicited_handlers->data;

        if (handler->notify)
            handler->notify (handler->user_data);
        g_free (handler);
        self->priv->unsolicited_handlers = g_list_delete_link (self->priv->unsolicited_handlers,
                                                                self->priv->unsolicited_handlers);
    }

    if (self->priv->tx_id)
        g_source_remove (self->priv->tx_id);
    g_hash_table_unref (self->priv->pending);
    g_queue_free_full (self->priv->tx, (GDestroyNotify) g_byte_array_unref);

    G_OBJECT_CLASS (mm_port_serial_sit_parent_class)->finalize (object);
}

static void
mm_port_serial_sit_class_init (MMPortSerialSitClass *klass)
{
    GObjectClass      *object_class = G_OBJECT_CLASS (klass);
    MMPortSerialClass *serial_class = MM_PORT_SERIAL_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMPortSerialSitPrivate));

    object_class->finalize = finalize;
    serial_class->parse_response = parse_response;
    serial_class->parse_unsolicited = parse_unsolicited;
    serial_class->debug_log = debug_log;
}
