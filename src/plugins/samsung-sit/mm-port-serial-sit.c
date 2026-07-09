/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

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

static void
drain_leading_indications (MMPortSerialSit *self,
                           GByteArray      *buffer)
{
    while (buffer->len >= MM_SIT_HEADER_SIZE) {
        g_autoptr(MMSitMessage) message = NULL;
        g_autoptr(GError)       error = NULL;
        guint16                 frame_len;

        if (!peek_complete_frame (self, buffer, &frame_len))
            return;

        message = mm_sit_message_parse (buffer->data, frame_len, &error);
        if (!message) {
            mm_obj_warn (self, "could not parse SIT message: %s", error->message);
            g_byte_array_remove_range (buffer, 0, frame_len);
            continue;
        }
        if (message->type != MM_SIT_MESSAGE_TYPE_INDICATION)
            return;

        dispatch_indication (self, message);
        g_byte_array_remove_range (buffer, 0, frame_len);
    }
}

static void
parse_unsolicited (MMPortSerial *port,
                   GByteArray   *response_buffer)
{
    drain_leading_indications (MM_PORT_SERIAL_SIT (port), response_buffer);
}

static MMPortSerialResponseType
parse_response (MMPortSerial  *port,
                GByteArray    *response_buffer,
                GByteArray   **parsed_response,
                GError       **error)
{
    MMPortSerialSit          *self = MM_PORT_SERIAL_SIT (port);
    g_autoptr(MMSitMessage)   message = NULL;
    guint16                   frame_len;

    drain_leading_indications (self, response_buffer);
    if (!peek_complete_frame (self, response_buffer, &frame_len))
        return MM_PORT_SERIAL_RESPONSE_NONE;

    message = mm_sit_message_parse (response_buffer->data, frame_len, error);
    if (!message) {
        g_byte_array_remove_range (response_buffer, 0, frame_len);
        return MM_PORT_SERIAL_RESPONSE_ERROR;
    }

    if (message->type != MM_SIT_MESSAGE_TYPE_RESPONSE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Unexpected SIT message type %u while waiting for a response",
                     message->type);
        g_byte_array_remove_range (response_buffer, 0, frame_len);
        return MM_PORT_SERIAL_RESPONSE_ERROR;
    }

    log_message (self, "<-- response", message);
    *parsed_response = g_byte_array_sized_new (frame_len);
    g_byte_array_append (*parsed_response, response_buffer->data, frame_len);
    g_byte_array_remove_range (response_buffer, 0, frame_len);

    /* A single WWAN read may contain the response followed by indications. */
    drain_leading_indications (self, response_buffer);
    return MM_PORT_SERIAL_RESPONSE_BUFFER;
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

typedef struct {
    guint16 expected_id;
    guint16 expected_token;
} CommandContext;

static void
serial_command_ready (MMPortSerialSit *self,
                      GAsyncResult    *res,
                      GTask           *task)
{
    g_autoptr(GByteArray)  response_buffer = NULL;
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    CommandContext         *ctx;

    response_buffer = mm_port_serial_command_finish (MM_PORT_SERIAL (self), res, &error);
    if (!response_buffer) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    response = mm_sit_message_parse (response_buffer->data, response_buffer->len, &error);
    if (!response) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    ctx = g_task_get_task_data (task);
    if (response->type != MM_SIT_MESSAGE_TYPE_RESPONSE ||
        response->id != ctx->expected_id ||
        response->token != ctx->expected_token) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Unexpected SIT response 0x%04x token 0x%04x; expected 0x%04x token 0x%04x",
                                 response->id, response->token,
                                 ctx->expected_id, ctx->expected_token);
        g_object_unref (task);
        return;
    }

    g_task_return_pointer (task,
                           g_steal_pointer (&response),
                           (GDestroyNotify) mm_sit_message_free);
    g_object_unref (task);
}

MMSitMessage *
mm_port_serial_sit_command_finish (MMPortSerialSit  *self,
                                   GAsyncResult     *res,
                                   GError          **error)
{
    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

void
mm_port_serial_sit_command (MMPortSerialSit     *self,
                            GByteArray          *request,
                            guint32              timeout_seconds,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
    CommandContext *ctx;
    GTask          *task;

    g_return_if_fail (MM_IS_PORT_SERIAL_SIT (self));
    g_return_if_fail (request != NULL);
    g_return_if_fail (request->len >= MM_SIT_HEADER_SIZE);

    ctx = g_new0 (CommandContext, 1);
    ctx->expected_id = request->data[2] | (((guint16) request->data[3]) << 8);
    ctx->expected_token = request->data[6] | (((guint16) request->data[7]) << 8);

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, g_free);
    mm_port_serial_command (MM_PORT_SERIAL (self),
                            request,
                            timeout_seconds,
                            FALSE,
                            FALSE,
                            cancellable,
                            (GAsyncReadyCallback) serial_command_ready,
                            task);
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

static void
mm_port_serial_sit_init (MMPortSerialSit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_PORT_SERIAL_SIT,
                                              MMPortSerialSitPrivate);
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
