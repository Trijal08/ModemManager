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

/*
 * How long to wait before trying again after the CP refuses a step.
 *
 * The first delay is short because the common refusal is a transient. Measured
 * across twelve boots, the first IMS ask is refused on ten of them with cause
 * 168 returned in 9-61ms -- a local refusal, far too quick to be a network
 * round trip -- and a later, identical ask is granted. Nothing we send
 * separates the two: the AP->CP request streams before a granted ask and a
 * refused one are 98% identical, so this is a condition to recover from rather
 * than one to predict, and sleeping 30s on it was most of the time VoLTE took
 * to come up.
 *
 * It still backs off to the cap, because a data-only subscription may refuse
 * the IMS PDN for good and because the CP throttles its own SIP registration
 * (RILC_UNSOL_IMS_THROTTLED_TIME), which is what the patience here was
 * originally for. The cooldown below takes over once the attempts run out.
 */
#define MM_SIT_IMS_RETRY_FIRST_SECS 2
#ifndef MM_SIT_IMS_RETRY_TIMEOUT_SECS
#define MM_SIT_IMS_RETRY_TIMEOUT_SECS 30
#endif

/* Data-only subscriptions may always refuse the IMS PDN. Bound AP-side
 * provisioning attempts per lifecycle; the CP manages SIP retries itself. */
#define MAX_PROVISIONING_ATTEMPTS 5

/*
 * Floor on how often the whole provisioning sequence may run, and how fast it
 * backs off when a run does not reach registration.
 *
 * MAX_PROVISIONING_ATTEMPTS does not bound this on its own. The counter is
 * reset both by mm_sit_ims_stop() and by reaching IMS_STATE_STARTED -- and a
 * PDN granted with cause 0 but no addresses reaches STARTED, so an empty grant
 * is not even counted as a failure. The RAT/registration gate then stops and
 * restarts us on every dip, each time with a fresh count, so the sequence can
 * run indefinitely.
 *
 * That is not merely untidy: each run makes the CP reload its carrier policy,
 * which re-reads ~128KB of carrier-config files over the oem channel. Measured
 * on hardware, the repeats put 26x Android's volume through a 4KiB ring --
 * delaying an IMS SMS by 11s and 39s, and once hanging the CP into its
 * watchdog. A retry loop in the AP must not be able to do that whatever the
 * IMS layer below it is doing.
 *
 * So the floor is kept in monotonic time rather than a counter, because time
 * is the thing a stop/start cycle cannot reset.
 */
#ifndef PROVISIONING_COOLDOWN_SECS
#define PROVISIONING_COOLDOWN_SECS     30
#endif
#define PROVISIONING_COOLDOWN_MAX_SECS (PROVISIONING_COOLDOWN_SECS * 10)

/*
 * How long a RAT that cannot carry IMS is tolerated before the stack is torn
 * down. On a dual-SIM device the two stacks share the radio, so the arbiter
 * parks this one on UMTS for twenty to forty seconds at a time while the other
 * SIM transmits. Re-registering costs about ten seconds, and the phone cannot
 * be called for any of them, so a dip that short is worth sitting out.
 */
#define RAT_GRACE_SECS 60

/*
 * How long the CP needs after the radio comes up before it will grant a PDN
 * that is not the attach PDN.
 *
 * The CP announces its physical-channel configuration
 * (SIT_IND_PHYSICAL_CHANNEL_CONFIG) once the radio has carriers assigned, and
 * it is not ready for a second context the moment that lands. Measured across
 * six revisions and two carriers, with the gap from that first indication to
 * our SETUP_DATA_CALL on the x axis:
 *
 *     266-694ms   10 asks   10x cause 168 (a local refusal, back in 9-61ms)
 *     5065-6647ms  2 asks   2x granted    (stock, on this same firmware)
 *     7550-15066ms 2 asks   2x granted    (ours, once this wait was added)
 *
 * No ask inside 700ms has ever been granted and no ask past 5s has ever been
 * refused, so wait out stock's margin. This is a property of the modem, not of
 * any network: it is keyed to the CP's own indication and to our registration,
 * and no part of it depends on the operator.
 */
#define PDN_SETTLE_MS 5000

/*
 * How long a provisioned stack is given to register before the PDN is thrown
 * away and asked for again.
 *
 * The CP registers within about 15 s of provisioning when it is going to at
 * all -- measured 14 s and 17 s on the boots that worked. What it cannot do is
 * register against a P-CSCF set the network will not answer on, and which set
 * arrives is the network's choice per establishment.
 *
 * This was written for a theory that is now dead. It said the pool at
 * 2001:568:fff2:300 registered every time and 2001:568:ffff:3002 never did, so
 * a fresh establishment was worth it to draw a fresh set. Both halves are
 * false: with requests pipelined, Telus registered on ffff:3002 twice, and on
 * 2026-11-06 Fizz was granted the *same* P-CSCF three times and registered on
 * the third. A fresh set of addresses is not what fixes this.
 *
 * What is left is narrower and still worth having: provisioned, and still not
 * registered long after the CP would have been, is a state to get out of. But
 * it is not a *provisioning* failure -- the PDN was granted -- so it must not
 * grow the provisioning floor. On 2026-11-06 it did, and the two teardowns
 * doubled the floor to ~240s, which is where 251s of Fizz's 6.9-minute
 * registration went:
 *
 *      36s  provisioned        watchdog 81s   -> stop
 *     110s  provisioned        watchdog 155s  -> stop, floor now ~240s
 *     406s  provisioned        registered 413s
 *
 * So the restart no longer feeds the floor, and the period is set well clear
 * of how long the CP actually takes: stock registers 23-44s after its own
 * establishment across five captures, and 45s left one second of margin over
 * its slowest. A CP merely slower than stock's worst case was being torn down
 * mid-registration.
 */
#define REGISTRATION_WATCHDOG_SECS 180

typedef enum {
    IMS_STATE_STOPPED,
    IMS_STATE_STARTING,
    IMS_STATE_STARTED,
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
    guint32      ims_bearers;
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
    /*
     * The radio technology the registration response reports, which is the one
     * the network actually granted. `last_act` comes from the signal
     * indication's RAT mask, and that is a measurement: on a dual-SIM device it
     * loses its LTE bit for as long as the arbiter has the radio parked on the
     * other stack, leaving only a WCDMA neighbour behind. That reads as a
     * reselection to UMTS and it is not one -- the UE is still EPS-registered
     * throughout -- so the measurement may add a RAT here but never remove one.
     */
    guint8                  reg_rat;
    /* The modem's own registration -- the radio being up and settled. Distinct
     * from `registered` below, which is the CP's IMS/SIP registration. */
    gboolean                net_registered;
    /*
     * When the radio registered and when the CP first described its physical
     * channel, plus the pending wake-up that lets PDN_SETTLE_MS pass after the
     * later of the two before we ask for the IMS PDN.
     */
    gint64                  net_registered_us;
    gint64                  phych_us;
    guint                   settle_id;
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
    /* Pending teardown for a RAT that cannot carry IMS; see RAT_GRACE_SECS. */
    guint         rat_grace_id;
    guint         provisioning_failures;
    /* Provisioning floor; see PROVISIONING_COOLDOWN_SECS. Monotonic, so it
     * outlives the stop/start cycles that clear provisioning_failures. */
    gint64        last_start_us;
    guint         cooldown_secs;
    /* Set when the next start comes from the registration watchdog rather than
     * from a provisioning failure; see REGISTRATION_WATCHDOG_SECS. */
    gboolean      after_registration_timeout;
    guint         cooldown_id;
    /* Armed when provisioning completes, disarmed by registration; see
     * REGISTRATION_WATCHDOG_SECS. */
    guint         registration_watchdog_id;
};

enum {
    SIGNAL_CALLS_CHANGED,
    SIGNAL_PDN_REFUSED,
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

static gboolean
reg_rat_supports_ims (guint8 rat)
{
    return rat == MM_SIT_REGISTRATION_RAT_LTE ||
           rat == MM_SIT_REGISTRATION_RAT_NR5G;
}

static void
clear_cooldown (MMSitIms *self)
{
    if (self->priv->cooldown_id) {
        g_source_remove (self->priv->cooldown_id);
        self->priv->cooldown_id = 0;
    }
}

static void
clear_retry (MMSitIms *self)
{
    if (self->priv->retry_id) {
        g_source_remove (self->priv->retry_id);
        self->priv->retry_id = 0;
    }
}

static void
clear_rat_grace (MMSitIms *self)
{
    if (self->priv->rat_grace_id) {
        g_source_remove (self->priv->rat_grace_id);
        self->priv->rat_grace_id = 0;
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

/* See MM_SIT_IMS_RETRY_FIRST_SECS: quick first, then doubling to the cap. */
static guint
retry_delay_secs (guint failures)
{
    guint secs = MM_SIT_IMS_RETRY_FIRST_SECS;

    while (failures > 1 && secs < MM_SIT_IMS_RETRY_TIMEOUT_SECS) {
        secs *= 2;
        failures--;
    }
    return MIN (secs, (guint) MM_SIT_IMS_RETRY_TIMEOUT_SECS);
}

static void
schedule_retry (MMSitIms    *self,
                const gchar *what,
                GError      *error)
{
    guint retry_secs;
    clear_retry (self);
    /*
     * Only a refusal counts against the burst. "Serial command timed out" means
     * the CP never answered, which is not a decision about the subscription --
     * and it costs an attempt the stack usually needs: on one boot a timeout
     * hit both stacks at once and ate the retry that normally succeeds. The
     * cause is parsed out below for the same reason.
     */
    if (error && !strstr (error->message, "cause "))
        mm_obj_dbg (self, "IMS %s attempt failed without a cause (%s); "
                    "not counting it against the burst", what, error->message);
    else
        self->priv->provisioning_failures++;

    if (self->priv->provisioning_failures >= MAX_PROVISIONING_ATTEMPTS) {
        mm_obj_info (self, "IMS %s failed after %u attempts (%s); giving up until the next registration cycle",
                     what, self->priv->provisioning_failures,
                     error ? error->message : "unknown");
        g_clear_error (&error);
        /*
         * Clean up any PDN or stack left by a failure after SETUP, and go back
         * to STOPPED rather than a dead end.
         *
         * This used to set IMS_STATE_FAILED, which nothing ever compared
         * against -- and the start gate only fires from STOPPED, so the stack
         * could never come back. Recovery needed an outside event that happened
         * to call stop() again (a VoPS drop, a RAT change); on a network where
         * nothing churns there was none, so three refusals meant no IMS until
         * ModemManager restarted. Measured repeatedly: Telus burns the burst
         * with 168/168/36 and is then down for the boot, while the same request
         * was granted on a later attempt at 48 s, 69 s and 464 s in other runs.
         * It does not need fewer attempts, it needs a fourth.
         *
         * The CP is still protected, by the thing that was actually measured to
         * protect it: the monotonic floor above, which doubles to
         * PROVISIONING_COOLDOWN_MAX_SECS and which a stop/start cannot reset.
         * So a burst can repeat, but only every few minutes -- not the
         * unbounded loop that once put 26x Android's volume through the oem
         * ring. mm_sit_ims_stop() leaves the state STOPPED and clears the
         * failure count for us.
         */
        mm_sit_ims_stop (self);
        return;
    }
    retry_secs = retry_delay_secs (self->priv->provisioning_failures);
    mm_obj_dbg (self, "IMS %s attempt %u/%u failed (%s); retrying in %us",
                what, self->priv->provisioning_failures, MAX_PROVISIONING_ATTEMPTS,
                error ? error->message : "unknown", retry_secs);
    /*
     * Tell whoever is listening why, so it can change something before the
     * retry. The cause travels in the message because that is where
     * mm_sit_client_setup_data_call() puts it; parsing it back out is ugly but
     * keeps the client's error contract alone.
     */
    if (error) {
        const gchar *at = strstr (error->message, "cause ");
        guint64      cause = at ? g_ascii_strtoull (at + strlen ("cause "), NULL, 10) : 0;

        if (cause)
            g_signal_emit (self, signals[SIGNAL_PDN_REFUSED], 0, (guint) cause);
    }
    g_clear_error (&error);
    self->priv->retry_id = g_timeout_add_seconds (retry_secs,
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
    /*
     * The stack comes up before the PDN is asked for, which is the order this
     * modem's own Android uses: STACK_START and XCAPM_START go out three
     * seconds before its SETUP_DATA_CALL, and ADD_PDN_INFO only afterwards.
     * Asking for the context first was this plugin's own invention.
     */
    STEP_STACK_START,
    STEP_PROFILES,
    STEP_PREAMBLE,
    STEP_PDN,
    STEP_REASSERT_FILTER,
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
    /* The carrier's whole profile set, re-pushed before the PDN. */
    GPtrArray      *apns;
    guint           apn_sent;
    /* This run established the context rather than inheriting one. Only then
     * is the indication filter re-asserted, as stock does. */
    gboolean        pdn_fresh;
} SequenceContext;

static void
sequence_context_free (SequenceContext *ctx)
{
    samsung_aims_frame_list_free (&ctx->menu);
    g_clear_pointer (&ctx->config, mm_sit_ims_config_free);
    g_clear_pointer (&ctx->apns, g_ptr_array_unref);
    g_free (ctx);
}

static void sequence_step (GTask *task);
static void arm_registration_watchdog (MMSitIms *self);
static void clear_registration_watchdog (MMSitIms *self);

/*
 * One profile is in. Keep going either way: a refused profile is the CP saying
 * the subscription does not cover it -- which is its answer to the stock stack
 * too -- and stopping there would mean never reaching the PDN at all.
 */
static void
profile_ready (MMSitClient  *client,
               GAsyncResult *res,
               GTask        *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_set_data_profile_finish (client, res, &error))
        mm_obj_dbg (g_task_get_source_object (task),
                    "IMS profile push refused (%s); continuing", error->message);
    sequence_step (task);
}

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

    ctx->pdn_fresh = TRUE;
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
reassert_screen_ready (MMSitClient  *client,
                       GAsyncResult *res,
                       GTask        *task)
{
    SequenceContext *ctx = g_task_get_task_data (task);

    /* Advisory: the context is already up and nothing here may undo it. */
    mm_sit_client_set_screen_state_finish (client, res, NULL);
    ctx->step++;
    sequence_step (task);
}

static void
reassert_filter_ready (MMSitClient  *client,
                       GAsyncResult *res,
                       GTask        *task)
{
    MMSitIms *self = g_task_get_source_object (task);

    mm_sit_client_set_indication_filter_finish (client, res, NULL);
    mm_sit_client_set_screen_state (self->priv->client, TRUE,
                                    g_task_get_cancellable (task),
                                    (GAsyncReadyCallback) reassert_screen_ready,
                                    task);
}

static void
clear_registration_watchdog (MMSitIms *self)
{
    if (self->priv->registration_watchdog_id) {
        g_source_remove (self->priv->registration_watchdog_id);
        self->priv->registration_watchdog_id = 0;
    }
}

static gboolean
registration_watchdog_cb (MMSitIms *self)
{
    self->priv->registration_watchdog_id = 0;

    if (self->priv->registered)
        return G_SOURCE_REMOVE;

    /* See REGISTRATION_WATCHDOG_SECS. Throw the context away and ask again;
     * the floor decides how soon that may happen. */
    mm_obj_info (self, "IMS provisioned %us ago and still not registered; "
                 "discarding the context and asking for a new one",
                 REGISTRATION_WATCHDOG_SECS);
    /* Not a provisioning failure: the PDN was granted. The watchdog's own
     * period already rate-limits this, so leave the floor where it is. */
    self->priv->after_registration_timeout = TRUE;
    mm_sit_ims_stop (self);
    return G_SOURCE_REMOVE;
}

static void
arm_registration_watchdog (MMSitIms *self)
{
    clear_registration_watchdog (self);
    self->priv->registration_watchdog_id =
        g_timeout_add_seconds (REGISTRATION_WATCHDOG_SECS,
                               (GSourceFunc) registration_watchdog_cb, self);
}

static void
preamble_ready (MMSitClient  *client,
                GAsyncResult *res,
                GTask        *task)
{
    SequenceContext *ctx = g_task_get_task_data (task);

    /* It cannot fail, but read the result so a future one is not swallowed. */
    mm_sit_client_run_pdn_preamble_finish (client, res, NULL);
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
    case STEP_PROFILES:
        /*
         * Re-push the carrier's whole profile set before asking for the PDN.
         *
         * The stock stack sends SET_DATA_PROFILE eighteen times on a stack
         * between its attach and its IMS context, where this sent the ims
         * profile once and went straight on. That call then came back cause=0
         * with no address and no P-CSCF, while the stock stack's answer to a
         * byte-identical request carries both. The profiles the CP holds are
         * what its route selection matches a requested DNN against, so hand it
         * the full table, in the vendor's own order, immediately beforehand.
         */
        /*
         * Twice, back to back, which is what stock does: it pushes its whole
         * set at 9930 ms, again at 10709 ms and a third time at 10720 ms,
         * 11 ms apart and 518 ms before its context. We pushed it once here
         * plus the single profile mm_sit_client_setup_data_call() registers for
         * the APN it is about to ask for -- so the last profile frame the CP
         * saw was a lone `ims` where stock's was the complete table.
         */
        if (ctx->apns && ctx->apn_sent < ctx->apns->len * 2) {
            const MMSitApn *apn = g_ptr_array_index (ctx->apns,
                                                     ctx->apn_sent % ctx->apns->len);
            MMSitApnAuth    auth = { 0 };

            ctx->apn_sent++;
            /* The carrier's roaming protocol, not a copy of the home one. */
            mm_sit_client_set_data_profile_roaming (self->priv->client, apn->apn,
                                                    (MMSitPdpType) apn->protocol,
                                                    (MMSitPdpType) apn->roaming,
                                                    apn->types, apn->bearers << 1,
                                                    &auth, g_task_get_cancellable (task),
                                                    (GAsyncReadyCallback) profile_ready,
                                                    task);
            return;
        }
        ctx->step++;
        sequence_step (task);
        return;

    case STEP_PREAMBLE:
        /*
         * Stock's last act before asking for the context is a short block, not
         * the profile push: 0x0d39, 0x0d0b, 0x0d3b, 0x091a, 0x0d0b and two
         * queries, inside the 518 ms (ch 0xf5) / 618 ms (0xf6) between its last
         * SET_DATA_PROFILE and its SETUP_DATA_CALL.
         *
         * We send those same ids -- at radio-on, twenty seconds early. Aligning
         * our AP->CP stream against stock's on identical firmware left this as
         * the divergence at the point of the request: 59% of stock's order on
         * ch 0xf5 and 31% on 0xf6, with the tail of both missing exactly this
         * block. See MM_SIT_BOOT_PHASE_PRE_PDN.
         *
         * Advisory: a refused row is still a row the CP saw, so this cannot
         * fail the context.
         */
        mm_sit_client_run_pdn_preamble (self->priv->client,
                                        g_task_get_cancellable (task),
                                        (GAsyncReadyCallback) preamble_ready, task);
        return;

    case STEP_PDN:
        /* Kept from a previous attempt; do not ask for a second context. */
        if (self->priv->pdn_up) {
            ctx->step++;
            sequence_step (task);
            return;
        }
        mm_sit_client_setup_data_call (self->priv->client,
                                       self->priv->ims_apn ?: IMS_APN,
                                       self->priv->ims_pdp ? (MMSitPdpType) self->priv->ims_pdp
                                                           : MM_SIT_PDP_TYPE_IPV4V6,
                                       MM_SIT_APN_TYPE_IMS,
                                       /* The carrier record's mask, in the CP's
                                        * numbering. Zero would declare a profile
                                        * valid on no radio technology and the PDN
                                        * is then refused with 2253. */
                                       self->priv->ims_bearers,
                                       g_task_get_cancellable (task),
                                       (GAsyncReadyCallback) pdn_ready, task);
        return;

    case STEP_STACK_START:
        status = samsung_aims_build_stack_start (mm_sit_client_next_token (self->priv->client),
                                                 1, /* the SIM is usable by now */
                                                 (guint8) (self->priv->port_index + 1),
                                                 &frame, &len);
        break;

    case STEP_REASSERT_FILTER:
        /*
         * Stock re-asserts the indication filter and the display state once the
         * IMS context is answered -- a third pair, after the two it sends at
         * radio-on. Captured on identical firmware, and it is an AP-wide event:
         * both stacks fire within 3 ms of each other, ch 0xf6 just 2 ms after
         * its own SETUP_DATA_CALL response.
         *
         *   f6  13917 RSP SETUP_DATA_CALL granted
         *       13919 REQ SET_INDICATION_FILTER 7f000000
         *       13921 REQ SET_SCREEN_STATE      01000000
         *   f5  12789 RSP SETUP_DATA_CALL granted
         *       13759 IND AIMS_REGISTRATION
         *       13918 REQ SET_INDICATION_FILTER 7f000000
         *
         * Stock sends the pair three times per stack where we sent two. Both
         * bodies are already byte-identical to stock's, so this is only the
         * missing occurrence, placed where ch 0xf6 puts it. Skipped when the
         * context was inherited from an earlier attempt, since stock sends it
         * once.
         */
        if (!ctx->pdn_fresh) {
            ctx->step++;
            sequence_step (task);
            return;
        }
        mm_sit_client_set_indication_filter (self->priv->client,
                                             MM_SIT_INDICATION_FILTER_ALL,
                                             g_task_get_cancellable (task),
                                             (GAsyncReadyCallback) reassert_filter_ready,
                                             task);
        return;

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
    arm_registration_watchdog (self);
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

static gboolean
cooldown_cb (MMSitIms *self)
{
    self->priv->cooldown_id = 0;
    if (self->priv->state == IMS_STATE_STARTING)
        start_sequence (self);
    return G_SOURCE_REMOVE;
}

/*
 * Hold a run back until the floor has passed, and widen the floor for the next
 * one. A deferred run is rescheduled rather than dropped: the point is to slow
 * the sequence down, not to stop trying.
 */
static gboolean
provisioning_deferred (MMSitIms *self)
{
    gint64 now = g_get_monotonic_time ();
    guint  waited;

    /*
     * A bounded retry series is not what floods the CP, and it is the thing
     * that wins the race the CP sets up -- the IMS PDN is refused at data+0s
     * and granted around data+26s, so the attempts thirty seconds apart have
     * to be allowed to run. The floor is for *fresh* starts, which is where
     * the unbounded repetition comes from.
     */
    if (self->priv->provisioning_failures)
        return FALSE;

    if (!self->priv->last_start_us)
        return FALSE;

    waited = (guint) ((now - self->priv->last_start_us) / G_USEC_PER_SEC);
    if (waited >= self->priv->cooldown_secs)
        return FALSE;

    if (!self->priv->cooldown_id) {
        guint left = self->priv->cooldown_secs - waited;

        mm_obj_dbg (self, "IMS provisioning held back for %us (floor %us)",
                    left, self->priv->cooldown_secs);
        self->priv->cooldown_id = g_timeout_add_seconds (left,
                                                         (GSourceFunc) cooldown_cb,
                                                         self);
    }
    return TRUE;
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

    if (provisioning_deferred (self))
        return;

    clear_retry (self);
    clear_cooldown (self);
    self->priv->last_start_us = g_get_monotonic_time ();
    if (self->priv->after_registration_timeout) {
        self->priv->after_registration_timeout = FALSE;
    } else {
        self->priv->cooldown_secs = self->priv->cooldown_secs
            ? MIN (self->priv->cooldown_secs * 2, PROVISIONING_COOLDOWN_MAX_SECS)
            : PROVISIONING_COOLDOWN_SECS;
    }

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
        /*
         * The same SIM identity answers both questions, so take the carrier's
         * profile set here too; STEP_PROFILES re-pushes it before the PDN.
         */
        ctx->apns = mm_sit_carrier_apns_resolve (self->priv->ims_database,
                                                 &identity, NULL);
        if (!ctx->apns)
            mm_obj_dbg (self, "no carrier profile set to re-push");
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

    /*
     * A PDN already up means this is a retry after a later step failed; keep it
     * rather than asking the CP for a second context it would refuse. The walk
     * starts at the stack either way now, and STEP_PDN skips itself when the
     * context is already in hand.
     */
    ctx->step = STEP_STACK_START;
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
    if (self->priv->registered)
        clear_registration_watchdog (self);

    g_free (self->priv->associated_uris);
    self->priv->associated_uris = reg.uris[0] ? g_strdup (reg.uris) : NULL;

    if (was == self->priv->registered)
        return;

    if (self->priv->registered) {
        /*
         * The only outcome worth clearing the floor for. Provisioning
         * "succeeding" does not qualify -- an empty grant gets that far -- so
         * resetting there is what let the sequence run without bound.
         */
        self->priv->cooldown_secs = 0;
        self->priv->last_start_us = 0;
        clear_cooldown (self);
        mm_obj_info (self, "IMS registered%s%s",
                     self->priv->associated_uris ? ": " : "",
                     self->priv->associated_uris ?: "");
    }
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
                   gboolean             declined,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    guint8          *frame = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    /*
     * Reason 0 is an ordinary local hang-up, and the capture uses 2 for
     * declining a call that is still ringing. Sending 0 for a decline tells
     * the CP that a call it never answered ended normally, and it then stops
     * registering IMS at all: the network keeps accepting calls and routing
     * them to voicemail without ever paging, and nothing short of a reboot
     * recovers it -- re-provisioning replays the identical AIMS sequence,
     * every response succeeds, and the CP still never registers.
     */
    status = samsung_aims_build_hangup (mm_sit_client_next_token (self->priv->client),
                                        call_id, declined ? 2 : 0, &frame, &len);
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
    case MM_SIT_MESSAGE_PHYSICAL_CHANNEL_CONFIG:
        /* The other mark of the settle window; see PDN_SETTLE_MS. Only the
         * first one counts -- the CP repeats this throughout the session, and
         * stock asks 600ms after a later one. */
        if (!self->priv->phych_us) {
            self->priv->phych_us = g_get_monotonic_time ();
            mm_obj_dbg (self, "the CP described its physical channel; "
                        "IMS may ask for a PDN %u ms from now",
                        (guint) PDN_SETTLE_MS);
            mm_sit_ims_reevaluate (self);
        }
        break;
    default:
        break;
    }
}

/*
 * Re-evaluates rather than starting directly: a physical-channel indication
 * that arrives while we are waiting moves the deadline out, and the gate below
 * re-arms this for whatever is left. See PDN_SETTLE_MS.
 */
static gboolean
settle_expired (MMSitIms *self)
{
    self->priv->settle_id = 0;
    mm_sit_ims_reevaluate (self);
    return G_SOURCE_REMOVE;
}

static gboolean
rat_grace_expired (MMSitIms *self)
{
    self->priv->rat_grace_id = 0;
    mm_obj_dbg (self, "IMS still has no supporting RAT after %u s; stopping IMS",
                (guint) RAT_GRACE_SECS);
    mm_sit_ims_stop (self);
    return G_SOURCE_REMOVE;
}

/*
 * Re-run the start gate with whatever is already known.
 *
 * The gate only runs when the modem pushes an access-tech update, so a
 * condition that becomes true in between -- the boot sequence finishing -- has
 * to say so, or nothing would start until the next RAT indication happened to
 * arrive.
 */
void
mm_sit_ims_reevaluate (MMSitIms *self)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));
    mm_sit_ims_update_access_tech (self, self->priv->last_act);
}

void
mm_sit_ims_update_access_tech (MMSitIms                *self,
                               MMModemAccessTechnology  act)
{
    gboolean possible;
    gboolean rat_ok;
    gboolean vops_ok;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    rat_ok = act_supports_ims (act) || reg_rat_supports_ims (self->priv->reg_rat);
    vops_ok = self->priv->vops_support != MM_SIT_VOPS_SUPPORT_UNSUPPORTED;
    possible = rat_ok && vops_ok;
    self->priv->last_act = act;

    if (possible && self->priv->rat_grace_id) {
        clear_rat_grace (self);
        /*
         * The RAT came back inside the grace period, so the stack we declined
         * to tear down is the one we still want. The CP may have let the SIP
         * registration go while we were away, though, and nothing else ever
         * asks for it again -- so in that case stop here and let the restart
         * below run the provisioning sequence from the top.
         */
        if (self->priv->state == IMS_STATE_STARTED && !self->priv->registered) {
            mm_obj_dbg (self, "IMS lost its registration off-RAT; restarting");
            mm_sit_ims_stop (self);
        } else {
            mm_obj_dbg (self, "supporting RAT returned within the grace period; keeping IMS");
        }
    }

    if (possible && self->priv->state == IMS_STATE_STOPPED) {
        gint64 ready_us;

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
        /*
         * And let the CP settle before asking for a context that is not the
         * attach context; see PDN_SETTLE_MS. Both marks are first-event marks,
         * so this cannot starve: if the CP never describes a physical channel,
         * the registration mark still carries the deadline.
         */
        ready_us = MAX (self->priv->net_registered_us, self->priv->phych_us);
        if (ready_us) {
            gint64 waited_ms = (g_get_monotonic_time () - ready_us) / 1000;

            if (waited_ms < PDN_SETTLE_MS) {
                guint remaining = (guint) (PDN_SETTLE_MS - waited_ms);

                if (self->priv->settle_id)
                    g_source_remove (self->priv->settle_id);
                mm_obj_dbg (self, "not starting IMS yet: letting the CP settle for %u ms more",
                            remaining);
                self->priv->settle_id = g_timeout_add (remaining,
                                                       (GSourceFunc) settle_expired,
                                                       self);
                return;
            }
        }
        mm_obj_dbg (self, "packet-switched RAT available; starting IMS");
        self->priv->state = IMS_STATE_STARTING;
        g_clear_object (&self->priv->cancellable);
        self->priv->cancellable = g_cancellable_new ();
        start_sequence (self);
        return;
    }

    if (possible || self->priv->state == IMS_STATE_STOPPED)
        return;

    /*
     * Only a missing RAT is worth waiting out. VoPS is the network telling us
     * there is no IMS voice service here, which is a statement and not a gap in
     * our own measurements, so honour it at once.
     */
    if (!vops_ok) {
        clear_rat_grace (self);
        mm_obj_dbg (self, "IMS unavailable on this network; stopping IMS");
        mm_sit_ims_stop (self);
        return;
    }

    if (!self->priv->rat_grace_id) {
        mm_obj_dbg (self, "IMS unavailable on this RAT; stopping IMS in %u s unless it returns",
                    (guint) RAT_GRACE_SECS);
        self->priv->rat_grace_id = g_timeout_add_seconds (RAT_GRACE_SECS,
                                                          (GSourceFunc) rat_grace_expired,
                                                          self);
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
    /* The first mark of the settle window; see PDN_SETTLE_MS. */
    if (registered && !self->priv->net_registered_us)
        self->priv->net_registered_us = g_get_monotonic_time ();

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
    gboolean         changed = FALSE;

    g_return_if_fail (MM_IS_SIT_IMS (self));
    g_return_if_fail (registration != NULL);

    /* Stop before clearing a stale capability. Otherwise a loss of packet
     * registration could change unsupported to unknown and briefly start IMS
     * while MM core still reports the previous registered state. */
    if (!registration->registered)
        mm_sit_ims_set_registered (self, FALSE);
    /*
     * Remember the last RAT the CP reported *while registered*, instead of
     * zeroing it whenever an indication says not-registered.
     *
     * reg_rat exists so a signal measurement can only add a RAT to the gate,
     * never remove one: on DSDS the other stack's traffic leaves this one with
     * no LTE measurement and the mask reads as UMTS. But DSDS produces
     * packet-registration blips too, and each of those used to zero reg_rat --
     * after which the CP, which only sends registration indications on change,
     * never restores it. The phantom measurement then won the gate unopposed
     * and RAT_GRACE_SECS later IMS was torn down for good.
     *
     * Observed exactly that: mmcli reporting lte/registered/attached the whole
     * time while "SIT signal changed (RAT mask: 0x0002)" was followed sixty
     * seconds later by "IMS still has no supporting RAT; stopping IMS", and
     * nothing ever started it again.
     *
     * A real move off LTE still clears this, because it arrives as a
     * registered indication naming the new RAT.
     */
    if (registration->registered &&
        self->priv->reg_rat != registration->radio_technology) {
        self->priv->reg_rat = registration->radio_technology;
        changed = TRUE;
    }
    support = registration->registered ? registration->vops_support : MM_SIT_VOPS_SUPPORT_UNKNOWN;
    if (self->priv->vops_support != support) {
        self->priv->vops_support = support;
        changed = TRUE;
        mm_obj_info (self, "network IMS voice support (VoPS): %s",
                      support == MM_SIT_VOPS_SUPPORT_SUPPORTED ? "supported" :
                      support == MM_SIT_VOPS_SUPPORT_UNSUPPORTED ? "unsupported" : "unknown");
    }
    if (changed)
        mm_sit_ims_update_access_tech (self, self->priv->last_act);
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
                    guint8       pdp,
                    guint32      bearers)
{
    g_return_if_fail (MM_IS_SIT_IMS (self));

    if (!apn || !apn[0])
        return;
    if (g_strcmp0 (self->priv->ims_apn, apn) == 0 && self->priv->ims_pdp == pdp &&
        self->priv->ims_bearers == bearers)
        return;

    g_free (self->priv->ims_apn);
    self->priv->ims_apn = g_strdup (apn);
    self->priv->ims_pdp = pdp;
    self->priv->ims_bearers = bearers;
    mm_obj_dbg (self, "IMS PDN will use the carrier's APN '%s' (pdp %u, bearers 0x%x)",
                apn, pdp, bearers);
}

void
mm_sit_ims_stop (MMSitIms *self)
{
    guint8          *frame = NULL;
    gsize            len = 0;

    g_return_if_fail (MM_IS_SIT_IMS (self));

    clear_retry (self);
    clear_rat_grace (self);
    /* Cancel the pending run, but leave the floor: surviving a stop/start is
     * the whole reason it is kept in monotonic time. */
    clear_cooldown (self);
    self->priv->provisioning_failures = 0;
    if (self->priv->cancellable) {
        g_cancellable_cancel (self->priv->cancellable);
        g_clear_object (&self->priv->cancellable);
    }

    if (self->priv->state == IMS_STATE_STOPPED)
        return;
    self->priv->state = IMS_STATE_STOPPED;
    clear_registration_watchdog (self);
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

    clear_cooldown (self);
    clear_retry (self);
    if (self->priv->settle_id) {
        g_source_remove (self->priv->settle_id);
        self->priv->settle_id = 0;
    }
    clear_rat_grace (self);
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
    /* The CP refused the IMS PDN, with the cause it gave. Lets the modem
     * change what it attaches with before the retry; see
     * attach_apn_resolve(). */
    signals[SIGNAL_PDN_REFUSED] =
        g_signal_new (MM_SIT_IMS_SIGNAL_PDN_REFUSED,
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1, G_TYPE_UINT);
}
