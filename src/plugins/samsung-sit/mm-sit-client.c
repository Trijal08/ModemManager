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
    gchar           *device;
    MMPortSerialSit *port;
    guint            unsolicited_handler_id;
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
mm_sit_client_open (MMSitClient  *self,
                    GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    if (mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port)))
        return TRUE;
    if (!mm_port_serial_open (MM_PORT_SERIAL (self->priv->port), error))
        return FALSE;

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
    gboolean        request_sent;
} SetRadioContext;

static void set_radio_poll (GTask *task);
static void set_radio_request_ready (MMSitClient *self,
                                     GAsyncResult *res,
                                     GTask *task);

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
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    if (!ctx->request_sent) {
        g_autoptr(GByteArray) request = NULL;

        ctx->request_sent = TRUE;
        request = mm_sit_request_new_set_radio_power (next_token (self), ctx->state);
        run_checked_command (self, request, 30, g_task_get_cancellable (task),
                             (GAsyncReadyCallback) set_radio_request_ready, task);
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
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
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
    SetRadioContext *ctx;
    GTask           *task;

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
    set_radio_poll (task);
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
    if (response && mm_sit_message_parse_sim_status (response, status, &error))
        g_task_return_pointer (task, status, g_free);
    else {
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

static void
get_registration_ready (MMSitClient *self,
                        GAsyncResult *res,
                        GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitRegistration     *registration;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    registration = g_new0 (MMSitRegistration, 1);
    if (response && mm_sit_message_parse_registration (response, registration, &error))
        g_task_return_pointer (task, registration, g_free);
    else {
        g_free (registration);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
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

typedef enum {
    CONNECT_STEP_PREFERRED_MODEM,
    CONNECT_STEP_DATA_PROFILE,
    CONNECT_STEP_ATTACH_APN,
    CONNECT_STEP_CONFIG_0900,
    CONNECT_STEP_CONFIG_0106,
    CONNECT_STEP_CONFIG_0107,
    CONNECT_STEP_REGISTRATION,
    CONNECT_STEP_SETUP,
    CONNECT_STEP_DATA_CALL_LIST,
    CONNECT_STEP_LAST,
} ConnectStep;

typedef struct {
    ConnectStep    step;
    gchar         *apn;
    guint          registration_attempts;
    guint          setup_attempts;
    guint          retry_id;
    MMSitDataCall  call;
    MMSitDataCall  indicated_call;
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
    g_clear_object (&ctx->client);
    g_free (ctx->apn);
    g_free (ctx);
}

static void
connect_indication (MMSitClient        *self,
                    const MMSitMessage *message,
                    GTask              *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);
    MMSitDataCall   call;

    if (message->id != MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED ||
        !mm_sit_message_parse_data_call (message, &call, NULL) ||
        !call.has_ipv4)
        return;

    ctx->indicated_call = call;
    if (ctx->step == CONNECT_STEP_SETUP && ctx->retry_id) {
        g_source_remove (ctx->retry_id);
        ctx->retry_id = 0;
        ctx->call = call;
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

    if (!ctx->call.has_ipv4 && ctx->indicated_call.has_ipv4)
        ctx->call = ctx->indicated_call;

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
        return mm_sit_request_new_set_data_profile (next_token (self), ctx->apn, error);
    case CONNECT_STEP_ATTACH_APN:
        return mm_sit_request_new_set_initial_attach_apn (next_token (self), ctx->apn, error);
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
        return mm_sit_request_new_setup_data_call (next_token (self), ctx->apn, error);
    case CONNECT_STEP_DATA_CALL_LIST:
        return mm_sit_request_new_get_data_call_list (next_token (self));
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

void
mm_sit_client_disconnect_data (MMSitClient        *self,
                               guint8              cid,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_deactivate_data_call (next_token (self), cid, 0);
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_disconnect_data_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
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
