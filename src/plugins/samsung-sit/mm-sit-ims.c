/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

/*
 * The CP's IMS stack, driven from the AP.
 *
 * It does not register by itself. What it needs is four requests, in order:
 *
 *   SETUP_DATA_CALL(ims)   its own context, asking the network for P-CSCF
 *   STACK_START  (0x0d39)
 *   HIDDEN_MENU  (0x0d07)  the configuration, User-Agent included
 *   ADD_PDN_INFO (0x0d08)  the granted address and the proxies to register at
 *
 * and then it reports REGISTRATION (0x0d05) about a second later. That sequence
 * and its timing are HW-validated; see research/ims.md.
 *
 * RAT is the trigger, registration is the gate. A packet-switched RAT is what
 * makes IMS possible, so it starts the sequence and its loss ends it -- but
 * whether a call can go over IMS is only ever what the CP last said, because
 * the network can refuse on a perfectly good LTE cell, and because during the
 * seconds either side of a RAT change a test on RAT is simply wrong.
 */

#include <string.h>

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-log-object.h"
#include "mm-sit-ims.h"
#include "mm-sit-protocol.h"

/* The APN an IMS context uses. Not configurable yet: every capture and every
 * carrier config seen so far names it "ims", and the day one does not, it will
 * come from the same database the rest of the configuration will. */
#define IMS_APN "ims"

/* How long to wait before trying again after the CP refuses a step. The CP
 * throttles its own registration attempts (RILC_UNSOL_IMS_THROTTLED_TIME), so
 * being patient here costs nothing and hammering it would be actively wrong. */
#define RETRY_TIMEOUT_SECS 30

typedef enum {
    IMS_STATE_STOPPED,
    IMS_STATE_STARTING,
    IMS_STATE_STARTED,
} ImsState;

struct _MMSitImsPrivate {
    MMSitClient *client; /* borrowed: the modem owns it and outlives us */
    guint        port_index;
    gulong       indication_id;

    ImsState     state;
    gboolean     have_identity;
    MMSitImsDeviceIdentity device;
    gchar       *manufacturer;
    gchar       *model;
    gchar       *os_name;
    gchar       *os_version;
    gchar       *software_version;
    gchar       *operator_code;
    /* Remembered so learning the operator, or the data connection coming up,
     * can start what the RAT alone could not. */
    MMModemAccessTechnology last_act;
    gboolean                data_connected;

    /* The context the CP granted, kept because every later step describes it --
     * a retry that had forgotten it would hand the stack an empty PDN. */
    gboolean      pdn_up;
    guint8        pdn_cid;
    MMSitDataCall pdn;

    gboolean     registered;
    gchar       *associated_uris;

    GCancellable *cancellable;
    guint         retry_id;
};

static void log_object_iface_init (MMLogObjectInterface *iface);

G_DEFINE_TYPE_EXTENDED (MMSitIms, mm_sit_ims, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_LOG_OBJECT, log_object_iface_init))

/* Reshape a data call into what AIMS ADD_PDN_INFO wants: the two describe the
 * same context and only the shape differs. Here rather than in the SIT protocol
 * module because it is the one place the two protocols meet. */
static void
data_call_to_aims_pdn_info (const MMSitDataCall *call,
                                   SamsungAimsPdnInfo  *out_info)
{
    guint i;

    g_return_if_fail (call != NULL);
    g_return_if_fail (out_info != NULL);

    memset (out_info, 0, sizeof (*out_info));
    /* ApnNetType's IMS category; the only one this plugin activates. */
    out_info->category = 2;
    /* What was granted, not what was asked for: the CP's own addr_type. */
    out_info->ip_type = (call->has_ipv4 ? 1 : 0) | (call->has_ipv6 ? 2 : 0);

    if (call->has_ipv4)
        g_strlcpy (out_info->address_v4, call->ipv4, sizeof (out_info->address_v4));
    if (call->has_ipv6)
        g_strlcpy (out_info->address_v6, call->ipv6, sizeof (out_info->address_v6));

    out_info->pcscf_v4_count = call->pcscf_v4_count;
    for (i = 0; i < call->pcscf_v4_count; i++)
        g_strlcpy (out_info->pcscf_v4[i], call->pcscf_v4[i], sizeof (out_info->pcscf_v4[i]));
    out_info->pcscf_v6_count = call->pcscf_v6_count;
    for (i = 0; i < call->pcscf_v6_count; i++)
        g_strlcpy (out_info->pcscf_v6[i], call->pcscf_v6[i], sizeof (out_info->pcscf_v6[i]));

    g_strlcpy (out_info->dns_v4[0], call->dns1, sizeof (out_info->dns_v4[0]));
    g_strlcpy (out_info->dns_v4[1], call->dns2, sizeof (out_info->dns_v4[1]));
    g_strlcpy (out_info->dns_v6[0], call->dns1_v6, sizeof (out_info->dns_v6[0]));
    g_strlcpy (out_info->dns_v6[1], call->dns2_v6, sizeof (out_info->dns_v6[1]));
}

static void start_sequence (MMSitIms *self);

static gchar *
log_object_build_id (MMLogObject *_self)
{
    MMSitIms *self = MM_SIT_IMS (_self);

    return g_strdup_printf ("ims%u", self->priv->port_index);
}

static void
log_object_iface_init (MMLogObjectInterface *iface)
{
    iface->build_id = log_object_build_id;
}

/* IMS lives on the packet-switched side, so a CS-only RAT cannot carry it. 5G
 * counts: this network runs EN-DC with an LTE anchor, and where it is standalone
 * the CP falls back to EPS itself. */
static gboolean
act_supports_ims (MMModemAccessTechnology act)
{
    return !!(act & (MM_MODEM_ACCESS_TECHNOLOGY_LTE |
                     MM_MODEM_ACCESS_TECHNOLOGY_5GNR));
}

static void
clear_retry (MMSitIms *self)
{
    if (self->priv->retry_id) {
        g_source_remove (self->priv->retry_id);
        self->priv->retry_id = 0;
    }
}

static gboolean
retry_cb (MMSitIms *self)
{
    self->priv->retry_id = 0;
    if (self->priv->state == IMS_STATE_STARTING)
        start_sequence (self);
    return G_SOURCE_REMOVE;
}

static void
schedule_retry (MMSitIms    *self,
                const gchar *what,
                GError      *error)
{
    mm_obj_dbg (self, "IMS %s failed (%s); retrying in %us",
                what, error ? error->message : "unknown", RETRY_TIMEOUT_SECS);
    g_clear_error (&error);
    clear_retry (self);
    self->priv->retry_id = g_timeout_add_seconds (RETRY_TIMEOUT_SECS,
                                                  (GSourceFunc) retry_cb, self);
}

/* Send one AIMS frame the FFI built and report whether the CP took it. */
static void
send_ready (MMSitClient  *client,
            GAsyncResult *res,
            GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (client, res, &error);
    if (!response) {
        g_task_return_error (task, error);
    } else if (response->has_status && response->status) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "the CP refused %s: status %u",
                                 mm_sit_message_id_get_string (response->id),
                                 response->status);
    } else {
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

/*
 * The AIMS steps are a fixed chain, so they run as one walk rather than as a
 * pile of paired callbacks: each step builds its frame, sends it, and the
 * shared continuation moves to the next.
 */
typedef enum {
    STEP_PDN,
    STEP_STACK_START,
    STEP_HIDDEN_MENU,
    STEP_ADD_PDN_INFO,
    STEP_DONE,
} Step;

typedef struct {
    Step           step;
    MMSitDataCall  call;
    /* Hidden-menu frames, sent in order; a large carrier set segments. */
    SamsungAimsFrameList menu;
    gsize          menu_sent;
    MMSitImsConfig *config;
} SequenceContext;

static void
sequence_context_free (SequenceContext *ctx)
{
    samsung_aims_frame_list_free (&ctx->menu);
    g_clear_pointer (&ctx->config, mm_sit_ims_config_free);
    g_free (ctx);
}

static void sequence_step (GTask *task);

static void
sequence_step_ready (MMSitIms     *self,
                     GAsyncResult *res,
                     GTask        *task)
{
    SequenceContext *ctx = g_task_get_task_data (task);
    GError          *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* A segmented hidden menu stays on its step until every frame has gone. */
    if (ctx->step == STEP_HIDDEN_MENU && ctx->menu_sent < ctx->menu.count) {
        sequence_step (task);
        return;
    }
    ctx->step++;
    sequence_step (task);
}

static void
send_frame (GTask       *task,
            guint8      *frame,
            gsize        len,
            const gchar *what)
{
    MMSitIms              *self = g_task_get_source_object (task);
    g_autoptr(GByteArray)  request = NULL;
    GTask                 *inner;

    request = g_byte_array_sized_new (len);
    g_byte_array_append (request, frame, len);

    mm_obj_dbg (self, "IMS: %s", what);
    inner = g_task_new (self, g_task_get_cancellable (task),
                        (GAsyncReadyCallback) sequence_step_ready, task);
    mm_sit_client_command (self->priv->client, request, 10,
                           g_task_get_cancellable (task),
                           (GAsyncReadyCallback) send_ready, inner);
}

static void
pdn_ready (MMSitClient  *client,
           GAsyncResult *res,
           GTask        *task)
{
    MMSitIms        *self = g_task_get_source_object (task);
    SequenceContext *ctx = g_task_get_task_data (task);
    GError          *error = NULL;

    if (!mm_sit_client_setup_data_call_finish (client, res, &ctx->call, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    self->priv->pdn_up = TRUE;
    self->priv->pdn_cid = ctx->call.cid;
    self->priv->pdn = ctx->call;
    mm_obj_info (self, "IMS PDN up on context %u", ctx->call.cid);

    /*
     * No proxies means no REGISTER can be sent, and the CP will sit there
     * silently rather than tell us. Say so once, here, rather than let it look
     * like the configuration failed.
     */
    if (!ctx->call.pcscf_v4_count && !ctx->call.pcscf_v6_count)
        mm_obj_warn (self, "the network granted no P-CSCF addresses; "
                           "the CP has nowhere to register");

    ctx->step++;
    sequence_step (task);
}

static void
sequence_step (GTask *task)
{
    MMSitIms        *self = g_task_get_source_object (task);
    SequenceContext *ctx = g_task_get_task_data (task);
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case STEP_PDN:
        mm_sit_client_setup_data_call (self->priv->client, IMS_APN,
                                       /* Ask for both families: the network
                                        * grants what it has, and on this one
                                        * that is v6 only. */
                                       MM_SIT_PDP_TYPE_IPV4V6,
                                       MM_SIT_APN_TYPE_IMS,
                                       g_task_get_cancellable (task),
                                       (GAsyncReadyCallback) pdn_ready, task);
        return;

    case STEP_STACK_START:
        status = samsung_aims_build_stack_start (mm_sit_client_next_token (self->priv->client),
                                                 1, /* the SIM is usable by now */
                                                 (guint8) (self->priv->port_index + 1),
                                                 &frame, &len);
        break;

    case STEP_HIDDEN_MENU:
        send_frame (task,
                    ctx->menu.frames[ctx->menu_sent],
                    ctx->menu.lens[ctx->menu_sent],
                    "pushing configuration");
        ctx->menu_sent++;
        return;

    case STEP_ADD_PDN_INFO: {
        SamsungAimsPdnInfo info;

        data_call_to_aims_pdn_info (&ctx->call, &info);
        status = samsung_aims_build_add_pdn_info (mm_sit_client_next_token (self->priv->client),
                                                  &info, &frame, &len);
        break;
    }

    case STEP_DONE:
    default:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    if (status != SAMSUNG_SIT_STATUS_OK) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "could not build IMS request: %s",
                                 samsung_sit_status_name (status));
        g_object_unref (task);
        return;
    }

    send_frame (task, frame, len,
                ctx->step == STEP_STACK_START ? "starting the stack" : "handing over the PDN");
    samsung_sit_buf_free (frame, len);
}

static void
sequence_ready (MMSitIms     *self,
                GAsyncResult *res,
                gpointer      unused)
{
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error (&error);
            return;
        }
        schedule_retry (self, "provisioning", error);
        return;
    }

    /*
     * Provisioned, not registered. The CP answers REGISTRATION on its own about
     * a second later, and until it does there is nothing more to do here --
     * which is why this is not treated as success for call routing.
     */
    self->priv->state = IMS_STATE_STARTED;
    mm_obj_info (self, "IMS provisioned; waiting for the CP to register");
}

/*
 * Pull the identity from the client rather than having the modem push it. The
 * client resolves it during boot configuration, which is after this object is
 * created and before any RAT appears, so a push would need ordering that a pull
 * does not.
 */
static void
ensure_device_identity (MMSitIms *self)
{
    MMSitImsDeviceIdentity device = { 0 };

    if (self->priv->have_identity)
        return;

    mm_sit_client_get_device_identity (self->priv->client,
                                       &device.manufacturer,
                                       &device.model,
                                       &device.os_name,
                                       &device.os_version,
                                       &device.software_version);
    if (!device.model || !device.software_version)
        return;

    mm_sit_ims_set_device_identity (self, &device);
}

static void
start_sequence (MMSitIms *self)
{
    SequenceContext *ctx;
    GTask           *task;
    gsize            n_entries = 0;
    const SamsungAimsConfigEntry *entries;
    SamsungSitStatus status;

    clear_retry (self);

    ctx = g_new0 (SequenceContext, 1);
    ctx->config = mm_sit_ims_config_resolve (&self->priv->device,
                                             self->priv->operator_code);
    entries = mm_sit_ims_config_peek_entries (ctx->config, &n_entries);

    status = samsung_aims_build_hidden_menu (mm_sit_client_next_token (self->priv->client),
                                             entries, n_entries, &ctx->menu);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        mm_obj_warn (self, "could not build the IMS configuration: %s",
                     samsung_sit_status_name (status));
        sequence_context_free (ctx);
        return;
    }

    mm_obj_dbg (self, "IMS configuration for '%s': %" G_GSIZE_FORMAT " entries in %"
                G_GSIZE_FORMAT " frame(s), UA '%s'",
                mm_sit_ims_config_get_carrier (ctx->config), n_entries,
                ctx->menu.count, mm_sit_ims_config_get_user_agent (ctx->config));

    /* A PDN already up means this is a retry after a later step failed; keep it
     * rather than asking the CP for a second context it would refuse. */
    ctx->step = self->priv->pdn_up ? STEP_STACK_START : STEP_PDN;
    if (self->priv->pdn_up)
        ctx->call = self->priv->pdn;

    task = g_task_new (self, self->priv->cancellable,
                       (GAsyncReadyCallback) sequence_ready, NULL);
    g_task_set_task_data (task, ctx, (GDestroyNotify) sequence_context_free);
    sequence_step (task);
}

static void
handle_registration (MMSitIms           *self,
                     const MMSitMessage *message)
{
    SamsungAimsRegistration reg;
    const guint8           *data;
    gsize                   len;
    gboolean                was;
    SamsungSitStatus        status;

    data = mm_sit_message_get_data (message, &len);
    if (!data)
        return;

    status = samsung_aims_parse_registration (data, len, &reg);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        mm_obj_dbg (self, "undecodable IMS registration indication: %s",
                    samsung_sit_status_name (status));
        return;
    }

    was = self->priv->registered;
    self->priv->registered = !!reg.registered;

    g_free (self->priv->associated_uris);
    self->priv->associated_uris = reg.uris[0] ? g_strdup (reg.uris) : NULL;

    if (was == self->priv->registered)
        return;

    if (self->priv->registered)
        mm_obj_info (self, "IMS registered%s%s",
                     self->priv->associated_uris ? ": " : "",
                     self->priv->associated_uris ?: "");
    else
        mm_obj_info (self, "IMS deregistered (SIP status %u)", reg.status_code);
}

static void
indication_cb (MMSitClient        *client,
               const MMSitMessage *message,
               MMSitIms           *self)
{
    if (message->id == MM_SIT_MESSAGE_AIMS_REGISTRATION)
        handle_registration (self, message);
}

void
mm_sit_ims_update_access_tech (MMSitIms                *self,
                               MMModemAccessTechnology  act)
{
    gboolean possible;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    possible = act_supports_ims (act);
    self->priv->last_act = act;

    if (possible && self->priv->state == IMS_STATE_STOPPED) {
        ensure_device_identity (self);
        if (!self->priv->have_identity) {
            mm_obj_dbg (self, "not starting IMS: no device identity for the User-Agent");
            return;
        }
        /*
         * The carrier's configuration is selected by MCCMNC, and the CP is
         * provisioned once. Starting before the serving network is known would
         * push defaults -- wrong timers, wrong MTU, no certification suffix --
         * and nothing would revisit them. A RAT without an operator lasts a
         * moment; the next indication carries both.
         */
        if (!self->priv->operator_code) {
            mm_obj_dbg (self, "not starting IMS yet: no serving network to configure for");
            return;
        }
        /*
         * See mm_sit_ims_set_data_connected(): the enable path power-cycles the
         * radio to apply the attach APN, and that drops every context. Starting
         * before then means provisioning a stack whose PDN is about to vanish.
         */
        if (!self->priv->data_connected) {
            mm_obj_dbg (self, "not starting IMS yet: waiting for the default context");
            return;
        }
        mm_obj_dbg (self, "packet-switched RAT available; starting IMS");
        self->priv->state = IMS_STATE_STARTING;
        g_clear_object (&self->priv->cancellable);
        self->priv->cancellable = g_cancellable_new ();
        start_sequence (self);
        return;
    }

    if (!possible && self->priv->state != IMS_STATE_STOPPED) {
        mm_obj_dbg (self, "no packet-switched RAT; stopping IMS");
        mm_sit_ims_stop (self);
    }
}

void
mm_sit_ims_set_data_connected (MMSitIms *self,
                               gboolean  connected)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));

    if (self->priv->data_connected == !!connected)
        return;
    self->priv->data_connected = !!connected;

    if (!connected) {
        /* The context that carried IMS went with it. */
        self->priv->pdn_up = FALSE;
        mm_sit_ims_stop (self);
        return;
    }
    if (self->priv->last_act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
        mm_sit_ims_update_access_tech (self, self->priv->last_act);
}

void
mm_sit_ims_set_operator_code (MMSitIms    *self,
                              const gchar *mccmnc)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));

    if (g_strcmp0 (self->priv->operator_code, mccmnc) == 0)
        return;
    g_free (self->priv->operator_code);
    self->priv->operator_code = g_strdup (mccmnc);

    /*
     * Learning the network is what unblocks a start that was waiting for it.
     * A change of network is not handled here: the CP is provisioned per
     * registration and re-provisioning a running stack is a separate question,
     * so this only ever starts what has not started.
     */
    if (mccmnc && self->priv->data_connected &&
        self->priv->last_act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
        mm_sit_ims_update_access_tech (self, self->priv->last_act);
}

void
mm_sit_ims_set_device_identity (MMSitIms                     *self,
                                const MMSitImsDeviceIdentity *device)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));
    g_return_if_fail (device != NULL);

    g_free (self->priv->manufacturer);
    g_free (self->priv->model);
    g_free (self->priv->os_name);
    g_free (self->priv->os_version);
    g_free (self->priv->software_version);

    self->priv->manufacturer = g_strdup (device->manufacturer);
    self->priv->model = g_strdup (device->model);
    self->priv->os_name = g_strdup (device->os_name);
    self->priv->os_version = g_strdup (device->os_version);
    self->priv->software_version = g_strdup (device->software_version);

    self->priv->device.manufacturer = self->priv->manufacturer;
    self->priv->device.model = self->priv->model;
    self->priv->device.os_name = self->priv->os_name;
    self->priv->device.os_version = self->priv->os_version;
    self->priv->device.software_version = self->priv->software_version;
    self->priv->have_identity = TRUE;
}

gboolean
mm_sit_ims_is_registered (MMSitIms *self)
{
    g_return_val_if_fail (MM_IS_SIT_IMS (self), FALSE);
    return self->priv->registered;
}

const gchar *
mm_sit_ims_get_associated_uris (MMSitIms *self)
{
    g_return_val_if_fail (MM_IS_SIT_IMS (self), NULL);
    return self->priv->associated_uris;
}

void
mm_sit_ims_stop (MMSitIms *self)
{
    guint8          *frame = NULL;
    gsize            len = 0;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    clear_retry (self);
    if (self->priv->cancellable) {
        g_cancellable_cancel (self->priv->cancellable);
        g_clear_object (&self->priv->cancellable);
    }

    if (self->priv->state == IMS_STATE_STOPPED)
        return;
    self->priv->state = IMS_STATE_STOPPED;
    self->priv->registered = FALSE;
    g_clear_pointer (&self->priv->associated_uris, g_free);

    /* Deregister before the stack goes, so the network sees a REGISTER with
     * Expires: 0 rather than a UE that stopped answering. Fire and forget:
     * there is nothing useful to do if the CP refuses, and we are on the way
     * down regardless. */
    if (samsung_aims_build_deregistration (mm_sit_client_next_token (self->priv->client),
                                           &frame, &len) == SAMSUNG_SIT_STATUS_OK) {
        g_autoptr(GByteArray) request = g_byte_array_sized_new (len);

        g_byte_array_append (request, frame, len);
        samsung_sit_buf_free (frame, len);
        mm_sit_client_command (self->priv->client, request, 5, NULL, NULL, NULL);
    }
    if (samsung_aims_build_stack_stop (mm_sit_client_next_token (self->priv->client),
                                       1, (guint8) (self->priv->port_index + 1),
                                       &frame, &len) == SAMSUNG_SIT_STATUS_OK) {
        g_autoptr(GByteArray) request = g_byte_array_sized_new (len);

        g_byte_array_append (request, frame, len);
        samsung_sit_buf_free (frame, len);
        mm_sit_client_command (self->priv->client, request, 5, NULL, NULL, NULL);
    }

    if (self->priv->pdn_up) {
        mm_sit_client_disconnect_data (self->priv->client, self->priv->pdn_cid,
                                       NULL, NULL, NULL);
        self->priv->pdn_up = FALSE;
        memset (&self->priv->pdn, 0, sizeof (self->priv->pdn));
    }
    mm_obj_info (self, "IMS stopped");
}

MMSitIms *
mm_sit_ims_new (MMSitClient *client,
                guint        port_index)
{
    MMSitIms *self;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (client), NULL);

    self = g_object_new (MM_TYPE_SIT_IMS, NULL);
    self->priv->client = client;
    self->priv->port_index = port_index;
    self->priv->indication_id = g_signal_connect (client, "indication",
                                                  G_CALLBACK (indication_cb), self);
    return self;
}

static void
mm_sit_ims_init (MMSitIms *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_SIT_IMS, MMSitImsPrivate);
    self->priv->state = IMS_STATE_STOPPED;
}

static void
dispose (GObject *object)
{
    MMSitIms *self = MM_SIT_IMS (object);

    clear_retry (self);
    if (self->priv->cancellable) {
        g_cancellable_cancel (self->priv->cancellable);
        g_clear_object (&self->priv->cancellable);
    }
    if (self->priv->indication_id && self->priv->client) {
        g_signal_handler_disconnect (self->priv->client, self->priv->indication_id);
        self->priv->indication_id = 0;
    }
    self->priv->client = NULL;
    G_OBJECT_CLASS (mm_sit_ims_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMSitIms *self = MM_SIT_IMS (object);

    g_free (self->priv->manufacturer);
    g_free (self->priv->model);
    g_free (self->priv->os_name);
    g_free (self->priv->os_version);
    g_free (self->priv->software_version);
    g_free (self->priv->operator_code);
    g_free (self->priv->associated_uris);
    G_OBJECT_CLASS (mm_sit_ims_parent_class)->finalize (object);
}

static void
mm_sit_ims_class_init (MMSitImsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSitImsPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;
}
