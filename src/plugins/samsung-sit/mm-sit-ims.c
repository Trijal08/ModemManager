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
 * and its timing are HW-validated; see docs/subsystems/modem/reference/ims.md.
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
/* The fallback when no carrier record names one; see mm_sit_ims_set_apn(). */
#define IMS_APN "ims"

/* How long to wait before trying again after the CP refuses a step. The CP
 * throttles its own registration attempts (RILC_UNSOL_IMS_THROTTLED_TIME), so
 * being patient here costs nothing and hammering it would be actively wrong. */
#ifndef MM_SIT_IMS_RETRY_TIMEOUT_SECS
#define MM_SIT_IMS_RETRY_TIMEOUT_SECS 30
#endif

/* Data-only subscriptions may always refuse the IMS PDN. Bound AP-side
 * provisioning attempts per lifecycle; the CP manages SIP retries itself. */
#define MAX_PROVISIONING_ATTEMPTS 3

typedef enum {
    IMS_STATE_STOPPED,
    IMS_STATE_STARTING,
    IMS_STATE_STARTED,
    IMS_STATE_FAILED,
} ImsState;

struct _MMSitImsPrivate {
    MMSitClient *client; /* borrowed: the modem owns it and outlives us */
    guint        port_index;
    gulong       indication_id;
    /* Where the per-carrier IMS configuration is read from, and which
     * subscription to read out of it. */
    gchar       *ims_database;
    /* The IMS APN and its address family, from the carrier's APN list. */
    gchar       *ims_apn;
    guint8       ims_pdp;
    gchar       *sim_mccmnc;
    gchar       *sim_spn;
    gchar       *sim_imsi;
    gchar       *sim_gid1;
    gchar       *sim_gid2;
    gchar       *sim_iccid;

    ImsState     state;
    gboolean     have_identity;
    MMSitImsDeviceIdentity device;
    gchar       *manufacturer;
    gchar       *model;
    gchar       *os_name;
    gchar       *os_version;
    gchar       *software_version;
    gchar       *operator_code;
    /* Remembered so learning the operator, or the radio registering, can start
     * what the RAT alone could not. */
    MMModemAccessTechnology last_act;
    /* The modem's own registration -- the radio being up and settled. Distinct
     * from `registered` below, which is the CP's IMS/SIP registration. */
    gboolean                net_registered;
    MMSitVopsSupport        vops_support;

    /* The context the CP granted, kept because every later step describes it --
     * a retry that had forgotten it would hand the stack an empty PDN. */
    gboolean      pdn_up;
    guint8        pdn_cid;
    MMSitDataCall pdn;

    gboolean     registered;
    gchar       *associated_uris;

    /* Calls the CP has reported, keyed by its own call id. */
    GPtrArray   *calls;

    GCancellable *cancellable;
    guint         retry_id;
    guint         provisioning_failures;
};

enum {
    SIGNAL_CALLS_CHANGED,
    SIGNAL_LAST,
};

static guint signals[SIGNAL_LAST];

/* One call the CP has told us about. AIMS numbers calls from zero and
 * ModemManager reserves zero for "no index", so the index is the id plus one --
 * the same shift the CS side uses. */
typedef struct {
    guint8      call_id;
    gchar      *number;
    MMCallState state;
} ImsCall;

static void
ims_call_free (ImsCall *call)
{
    g_free (call->number);
    g_free (call);
}

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
    clear_retry (self);
    if (++self->priv->provisioning_failures >= MAX_PROVISIONING_ATTEMPTS) {
        mm_obj_info (self, "IMS %s failed after %u attempts (%s); giving up until the next registration cycle",
                     what, self->priv->provisioning_failures,
                     error ? error->message : "unknown");
        g_clear_error (&error);
        /* Clean up any PDN or stack left by a failure after SETUP. Keep a
         * distinct state so ordinary RAT/operator polls cannot restart us. */
        mm_sit_ims_stop (self);
        self->priv->state = IMS_STATE_FAILED;
        return;
    }
    mm_obj_dbg (self, "IMS %s attempt %u/%u failed (%s); retrying in %us",
                what, self->priv->provisioning_failures, MAX_PROVISIONING_ATTEMPTS,
                error ? error->message : "unknown", MM_SIT_IMS_RETRY_TIMEOUT_SECS);
    g_clear_error (&error);
    self->priv->retry_id = g_timeout_add_seconds (MM_SIT_IMS_RETRY_TIMEOUT_SECS,
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
        mm_sit_client_setup_data_call (self->priv->client,
                                       self->priv->ims_apn ?: IMS_APN,
                                       self->priv->ims_pdp ? (MMSitPdpType) self->priv->ims_pdp
                                                           : MM_SIT_PDP_TYPE_IPV4V6,
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
    self->priv->provisioning_failures = 0;
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
    g_autoptr(GError) config_error = NULL;

    clear_retry (self);

    ctx = g_new0 (SequenceContext, 1);
    {
        /* The SIM's identity when it is known; the serving network only as a
         * last resort, because they disagree for exactly the carriers that
         * share an MCCMNC with a predecessor network. */
        MMSitCarrierIdentity identity = {
            .imsi = self->priv->sim_imsi,
            .iccid = self->priv->sim_iccid,
            .mccmnc = self->priv->sim_mccmnc ?: self->priv->operator_code,
            .spn = self->priv->sim_spn,
            .gid1 = self->priv->sim_gid1,
            .gid2 = self->priv->sim_gid2,
        };

        ctx->config = mm_sit_ims_config_resolve (self->priv->ims_database,
                                                 &self->priv->device,
                                                 &identity,
                                                 &config_error);
    }
    if (!ctx->config) {
        /* Without it the CP has no User-Agent and cannot register at all, so
         * there is nothing to fall back to and no point retrying: this is a
         * packaging fault, not a transient one. */
        mm_obj_warn (self, "no IMS configuration: %s", config_error->message);
        sequence_context_free (ctx);
        return;
    }
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

static ImsCall *
find_call (MMSitIms *self,
           guint8    call_id)
{
    guint i;

    for (i = 0; i < self->priv->calls->len; i++) {
        ImsCall *call = g_ptr_array_index (self->priv->calls, i);

        if (call->call_id == call_id)
            return call;
    }
    return NULL;
}

/* The CP's own call states; the first six coincide with RIL's, which is what
 * the CS group reports, so both halves of the list agree by construction. */
static MMCallState
aims_call_state_to_mm (guint8 state)
{
    switch (state) {
    case 0: return MM_CALL_STATE_ACTIVE;
    case 1: return MM_CALL_STATE_HELD;
    case 2: return MM_CALL_STATE_DIALING;
    case 3: return MM_CALL_STATE_RINGING_OUT;
    case 4: return MM_CALL_STATE_RINGING_IN;
    case 5: return MM_CALL_STATE_WAITING;
    case 6: return MM_CALL_STATE_TERMINATED;
    /* Held by the local or the remote party; both are held as far as the
     * interface is concerned. */
    case 7:
    case 8: return MM_CALL_STATE_HELD;
    default: return MM_CALL_STATE_UNKNOWN;
    }
}

static void
handle_call_ring (MMSitIms           *self,
                  const MMSitMessage *message)
{
    SamsungAimsCallRing ring;
    const guint8       *data;
    gsize               len;
    ImsCall            *call;
    SamsungSitStatus    status;

    data = mm_sit_message_get_data (message, &len);
    if (!data)
        return;

    status = samsung_aims_parse_call_ring (data, len, &ring);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        mm_obj_dbg (self, "undecodable IMS ring: %s", samsung_sit_status_name (status));
        return;
    }

    /* The ring repeats while the call is offered, so this is find-or-add. */
    call = find_call (self, ring.call_id);
    if (!call) {
        call = g_new0 (ImsCall, 1);
        call->call_id = ring.call_id;
        g_ptr_array_add (self->priv->calls, call);
    }
    g_free (call->number);
    call->number = g_strdup (ring.number);
    call->state = MM_CALL_STATE_RINGING_IN;

    mm_obj_info (self, "incoming IMS call from %s",
                 ring.number[0] ? ring.number : "(withheld)");
    g_signal_emit (self, signals[SIGNAL_CALLS_CHANGED], 0);
}

static void
handle_call_status (MMSitIms           *self,
                    const MMSitMessage *message)
{
    SamsungAimsCallStatus st;
    const guint8         *data;
    gsize                 len;
    ImsCall              *call;
    MMCallState           state;
    SamsungSitStatus      status;

    data = mm_sit_message_get_data (message, &len);
    if (!data)
        return;

    status = samsung_aims_parse_call_status (data, len, &st);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        mm_obj_dbg (self, "undecodable IMS call status: %s", samsung_sit_status_name (status));
        return;
    }

    state = aims_call_state_to_mm (st.state);
    call = find_call (self, st.call_id);

    if (state == MM_CALL_STATE_TERMINATED) {
        /* Report the release once and forget it: a call the interface can no
         * longer act on has no business staying in the list. */
        if (call) {
            mm_obj_dbg (self, "IMS call %u released (cause %u)", st.call_id, st.end_cause);
            g_ptr_array_remove_fast (self->priv->calls, call);
            g_signal_emit (self, signals[SIGNAL_CALLS_CHANGED], 0);
        }
        return;
    }

    if (state == MM_CALL_STATE_UNKNOWN) {
        mm_obj_dbg (self, "ignoring IMS call %u in unmodelled state %u", st.call_id, st.state);
        return;
    }

    if (!call) {
        /* An outgoing call first appears here rather than in a ring. */
        call = g_new0 (ImsCall, 1);
        call->call_id = st.call_id;
        g_ptr_array_add (self->priv->calls, call);
    }
    if (st.connected_number[0]) {
        g_free (call->number);
        call->number = g_strdup (st.connected_number);
    }
    if (call->state == state)
        return;
    call->state = state;
    mm_obj_dbg (self, "IMS call %u is now %s", st.call_id, mm_call_state_get_string (state));
    g_signal_emit (self, signals[SIGNAL_CALLS_CHANGED], 0);
}

/* One AIMS request, built by the FFI and sent through the client. The three
 * call operations differ only in which builder they call. */
static void
send_simple (MMSitIms            *self,
             guint8              *frame,
             gsize                len,
             SamsungSitStatus     status,
             const gchar         *what,
             GCancellable        *cancellable,
             GAsyncReadyCallback  callback,
             gpointer             user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "could not build IMS %s: %s", what,
                                 samsung_sit_status_name (status));
        g_object_unref (task);
        return;
    }
    request = g_byte_array_sized_new (len);
    g_byte_array_append (request, frame, len);
    samsung_sit_buf_free (frame, len);

    mm_obj_dbg (self, "IMS %s", what);
    mm_sit_client_command (self->priv->client, request, 30, cancellable,
                           (GAsyncReadyCallback) send_ready, task);
}

/*
 * `MEDIA_STATE_IND` (0x0d30) -- tell the CP what the call's media is doing.
 *
 * Android raises this at every call-state change and the CP answers nothing.
 * We never sent it, and the CP never published the CallInfo block the AoC
 * waits on to learn the call's codec, which is why a call could connect
 * perfectly and still carry no audio.  The state values are the ones observed
 * on this hardware: setup 2, alerting 4, connected 0, ended 6.
 */
static void
fire_and_forget_ready (MMSitClient  *client,
                       GAsyncResult *res,
                       gpointer      user_data)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    g_autofree gchar       *what = user_data;

    response = mm_sit_client_command_finish (client, res, &error);
    if (error)
        mm_obj_dbg (client, "IMS %s failed: %s", what, error->message);
}

/*
 * Send a request whose answer we do not act on.  These exist because Android
 * sends them and the CP's media pipeline does not start without them being
 * tried; nothing in ModemManager consumes the response.
 */
static void
send_media_request (MMSitIms    *self,
                    guint8      *frame,
                    gsize        len,
                    SamsungSitStatus status,
                    const gchar *what)
{
    g_autoptr(GByteArray) request = NULL;

    if (status != SAMSUNG_SIT_STATUS_OK) {
        mm_obj_dbg (self, "could not build IMS %s: %s", what,
                    samsung_sit_status_name (status));
        return;
    }
    request = g_byte_array_sized_new (len);
    g_byte_array_append (request, frame, len);
    samsung_sit_buf_free (frame, len);

    mm_obj_dbg (self, "IMS %s", what);
    mm_sit_client_command (self->priv->client, request, 10, NULL,
                           (GAsyncReadyCallback) fire_and_forget_ready,
                           g_strdup (what));
}

/*
 * `SET_FRAME_TIME` (0x0d0b). Android sends this as the next frame after the
 * dial, before the call connects, and the CP answers later with a FRAME_TIME
 * indication -- which komodo never produces, one of the signs its media
 * pipeline never starts.
 */
void
mm_sit_ims_set_frame_time (MMSitIms *self)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));
    status = samsung_aims_build_set_frame_time (mm_sit_client_next_token (self->priv->client),
                                                &frame, &len);
    send_media_request (self, frame, len, status, "set frame time");
}

/* `SET_RTP_THRESHOLD` (0x0d72), sent as the call becomes active. */
void
mm_sit_ims_set_rtp_threshold (MMSitIms *self)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));
    status = samsung_aims_build_set_rtp_threshold (mm_sit_client_next_token (self->priv->client),
                                                   &frame, &len);
    send_media_request (self, frame, len, status, "set RTP threshold");
}

void
mm_sit_ims_media_state (MMSitIms *self,
                        guint8    state)
{
    g_autoptr(GByteArray)  frame_array = NULL;
    guint8                *frame = NULL;
    gsize                  len = 0;
    SamsungSitStatus       status;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    status = samsung_aims_build_media_state_ind (state, &frame, &len);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        mm_obj_dbg (self, "could not build IMS media state %u: %s",
                    state, samsung_sit_status_name (status));
        return;
    }

    frame_array = g_byte_array_sized_new (len);
    g_byte_array_append (frame_array, frame, len);
    samsung_sit_buf_free (frame, len);

    mm_obj_dbg (self, "IMS media state %u", state);
    mm_sit_client_send_indication (self->priv->client, frame_array);
}

void
mm_sit_ims_dial (MMSitIms            *self,
                 const gchar         *number,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    status = samsung_aims_build_dial (mm_sit_client_next_token (self->priv->client),
                                      number, 0 /* network default CLIR */,
                                      &frame, &len);
    send_simple (self, frame, len, status, "dial", cancellable, callback, user_data);
}

gboolean
mm_sit_ims_dial_finish (MMSitIms      *self,
                        GAsyncResult  *res,
                        GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_ims_answer (MMSitIms            *self,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    status = samsung_aims_build_answer (mm_sit_client_next_token (self->priv->client),
                                        &frame, &len);
    send_simple (self, frame, len, status, "answer", NULL, callback, user_data);
}

gboolean
mm_sit_ims_answer_finish (MMSitIms      *self,
                          GAsyncResult  *res,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_ims_hangup (MMSitIms            *self,
                   guint8               call_id,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    /* Reason 0 is an ordinary local hang-up; the capture uses 2 for declining a
     * call that is still ringing, which the interface does not distinguish. */
    status = samsung_aims_build_hangup (mm_sit_client_next_token (self->priv->client),
                                        call_id, 0, &frame, &len);
    send_simple (self, frame, len, status, "hangup", NULL, callback, user_data);
}

gboolean
mm_sit_ims_hangup_finish (MMSitIms      *self,
                          GAsyncResult  *res,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_ims_send_dtmf (MMSitIms            *self,
                      guint8               call_id,
                      guint8               kind,
                      gchar                digit,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    /* The digit goes on the wire as its character, not its value -- the
     * vendor's field is a Java char written as one byte. */
    status = samsung_aims_build_send_dtmf (mm_sit_client_next_token (self->priv->client),
                                           call_id, kind, (guint8) digit, &frame, &len);
    send_simple (self, frame, len, status, "send dtmf", NULL, callback, user_data);
}

gboolean
mm_sit_ims_send_dtmf_finish (MMSitIms      *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

GList *
mm_sit_ims_get_call_list (MMSitIms *self)
{
    GList *out = NULL;
    guint  i;

    g_return_val_if_fail (MM_IS_SIT_IMS (self), NULL);

    for (i = 0; i < self->priv->calls->len; i++) {
        ImsCall    *call = g_ptr_array_index (self->priv->calls, i);
        MMCallInfo *info;

        info = g_slice_new0 (MMCallInfo);
        info->index = call->call_id + 1;
        info->state = call->state;
        info->direction = (call->state == MM_CALL_STATE_RINGING_IN ||
                           call->state == MM_CALL_STATE_WAITING)
            ? MM_CALL_DIRECTION_INCOMING : MM_CALL_DIRECTION_OUTGOING;
        info->number = g_strdup (call->number);
        out = g_list_append (out, info);
    }
    return out;
}

static void
indication_cb (MMSitClient        *client,
               const MMSitMessage *message,
               MMSitIms           *self)
{
    switch (message->id) {
    case MM_SIT_MESSAGE_AIMS_REGISTRATION:
        handle_registration (self, message);
        break;
    case MM_SIT_MESSAGE_AIMS_CALL_RING:
        handle_call_ring (self, message);
        break;
    case MM_SIT_MESSAGE_AIMS_CALL_STATUS:
        handle_call_status (self, message);
        break;
    default:
        break;
    }
}

void
mm_sit_ims_update_access_tech (MMSitIms                *self,
                               MMModemAccessTechnology  act)
{
    gboolean possible;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    possible = act_supports_ims (act) &&
               self->priv->vops_support != MM_SIT_VOPS_SUPPORT_UNSUPPORTED;
    self->priv->last_act = act;

    if (possible && self->priv->state == IMS_STATE_STOPPED) {
        ensure_device_identity (self);
        if (!self->priv->have_identity) {
            mm_obj_dbg (self, "not starting IMS: no device identity for the User-Agent");
            return;
        }
        /*
         * See mm_sit_ims_set_registered(): applying the attach APN power-cycles
         * the radio, and that drops every context. Starting while the radio is
         * down means provisioning a stack whose PDN is about to vanish -- but
         * the wait is for the radio, not for mobile data, which IMS does not
         * need and does not use.
         */
        if (!self->priv->net_registered) {
            mm_obj_dbg (self, "not starting IMS yet: waiting for the radio to register");
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
        mm_obj_dbg (self, "IMS unavailable on this RAT or network; stopping IMS");
        mm_sit_ims_stop (self);
    }
}

void
mm_sit_ims_set_registered (MMSitIms *self,
                           gboolean  registered)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));

    if (self->priv->net_registered == !!registered)
        return;
    self->priv->net_registered = !!registered;

    if (!registered) {
        /*
         * The radio went down, so the context that carried IMS went with it.
         * Forget the PDN rather than retrying onto a dead one: the restart
         * below re-runs SETUP_DATA_CALL from the top, which is what makes the
         * attach-APN power-cycle survivable instead of fatal.
         */
        self->priv->pdn_up = FALSE;
        mm_sit_ims_stop (self);
        return;
    }
    if (self->priv->last_act != MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN)
        mm_sit_ims_update_access_tech (self, self->priv->last_act);
}

void
mm_sit_ims_update_registration (MMSitIms                *self,
                                 const MMSitRegistration *registration)
{
    MMSitVopsSupport support;

    g_return_if_fail (MM_IS_SIT_IMS (self));
    g_return_if_fail (registration != NULL);

    /* Stop before clearing a stale capability. Otherwise a loss of packet
     * registration could change unsupported to unknown and briefly start IMS
     * while MM core still reports the previous registered state. */
    if (!registration->registered)
        mm_sit_ims_set_registered (self, FALSE);
    support = registration->registered ? registration->vops_support : MM_SIT_VOPS_SUPPORT_UNKNOWN;
    if (self->priv->vops_support != support) {
        self->priv->vops_support = support;
        mm_obj_info (self, "network IMS voice support (VoPS): %s",
                      support == MM_SIT_VOPS_SUPPORT_SUPPORTED ? "supported" :
                      support == MM_SIT_VOPS_SUPPORT_UNSUPPORTED ? "unsupported" : "unknown");
        mm_sit_ims_update_access_tech (self, self->priv->last_act);
    }
    /* MM may already be REGISTERED through CS, so a newly registered packet
     * service need not produce another modem-state notification. */
    mm_sit_ims_set_registered (self, registration->registered);
}

void
mm_sit_ims_set_carrier_identity (MMSitIms                   *self,
                                 const MMSitCarrierIdentity *identity)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));
    g_return_if_fail (identity != NULL);

    g_free (self->priv->sim_mccmnc);
    g_free (self->priv->sim_spn);
    g_free (self->priv->sim_imsi);
    g_free (self->priv->sim_gid1);
    g_free (self->priv->sim_gid2);
    g_free (self->priv->sim_iccid);
    self->priv->sim_mccmnc = g_strdup (identity->mccmnc);
    self->priv->sim_spn = g_strdup (identity->spn);
    self->priv->sim_imsi = g_strdup (identity->imsi);
    self->priv->sim_gid1 = g_strdup (identity->gid1);
    self->priv->sim_gid2 = g_strdup (identity->gid2);
    self->priv->sim_iccid = g_strdup (identity->iccid);
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
    if (mccmnc && self->priv->net_registered &&
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
mm_sit_ims_set_apn (MMSitIms    *self,
                    const gchar *apn,
                    guint8       pdp)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));

    if (!apn || !apn[0])
        return;
    if (g_strcmp0 (self->priv->ims_apn, apn) == 0 && self->priv->ims_pdp == pdp)
        return;

    g_free (self->priv->ims_apn);
    self->priv->ims_apn = g_strdup (apn);
    self->priv->ims_pdp = pdp;
    mm_obj_dbg (self, "IMS PDN will use the carrier's APN '%s' (pdp %u)", apn, pdp);
}

void
mm_sit_ims_stop (MMSitIms *self)
{
    guint8          *frame = NULL;
    gsize            len = 0;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    clear_retry (self);
    self->priv->provisioning_failures = 0;
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
                guint        port_index,
                const gchar *ims_database)
{
    MMSitIms *self;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (client), NULL);

    self = g_object_new (MM_TYPE_SIT_IMS, NULL);
    self->priv->client = client;
    self->priv->port_index = port_index;
    self->priv->ims_database = g_strdup (ims_database);
    self->priv->indication_id = g_signal_connect (client, "indication",
                                                  G_CALLBACK (indication_cb), self);
    return self;
}

static void
mm_sit_ims_init (MMSitIms *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_SIT_IMS, MMSitImsPrivate);
    self->priv->state = IMS_STATE_STOPPED;
    self->priv->calls = g_ptr_array_new_with_free_func ((GDestroyNotify) ims_call_free);
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
    g_free (self->priv->ims_database);
    g_free (self->priv->ims_apn);
    g_free (self->priv->sim_mccmnc);
    g_free (self->priv->sim_spn);
    g_free (self->priv->sim_imsi);
    g_free (self->priv->sim_gid1);
    g_free (self->priv->sim_gid2);
    g_free (self->priv->sim_iccid);
    g_free (self->priv->associated_uris);
    g_clear_pointer (&self->priv->calls, g_ptr_array_unref);
    G_OBJECT_CLASS (mm_sit_ims_parent_class)->finalize (object);
}

static void
mm_sit_ims_class_init (MMSitImsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSitImsPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;

    signals[SIGNAL_CALLS_CHANGED] =
        g_signal_new (MM_SIT_IMS_SIGNAL_CALLS_CHANGED,
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}
