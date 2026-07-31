/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <string.h>

#include "mm-log-object.h"
#include "mm-sit-client.h"

enum {
    SIGNAL_INDICATION,
    SIGNAL_LAST,
};

static guint signals[SIGNAL_LAST];

struct _MMSitClientPrivate {
    gchar               *device;
    MMPortSerialSit     *port;
    /* Device node to retry on if the proxy socket will not open; NULL when we
     * were told to talk to the node directly in the first place. */
    gchar               *proxy_fallback_device;
    guint                unsolicited_handler_id;
    gboolean             has_carrier_sim_info;
    MMSitCarrierSimInfo  carrier_sim_info;
    gboolean             boot_config_done;
    gboolean             radio_state_trusted;
    /* AID of the subscription application, cached from the last SIM status so
     * file reads can address it the way the downstream RIL does. */
    guint8               sim_aid[MM_SIT_MAX_AID_LENGTH];
    gsize                sim_aid_len;
    /* Service centre the CP reports, cached on first use: the downstream RIL
     * puts it in every send and this CP answers a send that lacks one with an
     * empty response. */
    guint8               smsc[MM_SIT_MAX_SMSC_LEN];
    gsize                smsc_len;
};

G_DEFINE_TYPE (MMSitClient, mm_sit_client, G_TYPE_OBJECT)

static gboolean
message_check_success (const MMSitMessage *message,
                       GError            **error)
{
    if (!message->has_status) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT %s response has no status",
                     mm_sit_message_id_get_string (message->id));
        return FALSE;
    }
    if (message->status != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "SIT %s failed with status %u",
                     mm_sit_message_id_get_string (message->id),
                     message->status);
        return FALSE;
    }
    return TRUE;
}

static void
port_indication (MMPortSerialSit    *port,
                 const MMSitMessage *message,
                 MMSitClient        *self)
{
    if (message->id == MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO) {
        MMSitCarrierSimInfo info;

        if (mm_sit_message_parse_carrier_sim_info (message, &info, NULL)) {
            self->priv->carrier_sim_info = info;
            self->priv->has_carrier_sim_info = TRUE;
        }
    }
    g_signal_emit (self, signals[SIGNAL_INDICATION], 0, message);
}

MMSitClient *
mm_sit_client_new (const gchar *device)
{
    MMSitClient *self;
    const gchar *name;

    g_return_val_if_fail (device != NULL, NULL);

    name = g_str_has_prefix (device, "/dev/") ? device + strlen ("/dev/") : device;
    self = g_object_new (MM_TYPE_SIT_CLIENT, NULL);
    self->priv->device = g_strdup (name);
    self->priv->port = mm_port_serial_sit_new (name);
    return self;
}

gchar *
mm_sit_proxy_socket_path (const gchar *device)
{
    const gchar *sit;
    const gchar *index = NULL;

    g_return_val_if_fail (device != NULL, NULL);

    /* sit-proxy names one socket per device node, by the node's own index:
     * wwan0sit0 -> /run/sit-proxy/0.  The port is the stack selector under
     * DSDS, so this mapping is the whole routing story -- there is no selector
     * inside the protocol. */
    sit = g_strrstr (device, "sit");
    if (sit) {
        const gchar *p = sit + strlen ("sit");

        if (g_ascii_isdigit (*p))
            index = p;
    }
    if (!index) {
        /* An unrecognised name (a downstream-style /dev/umts_ipc0, say) has no
         * index to read; assume the first stack, which is the one that carries
         * everything today. */
        index = "0";
    }
    return g_build_filename (MM_SIT_PROXY_SOCKET_DIRECTORY, index, NULL);
}

MMSitClient *
mm_sit_client_new_proxy (const gchar *device)
{
    MMSitClient      *self;
    const gchar      *name;
    g_autofree gchar *socket_path = NULL;

    g_return_val_if_fail (device != NULL, NULL);

    name = g_str_has_prefix (device, "/dev/") ? device + strlen ("/dev/") : device;
    socket_path = mm_sit_proxy_socket_path (name);

    self = g_object_new (MM_TYPE_SIT_CLIENT, NULL);
    /* Keep the device node as the identity -- it is what logs, DSDS and the
     * user think in, and it is what libqmi does when proxying too. Only the
     * transport underneath differs. */
    self->priv->device = g_strdup (name);
    self->priv->proxy_fallback_device = g_strdup (name);
    self->priv->port = mm_port_serial_sit_new_socket (socket_path);
    return self;
}

MMSitClient *
mm_sit_client_new_from_port (MMPortSerialSit *port)
{
    MMSitClient *self;

    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (port), NULL);

    self = g_object_new (MM_TYPE_SIT_CLIENT, NULL);
    self->priv->device = g_strdup (mm_port_get_device (MM_PORT (port)));
    self->priv->port = g_object_ref (port);
    return self;
}

const gchar *
mm_sit_client_get_device (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return self->priv->device;
}

gboolean
mm_sit_client_is_proxied (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    /* Ask the port rather than remembering what was asked for: open() may have
     * fallen back to the device node, and a log that claims otherwise is worse
     * than no log at all. */
    return mm_port_get_subsys (MM_PORT (self->priv->port)) == MM_PORT_SUBSYS_UNIX;
}

gboolean
mm_sit_client_open (MMSitClient  *self,
                    GError      **error)
{
    g_autoptr(GError) inner = NULL;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    if (mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port)))
        return TRUE;

    if (!mm_port_serial_open (MM_PORT_SERIAL (self->priv->port), &inner)) {
        g_autofree gchar *node = NULL;

        /* Configured for the proxy but it did not answer. Rather than leave the
         * modem dead, say so loudly and take the device node -- the two
         * transports are interchangeable, so a proxy that is not running should
         * cost debuggability, not the network. */
        if (!self->priv->proxy_fallback_device) {
            g_propagate_error (error, g_steal_pointer (&inner));
            return FALSE;
        }

        mm_obj_warn (self->priv->port,
                     "couldn't reach sit-proxy (%s); falling back to %s directly, "
                     "so tools cannot share the port",
                     inner->message, self->priv->proxy_fallback_device);

        node = g_steal_pointer (&self->priv->proxy_fallback_device);
        g_clear_object (&self->priv->port);
        self->priv->port = mm_port_serial_sit_new (node);
        if (!mm_port_serial_open (MM_PORT_SERIAL (self->priv->port), error))
            return FALSE;
    }

    self->priv->unsolicited_handler_id =
        mm_port_serial_sit_add_unsolicited_handler (self->priv->port,
                                                     (MMPortSerialSitUnsolicitedFn) port_indication,
                                                     self,
                                                     NULL);
    return TRUE;
}

void
mm_sit_client_close (MMSitClient *self)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    self->priv->has_carrier_sim_info = FALSE;
    memset (&self->priv->carrier_sim_info, 0, sizeof (self->priv->carrier_sim_info));
    self->priv->boot_config_done = FALSE;
    self->priv->radio_state_trusted = FALSE;
    if (!mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port)))
        return;
    if (self->priv->unsolicited_handler_id) {
        mm_port_serial_sit_remove_unsolicited_handler (self->priv->port,
                                                        self->priv->unsolicited_handler_id);
        self->priv->unsolicited_handler_id = 0;
    }
    mm_port_serial_close (MM_PORT_SERIAL (self->priv->port));
}

gboolean
mm_sit_client_is_open (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port));
}

gboolean
mm_sit_client_get_carrier_sim_info (MMSitClient         *self,
                                    MMSitCarrierSimInfo *out_info)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_info != NULL, FALSE);

    if (!self->priv->has_carrier_sim_info)
        return FALSE;
    *out_info = self->priv->carrier_sim_info;
    return TRUE;
}

gboolean
mm_sit_client_radio_state_trusted (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return self->priv->radio_state_trusted;
}

static void
port_command_ready (MMPortSerialSit *port,
                    GAsyncResult    *res,
                    GTask           *task)
{
    MMSitMessage *response;
    GError       *error = NULL;

    response = mm_port_serial_sit_command_finish (port, res, &error);
    if (!response)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, response, (GDestroyNotify) mm_sit_message_free);
    g_object_unref (task);
}

void
mm_sit_client_command (MMSitClient        *self,
                       GByteArray         *request,
                       guint32             timeout_seconds,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
    GTask *task;

    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    task = g_task_new (self, cancellable, callback, user_data);
    mm_port_serial_sit_command (self->priv->port,
                                request,
                                timeout_seconds,
                                cancellable,
                                (GAsyncReadyCallback) port_command_ready,
                                task);
}

MMSitMessage *
mm_sit_client_command_finish (MMSitClient   *self,
                              GAsyncResult  *res,
                              GError       **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

static guint16
next_token (MMSitClient *self)
{
    return mm_port_serial_sit_next_token (self->priv->port);
}

static void
get_radio_ready (MMSitClient *self,
                 GAsyncResult *res,
                 GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    MMSitRadioState         state;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && message_check_success (response, &error) &&
        mm_sit_message_parse_radio_state (response, &state, &error))
        g_task_return_int (task, state);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

void
mm_sit_client_get_radio_state (MMSitClient        *self,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_RADIO_POWER,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_radio_ready, task);
}

MMSitRadioState
mm_sit_client_get_radio_state_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), MM_SIT_RADIO_STATE_UNKNOWN);
    return g_task_propagate_int (G_TASK (res), error);
}

static void
checked_command_ready (MMSitClient *self,
                       GAsyncResult *res,
                       GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && message_check_success (response, &error))
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

static void
run_checked_command (MMSitClient        *self,
                     GByteArray         *request,
                     guint32             timeout_seconds,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);
    mm_sit_client_command (self, request, timeout_seconds, cancellable,
                           (GAsyncReadyCallback) checked_command_ready, task);
}

typedef struct {
    MMSitRadioState state;
    guint           poll_attempts;
    guint           retry_id;
} SetRadioContext;

static void set_radio_poll (GTask *task);

static void
set_radio_context_free (SetRadioContext *ctx)
{
    if (ctx->retry_id)
        g_source_remove (ctx->retry_id);
    g_free (ctx);
}

static gboolean
set_radio_retry (GTask *task)
{
    SetRadioContext *ctx = g_task_get_task_data (task);

    ctx->retry_id = 0;
    set_radio_poll (task);
    return G_SOURCE_REMOVE;
}

static void
set_radio_poll_ready (MMSitClient *self,
                      GAsyncResult *res,
                      GTask *task)
{
    SetRadioContext *ctx = g_task_get_task_data (task);
    MMSitRadioState  state;
    GError          *error = NULL;

    state = mm_sit_client_get_radio_state_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (state == ctx->state) {
        self->priv->radio_state_trusted = TRUE;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    if (++ctx->poll_attempts >= 15) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                 "SIT radio did not reach state %u", ctx->state);
        g_object_unref (task);
        return;
    }
    ctx->retry_id = g_timeout_add_seconds (1, (GSourceFunc) set_radio_retry, task);
}

static void
set_radio_request_ready (MMSitClient *self,
                         GAsyncResult *res,
                         GTask *task)
{
    g_autoptr(GError) error = NULL;

    /*
     * The CP answers GENERIC_FAILURE when asked to put the radio into the state
     * it is already in, which is not a failure of anything we care about -- and
     * we send the request unconditionally on purpose (see below), so it is the
     * expected answer half the time.  The poll that follows is the authority on
     * the resulting state either way, so note the refusal and carry on; a state
     * that genuinely never arrives still fails there.
     */
    if (!g_task_propagate_boolean (G_TASK (res), &error))
        mm_obj_dbg (self->priv->port, "radio power request refused (%s); polling the state anyway",
                    error->message);

    set_radio_poll (task);
}

static void
set_radio_poll (GTask *task)
{
    MMSitClient *self = g_task_get_source_object (task);

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_radio_state (self, g_task_get_cancellable (task),
                                   (GAsyncReadyCallback) set_radio_poll_ready, task);
}

void
mm_sit_client_set_radio_state (MMSitClient        *self,
                               MMSitRadioState     state,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    SetRadioContext       *ctx;
    GTask                 *task;
    g_autoptr(GByteArray)  request = NULL;

    task = g_task_new (self, cancellable, callback, user_data);
    if (state != MM_SIT_RADIO_STATE_OFF && state != MM_SIT_RADIO_STATE_ON) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "Invalid SIT radio state %u", state);
        g_object_unref (task);
        return;
    }

    ctx = g_new0 (SetRadioContext, 1);
    ctx->state = state;
    g_task_set_task_data (task, ctx, (GDestroyNotify) set_radio_context_free);

    /* Always send the request, even when the CP already reports the target
     * state: a freshly booted CP claims the radio is on while it is parked
     * (never searching), and only an explicit SET_RADIO_POWER starts the
     * PLMN search (HW-verified; the downstream RIL sends it too). */
    request = mm_sit_request_new_set_radio_power (next_token (self), state);
    run_checked_command (self, request, 30, cancellable,
                         (GAsyncReadyCallback) set_radio_request_ready, task);
}

gboolean
mm_sit_client_set_radio_state_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
get_sim_ready (MMSitClient *self,
               GAsyncResult *res,
               GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitSimStatus         *status;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    status = g_new0 (MMSitSimStatus, 1);
    if (response && mm_sit_message_parse_sim_status (response, status, &error)) {
        if (status->app_count) {
            self->priv->sim_aid_len = MIN (status->apps[0].aid_len, MM_SIT_MAX_AID_LENGTH);
            memcpy (self->priv->sim_aid, status->apps[0].aid, self->priv->sim_aid_len);
        } else
            self->priv->sim_aid_len = 0;
        g_task_return_pointer (task, status, g_free);
    } else {
        g_free (status);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_sim_status (MMSitClient        *self,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_STATUS,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_sim_ready, task);
}

gboolean
mm_sit_client_get_sim_status_finish (MMSitClient     *self,
                                     GAsyncResult    *res,
                                     MMSitSimStatus  *out_status,
                                     GError         **error)
{
    MMSitSimStatus *status;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    status = g_task_propagate_pointer (G_TASK (res), error);
    if (!status)
        return FALSE;
    *out_status = *status;
    g_free (status);
    return TRUE;
}

/*****************************************************************************/
/* SMS */

typedef struct {
    guint8   smsc[MM_SIT_MAX_SMSC_LEN];
    gsize    smsc_len;
    GBytes  *tpdu;
    gboolean expect_more;
    gboolean over_ims;
} SendSmsContext;

static void
send_sms_context_free (SendSmsContext *ctx)
{
    g_clear_pointer (&ctx->tpdu, g_bytes_unref);
    g_slice_free (SendSmsContext, ctx);
}

static void send_sms_step (GTask *task);

static void
send_sms_smsc_ready (MMSitClient  *self,
                     GAsyncResult *res,
                     GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    SendSmsContext         *ctx = g_task_get_task_data (task);
    guint8                  smsc[MM_SIT_MAX_SMSC_LEN];
    gsize                   len = 0;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_smsc_address (response, smsc, &len, &error) && len) {
        memcpy (self->priv->smsc, smsc, len);
        self->priv->smsc_len = len;
        memcpy (ctx->smsc, smsc, len);
        ctx->smsc_len = len;
    } else {
        /* Not fatal: send without one and let the CP decide. */
        mm_obj_dbg (self->priv->port, "couldn't read the service centre address: %s",
                    error ? error->message : "empty");
        self->priv->smsc_len = 0;
    }
    send_sms_step (task);
}

static void
send_sms_ready (MMSitClient  *self,
                GAsyncResult *res,
                GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    guint32                 reference = 0;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_send_sms_result (response, &reference, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_return_int (task, (gssize) reference);
    g_object_unref (task);
}

/* Send the part, first reading the service centre address if it is not known
 * and the caller supplied none. */
static void
send_sms_step (GTask *task)
{
    MMSitClient          *self = g_task_get_source_object (task);
    SendSmsContext       *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;
    gconstpointer         tpdu;
    gsize                 tpdu_len = 0;

    if (!ctx->smsc_len && !self->priv->smsc_len) {
        request = mm_sit_request_new_get_smsc_address (next_token (self));
        if (request) {
            mm_sit_client_command (self, request, 10, g_task_get_cancellable (task),
                                   (GAsyncReadyCallback) send_sms_smsc_ready, task);
            return;
        }
        /* Fall through and send without one. */
    }
    if (!ctx->smsc_len && self->priv->smsc_len) {
        memcpy (ctx->smsc, self->priv->smsc, self->priv->smsc_len);
        ctx->smsc_len = self->priv->smsc_len;
    }

    tpdu = g_bytes_get_data (ctx->tpdu, &tpdu_len);
    request = mm_sit_request_new_send_sms (next_token (self),
                                           ctx->smsc, ctx->smsc_len,
                                           tpdu, tpdu_len,
                                           ctx->expect_more, ctx->over_ims,
                                           &error);
    if (!request) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    /* The network round trip is on the CP's side of this, so allow for it. */
    mm_sit_client_command (self, request, 60, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) send_sms_ready, task);
}

void
mm_sit_client_send_sms (MMSitClient        *self,
                        const guint8       *smsc,
                        gsize               smsc_len,
                        const guint8       *tpdu,
                        gsize               tpdu_len,
                        gboolean            expect_more,
                        gboolean            over_ims,
                        GCancellable       *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    SendSmsContext *ctx;
    GTask          *task;

    task = g_task_new (self, cancellable, callback, user_data);

    ctx = g_slice_new0 (SendSmsContext);
    ctx->smsc_len = MIN (smsc_len, MM_SIT_MAX_SMSC_LEN);
    if (ctx->smsc_len)
        memcpy (ctx->smsc, smsc, ctx->smsc_len);
    ctx->tpdu = g_bytes_new (tpdu, tpdu_len);
    ctx->expect_more = expect_more;
    ctx->over_ims = over_ims;
    g_task_set_task_data (task, ctx, (GDestroyNotify) send_sms_context_free);

    send_sms_step (task);
}

gboolean
mm_sit_client_send_sms_finish (MMSitClient  *self,
                               GAsyncResult *res,
                               guint32      *out_reference,
                               GError      **error)
{
    GError *inner = NULL;
    gssize  value;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    value = g_task_propagate_int (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return FALSE;
    }
    if (out_reference)
        *out_reference = (guint32) value;
    return TRUE;
}

static void
ack_sms_ready (MMSitClient  *self,
               GAsyncResult *res,
               GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_sit_client_ack_sms (MMSitClient        *self,
                       gboolean            over_ims,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_sms_ack (next_token (self), over_ims);
    if (!request) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "couldn't build the SMS acknowledgement");
        g_object_unref (task);
        return;
    }
    mm_sit_client_command (self, request, 10, cancellable,
                           (GAsyncReadyCallback) ack_sms_ready, task);
}

gboolean
mm_sit_client_ack_sms_finish (MMSitClient  *self,
                              GAsyncResult *res,
                              GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*****************************************************************************/
/* Elementary-file reads
 *
 * One file read is two or more SIM_IO exchanges: GET_RESPONSE first, because
 * only the card knows whether the file is transparent or made of records and
 * how big it is, then READ_BINARY or READ_RECORD with those numbers. Callers
 * get the whole file (records concatenated) plus its geometry, and decode the
 * contents themselves -- that part is plain 3GPP, not SIT.
 */

/* P3 is one byte, so a transparent file longer than this is read in several
 * passes with the offset in P1/P2. */
#define SIM_IO_MAX_READ 255
/* Backstop on a file that claims an implausible number of records; a truncated
 * read is reported rather than passed off as the whole file. */
#define SIM_IO_MAX_PASSES 32
/* Absolute record addressing, the mode rild uses. */
#define SIM_IO_RECORD_MODE_ABSOLUTE 4

typedef struct {
    MMSitEfInfo  info;
    guint16      file_id;
    guint16      path[MM_SIT_MAX_SIM_IO_PATH];
    guint        path_len;
    GByteArray  *data;
    guint        passes;
    /* Transparent: bytes still to read. Records: the next record number. */
    guint        remaining;
    guint        next_record;
} ReadEfContext;

static void
read_ef_context_free (ReadEfContext *ctx)
{
    g_clear_pointer (&ctx->data, g_byte_array_unref);
    g_slice_free (ReadEfContext, ctx);
}

static void read_ef_step (GTask *task);

/* Turn a card refusal into an error naming the status word, which is the only
 * place a SIM_IO failure shows up: the transport status stays zero. */
static gboolean
sim_io_check (MMSitClient              *self,
              GAsyncResult             *res,
              guint16                   file_id,
              MMSitSimIoResponse       *out_response,
              GError                  **error)
{
    g_autoptr(MMSitMessage) response = NULL;

    response = mm_sit_client_command_finish (self, res, error);
    if (!response)
        return FALSE;
    if (!mm_sit_message_parse_sim_io (response, out_response, error))
        return FALSE;
    if (!out_response->success) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "the SIM refused a read of EF %04X (sw1 %02X sw2 %02X)",
                     file_id, out_response->sw1, out_response->sw2);
        return FALSE;
    }
    return TRUE;
}

static void
read_ef_data_ready (MMSitClient  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    ReadEfContext      *ctx = g_task_get_task_data (task);
    MMSitSimIoResponse  response;
    GError             *error = NULL;

    if (!sim_io_check (self, res, ctx->file_id, &response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_byte_array_append (ctx->data, response.data, response.data_len);
    if (ctx->info.structure == MM_SIT_EF_TRANSPARENT)
        ctx->remaining -= MIN (ctx->remaining, response.data_len);
    else {
        ctx->next_record++;
        ctx->remaining--;
    }
    read_ef_step (task);
}

static void
read_ef_info_ready (MMSitClient  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    ReadEfContext      *ctx = g_task_get_task_data (task);
    MMSitSimIoResponse  response;
    GError             *error = NULL;

    if (!sim_io_check (self, res, ctx->file_id, &response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!mm_sit_file_control_parameters_parse (response.data, response.data_len,
                                               &ctx->info, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (ctx->info.structure == MM_SIT_EF_TRANSPARENT)
        ctx->remaining = ctx->info.size;
    else {
        ctx->remaining = ctx->info.record_count;
        ctx->next_record = 1;
        if (!ctx->info.record_len) {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                     "EF %04X reports records of no length",
                                     ctx->file_id);
            g_object_unref (task);
            return;
        }
    }
    read_ef_step (task);
}

static void
read_ef_step (GTask *task)
{
    MMSitClient          *self = g_task_get_source_object (task);
    ReadEfContext        *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;
    MMSitSimIoRequest     sim_io = { 0 };
    GAsyncReadyCallback   ready;

    sim_io.file_id  = ctx->file_id;
    sim_io.path_len = ctx->path_len;
    memcpy (sim_io.path, ctx->path, sizeof (sim_io.path));

    if (!ctx->passes) {
        sim_io.command = MM_SIT_SIM_IO_GET_RESPONSE;
        sim_io.p3      = 15;
        ready = (GAsyncReadyCallback) read_ef_info_ready;
    } else if (!ctx->remaining) {
        g_task_return_pointer (task,
                               g_byte_array_free_to_bytes (g_steal_pointer (&ctx->data)),
                               (GDestroyNotify) g_bytes_unref);
        g_object_unref (task);
        return;
    } else if (ctx->passes >= SIM_IO_MAX_PASSES) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "EF %04X needs more than %u reads; giving up with "
                                 "%u of %u bytes",
                                 ctx->file_id, SIM_IO_MAX_PASSES,
                                 ctx->data->len, ctx->info.size);
        g_object_unref (task);
        return;
    } else if (ctx->info.structure == MM_SIT_EF_TRANSPARENT) {
        guint offset = ctx->data->len;

        sim_io.command = MM_SIT_SIM_IO_READ_BINARY;
        sim_io.p1      = (offset >> 8) & 0xff;
        sim_io.p2      = offset & 0xff;
        sim_io.p3      = MIN (ctx->remaining, SIM_IO_MAX_READ);
        ready = (GAsyncReadyCallback) read_ef_data_ready;
    } else {
        sim_io.command = MM_SIT_SIM_IO_READ_RECORD;
        sim_io.p1      = ctx->next_record;
        sim_io.p2      = SIM_IO_RECORD_MODE_ABSOLUTE;
        sim_io.p3      = ctx->info.record_len;
        ready = (GAsyncReadyCallback) read_ef_data_ready;
    }

    ctx->passes++;
    request = mm_sit_request_new_sim_io (next_token (self), &sim_io,
                                         self->priv->sim_aid, self->priv->sim_aid_len,
                                         &error);
    if (!request) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    mm_sit_client_command (self, request, 10, g_task_get_cancellable (task), ready, task);
}

void
mm_sit_client_read_ef (MMSitClient        *self,
                       guint16             file_id,
                       const guint16      *path,
                       guint               path_len,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
    ReadEfContext *ctx;
    GTask         *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (path_len > MM_SIT_MAX_SIM_IO_PATH) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "SIM file path is too deep");
        g_object_unref (task);
        return;
    }

    ctx = g_slice_new0 (ReadEfContext);
    ctx->file_id  = file_id;
    ctx->path_len = path_len;
    memcpy (ctx->path, path, path_len * sizeof (guint16));
    ctx->data = g_byte_array_new ();
    g_task_set_task_data (task, ctx, (GDestroyNotify) read_ef_context_free);

    read_ef_step (task);
}

GBytes *
mm_sit_client_read_ef_finish (MMSitClient  *self,
                              GAsyncResult *res,
                              MMSitEfInfo  *out_info,
                              GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);

    if (out_info) {
        ReadEfContext *ctx = g_task_get_task_data (G_TASK (res));

        *out_info = ctx->info;
    }
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
get_sim_slot_status_ready (MMSitClient *self,
                           GAsyncResult *res,
                           GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitSimSlotStatus     *status;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    status = g_new0 (MMSitSimSlotStatus, 1);
    if (response && mm_sit_message_parse_sim_slot_status (response, status, &error))
        g_task_return_pointer (task, status, g_free);
    else {
        g_free (status);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_sim_slot_status (MMSitClient        *self,
                                   GCancellable       *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_sim_slot_status_ready, task);
}

gboolean
mm_sit_client_get_sim_slot_status_finish (MMSitClient        *self,
                                          GAsyncResult       *res,
                                          MMSitSimSlotStatus *out_status,
                                          GError            **error)
{
    MMSitSimSlotStatus *status;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    status = g_task_propagate_pointer (G_TASK (res), error);
    if (!status)
        return FALSE;
    *out_status = *status;
    g_free (status);
    return TRUE;
}

void
mm_sit_client_set_logical_slot_mapping (MMSitClient                  *self,
                                        const MMSitLogicalSlotMapping *mappings,
                                        guint                         mapping_count,
                                        GCancellable                 *cancellable,
                                        GAsyncReadyCallback           callback,
                                        gpointer                      user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_logical_slot_mapping (next_token (self),
                                                            mappings,
                                                            mapping_count,
                                                            &error);
    if (!request) {
        GTask *task;

        task = g_task_new (self, cancellable, callback, user_data);
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_logical_slot_mapping_finish (MMSitClient  *self,
                                               GAsyncResult *res,
                                               GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * The 0x0701 data-registration state byte only reports registered once the
 * data-call prelude has run: while merely camped the CP answers state 0
 * with the serving RAT (the downstream capture shows 0/rat=14 during the
 * whole camped window, flipping to 1 only ~1.4 s after the initial-attach
 * APN and data profiles are set, right before SETUP_DATA_CALL) -- the
 * vendor RIL's AllowData flow.  Gating ModemManager's 3GPP registration on
 * it therefore deadlocks the core (it requires registration before it
 * connects the bearer that would run those steps).  When 0x0701 reports
 * exactly that hollow idle state, treat a serving PLMN in the operator
 * response as registered -- the camped state that lets the connect
 * sequence do the real work.  Meaningful non-registered states (searching,
 * denied, ...) are preserved.
 */
static void
get_registration_operator_ready (MMSitClient *self,
                                 GAsyncResult *res,
                                 GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitRegistration      *registration = g_task_get_task_data (task);
    MMSitRegistration      *result;
    MMSitOperator           operator_info;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_operator (response, &operator_info, &error)) {
        mm_obj_dbg (self->priv->port, "registration check: reg state %u (registered %s), rat %u, serving plmn '%s'",
                    registration->state,
                    registration->registered ? "yes" : "no",
                    registration->radio_technology,
                    operator_info.plmn);
        if (operator_info.plmn[0] != '\0' && registration->state == 0) {
            mm_obj_dbg (self->priv->port, "registration check: promoting camped PLMN to registered");
            registration->registered = TRUE;
            registration->state = 1;
        }
    } else {
        mm_obj_dbg (self->priv->port, "registration check: no operator response (%s); reg state %u",
                    error ? error->message : "parse failed", registration->state);
    }

    result = g_new (MMSitRegistration, 1);
    *result = *registration;
    g_task_return_pointer (task, result, g_free);
    g_object_unref (task);
}

static void
get_registration_ready (MMSitClient *self,
                        GAsyncResult *res,
                        GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GByteArray)   request = NULL;
    MMSitRegistration      *registration;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    registration = g_new0 (MMSitRegistration, 1);
    if (!response || !mm_sit_message_parse_registration (response, registration, &error)) {
        g_free (registration);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (registration->registered) {
        g_task_return_pointer (task, registration, g_free);
        g_object_unref (task);
        return;
    }

    g_task_set_task_data (task, registration, g_free);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_OPERATOR,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) get_registration_operator_ready,
                           task);
}

void
mm_sit_client_get_registration (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_DATA_REGISTRATION,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_registration_ready, task);
}

gboolean
mm_sit_client_get_registration_finish (MMSitClient       *self,
                                       GAsyncResult      *res,
                                       MMSitRegistration *out_registration,
                                       GError           **error)
{
    MMSitRegistration *registration;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_registration != NULL, FALSE);

    registration = g_task_propagate_pointer (G_TASK (res), error);
    if (!registration)
        return FALSE;
    *out_registration = *registration;
    g_free (registration);
    return TRUE;
}

/*
 * Registration is only reported for the packet-switched domain by 0x0701. The CS
 * domain has its own query, and without it the core consolidates a permanent
 * 'unknown' for circuit-switched service. The two responses share a layout apart
 * from the radio-technology byte, which the codec accounts for.
 */
void
mm_sit_client_get_voice_registration (MMSitClient        *self,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_VOICE_REGISTRATION,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_registration_ready, task);
}

gboolean
mm_sit_client_get_voice_registration_finish (MMSitClient       *self,
                                             GAsyncResult      *res,
                                             MMSitRegistration *out_registration,
                                             GError           **error)
{
    return mm_sit_client_get_registration_finish (self, res, out_registration, error);
}

static void
get_operator_ready (MMSitClient *self,
                    GAsyncResult *res,
                    GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitOperator          *operator_info;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    operator_info = g_new0 (MMSitOperator, 1);
    if (response && mm_sit_message_parse_operator (response, operator_info, &error))
        g_task_return_pointer (task, operator_info, g_free);
    else {
        g_free (operator_info);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_operator (MMSitClient        *self,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_OPERATOR,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_operator_ready, task);
}

gboolean
mm_sit_client_get_operator_finish (MMSitClient   *self,
                                   GAsyncResult  *res,
                                   MMSitOperator *out_operator,
                                   GError       **error)
{
    MMSitOperator *operator_info;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_operator != NULL, FALSE);

    operator_info = g_task_propagate_pointer (G_TASK (res), error);
    if (!operator_info)
        return FALSE;
    *out_operator = *operator_info;
    g_free (operator_info);
    return TRUE;
}

static void
get_network_time_ready (MMSitClient  *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitNitzTime          *time;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    time = g_new0 (MMSitNitzTime, 1);
    if (!response || !mm_sit_message_parse_nitz_time (response, time, &error)) {
        g_free (time);
        g_task_return_error (task, error);
    } else if (!time->valid) {
        /* The CP answered, it just has no NITZ to report -- before the first
         * attach, or on a network that sends none. MM_CORE_ERROR_RETRY is what
         * the Time interface's timezone poll looks for to try again later. */
        g_free (time);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "the modem has not received a network time yet");
    } else
        g_task_return_pointer (task, time, g_free);
    g_object_unref (task);
}

void
mm_sit_client_get_network_time (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_NITZ_TIME, next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_network_time_ready, task);
}

gboolean
mm_sit_client_get_network_time_finish (MMSitClient   *self,
                                       GAsyncResult  *res,
                                       MMSitNitzTime *out_time,
                                       GError       **error)
{
    MMSitNitzTime *time;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_time != NULL, FALSE);

    time = g_task_propagate_pointer (G_TASK (res), error);
    if (!time)
        return FALSE;
    *out_time = *time;
    g_free (time);
    return TRUE;
}

static void
get_signal_ready (MMSitClient *self,
                  GAsyncResult *res,
                  GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitSignal            *signal;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    signal = g_new0 (MMSitSignal, 1);
    if (response && mm_sit_message_parse_signal (response, signal, &error))
        g_task_return_pointer (task, signal, g_free);
    else {
        g_free (signal);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_signal (MMSitClient        *self,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH,
                                        next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_signal_ready, task);
}

gboolean
mm_sit_client_get_signal_finish (MMSitClient *self,
                                 GAsyncResult *res,
                                 MMSitSignal  *out_signal,
                                 GError      **error)
{
    MMSitSignal *signal;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_signal != NULL, FALSE);

    signal = g_task_propagate_pointer (G_TASK (res), error);
    if (!signal)
        return FALSE;
    *out_signal = *signal;
    g_free (signal);
    return TRUE;
}

static void
get_cell_info_ready (MMSitClient *self,
                     GAsyncResult *res,
                     GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GArray                 *cells = NULL;
    GArray                 *nr_cells = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_cell_info_list (response, &cells, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* One response carries both views. NR records only appear under EN-DC, so an
     * absent NR list is the normal LTE-only case, not a failure. */
    if (!mm_sit_message_parse_nr_cell_info_list (response, &nr_cells, &error)) {
        mm_obj_dbg (self->priv->port, "cell info: NR records unparsable: %s", error->message);
        g_clear_error (&error);
        nr_cells = g_array_new (FALSE, FALSE, sizeof (MMSitCellInfoNr));
    }
    g_task_set_task_data (task, nr_cells, (GDestroyNotify) g_array_unref);
    g_task_return_pointer (task, cells, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

void
mm_sit_client_get_cell_info (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_CELL_INFO_LIST, next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_cell_info_ready, task);
}

GArray *
mm_sit_client_get_cell_info_finish (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GArray      **out_nr_cells,
                                    GError      **error)
{
    GArray *lte;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);

    lte = g_task_propagate_pointer (G_TASK (res), error);
    if (lte && out_nr_cells) {
        GArray *nr = g_task_get_task_data (G_TASK (res));

        *out_nr_cells = nr ? g_array_ref (nr) : NULL;
    }
    return lte;
}

static void
get_call_list_ready (MMSitClient  *self,
                     GAsyncResult *res,
                     GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GArray                 *calls = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_call_list (response, &calls, &error))
        g_task_return_pointer (task, calls, (GDestroyNotify) g_array_unref);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

void
mm_sit_client_get_call_list (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_call_list (next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_call_list_ready, task);
}

GArray *
mm_sit_client_get_call_list_finish (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

void
mm_sit_client_dial (MMSitClient        *self,
                    const gchar        *number,
                    GCancellable       *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GError               *error = NULL;

    request = mm_sit_request_new_dial (next_token (self), number, &error);
    if (!request) {
        GTask *task;

        task = g_task_new (self, cancellable, callback, user_data);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* The CP acks DIAL quickly; call *progress* then arrives as
     * CALL_STATE_CHANGED indications, not in this response. */
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_dial_finish (MMSitClient  *self,
                           GAsyncResult *res,
                           GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_answer (MMSitClient        *self,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_answer (next_token (self));
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_answer_finish (MMSitClient  *self,
                             GAsyncResult *res,
                             GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_hangup (MMSitClient        *self,
                      guint32             call_id,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_hangup (next_token (self), call_id);
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_hangup_finish (MMSitClient  *self,
                             GAsyncResult *res,
                             GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_indication_filter (MMSitClient        *self,
                                     guint32             filter,
                                     GCancellable       *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_indication_filter (next_token (self), filter);
    run_checked_command (self, request, 5, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_indication_filter_finish (MMSitClient  *self,
                                            GAsyncResult *res,
                                            GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_screen_state (MMSitClient        *self,
                                gboolean            screen_on,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_screen_state (next_token (self), screen_on);
    run_checked_command (self, request, 5, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_screen_state_finish (MMSitClient  *self,
                                       GAsyncResult *res,
                                       GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_carrier_manifest (MMSitClient         *self,
                                    const gchar         *directory,
                                    const guint8         sha1[MM_SIT_SHA1_LENGTH],
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GError               *error = NULL;

    request = mm_sit_request_new_carrier_manifest (next_token (self), directory, sha1, &error);
    if (!request) {
        g_task_report_error (self, callback, user_data,
                             mm_sit_client_set_carrier_manifest, error);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_carrier_manifest_finish (MMSitClient  *self,
                                           GAsyncResult *res,
                                           GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/* Identity handshake the downstream RIL performs once per CP boot before
 * turning the radio on: the CP parks a radio-on request that arrives before
 * it has digested this batch (and the carrier-config manifest), leaving the
 * radio in state "on" but never searching.  The device strings are the
 * captured corpus values; the CP answers 0x0922 with a capability list. */
#define BOOT_CONFIG_DEVICE_MODEL "Pixel 9a"
#define BOOT_CONFIG_DEVICE_BUILD "BP4A.251205.006"

typedef enum {
    BOOT_CONFIG_STEP_AP_TIME,
    BOOT_CONFIG_STEP_DEVICE_INFO,
    BOOT_CONFIG_STEP_OEM_CONFIG,
    BOOT_CONFIG_STEP_MODEM_CONFIG,
    BOOT_CONFIG_STEP_LAST,
} BootConfigStep;

typedef struct {
    BootConfigStep step;
} BootConfigContext;

static void boot_config_step (GTask *task);

static void
boot_config_step_ready (MMSitClient *self,
                        GAsyncResult *res,
                        GTask *task)
{
    BootConfigContext *ctx = g_task_get_task_data (task);
    GError            *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx->step++;
    boot_config_step (task);
}

static void
boot_config_step (GTask *task)
{
    MMSitClient           *self = g_task_get_source_object (task);
    BootConfigContext     *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray)  request = NULL;
    GError                *error = NULL;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case BOOT_CONFIG_STEP_AP_TIME: {
        g_autoptr(GDateTime) now = g_date_time_new_now_local ();

        request = mm_sit_request_new_set_ap_system_time (next_token (self), now, &error);
        break;
    }
    case BOOT_CONFIG_STEP_DEVICE_INFO:
        request = mm_sit_request_new_device_info (next_token (self),
                                                  BOOT_CONFIG_DEVICE_MODEL,
                                                  BOOT_CONFIG_DEVICE_BUILD,
                                                  &error);
        break;
    case BOOT_CONFIG_STEP_OEM_CONFIG:
        request = mm_sit_request_new_oem_config (next_token (self));
        break;
    case BOOT_CONFIG_STEP_MODEM_CONFIG:
        request = mm_sit_request_new_set_modem_config (next_token (self));
        break;
    case BOOT_CONFIG_STEP_LAST:
        self->priv->boot_config_done = TRUE;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    default:
        g_assert_not_reached ();
    }

    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, g_task_get_cancellable (task),
                         (GAsyncReadyCallback) boot_config_step_ready, task);
}

void
mm_sit_client_send_boot_config (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    BootConfigContext *ctx;
    GTask             *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (self->priv->boot_config_done) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    ctx = g_new0 (BootConfigContext, 1);
    g_task_set_task_data (task, ctx, g_free);
    boot_config_step (task);
}

gboolean
mm_sit_client_send_boot_config_finish (MMSitClient  *self,
                                       GAsyncResult *res,
                                       GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

typedef enum {
    CONNECT_STEP_PREFERRED_MODEM,
    CONNECT_STEP_DATA_PROFILE,
    CONNECT_STEP_ATTACH_APN,
    CONNECT_STEP_CONFIG_0900,
    CONNECT_STEP_CONFIG_0106,
    CONNECT_STEP_CONFIG_0107,
    CONNECT_STEP_REGISTRATION,
    CONNECT_STEP_SETUP,
    CONNECT_STEP_FAST_DORMANCY,
    CONNECT_STEP_LAST,
} ConnectStep;

typedef struct {
    ConnectStep    step;
    gchar         *apn;
    MMSitPdpType   pdp;
    /* Credentials, owned: the properties they came from need not outlive the
     * connect attempt. auth.user/password point into these two. */
    MMSitApnAuth   auth;
    gchar         *auth_user;
    gchar         *auth_password;
    guint32        apn_types;
    guint          registration_attempts;
    guint          setup_attempts;
    guint          retry_id;
    MMSitDataCall  call;
    GArray        *indicated_calls; /* latest DATA_CALL_LIST_CHANGED list */
    MMSitClient   *client;
    gulong         indication_handler_id;
} ConnectContext;

static void connect_step (GTask *task);

static void
connect_context_free (ConnectContext *ctx)
{
    if (ctx->retry_id)
        g_source_remove (ctx->retry_id);
    if (ctx->indication_handler_id)
        g_signal_handler_disconnect (ctx->client, ctx->indication_handler_id);
    g_clear_pointer (&ctx->indicated_calls, g_array_unref);
    g_clear_object (&ctx->client);
    g_free (ctx->apn);
    g_free (ctx->auth_user);
    g_free (ctx->auth_password);
    g_free (ctx);
}

/* Pick the connected call we set up. Prefer the entry whose cid matches the one
 * the SETUP response assigned; once known, a non-matching list must not be used
 * (a DATA_CALL_LIST_CHANGED may also carry an IMS/tethering PDN). Before the cid
 * is known (0), fall back to the first call that already has an address. */
static const MMSitDataCall *
connect_pick_call (GArray *calls, guint8 cid)
{
    const MMSitDataCall *fallback = NULL;
    guint                i;

    for (i = 0; i < calls->len; i++) {
        const MMSitDataCall *c = &g_array_index (calls, MMSitDataCall, i);

        if (!c->has_ipv4)
            continue;
        if (cid && c->cid == cid)
            return c;
        if (!fallback)
            fallback = c;
    }
    return cid ? NULL : fallback;
}

static void
connect_indication (MMSitClient        *self,
                    const MMSitMessage *message,
                    GTask              *task)
{
    ConnectContext      *ctx = g_task_get_task_data (task);
    g_autoptr(GArray)    calls = NULL;
    const MMSitDataCall *chosen;

    if (message->id != MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED ||
        !mm_sit_message_parse_data_call_list (message, &calls, NULL))
        return;

    /* Keep the latest list so connect_setup_ready can match our cid once the
     * SETUP response assigns one. */
    g_clear_pointer (&ctx->indicated_calls, g_array_unref);
    ctx->indicated_calls = g_array_ref (calls);

    /* Advance a retrying SETUP only once our own call has an address. */
    if (ctx->step == CONNECT_STEP_SETUP && ctx->retry_id &&
        (chosen = connect_pick_call (calls, ctx->call.cid))) {
        g_source_remove (ctx->retry_id);
        ctx->retry_id = 0;
        ctx->call = *chosen;
        ctx->step++;
        connect_step (task);
    }
}

static gboolean
connect_retry (GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);

    ctx->retry_id = 0;
    connect_step (task);
    return G_SOURCE_REMOVE;
}

static void
connect_checked_ready (MMSitClient *self,
                       GAsyncResult *res,
                       GTask *task)
{
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ((ConnectContext *) g_task_get_task_data (task))->step++;
    connect_step (task);
}

static void
connect_registration_ready (MMSitClient *self,
                            GAsyncResult *res,
                            GTask *task)
{
    ConnectContext    *ctx = g_task_get_task_data (task);
    MMSitRegistration  registration;
    GError             *error = NULL;

    if (!mm_sit_client_get_registration_finish (self, res, &registration, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (registration.registered) {
        ctx->step++;
        connect_step (task);
        return;
    }

    ctx->registration_attempts++;
    if (ctx->registration_attempts >= 20) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                 "SIT modem did not register for packet data after 20 attempts");
        g_object_unref (task);
        return;
    }
    ctx->retry_id = g_timeout_add_seconds (2, (GSourceFunc) connect_retry, task);
}

static void
connect_setup_ready (MMSitClient *self,
                     GAsyncResult *res,
                     GTask *task)
{
    ConnectContext          *ctx = g_task_get_task_data (task);
    g_autoptr(MMSitMessage)  response = NULL;
    GError                  *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_data_call (response, &ctx->call, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* The SETUP response assigns the cid but may not carry the address yet; take
     * it from the matching entry of the latest DATA_CALL_LIST_CHANGED list. */
    if (!ctx->call.has_ipv4 && ctx->indicated_calls) {
        const MMSitDataCall *m = connect_pick_call (ctx->indicated_calls, ctx->call.cid);

        if (m)
            ctx->call = *m;
    }

    if (ctx->call.has_ipv4) {
        ctx->step++;
        connect_step (task);
        return;
    }

    ctx->setup_attempts++;
    if (ctx->setup_attempts >= 6) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "SIT data call returned no address after 6 attempts (cause %u)",
                                 ctx->call.cause);
        g_object_unref (task);
        return;
    }
    ctx->retry_id = g_timeout_add_seconds (3, (GSourceFunc) connect_retry, task);
}

static GByteArray *
connect_build_request (MMSitClient   *self,
                       ConnectContext *ctx,
                       GError        **error)
{
    guint8 body_0106[12] = { 0 };
    guint8 body_0107[8] = { 0 };
    guint8 reserved[4] = { 0 };

    switch (ctx->step) {
    case CONNECT_STEP_PREFERRED_MODEM:
        return mm_sit_request_new_set_preferred_modem (next_token (self), 0);
    case CONNECT_STEP_DATA_PROFILE:
        return mm_sit_request_new_set_data_profile (next_token (self), ctx->apn, ctx->pdp, ctx->apn_types, &ctx->auth, error);
    case CONNECT_STEP_ATTACH_APN:
        return mm_sit_request_new_set_initial_attach_apn (next_token (self), ctx->apn, ctx->pdp, &ctx->auth, error);
    case CONNECT_STEP_CONFIG_0900:
        return mm_sit_request_new (0x0900, next_token (self), reserved, sizeof (reserved), error);
    case CONNECT_STEP_CONFIG_0106:
        body_0106[4] = 0x01;
        body_0106[6] = 0x11;
        body_0106[7] = 0x04;
        body_0106[8] = 0x11;
        body_0106[10] = 0xff;
        body_0106[11] = 0x01;
        return mm_sit_request_new (0x0106, next_token (self), body_0106, sizeof (body_0106), error);
    case CONNECT_STEP_CONFIG_0107:
        return mm_sit_request_new (0x0107, next_token (self), body_0107, sizeof (body_0107), error);
    case CONNECT_STEP_SETUP:
        return mm_sit_request_new_setup_data_call (next_token (self), ctx->apn, ctx->pdp, ctx->apn_types, &ctx->auth, error);
    case CONNECT_STEP_FAST_DORMANCY:
        return mm_sit_request_new_set_fast_dormancy_info (next_token (self));
    case CONNECT_STEP_REGISTRATION:
    case CONNECT_STEP_LAST:
    default:
        return NULL;
    }
}

static void
connect_step (GTask *task)
{
    MMSitClient            *self = g_task_get_source_object (task);
    ConnectContext         *ctx = g_task_get_task_data (task);
    GCancellable           *cancellable = g_task_get_cancellable (task);
    g_autoptr(GByteArray)   request = NULL;
    GError                 *error = NULL;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    if (ctx->step == CONNECT_STEP_LAST) {
        MMSitDataCall *result = g_new (MMSitDataCall, 1);

        *result = ctx->call;
        g_task_return_pointer (task, result, g_free);
        g_object_unref (task);
        return;
    }

    if (ctx->step == CONNECT_STEP_REGISTRATION) {
        mm_sit_client_get_registration (self,
                                        cancellable,
                                        (GAsyncReadyCallback) connect_registration_ready,
                                        task);
        return;
    }

    request = connect_build_request (self, ctx, &error);
    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (ctx->step == CONNECT_STEP_SETUP)
        mm_sit_client_command (self, request, 10, cancellable,
                               (GAsyncReadyCallback) connect_setup_ready, task);
    else
        run_checked_command (self, request, 5, cancellable,
                             (GAsyncReadyCallback) connect_checked_ready, task);
}

void
mm_sit_client_connect_data (MMSitClient        *self,
                            const gchar        *apn,
                            MMSitPdpType        pdp,
                            guint32             apn_types,
                            const MMSitApnAuth *auth,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
    ConnectContext *ctx;
    GTask          *task;

    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    task = g_task_new (self, cancellable, callback, user_data);
    if (!apn || !apn[0] || strlen (apn) > MM_SIT_MAX_APN_LENGTH) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "A valid APN of at most %u bytes is required",
                                 MM_SIT_MAX_APN_LENGTH);
        g_object_unref (task);
        return;
    }

    ctx = g_new0 (ConnectContext, 1);
    ctx->apn = g_strdup (apn);
    ctx->pdp = pdp;
    ctx->apn_types = apn_types;
    if (auth) {
        ctx->auth_user = g_strdup (auth->user);
        ctx->auth_password = g_strdup (auth->password);
        ctx->auth.auth = auth->auth;
        ctx->auth.user = ctx->auth_user;
        ctx->auth.password = ctx->auth_password;
    }
    ctx->client = g_object_ref (self);
    g_task_set_task_data (task, ctx, (GDestroyNotify) connect_context_free);
    ctx->indication_handler_id = g_signal_connect (self, "indication",
                                                    G_CALLBACK (connect_indication), task);
    connect_step (task);
}

gboolean
mm_sit_client_connect_data_finish (MMSitClient    *self,
                                   GAsyncResult   *res,
                                   MMSitDataCall  *out_call,
                                   GError        **error)
{
    MMSitDataCall *call;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_call != NULL, FALSE);

    call = g_task_propagate_pointer (G_TASK (res), error);
    if (!call)
        return FALSE;
    *out_call = *call;
    g_free (call);
    return TRUE;
}

/* Second attempt at a teardown the CP refused, with the reason its own RIL
 * reserves for a context that is up but broken. */
static void
disconnect_reset_ready (MMSitClient  *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_normal_ready (MMSitClient  *self,
                         GAsyncResult *res,
                         GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GByteArray)   retry = NULL;
    g_autoptr(GError)       error = NULL;
    guint8                  cid;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && message_check_success (response, &error)) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /*
     * A NORMAL deactivate is refused (status 2, GENERIC_FAILURE) for a context
     * the CP will not drop on request -- seen after a long suspend, where the
     * network had degraded a dual-stack context to IPv4-only.  Leaving it up is
     * worse than it sounds: SETUP_DATA_CALL then keeps handing that same
     * context back and ignores the PDP type asked for, so the bearer can never
     * renegotiate its address families.  The RIL's own recovery for this is
     * DEACT_REASON_PDP_RESET, so escalate once before giving up.
     */
    cid = GPOINTER_TO_UINT (g_task_get_task_data (task));
    mm_obj_dbg (self->priv->port,
                "data call %u refused a normal deactivate (%s); retrying as PDP reset",
                cid, error->message);

    retry = mm_sit_request_new_deactivate_data_call (next_token (self), cid,
                                                     MM_SIT_DEACT_REASON_PDP_RESET);
    mm_sit_client_command (self, retry, 10, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) disconnect_reset_ready, task);
}

void
mm_sit_client_disconnect_data (MMSitClient        *self,
                               guint8              cid,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER ((guint) cid), NULL);
    request = mm_sit_request_new_deactivate_data_call (next_token (self), cid,
                                                       MM_SIT_DEACT_REASON_NORMAL);
    mm_sit_client_command (self, request, 10, cancellable,
                           (GAsyncReadyCallback) disconnect_normal_ready, task);
}

gboolean
mm_sit_client_disconnect_data_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * PLMN selection. Automatic selection is what recovers a modem that has fallen
 * out of service: the CP only starts searching on its own when radio power is
 * first enabled, so without this nothing re-triggers a search and the modem
 * stays unregistered indefinitely. The ack is empty; the outcome shows up as a
 * SERVING_NETWORK indication once the CP finds a network.
 */
void
mm_sit_client_set_network_selection_auto (MMSitClient        *self,
                                          GCancellable       *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_network_selection_auto (next_token (self));
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_network_selection_auto_finish (MMSitClient  *self,
                                                 GAsyncResult *res,
                                                 GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_network_selection_manual_ready (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* A refusal is the CP's status byte -- captured ones carry 2
     * (GENERIC_FAILURE) and 7 (CANCELLED) -- so message_check_success above has
     * already caught it and there is nothing further to inspect. */
    if (!mm_sit_message_parse_network_selection_manual_ack (response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*
 * Manual selection. Downstream stops any running scan first, then asks for the
 * PLMN on a specific RAT; a refused network is answered after a long delay (over
 * a minute in the capture), so the timeout here is generous.
 */
void
mm_sit_client_set_network_selection_manual (MMSitClient        *self,
                                            const gchar        *plmn,
                                            guint8              rat,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_set_network_selection_manual (next_token (self), plmn,
                                                              rat, &error);
    if (!request) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    mm_sit_client_command (self, request, 180, cancellable,
                           (GAsyncReadyCallback) set_network_selection_manual_ready, task);
}

gboolean
mm_sit_client_set_network_selection_manual_finish (MMSitClient  *self,
                                                   GAsyncResult *res,
                                                   GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
get_facility_lock_ready (MMSitClient  *self,
                         GAsyncResult *res,
                         GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    gboolean                locked = FALSE;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_facility_lock (response, &locked, &error))
        g_task_return_boolean (task, locked);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

/*
 * Whether one facility is locked. The reply carries no retry counter -- like
 * classic RIL, the CP only reports remaining attempts in the response to an
 * actual PIN operation -- so this reads the lock state without spending one.
 */
void
mm_sit_client_get_facility_lock (MMSitClient        *self,
                                 guint8              facility,
                                 GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_facility_lock (next_token (self), facility);
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_facility_lock_ready, task);
}

gboolean
mm_sit_client_get_facility_lock_finish (MMSitClient  *self,
                                        GAsyncResult *res,
                                        gboolean     *out_locked,
                                        GError      **error)
{
    GError *inner = NULL;
    gboolean locked;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_locked != NULL, FALSE);

    /* The task's boolean *is* the lock state, so a FALSE result is only an error
     * when one was set. */
    locked = g_task_propagate_boolean (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return FALSE;
    }
    *out_locked = locked;
    return TRUE;
}

static void
get_allowed_network_types_ready (MMSitClient  *self,
                                 GAsyncResult *res,
                                 GTask        *task)
{
    g_autoptr(MMSitMessage)  response = NULL;
    MMSitNetworkGenerations *generations;
    GError                  *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    generations = g_new0 (MMSitNetworkGenerations, 1);
    if (response && mm_sit_message_parse_allowed_network_types (response, generations, &error))
        g_task_return_pointer (task, generations, g_free);
    else {
        g_free (generations);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

/*
 * Which radio generations the CP is allowed to use. Downstream sets the
 * all-generations bitmap on every boot and narrows it only when the user picks a
 * specific mode; the CP keeps the value across our restarts, so it has to be
 * read back rather than assumed.
 */
void
mm_sit_client_get_allowed_network_types (MMSitClient        *self,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_allowed_network_types (next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_allowed_network_types_ready, task);
}

gboolean
mm_sit_client_get_allowed_network_types_finish (MMSitClient             *self,
                                               GAsyncResult            *res,
                                               MMSitNetworkGenerations *out_generations,
                                               GError                 **error)
{
    MMSitNetworkGenerations *generations;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_generations != NULL, FALSE);

    generations = g_task_propagate_pointer (G_TASK (res), error);
    if (!generations)
        return FALSE;
    *out_generations = *generations;
    g_free (generations);
    return TRUE;
}

void
mm_sit_client_set_initial_attach_apn (MMSitClient        *self,
                                      const gchar        *apn,
                                      MMSitPdpType        pdp,
                                      const MMSitApnAuth *auth,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_initial_attach_apn (next_token (self), apn, pdp, auth, &error);
    if (!request) {
        GTask *task = g_task_new (self, cancellable, callback, user_data);

        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_initial_attach_apn_finish (MMSitClient  *self,
                                             GAsyncResult *res,
                                             GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_allowed_network_types (MMSitClient                   *self,
                                         const MMSitNetworkGenerations *generations,
                                         GCancellable                  *cancellable,
                                         GAsyncReadyCallback            callback,
                                         gpointer                       user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_allowed_network_types (next_token (self), generations, &error);
    if (!request) {
        GTask *task = g_task_new (self, cancellable, callback, user_data);

        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_allowed_network_types_finish (MMSitClient  *self,
                                                GAsyncResult *res,
                                                GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * Network scan. The CP has no "search complete" message: it pushes a
 * NET_SCAN_RESULT batch every few seconds, each one repeating every network
 * found so far, until it is told to stop. In the captures the batches arrive
 * 3-6 s apart and downstream stops the scan a few seconds after the last one, so
 * the scan is treated as finished once a batch has gone unanswered for
 * SCAN_QUIET_SECONDS, with SCAN_MAX_SECONDS as the hard stop.
 */
#define SCAN_QUIET_SECONDS 10
#define SCAN_MAX_SECONDS   70

typedef struct {
    GArray *networks;
    gulong  indication_id;
    guint   quiet_id;
    guint   deadline_id;
} ScanContext;

static void
scan_context_free (ScanContext *ctx)
{
    if (ctx->quiet_id)
        g_source_remove (ctx->quiet_id);
    if (ctx->deadline_id)
        g_source_remove (ctx->deadline_id);
    if (ctx->networks)
        g_array_unref (ctx->networks);
    g_free (ctx);
}

static void
scan_stop_ready (MMSitClient  *self,
                 GAsyncResult *res,
                 gpointer      user_data)
{
    g_autoptr(GError) error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error))
        mm_obj_dbg (self->priv->port, "network scan: stop request failed: %s", error->message);
}

/* Detach from the indication stream and stop the CP's search, then hand the
 * caller whatever the last batch held. */
static void
scan_complete (GTask *task)
{
    MMSitClient          *self = g_task_get_source_object (task);
    ScanContext          *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;

    if (ctx->indication_id) {
        g_signal_handler_disconnect (self, ctx->indication_id);
        ctx->indication_id = 0;
    }
    if (ctx->quiet_id) {
        g_source_remove (ctx->quiet_id);
        ctx->quiet_id = 0;
    }
    if (ctx->deadline_id) {
        g_source_remove (ctx->deadline_id);
        ctx->deadline_id = 0;
    }

    request = mm_sit_request_new_stop_network_scan (next_token (self));
    run_checked_command (self, request, 10, NULL,
                         (GAsyncReadyCallback) scan_stop_ready, NULL);

    if (ctx->networks) {
        g_task_return_pointer (task, g_steal_pointer (&ctx->networks),
                               (GDestroyNotify) g_array_unref);
    } else {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "CP reported no networks before the scan timed out");
    }
    g_object_unref (task);
}

static gboolean
scan_quiet_elapsed (GTask *task)
{
    ScanContext *ctx = g_task_get_task_data (task);

    ctx->quiet_id = 0;
    scan_complete (task);
    return G_SOURCE_REMOVE;
}

static gboolean
scan_deadline_elapsed (GTask *task)
{
    ScanContext *ctx = g_task_get_task_data (task);

    ctx->deadline_id = 0;
    scan_complete (task);
    return G_SOURCE_REMOVE;
}

static void
scan_indication (MMSitClient        *self,
                 const MMSitMessage *message,
                 GTask              *task)
{
    ScanContext       *ctx = g_task_get_task_data (task);
    GArray            *networks = NULL;
    g_autoptr(GError)  error = NULL;

    if (message->id != MM_SIT_MESSAGE_NET_SCAN_RESULT)
        return;

    if (!mm_sit_message_parse_net_scan_result (message, &networks, &error)) {
        mm_obj_dbg (self->priv->port, "network scan: unparsable result batch: %s",
                    error->message);
        return;
    }

    mm_obj_dbg (self->priv->port, "network scan: batch of %u network(s)", networks->len);
    /* Each batch supersedes the previous one rather than adding to it. */
    if (ctx->networks)
        g_array_unref (ctx->networks);
    ctx->networks = networks;

    if (ctx->quiet_id)
        g_source_remove (ctx->quiet_id);
    ctx->quiet_id = g_timeout_add_seconds (SCAN_QUIET_SECONDS,
                                           (GSourceFunc) scan_quiet_elapsed, task);
}

static void
scan_start_ready (MMSitClient  *self,
                  GAsyncResult *res,
                  GTask        *task)
{
    ScanContext *ctx = g_task_get_task_data (task);
    GError      *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->indication_id = g_signal_connect (self, "indication",
                                           G_CALLBACK (scan_indication), task);
    ctx->deadline_id = g_timeout_add_seconds (SCAN_MAX_SECONDS,
                                              (GSourceFunc) scan_deadline_elapsed, task);
}

void
mm_sit_client_scan_networks (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, g_new0 (ScanContext, 1), (GDestroyNotify) scan_context_free);

    request = mm_sit_request_new_start_network_scan (next_token (self));
    run_checked_command (self, request, 10, cancellable,
                         (GAsyncReadyCallback) scan_start_ready, task);
}

GArray *
mm_sit_client_scan_networks_finish (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
dispose (GObject *object)
{
    MMSitClient *self = MM_SIT_CLIENT (object);

    mm_sit_client_close (self);
    g_clear_object (&self->priv->port);
    G_OBJECT_CLASS (mm_sit_client_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMSitClient *self = MM_SIT_CLIENT (object);

    g_free (self->priv->device);
    g_free (self->priv->proxy_fallback_device);
    G_OBJECT_CLASS (mm_sit_client_parent_class)->finalize (object);
}

static void
mm_sit_client_init (MMSitClient *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_SIT_CLIENT,
                                              MMSitClientPrivate);
}

static void
mm_sit_client_class_init (MMSitClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSitClientPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;

    signals[SIGNAL_INDICATION] =
        g_signal_new ("indication",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_POINTER);
}
