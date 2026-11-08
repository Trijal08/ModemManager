/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <errno.h>

#include <glib/gstdio.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-bearer-samsung-sit.h"
#include "mm-base-sim.h"
#include "mm-broadband-modem-samsung-sit.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-signal.h"
#include "mm-iface-modem-time.h"
#include "mm-log-object.h"
#include "mm-call-samsung-sit.h"
#include "mm-iface-modem-voice.h"
#include "mm-modem-helpers.h"
#include "mm-iface-modem-messaging.h"
#include "mm-sim-samsung-sit.h"
#include "mm-sms-part-3gpp.h"
#include "mm-sms-samsung-sit.h"
#include "mm-sit-carrier-config.h"
#include "mm-sit-ims-config.h"
#include "mm-sit-ims.h"
#include "mm-sit-cp.h"
#include "mm-sit-boot-parity.h"
#include "mm-utils.h"

static void iface_modem_init      (MMIfaceModemInterface     *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface);
static void iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface);
static void iface_modem_signal_init (MMIfaceModemSignalInterface *iface);
static void iface_modem_time_init (MMIfaceModemTimeInterface *iface);
static void iface_modem_voice_init (MMIfaceModemVoiceInterface *iface);

/*
 * Delivered-SMS duplicate suppression.
 *
 * SMS delivery is at-least-once: when an acknowledgement is lost the SMSC
 * resends, and 3GPP makes that the sender's duty rather than a fault. The CP
 * passes the resend up as another NEW_SMS with a byte-identical PDU, and so
 * does its own fast retransmit. Measured here: the same PDU twice 21ms apart,
 * and again 30s apart, both byte-identical, while our acknowledgement went out
 * in 3-24ms every time -- so this is not something being slow enough to fix.
 *
 * Android drops these in the framework (InboundSmsHandler keeps a raw-message
 * table for exactly this), so suppressing them here is parity, not a
 * workaround. Keep the last few PDU digests with the time they arrived; an
 * exact repeat inside the window is still acknowledged -- dropping it silently
 * would only earn another resend -- but it is not handed up twice.
 *
 * Two genuinely distinct messages cannot collide: a deliver PDU carries the
 * originating address and the SMSC timestamp, so an identical digest means the
 * same message from the same sender in the same second.
 */
#define SMS_DEDUP_SLOTS   8
#define SMS_DEDUP_WINDOW_SECS 120

typedef struct {
    guint  digest;
    gint64 seen_us;
} SmsDedupSlot;

struct _MMBroadbandModemSamsungSitPrivate {
    MMSitCp     *cp;
    SmsDedupSlot sms_seen[SMS_DEDUP_SLOTS];
    guint        sms_seen_next;
    gint         logical_stack; /* -1: legacy single-active SIM switching */
    GPtrArray   *clients;
    MMSitClient *client; /* borrowed primary logical-stack client */
    /* The CP's IMS stack, on the primary client's port. One per stack: the port
     * a frame goes out on is what selects the stack, so this cannot be shared. */
    MMSitIms    *ims;
    gchar       *operator_code;
    gchar       *operator_name;
    gboolean     carrier_config_ready;
    gchar       *carrier_config_imsi;
    /* PLMNs the card lists as forbidden (EF_FPLMN), so a scan can say so. */
    GPtrArray   *forbidden_plmns;
    /* Pending attach retry after a recoverable rejection, and how many we have
     * spent since the last successful registration. */
    guint        reject_retry_id;
    guint        reject_retries;
    /* The attach APN we last gave the CP. The CP cannot read it back, so this
     * is what load reports, and in turn what the core compares a new request
     * against to decide whether it is a change worth a power cycle. */
    MMBearerProperties *initial_eps_bearer_settings;
    /* ims.db, cached from the carrier-config mapping at construction: the
     * pre-radio-on path needs the carrier's APN list from it, and by then the
     * mapping property is no longer convenient to re-derive. */
    gchar       *ims_database;
    /* Set once a data PDN has been refused while the IMS attach was in
     * force, which settles the choice below in data's favour for good. */
    gboolean  attach_data_wins;
    /* Per-stack: may a 2253 on the IMS PDN take the modem-wide attach APN?
     * ID_MM_SIT_ESCALATE_ATTACH_TO_IMS; see ims_pdn_refused(). */
    gboolean  escalate_attach_to_ims;
    /* Set once the CP has refused the IMS PDN; see attach_apn_resolve(). */
    gboolean     attach_ims_fallback;
    /* The carrier's APN list while it is being registered with the CP, one
     * profile per round trip, before the radio goes on. */
    GPtrArray   *boot_apns;
    guint        boot_apn_index;
    /* Cursor into mm_sit_parity_requests[] for the walk in progress. */
    guint        parity_index;

    /* Reach the SIT ports through sit-proxy rather than owning the device
     * nodes, so mm-sit-tool and the IMS helper can share them. Set from
     * ID_MM_SIT_PROXY on the port. */
    gboolean     use_proxy;
    /* Coalesce SIM refresh indications before checking the affected stack.
     * The inventory notification is global and arrives on stack 0. */
    gboolean     sim_hot_swap_enabled;
    gboolean     sim_check_running;
    gboolean     sim_check_pending;
    guint        sim_check_id;
    guint        sim_check_retries;
    MMSitClient *sim_inventory_client; /* borrowed from cp */
    gulong       sim_inventory_id;
    /* The CP's version block, read once. Three separate loaders want fields of
     * it and the values do not change while the CP is up. */
    MMSitBaseBandVersion  version;
    gboolean              version_valid;
    /* Which generations the modem has, from 0x0718. Read once; it does not
     * change while the CP is up, and three loaders want it. */
    MMSitNetworkGenerations  radio_capability;
    gboolean                 radio_capability_valid;
    /* Watchdog for a CP that took the radio-on but never started searching,
     * and how many radio cycles it has spent trying to shake it loose. */
    guint        search_watchdog_id;
    guint        search_retries;
    /* The access technology the last signal report measured, kept because the
     * 5G the registration offers has to be published alongside it rather than
     * instead of it. See publish_access_tech(). */
    MMModemAccessTechnology signal_act;
    /* The serving cell's 5G NSA offer, from the registration response. */
    gboolean     endc_available;
    gboolean     dcnr_restricted;
};

typedef struct {
    MMSitCarrierConfig *config;
    gchar              *imsi;
} CarrierManifestContext;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemSamsungSit,
                        mm_broadband_modem_samsung_sit,
                        MM_TYPE_BROADBAND_MODEM,
                        0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_MESSAGING, iface_modem_messaging_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_SIGNAL, iface_modem_signal_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_TIME, iface_modem_time_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_VOICE, iface_modem_voice_init))

static gboolean
sit_signal_get_lte_rssi (const MMSitSignal *signal,
                         gdouble           *out_rssi)
{
    if (!signal->has_lte || signal->lte_signal_strength < 0 ||
        signal->lte_signal_strength > 31)
        return FALSE;

    *out_rssi = -113.0 + (2.0 * signal->lte_signal_strength);
    return TRUE;
}

static gboolean
sit_signal_get_lte_rsrp (const MMSitSignal *signal,
                         gdouble           *out_rsrp)
{
    /* Android's SIT/RIL ABI carries LTE RSRP as a positive dBm magnitude. */
    if (!signal->has_lte || signal->lte_rsrp < 44 || signal->lte_rsrp > 140)
        return FALSE;

    *out_rsrp = -(gdouble) signal->lte_rsrp;
    return TRUE;
}

static MMSignal *
sit_signal_build_lte (const MMSitSignal *signal)
{
    g_autoptr(MMSignal) result = NULL;
    gdouble             value;

    if (!signal->has_lte)
        return NULL;

    result = mm_signal_new ();
    if (sit_signal_get_lte_rssi (signal, &value))
        mm_signal_set_rssi (result, value);
    if (sit_signal_get_lte_rsrp (signal, &value))
        mm_signal_set_rsrp (result, value);
    /* RSRQ is also encoded as a positive dB magnitude in the Android ABI. */
    if (signal->lte_rsrq >= 3 && signal->lte_rsrq <= 34)
        mm_signal_set_rsrq (result, -(gdouble) signal->lte_rsrq);
    /* RSSNR is signed and expressed in tenths of a dB. */
    if (signal->lte_rssnr >= -200 && signal->lte_rssnr <= 300)
        mm_signal_set_snr (result, signal->lte_rssnr / 10.0);

    return g_steal_pointer (&result);
}

/* The NR leg of an EN-DC connection. It appears alongside the LTE block rather
 * than instead of it, and only while the network has actually added the NR
 * secondary cell -- so this is NULL on an idle or LTE-only connection. The
 * synchronisation-signal metrics are the ones that map onto MM's fields; the CSI
 * ones have no home there. */
static MMSignal *
sit_signal_build_nr5g (const MMSitSignal *signal)
{
    g_autoptr(MMSignal) result = NULL;

    if (!signal->has_nr5g)
        return NULL;

    result = mm_signal_new ();
    /* Same positive-magnitude encoding as the LTE fields. */
    if (signal->nr5g_ss_rsrp >= 44 && signal->nr5g_ss_rsrp <= 140)
        mm_signal_set_rsrp (result, -(gdouble) signal->nr5g_ss_rsrp);
    if (signal->nr5g_ss_rsrq >= 3 && signal->nr5g_ss_rsrq <= 43)
        mm_signal_set_rsrq (result, -(gdouble) signal->nr5g_ss_rsrq);
    /* SS-SINR is signed, in tenths of a dB, like the LTE RSSNR. */
    if (signal->nr5g_ss_sinr >= -230 && signal->nr5g_ss_sinr <= 400)
        mm_signal_set_snr (result, signal->nr5g_ss_sinr / 10.0);

    return g_steal_pointer (&result);
}

/*
 * The signal report's RAT mask is the only thing that says which radio the CP is
 * actually using: the registration responses report the LTE anchor even under
 * EN-DC, so 5G is invisible there. The mask is a set rather than a value, which
 * is what MM wants -- an NSA modem carries the LTE and NR bits at once and shows
 * up as "lte, 5gnr".
 */
static MMModemAccessTechnology
sit_rat_mask_to_access_technology (guint16 rat_mask)
{
    MMModemAccessTechnology act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;

    if (rat_mask & MM_SIT_SIGNAL_RAT_GSM)
        act |= MM_MODEM_ACCESS_TECHNOLOGY_GSM;
    if (rat_mask & MM_SIT_SIGNAL_RAT_WCDMA)
        act |= MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
    if (rat_mask & MM_SIT_SIGNAL_RAT_LTE)
        act |= MM_MODEM_ACCESS_TECHNOLOGY_LTE;
    if (rat_mask & MM_SIT_SIGNAL_RAT_NR5G)
        act |= MM_MODEM_ACCESS_TECHNOLOGY_5GNR;
    return act;
}

/*
 * Signal quality percentage.
 *
 * MM_RSRP_TO_QUALITY() is a linear ramp over [-110,-60] dBm, which puts 100% at
 * a level you only see standing under the antenna: a genuinely good -94 dBm came
 * out as 32%, and a phone alongside showed three bars out of four for the same
 * cell. Real RSRP spans roughly -75 at best to -120 at the cell edge, so that
 * ramp compresses every usable signal into the bottom half of the scale.
 *
 * Interpolate between anchor points instead, positioned so that each level a
 * handset would bucket a measurement into lands inside the band a UI reads as
 * the matching number of bars. For LTE the anchors are AOSP's default
 * lte_rsrp_thresholds_int_array {-128,-118,-108,-98} and for NR its
 * ss_rsrp_thresholds {-110,-90,-80,-65}; the quality values 6/31/56/81 are the
 * lower edges of Phosh's weak/ok/good/excellent bands (>5, >30, >55, >80), which
 * quantises the scale the same way. So -98 dBm reads "excellent" and -118 reads
 * "ok", as they do on a phone.
 *
 * The GSM and WCDMA anchors are the conventional bands for those measurements
 * rather than values pinned against a reference device, and matter much less --
 * they are only reachable now that the allowed-generation bitmap can select 2G
 * or 3G at all.
 */
typedef struct {
    gdouble dbm;
    guint   quality;
} SitQualityAnchor;

static const SitQualityAnchor lte_rsrp_anchors[] = {
    { -128.0, 6 }, { -118.0, 31 }, { -108.0, 56 }, { -98.0, 81 }, { -75.0, 100 },
};
static const SitQualityAnchor nr_ss_rsrp_anchors[] = {
    { -110.0, 6 }, { -90.0, 31 }, { -80.0, 56 }, { -65.0, 81 }, { -55.0, 100 },
};
static const SitQualityAnchor gsm_rssi_anchors[] = {
    { -107.0, 6 }, { -103.0, 31 }, { -97.0, 56 }, { -89.0, 81 }, { -75.0, 100 },
};
static const SitQualityAnchor wcdma_rscp_anchors[] = {
    { -115.0, 6 }, { -105.0, 31 }, { -95.0, 56 }, { -85.0, 81 }, { -75.0, 100 },
};

static guint
sit_interpolate_quality (gdouble                 dbm,
                         const SitQualityAnchor *anchors,
                         gsize                   n)
{
    gsize i;

    if (dbm < anchors[0].dbm)
        return 0;
    if (dbm >= anchors[n - 1].dbm)
        return anchors[n - 1].quality;

    for (i = 1; i < n; i++) {
        const SitQualityAnchor *lo = &anchors[i - 1];
        const SitQualityAnchor *hi = &anchors[i];

        if (dbm < hi->dbm)
            return lo->quality +
                   (guint) ((dbm - lo->dbm) * (hi->quality - lo->quality) / (hi->dbm - lo->dbm));
    }
    return anchors[n - 1].quality;
}

static gboolean
sit_signal_get_quality (const MMSitSignal *signal,
                        guint             *out_quality)
{
    gdouble value;

    /* LTE first, and under EN-DC deliberately so: the anchor carries the
     * connection, and its RSRP is what a handset bases its bars on. */
    if (sit_signal_get_lte_rsrp (signal, &value)) {
        *out_quality = sit_interpolate_quality (value, lte_rsrp_anchors,
                                                G_N_ELEMENTS (lte_rsrp_anchors));
        return TRUE;
    }
    /* Standalone NR, or an EN-DC report that somehow lost its LTE block. */
    if (signal->has_nr5g && signal->nr5g_ss_rsrp >= 44 && signal->nr5g_ss_rsrp <= 140) {
        *out_quality = sit_interpolate_quality (-(gdouble) signal->nr5g_ss_rsrp,
                                                nr_ss_rsrp_anchors,
                                                G_N_ELEMENTS (nr_ss_rsrp_anchors));
        return TRUE;
    }
    /* 2G: the CP reports a genuine narrowband RSSI as a 0-31 index. */
    if (signal->has_gsm && signal->gsm_signal_strength >= 0 &&
        signal->gsm_signal_strength <= 31) {
        *out_quality = sit_interpolate_quality (-113.0 + (2.0 * signal->gsm_signal_strength),
                                                gsm_rssi_anchors,
                                                G_N_ELEMENTS (gsm_rssi_anchors));
        return TRUE;
    }
    /* 3G: RSCP, carried as a positive dBm magnitude like the LTE fields. */
    if (signal->has_wcdma && signal->wcdma_rscp >= 25 && signal->wcdma_rscp <= 120) {
        *out_quality = sit_interpolate_quality (-(gdouble) signal->wcdma_rscp,
                                                wcdma_rscp_anchors,
                                                G_N_ELEMENTS (wcdma_rscp_anchors));
        return TRUE;
    }
    /* Last resort: the LTE 0-31 index. Wideband, so MM's narrowband RSSI range
     * flatters it -- only used when the CP gave no RSRP at all, which has not
     * been seen on hardware. */
    if (sit_signal_get_lte_rssi (signal, &value)) {
        *out_quality = MM_RSSI_TO_QUALITY (value);
        return TRUE;
    }
    return FALSE;
}

static MMModem3gppRegistrationState
sit_registration_state (const MMSitRegistration *registration)
{
    MMModem3gppRegistrationState state;

    switch (registration->state) {
    case 0:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_IDLE;
        break;
    case 1:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_HOME;
        break;
    case 2:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;
        break;
    case 3:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_DENIED;
        break;
    case 4:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
        break;
    case 5:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING;
        break;
    /* The s5400 firmware reports the camped state as 20, not 0 (confirmed on
     * device 2026-08-30: stable state=20 rat=14 while camped on the home PLMN
     * 24202 at ~-97 dBm, no reject indication), where state 0 instead means
     * "no serving cell" (empty PLMN). Upstream dropped the promote-to-
     * registered hack this used to extend, so name the state here instead --
     * which also keeps a roaming subscription roaming, the reason it went. */
    case 20:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_HOME;
        break;
    case 10:
    case 12:
    case 13:
    case 14:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_EMERGENCY_ONLY;
        break;
    default:
        state = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
        break;
    }

    return state;
}

/*
 * Android shows 5G from the network's EN-DC offer, not from an active NR
 * carrier: NR_STATE_NOT_RESTRICTED means "this cell will give you 5G", and the
 * indicator appears before any NR leg exists. The signal report's RAT mask only
 * gains its NR bit once a secondary carrier is actually carrying something,
 * which on a SIM with no data never happens -- so publishing the mask alone
 * hides 5G everywhere Android would show it.
 */
static MMModemAccessTechnology
decorate_access_tech (MMBroadbandModemSamsungSit *self,
                      MMModemAccessTechnology     act)
{
    if (self->priv->endc_available && !self->priv->dcnr_restricted)
        act |= MM_MODEM_ACCESS_TECHNOLOGY_5GNR;
    return act;
}

static void
publish_access_tech (MMBroadbandModemSamsungSit *self)
{
    mm_iface_modem_update_access_technologies (MM_IFACE_MODEM (self),
                                               decorate_access_tech (self, self->priv->signal_act),
                                               MM_MODEM_ACCESS_TECHNOLOGY_ANY);
}

/* 0x0701 covers the packet-switched domain only. The three states move together
 * because the CP reports one packet-service registration for whichever RAT it is
 * on, rather than separate GPRS/EPS/5GS views. */
static void
update_ps_registration (MMBroadbandModemSamsungSit *self,
                        const MMSitRegistration    *registration)
{
    MMModem3gppRegistrationState state = sit_registration_state (registration);

    /*
     * No post-registration profile batch here, deliberately.
     *
     * Android sends its whole APN table again once registered and re-sends the
     * attach APN right after. Copying that was tried and measured: it changed
     * nothing about the IMS refusal -- the status 2 answers it draws are the
     * same ones Android gets for the same APNs, so they were never a missing
     * table -- and the attach it re-sends fights the connect path, which has to
     * re-point the attach at each bearer's own APN for that bearer's PDN to
     * exist at all (see ConnectStep in mm-sit-client.c). The re-send put the
     * attach back to the IMS APN behind a live data bearer and cost mobile data
     * on both stacks.
     *
     * If the IMS refusal is ever understood, this is worth revisiting together
     * with the connect-path attach, since the two only conflict because of it.
     */
    /* Install VoPS before publishing REGISTERED: its state notification may
     * start IMS immediately, even before the next signal/RAT indication.
     */
    if (self->priv->ims)
        mm_sit_ims_update_registration (self->priv->ims, registration);

    if (self->priv->endc_available != registration->endc_available ||
        self->priv->dcnr_restricted != registration->dcnr_restricted) {
        self->priv->endc_available  = registration->endc_available;
        self->priv->dcnr_restricted = registration->dcnr_restricted;
        mm_obj_info (self, "network 5G NSA (EN-DC) offer: %s",
                     !registration->endc_available ? "not advertised" :
                     registration->dcnr_restricted ? "advertised but restricted for this subscriber" :
                     "available");
        publish_access_tech (self);
    }

    /* Registered again: forget the rejections, so a later one gets the full
     * retry budget rather than inheriting an exhausted one. */
    if (registration->registered) {
        if (self->priv->reject_retry_id) {
            g_source_remove (self->priv->reject_retry_id);
            self->priv->reject_retry_id = 0;
        }
        self->priv->reject_retries = 0;
    }

    mm_iface_modem_3gpp_update_ps_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_update_eps_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_update_5gs_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_apply_deferred_registration_state (MM_IFACE_MODEM_3GPP (self));
}

/* 0x0700, the CS-domain counterpart. */
static void
update_cs_registration (MMBroadbandModemSamsungSit *self,
                        const MMSitRegistration    *registration)
{
    mm_iface_modem_3gpp_update_cs_registration_state (MM_IFACE_MODEM_3GPP (self),
                                                      sit_registration_state (registration),
                                                      TRUE);
    mm_iface_modem_3gpp_apply_deferred_registration_state (MM_IFACE_MODEM_3GPP (self));
}

static gboolean
update_operator (MMBroadbandModemSamsungSit *self,
                 const MMSitOperator        *operator_info)
{
    const gchar *operator_name;
    gboolean     operator_changed;

    operator_changed = g_strcmp0 (self->priv->operator_code,
                                  operator_info->plmn[0] ? operator_info->plmn : NULL) != 0;
    g_free (self->priv->operator_code);
    self->priv->operator_code = operator_info->plmn[0] ? g_strdup (operator_info->plmn) : NULL;
    if (self->priv->ims)
        mm_sit_ims_set_operator_code (self->priv->ims, self->priv->operator_code);
    if (!operator_info->plmn[0] || operator_changed)
        g_clear_pointer (&self->priv->operator_name, g_free);

    operator_name = operator_info->operator_long[0] ? operator_info->operator_long :
                    operator_info->operator_short[0] ? operator_info->operator_short : NULL;
    if (operator_info->plmn[0] && operator_name &&
        g_strcmp0 (self->priv->operator_name, operator_name) != 0) {
        operator_changed = TRUE;
        g_free (self->priv->operator_name);
        self->priv->operator_name = g_strdup (operator_name);
    }
    return operator_changed;
}

static void
carrier_manifest_context_free (CarrierManifestContext *ctx)
{
    mm_sit_carrier_config_free (ctx->config);
    g_free (ctx->imsi);
    g_free (ctx);
}

static void
invalidate_carrier_config (MMBroadbandModemSamsungSit *self)
{
    self->priv->carrier_config_ready = FALSE;
    g_clear_pointer (&self->priv->carrier_config_imsi, g_free);
}

typedef struct {
    MMBroadbandModemSamsungSit *self;
    GArray                     *calls;
} ReportDataCallsContext;

/* A cid the CP still lists as active keeps its bearer; anything else has been
 * torn down behind our back. */
static void
bearer_report_lost_data_call (MMBaseBearer *bearer,
                              gpointer      user_data)
{
    ReportDataCallsContext *ctx = user_data;
    guint8                  cid;
    guint                   i;

    if (!MM_IS_BEARER_SAMSUNG_SIT (bearer))
        return;
    if (!mm_bearer_samsung_sit_get_cid (MM_BEARER_SAMSUNG_SIT (bearer), &cid))
        return;

    for (i = 0; i < ctx->calls->len; i++) {
        const MMSitDataCall *call = &g_array_index (ctx->calls, MMSitDataCall, i);

        if (call->cid == cid && call->active)
            return;
    }

    mm_obj_msg (ctx->self, "CP dropped data call %u; reporting bearer '%s' disconnected",
                cid, mm_base_bearer_get_path (bearer));
    mm_base_bearer_report_connection_status (bearer, MM_BEARER_CONNECTION_STATUS_DISCONNECTED);
}

/*
 * The CP announces its own PDN teardowns with a DATA_CALL_LIST_CHANGED that no
 * longer contains the context -- an empty 9-byte list when nothing is left.
 * Ignoring it left the bearer up with a dead context: the address stayed on
 * rmnet0 and both MM and NetworkManager still called it connected while no
 * packets flowed. Reproduced by narrowing the allowed network types, which makes
 * the CP re-attach on another RAT and drop the PDN on the way.
 */
static void
handle_data_call_list_changed (MMBroadbandModemSamsungSit *self,
                              const MMSitMessage         *message)
{
    g_autoptr(MMBearerList)  bearer_list = NULL;
    g_autoptr(GArray)        calls = NULL;
    g_autoptr(GError)        error = NULL;
    ReportDataCallsContext   ctx;

    if (!mm_sit_message_parse_data_call_list (message, &calls, &error)) {
        mm_obj_dbg (self, "unparsable data-call list indication: %s", error->message);
        return;
    }

    g_object_get (self, MM_IFACE_MODEM_BEARER_LIST, &bearer_list, NULL);
    if (!bearer_list)
        return;

    ctx.self = self;
    ctx.calls = calls;
    mm_bearer_list_foreach (bearer_list, (MMBearerListForeachFunc) bearer_report_lost_data_call,
                            &ctx);
}

/*****************************************************************************/
/* Messaging
 *
 * The generic path drives SMS over AT (+CPMS/+CNMI/+CMGF/+CSCS). The CP has
 * its own messages for it, on two transports: while IMS carries messages they
 * ride the AIMS group, otherwise the classic one. Delivery arrives as an
 * indication on whichever transport carried it, and has to be acknowledged on
 * that same one -- the CP holds the message until it is.
 *
 * There is no message store: the CP has none of its own, and messages on the
 * SIM live in EF_SMS, which is a file access rather than an SMS operation. So
 * a single ME storage is reported and parts are handed to the core as they
 * arrive.
 */

static void
ack_sms_ready (MMSitClient  *client,
               GAsyncResult *res,
               gpointer      user_data)
{
    g_autoptr(MMBroadbandModemSamsungSit) self = user_data;
    g_autoptr(GError)                     error = NULL;

    if (!mm_sit_client_ack_sms_finish (client, res, &error))
        mm_obj_warn (self, "couldn't acknowledge the delivered SMS: %s", error->message);
}

/*
 * True when this exact PDU has already been handed up recently, and records it
 * otherwise. Status reports go through the same door and are deduplicated the
 * same way: a repeated report would mark the same message delivered twice.
 */
static gboolean
sms_already_delivered (MMBroadbandModemSamsungSit *self,
                       const guint8               *pdu,
                       gsize                       pdu_len)
{
    gint64 now = g_get_monotonic_time ();
    guint  digest;
    guint  i;

    if (!pdu || !pdu_len)
        return FALSE;

    /* A plain FNV-1a walk: the PDU is short and this never leaves the host. */
    digest = 2166136261u;
    for (i = 0; i < pdu_len; i++) {
        digest ^= pdu[i];
        digest *= 16777619u;
    }

    for (i = 0; i < SMS_DEDUP_SLOTS; i++) {
        SmsDedupSlot *slot = &self->priv->sms_seen[i];

        if (!slot->seen_us || slot->digest != digest)
            continue;
        if (now - slot->seen_us > (gint64) SMS_DEDUP_WINDOW_SECS * G_USEC_PER_SEC)
            continue;
        slot->seen_us = now;	/* keep a repeatedly resent message suppressed */
        return TRUE;
    }

    self->priv->sms_seen[self->priv->sms_seen_next].digest = digest;
    self->priv->sms_seen[self->priv->sms_seen_next].seen_us = now;
    self->priv->sms_seen_next = (self->priv->sms_seen_next + 1) % SMS_DEDUP_SLOTS;
    return FALSE;
}

/* A delivered PDU on its way up, owned by the idle below. */
typedef struct {
    MMBroadbandModemSamsungSit *self;
    MMSmsPart                  *part;
} TakePartContext;

static void
take_part_context_free (TakePartContext *ctx)
{
    g_clear_object (&ctx->self);
    g_slice_free (TakePartContext, ctx);
}

/*
 * Hand the part to the core.
 *
 * A delivery report is matched to the message it belongs to by its reference,
 * and the reference is only recorded when the SEND_SMS response completes. The
 * CP answers the send and reports the delivery in the same batch, so handling
 * the report straight out of the indication runs it while the send's
 * completion is still queued: nothing claims that reference yet, the match
 * fails, and the report lands as a stray inbound message while the message it
 * belonged to is never marked delivered. Observed exactly that way -- report
 * for reference 237 taken at 21:59:09, "SMS part accepted, message reference
 * 237" in the same second but after it.
 *
 * So let the main loop drain first. A GTask completes at G_PRIORITY_DEFAULT
 * and this runs at G_PRIORITY_DEFAULT_IDLE, which is lower, so an already
 * queued send completion is guaranteed to record its reference before the
 * report looks for it. The acknowledgement does not wait for any of this.
 */
static gboolean
take_part_idle (TakePartContext *ctx)
{
    g_autoptr(GError) error = NULL;

    if (!mm_iface_modem_messaging_take_part (MM_IFACE_MODEM_MESSAGING (ctx->self),
                                             mm_broadband_modem_create_sms (MM_BROADBAND_MODEM (ctx->self)),
                                             ctx->part,
                                             MM_SMS_STATE_RECEIVED,
                                             MM_SMS_STORAGE_ME,
                                             &error))
        mm_obj_warn (ctx->self, "couldn't store the delivered SMS: %s", error->message);

    take_part_context_free (ctx);
    return G_SOURCE_REMOVE;
}

static void
handle_new_sms (MMBroadbandModemSamsungSit *self,
                const MMSitMessage         *message)
{
    g_autoptr(GError)  error = NULL;
    MMSitSmsDeliver    deliver;
    MMSmsPart         *part;
    MMSitClient       *client;
    TakePartContext   *ctx;

    if (!mm_sit_message_parse_new_sms (message, &deliver, &error)) {
        mm_obj_warn (self, "couldn't parse a delivered SMS: %s", error->message);
        return;
    }

    mm_obj_dbg (self, "%s delivered over %s, %u bytes",
                deliver.status_report ? "SMS delivery report" : "SMS",
                deliver.over_ims ? "IMS" : "the CS group", deliver.pdu_len);

    if (sms_already_delivered (self, deliver.pdu, deliver.pdu_len)) {
        mm_obj_dbg (self, "%s is a byte-identical resend; acknowledging without delivering it again",
                    deliver.status_report ? "SMS delivery report" : "SMS");
        goto ack;
    }

    part = mm_sms_part_3gpp_new_from_binary_pdu (SMS_PART_INVALID_INDEX,
                                                 deliver.pdu, deliver.pdu_len,
                                                 self, FALSE, &error);
    if (!part) {
        mm_obj_warn (self, "couldn't parse the delivered SMS PDU: %s", error->message);
    } else {
        /* A delivery report goes through the same door, and has to wait for
         * the main loop -- see take_part_idle(). */
        ctx = g_slice_new0 (TakePartContext);
        ctx->self = g_object_ref (self);
        ctx->part = part;
        g_idle_add ((GSourceFunc) take_part_idle, ctx);
    }

ack:
    /* Acknowledge either way: a message the core could not take is still one
     * the CP is holding, and leaving it unacknowledged stalls delivery. */
    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    if (client)
        mm_sit_client_ack_sms (client, deliver.over_ims, NULL,
                               (GAsyncReadyCallback) ack_sms_ready, g_object_ref (self));
}

/* NITZ_TIME_RECEIVED (0x0905): the CP pushing a network time as it arrives.
 * Same payload as the GET_NITZ_TIME response minus its status word, so the same
 * parser reads both -- and the age is always zero here, which is the tell that
 * this is the arrival rather than a cached read. See the Time interface below.
 */
static void
handle_nitz_time (MMBroadbandModemSamsungSit *self,
                  const MMSitMessage         *message)
{
    g_autoptr(GError)            error = NULL;
    g_autoptr(MMNetworkTimezone) tz = NULL;
    g_autofree gchar            *iso8601 = NULL;
    MMSitNitzTime                time;

    if (!mm_sit_message_parse_nitz_time (message, &time, &error)) {
        mm_obj_warn (self, "couldn't parse a pushed network time: %s", error->message);
        return;
    }
    /* The CP pushes the indication whenever it has one to push, including
     * before it has anything meaningful; an empty NITZ is not an error. */
    if (!time.valid) {
        mm_obj_dbg (self, "CP pushed an empty network time");
        return;
    }

    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    if (!iso8601) {
        mm_obj_warn (self, "couldn't render a pushed network time: %s", error->message);
        return;
    }
    mm_obj_dbg (self, "CP pushed a network time: %s", iso8601);
    mm_iface_modem_time_update_network_time (MM_IFACE_MODEM_TIME (self), iso8601);

    tz = mm_network_timezone_new ();
    mm_network_timezone_set_offset (tz, time.timezone_quarter_hours * 15);
    mm_network_timezone_set_dst_offset (tz, time.daylight_saving_hours * 60);
    mm_iface_modem_time_update_network_timezone (MM_IFACE_MODEM_TIME (self), tz);
}

static gboolean
messaging_check_support_finish (MMIfaceModemMessaging *self,
                                GAsyncResult          *res,
                                GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
messaging_check_support (MMIfaceModemMessaging *self,
                         GAsyncReadyCallback    callback,
                         gpointer               user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
messaging_load_supported_storages_finish (MMIfaceModemMessaging *self,
                                          GAsyncResult          *res,
                                          GArray               **mem1,
                                          GArray               **mem2,
                                          GArray               **mem3,
                                          GError               **error)
{
    MMSmsStorage storage = MM_SMS_STORAGE_ME;

    /* The CP delivers messages to us rather than storing them, so the same
     * single storage answers for reading, writing and receiving. */
    *mem1 = g_array_sized_new (FALSE, FALSE, sizeof (MMSmsStorage), 1);
    g_array_append_val (*mem1, storage);
    *mem2 = g_array_ref (*mem1);
    *mem3 = g_array_ref (*mem1);
    return TRUE;
}

static void
messaging_load_supported_storages (MMIfaceModemMessaging *self,
                                   GAsyncReadyCallback    callback,
                                   gpointer               user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
messaging_setup_cleanup_unsolicited_events_finish (MMIfaceModemMessaging *self,
                                                   GAsyncResult          *res,
                                                   GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
messaging_setup_cleanup_unsolicited_events (MMIfaceModemMessaging *self,
                                            GAsyncReadyCallback    callback,
                                            gpointer               user_data)
{
    GTask *task;

    /* Deliveries arrive on the SIT port's indication signal, which the plugin
     * already listens to for everything else; nothing to arm per interface. */
    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
messaging_load_initial_sms_parts_finish (MMIfaceModemMessaging *self,
                                         GAsyncResult          *res,
                                         GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
messaging_load_initial_sms_parts (MMIfaceModemMessaging *self,
                                  MMSmsStorage           storage,
                                  GAsyncReadyCallback    callback,
                                  gpointer               user_data)
{
    GTask *task;

    /* Nothing is stored to enumerate: what the CP had before we attached is
     * gone, and SIM-held messages are in EF_SMS, which is not read here. */
    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static MMBaseSms *
create_sms (MMBroadbandModem *self)
{
    return mm_sms_samsung_sit_new (MM_BASE_MODEM (self));
}

/*****************************************************************************/

/*****************************************************************************/
/* Initial EPS bearer settings (the attach APN) */

/* The APN the CP attaches with is not part of a data call: it is state the CP
 * carries into the attach itself, and it only takes effect at the *next*
 * attach. Setting it while already attached leaves the CP attached without it,
 * and SETUP_DATA_CALL then binds to an attach that has no APN behind it and
 * comes back with no address at all.
 *
 * Forcing that next attach is not this code's job: mm-iface-modem-3gpp powers
 * the modem down, calls in here, and then restores the previous power state, so
 * the CP attaches again from scratch with whatever was just set. It also skips
 * the whole sequence when the request matches what load reported, so repeating
 * a setting costs nothing.
 *
 * ModemManager's model already has the right shape for this -- the attach APN
 * is the initial EPS bearer, configured out of band by whoever owns the
 * connection rather than assembled inside a connect attempt -- which is why
 * this lives here and not in the bearer. */

static gboolean
set_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                        GAsyncResult      *res,
                                        GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * The attach APN the CP should be carrying, and the only place that decision is
 * made. SetInitialEpsBearerSettings wins where the user set one; otherwise it
 * is the carrier's own IA entry, or its DEFAULT entry for the majority of
 * carriers that mark no IA (511 of the 588 in the database). "" is the last
 * resort and asks the HSS for the subscribed default.
 *
 * @auth is filled from the configured properties when there are any, and zeroed
 * otherwise -- the carrier database carries no credentials for the attach
 * entry, and none of the three carriers tested needs them.
 *
 * Returns the APN, never NULL. A bearer's own APN is never a candidate: see the
 * comment above ConnectStep in mm-sit-client.c.
 */
static const gchar *
attach_apn_resolve (MMBroadbandModemSamsungSit *self,
                    MMSitPdpType               *out_pdp,
                    MMSitApnAuth               *out_auth)
{
    const MMSitApn *entry;

    memset (out_auth, 0, sizeof (*out_auth));

    if (self->priv->initial_eps_bearer_settings) {
        MMBearerProperties *properties = self->priv->initial_eps_bearer_settings;
        g_autoptr(GError)   error = NULL;

        if (!mm_sit_apn_auth_from_properties (properties, out_auth, &error)) {
            mm_obj_warn (self, "couldn't read the configured attach APN credentials: %s",
                         error->message);
            memset (out_auth, 0, sizeof (*out_auth));
        }
        *out_pdp = mm_sit_pdp_type_from_ip_family (mm_bearer_properties_get_ip_type (properties));
        return mm_bearer_properties_get_apn (properties) ?: "";
    }

    /*
     * Only after the CP has refused the IMS PDN. Until then we attach the way
     * Android does, on the carrier's IA entry, and most of the time that is the
     * end of it.
     *
     * This CP will not create a PDN for the IMS DNN on request. A
     * SETUP_DATA_CALL naming 'ims' is refused with MATCH_ALL_RULE_NOT_ALLOWED
     * (2253) whatever accompanies it -- proven by injecting this modem's own
     * Android frame and changing one field at a time: verbatim it is refused,
     * with pdp 3 instead of 2 refused, typed DEFAULT instead of IMS refused,
     * and the same frame with the APN swapped for 'sp.telus.com' (still
     * IMS-typed) is granted an address. Both SIMs behave the same way and both
     * carriers' IMS APN is literally the string 'ims'.
     *
     * Attaching on it is the way through: the attach creates the IMS context
     * itself, so the SETUP_DATA_CALL that follows only has to resolve to a
     * context that already exists, which the CP allows. IPv4v6 because that is
     * the combination measured to work; the IMS PDN that reuses the context
     * still asks for IPv6, as Android does.
     *
     * Android attaches on the carrier's IA entry and is granted the IMS PDN on
     * request. Why its CP accepts the DNN and ours does not is not understood,
     * and it is not for want of looking: the attach, the IMS data profile and
     * the IMS SETUP_DATA_CALL are byte-identical, as are the CP firmware, the
     * NV, the carrier policy, ap2cp_united_status and the AP capability word,
     * and every vendor channel Android drives is driven here too. The CP
     * reports no reason beyond the cause code, and its diagnostic channel
     * cannot be enabled on this hardware -- the log-mask plumbing ships without
     * the HAL it calls. Taking the IA entry instead, as the fall-through below
     * does, costs VoLTE entirely.
     */
    if (self->priv->attach_ims_fallback) {
        entry = mm_sit_carrier_apns_find (self->priv->boot_apns, MM_SIT_APN_TYPE_IMS);
        if (entry) {
            *out_pdp = MM_SIT_PDP_TYPE_IPV4V6;
            return entry->apn;
        }
    }

    entry = mm_sit_carrier_apns_find (self->priv->boot_apns, MM_SIT_APN_TYPE_IA);
    if (!entry)
        entry = mm_sit_carrier_apns_find (self->priv->boot_apns, MM_SIT_APN_TYPE_DEFAULT);

    *out_pdp = entry ? (MMSitPdpType) entry->protocol : MM_SIT_PDP_TYPE_IPV4V6;
    return entry ? entry->apn : "";
}

/*
 * What the attach asks the network for at @0xdb, which follows the same
 * condition as the APN above.
 *
 * Stock asks for nothing here: its CP grants the IMS PDN on request, and the
 * P-CSCF addresses arrive with that PDN, in the SETUP_DATA_CALL response. Once
 * we fall back to attaching on the IMS APN, that PDN is no longer created on
 * request -- the attach creates it, and the SETUP_DATA_CALL only resolves to
 * the context that already exists, returning the parameters it was created
 * with. A P-CSCF request made at that point is too late to change anything, so
 * it has to be made by the attach that creates the context or the network
 * grants no addresses at all and the CP has nowhere to register.
 *
 * Measured: with the fallback active and this left at 0, IMS provisions and
 * AIMS_REGISTRATION stays at length 36 (not registered); asking here is what
 * takes it to 159.
 */
static guint8
attach_pcscf_request (MMBroadbandModemSamsungSit *self)
{
    return self->priv->attach_ims_fallback ? MM_SIT_PCSCF_REQUEST_BOTH
                                           : MM_SIT_PCSCF_REQUEST_NONE;
}

/*
 * Per-stack escalation policy, from the port's ID_MM_SIT_ESCALATE_ATTACH_TO_IMS.
 *
 * Default FALSE, which is what the stock stack does -- it never moves the
 * attach APN and still gets its IMS PDN. The escalation is a workaround for
 * 2253 and it is not free: it takes the modem-wide attach APN, so the CP then
 * refuses that stack's data APN and the SIM loses data.
 *
 * On a dual-SIM device that trade only makes sense on the SIM you want VoLTE
 * on. Setting it globally makes *both* stacks escalate -- the second one gets
 * there before NetworkManager has brought its bearer up, so the
 * "don't escalate over a live connection" guard below never sees it, and the
 * device ends up with no data on either SIM. Hence per stack, set by the
 * device package next to ID_MM_SIT_LOGICAL_STACK.
 */
static gboolean
escalate_property (MMPort *port)
{
    const gchar *v;

    v = mm_kernel_device_get_property (mm_port_peek_kernel_device (port),
                                       "ID_MM_SIT_ESCALATE_ATTACH_TO_IMS");
    return v && (g_str_equal (v, "1") || !g_ascii_strcasecmp (v, "true"));
}

/*
 * The CP refused the IMS PDN. Escalate once: re-attach on the carrier's IMS
 * APN, which is the only way this CP will produce an IMS context.
 *
 * MATCH_ALL_RULE_NOT_ALLOWED (2253) is the refusal we know how to answer. It
 * means the CP will not create a PDN for the IMS DNN on request -- proven by
 * injecting this modem's own Android frame and changing one field at a time,
 * where the same frame with a different APN is granted an address. Attaching on
 * 'ims' makes the attach create the context, so the SETUP_DATA_CALL that
 * follows only has to resolve to one that exists.
 *
 * Android does not need this: it attaches on the carrier's IA entry and is
 * granted the IMS PDN on request. Why its CP accepts the DNN and ours does not
 * is unknown, and not for want of looking -- the attach, the IMS data profile
 * and the IMS SETUP_DATA_CALL are byte-identical, as are the CP firmware, the
 * NV, the carrier policy and every AP-to-CP status word. So this stays a
 * fallback rather than the default: parity is tried first on every boot, and a
 * CP or firmware that accepts the DNN never reaches this path. The IMS sequence
 * retries on its own timer, so the next attempt uses the new attach APN.
 */

static void search_watchdog_radio_off_ready (MMSitClient  *client,
                                             GAsyncResult *res,
                                             GTask        *task);

static void
ims_pdn_refused (MMSitIms                   *ims,
                 guint                       cause,
                 MMBroadbandModemSamsungSit *self)
{
    MMSitApnAuth  auth;
    MMSitPdpType  pdp;
    const gchar  *apn;
    MMSitClient  *client;
    GTask        *task;
    MMModemState  state = MM_MODEM_STATE_UNKNOWN;

    if (cause != MM_SIT_REJECT_CAUSE_MATCH_ALL_RULE_NOT_ALLOWED)
        return;

    /*
     * Never escalate. The stock stack does not, and the escalation is actively
     * harmful.
     *
     * Captured from komodo's own Android, 2026-11-03, both stacks: the attach
     * APN stays on the carrier's data entry ('sp.telus.com' / 'mobile.bm') for
     * the whole boot and the CP still grants the IMS PDN -- on stack 1 it holds
     * the data PDN on cid 17 and the IMS PDN on cid 18 *at the same time*, both
     * cause 0. So "the CP grants only one PDN per stack" is false, VoLTE-xor-
     * data is false, and 2253 is not inherent to this CP.
     *
     * What the escalation bought was not a PDN at all. With the attach moved to
     * 'ims' the CP answers a later IMS request in 3-4 ms -- "Matched with exist
     * PDU session", i.e. it just echoes the attach context -- where stock takes
     * 295-710 ms doing a real over-the-air establishment. The P-CSCF addresses
     * then come from whatever the *attach* happened to be given, and that is a
     * dice roll: 2001:568:fff2:300/fff3:300 registers (stock always gets this
     * pool), 2001:568:ffff:3002/ffff:4002 never does. Measured 6 for 6. That is
     * the whole reason VoLTE here was a one-in-five coin flip.
     *
     * And it costs the SIM its data, because with the attach on 'ims' the CP
     * refuses 2253 for the carrier's data APN.
     *
     * So this is now a diagnostic no-op: it reports the refusal and leaves the
     * attach alone. Until 2253 is fixed that means no VoLTE on a SIM whose IMS
     * PDN is refused -- but it also means the refusal is visible instead of
     * being papered over by a context that cannot register, and the SIM keeps
     * its data. Re-enabling the old behaviour is flipping the gate below.
     */
    if (!self->priv->escalate_attach_to_ims) {
        mm_obj_info (self, "the CP refused the IMS PDN (cause %u); leaving the attach APN "
                     "alone -- the stock stack never escalates and still gets the PDN",
                     cause);
        return;
    }

    if (self->priv->attach_ims_fallback)
        return;
    if (!mm_sit_carrier_apns_find (self->priv->boot_apns, MM_SIT_APN_TYPE_IMS))
        return;

    /*
     * Data outranks VoLTE here, because taking the attach APN for IMS is what
     * stops a data PDN being created at all: with the attach on 'ims' the CP
     * refused 2253 for both carriers' data APNs, and with the attach on the
     * carrier APN that same APN connects. So never escalate over a connection
     * that is already up, and never escalate again once a data call has been
     * refused while the IMS attach was in force.
     *
     * The IMS sequence keeps retrying on its own timer, so this is a deferral
     * rather than a refusal: on a stack with no data connection configured --
     * the other SIM of a dual-SIM pair, typically -- the next retry escalates
     * and VoLTE comes up there. That is the split this reaches on its own,
     * with nothing configured by hand: data on the SIM that carries it, VoLTE
     * on the one that does not.
     */
    if (self->priv->attach_data_wins) {
        mm_obj_dbg (self, "not attaching on the IMS APN: a data call was already refused under it");
        return;
    }
    /*
     * CONNECTING counts, not just CONNECTED: the two happen within the same
     * second on this modem, and testing for CONNECTED alone lost that race --
     * the escalation and "connecting -> connected" were logged in the same
     * second, so the attach was pulled out from under a connect that was about
     * to succeed. An in-flight connect is a connection as far as this choice
     * goes; data_pdn_refused() is the backstop if it fails for its own reasons.
     */
    g_object_get (self, MM_IFACE_MODEM_STATE, &state, NULL);
    if (state >= MM_MODEM_STATE_CONNECTING && !self->priv->escalate_attach_to_ims) {
        mm_obj_info (self, "the CP refused the IMS PDN (cause %u), but data is %s; "
                     "leaving the attach APN alone", cause,
                     state == MM_MODEM_STATE_CONNECTING ? "connecting" : "connected");
        return;
    }

    self->priv->attach_ims_fallback = TRUE;
    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    if (!client)
        return;

    apn = attach_apn_resolve (self, &pdp, &auth);
    mm_obj_info (self, "the CP refused the IMS PDN (cause %u); attaching on '%s' instead",
                 cause, apn);
    mm_sit_client_set_initial_attach_apn (client, apn, pdp, attach_pcscf_request (self),
                                          &auth, NULL, NULL, NULL);

    /*
     * Sending the attach APN is not enough on its own -- the same thing the
     * back-out below has always known: "the CP keeps serving the attach it
     * already has, so a change only takes effect at the next attach." The
     * escalation never produced one, so the APN was accepted on the wire and
     * then ignored.
     *
     * Measured 2026-11-04, exynos-modem r23 / MM r30, ch0xf5: SET_INITIAL_
     * ATTACH_APN 'ims' went out at t=21.8 s and every later IMS request that
     * boot was still refused 2253 -- 5 of them -- because the CP was still
     * attached on 'sp.telus.com'. The journal shows no "applying attach APN
     * ... before radio-on" for that stack, while the back-out at t=72 s does
     * log one. That asymmetry is the whole bug, and it is why a grant looked
     * like a dice roll: it only ever happened when something *else* (a
     * registration blip, the search watchdog) produced a re-attach.
     *
     * Only once registered. power_up_chain_start() takes the radio down first,
     * and an OFF inside the startup window parks the CP with cp_active 1;
     * REGISTERED means an attach has already completed, so that window is
     * closed. If we are not there yet the retry timer comes back round.
     */
    if (state < MM_MODEM_STATE_REGISTERED) {
        mm_obj_dbg (self, "not cycling the radio for the IMS attach yet (state %d); "
                    "the next provisioning retry will", (int) state);
        return;
    }

    mm_obj_info (self, "cycling the radio so the '%s' attach takes effect", apn);
    task = g_task_new (self, NULL, NULL, NULL);
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_OFF, NULL,
                                   (GAsyncReadyCallback) search_watchdog_radio_off_ready,
                                   task);
}

/*
 * The other direction: a data PDN the CP would not create while we were
 * attached on the IMS APN. That is the cost of the escalation above showing
 * up, so give it back -- restore the carrier's own attach entry, and do not
 * escalate again for the rest of this power-up.
 *
 * Only 2253 counts. The ordinary transient refusals a network can hand out
 * mid-attach say nothing about the attach APN, and reacting to them would
 * drop VoLTE for no reason.
 */
static void search_watchdog_radio_off_ready (MMSitClient  *client,
                                             GAsyncResult *res,
                                             GTask        *task);

static void
data_pdn_refused (MMSitClient                *client,
                  guint                       cause,
                  MMBroadbandModemSamsungSit *self)
{
    MMSitApnAuth  auth;
    MMSitPdpType  pdp;
    const gchar  *apn;
    GTask        *task;

    if (cause != MM_SIT_REJECT_CAUSE_MATCH_ALL_RULE_NOT_ALLOWED)
        return;
    if (!self->priv->attach_ims_fallback)
        return;

    /*
     * On the stack the device package designated as the VoLTE stack
     * (ID_MM_SIT_ESCALATE_ATTACH_TO_IMS) data does not outrank VoLTE: losing
     * that SIM's data is exactly the trade the designation asks for. Backing
     * out here threw the trade away 44 s after taking it -- measured on
     * hardware 2026-11-04 with exynos-modem r23 / MM r30:
     *
     *   11:14:35 [modem0] the CP refused the IMS PDN (2253); attaching on 'ims'
     *   11:15:19 [modem0] a data PDN was refused (2253) while attached on the
     *                     IMS APN; going back to 'sp.telus.com'
     *
     * and every later IMS request on that stack was refused for the rest of
     * the boot -- 7 of them, all 2253 -- because the attach was no longer
     * 'ims'. The SIM ended up with neither VoLTE nor (wanted) data.
     *
     * So on the designated stack keep the IMS attach and let that SIM's data
     * APN stay refused. attach_data_wins is deliberately left clear, so a
     * later refusal does not permanently block re-escalation either. The stack
     * that carries data is the one *without* the property.
     */
    if (self->priv->escalate_attach_to_ims) {
        mm_obj_info (self, "a data PDN was refused (cause %u) while attached on the IMS APN, "
                     "but this stack is designated for VoLTE; keeping the IMS attach",
                     cause);
        return;
    }

    self->priv->attach_ims_fallback = FALSE;
    self->priv->attach_data_wins = TRUE;

    apn = attach_apn_resolve (self, &pdp, &auth);
    mm_obj_info (self, "a data PDN was refused (cause %u) while attached on the IMS APN; "
                 "going back to '%s' so data works, at the cost of VoLTE on this SIM",
                 cause, apn);

    /*
     * Sending the attach APN is not enough on its own: the CP keeps serving
     * the attach it already has, so a change only takes effect at the next
     * attach. Cycle the radio to produce one -- power_up_chain_start() walks
     * the same path as a power-up and applies the APN before radio-on, which
     * is the ordering that works. Safe here for the same reason the search
     * watchdog's cycle is: this is a long way past the startup window, where
     * an OFF would wedge the CP.
     */
    task = g_task_new (self, NULL, NULL, NULL);
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_OFF, NULL,
                                   (GAsyncReadyCallback) search_watchdog_radio_off_ready,
                                   task);
}

static void
set_initial_attach_apn_ready (MMSitClient  *client,
                              GAsyncResult *res,
                              GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMBearerProperties         *properties = g_task_get_task_data (task);
    g_autoptr(GError)           error = NULL;

    if (!mm_sit_client_set_initial_attach_apn_finish (client, res, &error)) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    g_clear_object (&self->priv->initial_eps_bearer_settings);
    self->priv->initial_eps_bearer_settings = g_object_ref (properties);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_initial_eps_bearer_settings (MMIfaceModem3gpp    *_self,
                                 MMBearerProperties  *properties,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (_self);
    GTask                      *task;
    MMSitClient                *client;
    g_autoptr(GError)           error = NULL;
    GError                     *inner_error = NULL;
    MMSitApnAuth                auth;
    const gchar                *apn;

    task = g_task_new (self, NULL, callback, user_data);

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    /* An empty APN is a request, not an omission: it asks the network for the
     * subscribed default. The CP takes it and attaches. */
    apn = mm_bearer_properties_get_apn (properties) ?: "";

    if (!mm_sit_apn_auth_from_properties (properties, &auth, &inner_error)) {
        g_task_return_error (task, g_steal_pointer (&inner_error));
        g_object_unref (task);
        return;
    }

    g_task_set_task_data (task, g_object_ref (properties), g_object_unref);

    mm_sit_client_set_initial_attach_apn (client, apn,
                                          mm_sit_pdp_type_from_ip_family (mm_bearer_properties_get_ip_type (properties)),
                                          attach_pcscf_request (self),
                                          &auth,
                                          NULL,
                                          (GAsyncReadyCallback) set_initial_attach_apn_ready,
                                          task);
}

static MMBearerProperties *
load_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                         GAsyncResult      *res,
                                         GError           **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

/* The CP has no way to read the attach APN back, so report what we last set --
 * which is also what ModemManager needs to tell whether a new request is a
 * change. Before anything has been set this is empty rather than an error: an
 * unconfigured attach APN is a legitimate state, not a failure. */
static void
load_initial_eps_bearer_settings (MMIfaceModem3gpp    *_self,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (_self);
    GTask                      *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_pointer (task,
                           self->priv->initial_eps_bearer_settings ?
                           g_object_ref (self->priv->initial_eps_bearer_settings) :
                           mm_bearer_properties_new (),
                           g_object_unref);
    g_object_unref (task);
}

static MMBearerProperties *
load_initial_eps_bearer_finish (MMIfaceModem3gpp  *self,
                                GAsyncResult      *res,
                                GError           **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

/* What the *network* settled on, as opposed to what we asked for above. The CP
 * cannot report it -- there is no SIT request that reads the live attach back --
 * so decline, rather than pass the settings we requested off as what we got.
 * Those differ in the ordinary case: with nothing configured the CP still
 * attaches, on the default APN the HSS holds for the subscriber, and claiming
 * the empty APN we never sent would be worse than admitting we cannot see it.
 * Callers treat a failure as "no information" and carry on.
 *
 * This has to be implemented even though it only ever fails. MMBroadbandModem's
 * AT version asserts on a context-id lookup that only the AT
 * load_initial_eps_bearer_settings performs -- and we replace that one -- so
 * inheriting it aborts the daemon the moment registration reloads the bearer. */
static void
load_initial_eps_bearer (MMIfaceModem3gpp    *self,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "The CP cannot report the negotiated initial EPS bearer");
    g_object_unref (task);
}

/*****************************************************************************/

/* How many times to answer a recoverable rejection before leaving it alone, and
 * how long to wait before each. The first is short enough to rescue a boot
 * before anyone notices; the last is long enough not to hammer a network that
 * keeps saying no. */
#define REJECT_RETRY_MAX 3
static const guint reject_retry_delays_secs[REJECT_RETRY_MAX] = { 2, 15, 60 };

static void
reject_retry_radio_on_ready (MMSitClient                *client,
                             GAsyncResult               *res,
                             MMBroadbandModemSamsungSit *self)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_set_radio_state_finish (client, res, &error))
        mm_obj_warn (self, "attach retry: couldn't bring the radio back up: %s", error->message);
    g_object_unref (self);
}

static void
reject_retry_radio_off_ready (MMSitClient                *client,
                              GAsyncResult               *res,
                              MMBroadbandModemSamsungSit *self)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_set_radio_state_finish (client, res, &error)) {
        mm_obj_warn (self, "attach retry: couldn't take the radio down: %s", error->message);
        g_object_unref (self);
        return;
    }
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_ON, NULL,
                                   (GAsyncReadyCallback) reject_retry_radio_on_ready, self);
}

/* Cause 10 asks for a *new* attach, and the CP will not start one on its own --
 * it makes its burst of attempts, gives up and parks with the radio still
 * nominally on. Taking the radio down and back up is what makes it try again
 * from scratch; it is also exactly what recovers this by hand. */
static gboolean
reject_retry_cb (MMBroadbandModemSamsungSit *self)
{
    g_autoptr(GError)  error = NULL;
    MMSitClient       *client;

    self->priv->reject_retry_id = 0;

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        mm_obj_dbg (self, "attach retry: no SIT client: %s", error->message);
        return G_SOURCE_REMOVE;
    }

    self->priv->reject_retries++;
    mm_obj_msg (self, "attach retry %u/%u: cycling the radio to force a new attach",
                self->priv->reject_retries, REJECT_RETRY_MAX);
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_OFF, NULL,
                                   (GAsyncReadyCallback) reject_retry_radio_off_ready,
                                   g_object_ref (self));
    return G_SOURCE_REMOVE;
}

static void
handle_registration_failed (MMBroadbandModemSamsungSit *self,
                            const MMSitMessage         *message)
{
    MMSitRegistrationFailure  failure;
    g_autoptr(GError)         error = NULL;
    const gchar              *domain;
    guint                     delay;

    if (!mm_sit_message_parse_registration_failed (message, &failure, &error)) {
        mm_obj_dbg (self, "couldn't parse the registration failure: %s", error->message);
        return;
    }

    if (failure.circuit_switched && failure.packet_switched)
        domain = "CS+PS";
    else if (failure.circuit_switched)
        domain = "CS";
    else if (failure.packet_switched)
        domain = "PS";
    else
        domain = "no domain";

    if (failure.cause == MM_SIT_REJECT_CAUSE_NONE)
        mm_obj_msg (self, "network %s refused the %s attach, with no cause given",
                    failure.plmn, domain);
    else
        mm_obj_msg (self, "network %s refused the %s attach: 3GPP cause %u%s",
                    failure.plmn, domain, failure.cause,
                    failure.retryable ? " (implicitly detached -- will retry)" : "");

    /* A permanent refusal is the network's decision: cause 11 is what puts the
     * PLMN on the card's forbidden list, and cause 15 is answered by cell
     * reselection, which the CP does itself. Retrying either would just be a
     * busy loop. */
    if (!failure.retryable)
        return;

    /* One rejection arrives as several frames within a few milliseconds -- the
     * combined attach and its per-domain retries -- so the first one schedules
     * the retry and the rest ride along with it. */
    if (self->priv->reject_retry_id)
        return;

    if (self->priv->reject_retries >= REJECT_RETRY_MAX) {
        mm_obj_warn (self, "network %s still refusing after %u attach retries; leaving it alone",
                     failure.plmn, self->priv->reject_retries);
        return;
    }

    /*
     * Cause 10 (implicitly detached) on the home PLMN is the CP's own attach
     * giving up, not a network decision -- it makes its burst of attempts and
     * parks, and the radio cycle below is what restarts it. The cycle spends
     * the attach APN the CP keeps until told otherwise, which is fine while it
     * is retrying the same attach, but once the retries are spent the CP parks
     * with the APN gone and the next attach fails with cause 19. Re-apply it
     * when the radio comes back up so the parked state still carries one.
     */
    if (failure.cause == MM_SIT_REJECT_CAUSE_IMPLICITLY_DETACHED &&
        self->priv->reject_retries == REJECT_RETRY_MAX - 1) {
        MMSitClient  *client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
        MMSitApnAuth  auth;
        MMSitPdpType  pdp;
        const gchar  *apn;

        /* Without a configured profile this still has the carrier's own entry
         * to fall back on, which is the whole point of not needing one. */
        apn = attach_apn_resolve (self, &pdp, &auth);
        if (client) {
            mm_obj_dbg (self, "re-applying attach APN '%s' after the last attach retry", apn);
            mm_sit_client_set_initial_attach_apn (client, apn, pdp,
                                                  attach_pcscf_request (self),
                                                  &auth, NULL, NULL, NULL);
        }
    }

    delay = reject_retry_delays_secs[self->priv->reject_retries];
    mm_obj_dbg (self, "scheduling attach retry in %u s", delay);
    self->priv->reject_retry_id = g_timeout_add_seconds (delay,
                                                         (GSourceFunc) reject_retry_cb,
                                                         self);
}

/* A refresh produces several status notifications while the card restarts.
 * Give it time to publish a usable ICCID, then use MM's existing comparison
 * and disable/reprobe path. Unchanged identities do not disturb the other
 * DSDS stack, even though it also receives a global inventory notification. */
#define SIM_CHECK_SETTLE_MS 1500
#define SIM_CHECK_RETRY_MS  2000
#define SIM_CHECK_RETRIES   5

static gboolean sim_check_timeout (MMBroadbandModemSamsungSit *self);

static gboolean
sim_check_allowed (MMBroadbandModemSamsungSit *self)
{
    return self->priv->sim_hot_swap_enabled &&
           mm_base_modem_get_valid (MM_BASE_MODEM (self)) &&
           !mm_base_modem_get_reprobe (MM_BASE_MODEM (self));
}

static void
schedule_sim_check (MMBroadbandModemSamsungSit *self)
{
    if (!sim_check_allowed (self))
        return;
    self->priv->sim_check_pending = TRUE;
    self->priv->sim_check_retries = 0;
    if (self->priv->sim_check_running)
        return;
    if (self->priv->sim_check_id)
        g_source_remove (self->priv->sim_check_id);
    self->priv->sim_check_id = g_timeout_add (SIM_CHECK_SETTLE_MS,
                                              (GSourceFunc) sim_check_timeout, self);
}

static void
sim_swap_checked (MMIfaceModem *iface,
                  GAsyncResult *res,
                  MMBroadbandModemSamsungSit *self)
{
    g_autoptr(GError) error = NULL;

    if (!mm_iface_modem_check_for_sim_swap_finish (iface, res, &error))
        mm_obj_dbg (self, "SIT SIM identity check failed: %s", error->message);
    self->priv->sim_check_running = FALSE;
    if (self->priv->sim_check_pending)
        schedule_sim_check (self);
    g_object_unref (self);
}

static void
sim_check_empty_slots_ready (MMSitClient *client,
                             GAsyncResult *res,
                             MMBroadbandModemSamsungSit *self)
{
    MMSitSimSlotStatus inventory;
    g_autoptr(MMBaseSim) sim = NULL;
    g_autoptr(GError) error = NULL;
    guint i, j;

    if (!mm_sit_client_get_sim_slot_status_finish (client, res, &inventory, &error)) {
        mm_obj_warn (self, "couldn't confirm empty eSIM: %s", error->message);
        goto done;
    }
    if (!sim_check_allowed (self))
        goto done;

    /* A present card without an application is only a profileless eUICC
     * when the inventory agrees and identifies that card by its EID. */
    for (i = 0; i < inventory.slot_count; i++) {
        const MMSitSimSlot *slot = &inventory.slots[i];

        if (slot->card_state != 1 || !slot->eid_len)
            continue;
        for (j = 0; j < slot->port_count; j++) {
            const MMSitSimPort *port = &slot->ports[j];

            if (port->logical_slot_id != (guint) MAX (self->priv->logical_stack, 0) ||
                port->state != 1 || port->iccid_len)
                continue;
            g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
            if (!sim || !mm_base_sim_is_esim_without_profiles (sim)) {
                mm_obj_info (self, "SIT eUICC has no enabled profile");
                mm_sit_client_invalidate_sim (self->priv->client);
                mm_iface_modem_process_sim_event (MM_IFACE_MODEM (self));
            }
            goto done;
        }
    }
    mm_obj_dbg (self, "SIM inventory does not confirm a profileless eUICC");

done:
    self->priv->sim_check_running = FALSE;
    if (self->priv->sim_check_pending)
        schedule_sim_check (self);
    g_object_unref (self);
}

static void
sim_check_status_ready (MMSitClient *client,
                        GAsyncResult *res,
                        MMBroadbandModemSamsungSit *self)
{
    MMSitSimStatus status;
    g_autoptr(GError) error = NULL;
    gboolean loaded;

    loaded = mm_sit_client_get_sim_status_finish (client, res, &status, &error);
    if (!sim_check_allowed (self))
        goto done;
    if (!loaded || !status.card_state || !status.iccid_len) {
        self->priv->sim_check_running = FALSE;
        if (self->priv->sim_check_retries++ < SIM_CHECK_RETRIES) {
            mm_obj_dbg (self, "SIT SIM not ready after status change; retrying");
            self->priv->sim_check_id = g_timeout_add (SIM_CHECK_RETRY_MS,
                                                      (GSourceFunc) sim_check_timeout, self);
        } else if (loaded && !status.card_state) {
            g_autoptr(MMBaseSim) sim = NULL;

            g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
            if (sim) {
                mm_obj_info (self, "SIT SIM remains absent after status change");
                mm_sit_client_invalidate_sim (client);
                mm_iface_modem_process_sim_event (MM_IFACE_MODEM (self));
            }
        } else if (loaded && status.card_state == 1 && !status.app_count && !status.iccid_len) {
            self->priv->sim_check_running = TRUE;
            mm_sit_client_get_sim_slot_status (
                client, mm_base_modem_peek_cancellable (MM_BASE_MODEM (self)),
                (GAsyncReadyCallback) sim_check_empty_slots_ready, self);
            return;
        } else {
            mm_obj_warn (self, "SIT SIM readiness check failed: %s",
                          error ? error->message : "no ICCID after refresh");
        }
        g_object_unref (self);
        return;
    }

    {
        g_autoptr(MMBaseSim) sim = NULL;

        g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
        if (sim && mm_base_sim_is_esim_without_profiles (sim)) {
            /* The generic SIM identifier loader deliberately skips reads
             * for an empty eUICC. We already have a fresh ICCID here. */
            mm_obj_info (self, "SIT eSIM profile became available");
            mm_sit_client_invalidate_sim (client);
            mm_iface_modem_process_sim_event (MM_IFACE_MODEM (self));
            goto done;
        }
    }
    mm_obj_dbg (self, "SIT SIM ready after status change; checking identity");
    mm_iface_modem_check_for_sim_swap (MM_IFACE_MODEM (self),
                                       (GAsyncReadyCallback) sim_swap_checked, self);
    return;

done:
    self->priv->sim_check_running = FALSE;
    g_object_unref (self);
}

static gboolean
sim_check_timeout (MMBroadbandModemSamsungSit *self)
{
    self->priv->sim_check_id = 0;
    if (!sim_check_allowed (self))
        return G_SOURCE_REMOVE;
    self->priv->sim_check_pending = FALSE;
    self->priv->sim_check_running = TRUE;
    mm_sit_client_get_sim_status (self->priv->client,
                                  mm_base_modem_peek_cancellable (MM_BASE_MODEM (self)),
                                  (GAsyncReadyCallback) sim_check_status_ready,
                                  g_object_ref (self));
    return G_SOURCE_REMOVE;
}

static void
sim_inventory_indication (MMSitClient *client,
                          const MMSitMessage *message,
                          MMBroadbandModemSamsungSit *self)
{
    if (message->id == MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED)
        schedule_sim_check (self);
}

static void
cleanup_sim_hot_swap (MMIfaceModem *iface)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);

    self->priv->sim_hot_swap_enabled = FALSE;
    self->priv->sim_check_pending = FALSE;
    if (self->priv->sim_check_id) {
        g_source_remove (self->priv->sim_check_id);
        self->priv->sim_check_id = 0;
    }
    if (self->priv->sim_inventory_id) {
        g_signal_handler_disconnect (self->priv->sim_inventory_client, self->priv->sim_inventory_id);
        self->priv->sim_inventory_id = 0;
    }
    self->priv->sim_inventory_client = NULL;
}

static gboolean
setup_sim_hot_swap_finish (MMIfaceModem *iface, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
setup_sim_hot_swap (MMIfaceModem *iface, GAsyncReadyCallback callback, gpointer user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    g_autoptr(GTask) task = g_task_new (iface, NULL, callback, user_data);
    g_autoptr(GError) error = NULL;

    if (!mm_broadband_modem_samsung_sit_peek_client (self, &error)) {
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }
    if (self->priv->cp && !self->priv->sim_inventory_id) {
        MMSitClient *inventory = mm_sit_cp_peek_client (self->priv->cp, 0);

        if (inventory != self->priv->client) {
            self->priv->sim_inventory_client = inventory;
            self->priv->sim_inventory_id = g_signal_connect_object (
                inventory, "indication", G_CALLBACK (sim_inventory_indication), self, 0);
        }
    }
    self->priv->sim_hot_swap_enabled = TRUE;
    g_task_return_boolean (task, TRUE);
}

static void
sit_indication (MMSitClient              *client,
                const MMSitMessage       *message,
                MMBroadbandModemSamsungSit *self)
{
    MMSitOperator operator_info;

    if (message->id == MM_SIT_MESSAGE_REGISTRATION_FAILED)
        handle_registration_failed (self, message);

    if (message->id == MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED)
        handle_data_call_list_changed (self, message);

    if (message->id == MM_SIT_MESSAGE_CALL_STATE_CHANGED) {
        /* Empty poke -- the state is not in the indication, so re-read the
         * list and let the voice interface diff it against its call objects. */
        mm_obj_dbg (self, "SIT call state changed; reloading call list");
        mm_iface_modem_voice_reload_all_calls (MM_IFACE_MODEM_VOICE (self), NULL, NULL);
    }

    if (message->id == MM_SIT_MESSAGE_NEW_SMS ||
        message->id == MM_SIT_MESSAGE_NEW_SMS_STATUS_REPORT ||
        message->id == MM_SIT_MESSAGE_IMS_NEW_SMS)
        handle_new_sms (self, message);

    if (message->id == MM_SIT_MESSAGE_SIM_STATUS_CHANGED) {
        invalidate_carrier_config (self);
        schedule_sim_check (self);
    } else if (message->id == MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED)
        schedule_sim_check (self);
    else if (message->id == MM_SIT_MESSAGE_CARRIER_CONFIG_STATUS) {
        gboolean applied = FALSE;

        /*
         * The CP's verdict on the configuration it was last pointed at: it
         * fetched and applied it, or it already had that manifest and did
         * nothing. Both are normal -- a CP that has the configuration reports
         * nothing to do on every later boot -- so this is a debug line.
         *
         * Proven rather than assumed: handed a manifest hash it cannot have,
         * the CP opens <dir>/manifests/<hash> over RFS within a second, and
         * handed a different valid one it fetches that manifest and all of its
         * confseqs. It is silent only for a hash it already holds.
         */
        if (mm_sit_message_parse_carrier_config_status (message, &applied, NULL))
            mm_obj_dbg (self, "CP %s the carrier configuration",
                        applied ? "applied" : "already had");
    }
    else if (message->id == MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO) {
        MMSitCarrierSimInfo info;

        if (mm_sit_message_parse_carrier_sim_info (message, &info, NULL) &&
            self->priv->carrier_config_imsi &&
            !g_str_equal (self->priv->carrier_config_imsi, info.imsi))
            invalidate_carrier_config (self);
    }

    if (message->id == MM_SIT_MESSAGE_SERVING_NETWORK &&
        mm_sit_message_parse_operator (message, &operator_info, NULL)) {
        mm_obj_dbg (self, "SIT serving network changed (operator: %s)",
                    operator_info.plmn[0] ? operator_info.plmn : "unknown");
        /* Propagate a changed operator to the DBus properties: the initial
         * load may have run while the CP was still (re-)camping and the
         * core does not re-query without a registration transition. */
        if (update_operator (self, &operator_info))
            mm_iface_modem_3gpp_reload_current_registration_info (MM_IFACE_MODEM_3GPP (self), NULL, NULL);
        /*
         * And re-read the registration *state*, which the line above does not:
         * it reloads who we are registered to, not whether we are. The CP has
         * no indication for the state itself, so this is the only edge that
         * says "something changed, go and look".
         *
         * Without it an attach that completes after enabling is invisible.
         * That is the ordinary case rather than a corner one: the attach lands
         * a second or so after the attach APN is set, by which time the
         * enabling sequence has already run its checks and seen nothing, and
         * nothing would ever run them again -- the modem stays `enabled` with a
         * correct operator and a stale `idle`, and IMS waits for a registration
         * that already happened.
         */
        mm_iface_modem_3gpp_run_registration_checks (MM_IFACE_MODEM_3GPP (self), NULL, NULL);
    }

    if (message->id == MM_SIT_MESSAGE_NITZ_TIME_RECEIVED)
        handle_nitz_time (self, message);

    if (message->id == MM_SIT_MESSAGE_SIGNAL_STRENGTH_CHANGED) {
        MMSitSignal             signal;
        g_autoptr(MMSignal)     lte = NULL;
        g_autoptr(MMSignal)     nr5g = NULL;
        MMModemAccessTechnology act;
        guint                   quality;

        if (!mm_sit_message_parse_signal (message, &signal, NULL))
            return;

        mm_obj_dbg (self, "SIT signal changed (schema: V%u, RAT mask: 0x%04x)",
                    signal.version, signal.rat_mask);
        lte = sit_signal_build_lte (&signal);
        nr5g = sit_signal_build_nr5g (&signal);
        if (lte || nr5g)
            mm_iface_modem_signal_update (MM_IFACE_MODEM_SIGNAL (self),
                                          NULL, NULL, NULL, NULL, lte, nr5g);
        if (sit_signal_get_quality (&signal, &quality))
            mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), quality);
        act = sit_rat_mask_to_access_technology (signal.rat_mask);
        self->priv->signal_act = act;
        publish_access_tech (self);
        /* IMS is gated on what the radio is measurably on, so it gets the mask
         * itself rather than the 5G the network merely offers. */
        if (self->priv->ims)
            mm_sit_ims_update_access_tech (self->priv->ims, act);
    }
}

/* An IMS call changing is the same event as the CS group's empty poke: re-read
 * the list and let the voice interface diff it against its call objects. */
static void
ims_calls_changed (MMBroadbandModemSamsungSit *self)
{
    mm_iface_modem_voice_reload_all_calls (MM_IFACE_MODEM_VOICE (self), NULL, NULL);
}

/* Follow the modem's own state rather than a bearer: it is the one signal that
 * is already correct for every way the radio can come and go -- including the
 * power-down/set/restore MM core does around the attach APN, which drops every
 * context and passes through DISABLED on the way.
 *
 * REGISTERED and not CONNECTED: IMS carries its own context on its own APN and
 * does not need mobile data, so gating it on a default bearer kept VoLTE down
 * whenever data was off. Android keeps VoLTE working in exactly that state. */
static void
modem_state_changed_for_ims (MMBroadbandModemSamsungSit *self)
{
    MMModemState state = MM_MODEM_STATE_UNKNOWN;

    if (!self->priv->ims)
        return;
    g_object_get (self, MM_IFACE_MODEM_STATE, &state, NULL);
    mm_sit_ims_set_registered (self->priv->ims, state >= MM_MODEM_STATE_REGISTERED);
}

static gchar *
variant_to_hex (GVariant *value)
{
    const guint8 *data;
    gsize         len;

    if (!value)
        return NULL;
    data = g_variant_get_fixed_array (value, &len, sizeof (guint8));
    if (!data || !len)
        return NULL;
    return mm_utils_bin2hexstr (data, len);
}

static void
carrier_manifest_ready (MMSitClient *client,
                        GAsyncResult *res,
                        GTask        *task)
{
    MMBroadbandModemSamsungSit *self;
    CarrierManifestContext     *ctx;
    MMSitCarrierConfig         *config;
    MmGdbusModem               *skeleton = NULL;
    GError                     *error = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);
    config = ctx->config;
    if (!mm_sit_client_set_carrier_manifest_finish (client, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!self->priv->carrier_config_imsi ||
        !g_str_equal (self->priv->carrier_config_imsi, ctx->imsi)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "SIM changed while provisioning Samsung carrier profile");
        g_object_unref (task);
        return;
    }

    self->priv->carrier_config_ready = TRUE;
    g_object_get (self, MM_IFACE_MODEM_DBUS_SKELETON, &skeleton, NULL);
    if (skeleton) {
        g_autofree gchar *fallback_name = NULL;

        if (!config->carrier_name || !config->carrier_name[0])
            fallback_name = g_strdup_printf ("carrier-%u", config->carrier_id);
        mm_gdbus_modem_set_carrier_configuration (
            skeleton, fallback_name ? fallback_name : config->carrier_name);
        mm_gdbus_modem_set_carrier_configuration_revision (
            skeleton, config->manifest_sha1);
        g_object_unref (skeleton);
    }

    mm_obj_info (self, "selected Samsung carrier profile %s (carrier ID %u)",
                 (config->carrier_name && config->carrier_name[0]) ? config->carrier_name : "unnamed",
                 config->carrier_id);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}


/*
 * Where the CP's property reads are answered from, one file per property name.
 *
 * The CP asks the AP for a handful of Android system properties over the
 * oem/GEMS channel, and the one that matters here arrives right after the
 * carrier config is announced: `vendor.ril.modem.cfg.carrier_id<stack>`, which
 * downstream answers with the cfg.db carrier id (captured from this modem's own
 * Android: 1404 for ca_telus, 2417 for ca_fizz). ModemManager is what resolves
 * that id, so it is what has to publish it; exynos-modem serves the read.
 */
#define SIT_MODEM_PROPERTY_DIR "/run/exynos-modem/properties"

static void
publish_carrier_id (MMBroadbandModemSamsungSit *self,
                    guint                       carrier_id)
{
    g_autofree gchar *name = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *value = NULL;
    g_autoptr(GError) error = NULL;

    /* The property is indexed by logical stack, so without one there is no
     * name to publish under. */
    if (self->priv->logical_stack < 0) {
        mm_obj_dbg (self, "no logical stack; not publishing the carrier id");
        return;
    }

    if (g_mkdir_with_parents (SIT_MODEM_PROPERTY_DIR, 0755) != 0) {
        mm_obj_dbg (self, "couldn't create %s: %s",
                    SIT_MODEM_PROPERTY_DIR, g_strerror (errno));
        return;
    }
    name = g_strdup_printf ("vendor.ril.modem.cfg.carrier_id%d",
                            self->priv->logical_stack);
    path = g_build_filename (SIT_MODEM_PROPERTY_DIR, name, NULL);
    value = g_strdup_printf ("%u", carrier_id);
    if (!g_file_set_contents (path, value, -1, &error)) {
        mm_obj_dbg (self, "couldn't publish %s: %s", name, error->message);
        return;
    }
    mm_obj_dbg (self, "published %s = %s", name, value);
}

static void
setup_carrier_config (MMIfaceModem        *iface,
                      const gchar         *imsi,
                      const gchar         *carrier_config_database,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self;
    g_autoptr(MMBaseSim)        sim = NULL;
    MMSitCarrierSimInfo         sit_info = { 0 };
    MMSitCarrierIdentity        identity = { 0 };
    g_autofree gchar           *gid1 = NULL;
    g_autofree gchar           *gid2 = NULL;
    g_autofree gchar           *directory = NULL;
    CarrierManifestContext     *ctx;
    MMSitCarrierConfig         *config = NULL;
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    task = g_task_new (self, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
    identity.imsi = imsi;
    if (sim) {
        GVariant *gid;

        identity.iccid = mm_gdbus_sim_get_sim_identifier (MM_GDBUS_SIM (sim));
        identity.mccmnc = mm_gdbus_sim_get_operator_identifier (MM_GDBUS_SIM (sim));
        identity.spn = mm_gdbus_sim_get_operator_name (MM_GDBUS_SIM (sim));
        gid = mm_gdbus_sim_get_gid1 (MM_GDBUS_SIM (sim));
        gid1 = variant_to_hex (gid);
        gid = mm_gdbus_sim_get_gid2 (MM_GDBUS_SIM (sim));
        gid2 = variant_to_hex (gid);
        identity.gid1 = gid1;
        identity.gid2 = gid2;
    }

    if (mm_sit_client_get_carrier_sim_info (client, &sit_info) &&
        g_str_equal (sit_info.imsi, imsi) && !identity.iccid)
        identity.iccid = sit_info.iccid;

    /* The IMS configuration is chosen by the same identity, and for the same
     * reason: it belongs to the subscription, not to the network we happen to
     * be camped on. */
    if (self->priv->ims)
        mm_sit_ims_set_carrier_identity (self->priv->ims, &identity);

    if (!mm_sit_carrier_config_resolve (carrier_config_database, &identity, &config, &error)) {
        invalidate_carrier_config (self);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_free (self->priv->carrier_config_imsi);
    self->priv->carrier_config_imsi = g_strdup (imsi);
    /* Before the announce: the CP asks for this within a second of it. */
    publish_carrier_id (self, config->carrier_id);
    /* The CP is told downstream's directory, not ours: see
     * MM_SIT_ANNOUNCED_CARRIER_CONFIG_DIRECTORY. */
    directory = g_strdup (MM_SIT_ANNOUNCED_CARRIER_CONFIG_DIRECTORY);
    ctx = g_new0 (CarrierManifestContext, 1);
    ctx->config = config;
    ctx->imsi = g_strdup (imsi);
    g_task_set_task_data (task, ctx, (GDestroyNotify) carrier_manifest_context_free);
    mm_sit_client_set_carrier_manifest (client,
                                        directory,
                                        config->manifest_sha1_bytes,
                                        NULL,
                                        (GAsyncReadyCallback) carrier_manifest_ready,
                                        task);
}

static gboolean
setup_carrier_config_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void setup_carrier_config (MMIfaceModem        *iface,
                                  const gchar         *imsi,
                                  const gchar         *carrier_config_database,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);

typedef struct {
    GAsyncReadyCallback callback;
    gpointer            user_data;
    gchar              *database;
} EnsureCarrierConfigContext;

static void
ensure_carrier_config_context_free (EnsureCarrierConfigContext *ctx)
{
    g_free (ctx->database);
    g_free (ctx);
}

static void
carrier_sim_info_queried (MMSitClient  *client,
                          GAsyncResult *res,
                          gpointer      user_data)
{
    GTask *task = user_data;

    MMBroadbandModemSamsungSit *self;
    MMSitCarrierSimInfo        *info = NULL;
    g_autoptr(GError)           error = NULL;
    EnsureCarrierConfigContext *ctx;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    ctx = g_task_get_task_data (task);

    info = mm_sit_client_query_carrier_sim_info_finish (client, res, &error);
    if (!info) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    g_object_unref (task);
    setup_carrier_config (MM_IFACE_MODEM (self), info->imsi,
                          ctx->database, ctx->callback, ctx->user_data);
    g_free (info);
}

void
mm_broadband_modem_samsung_sit_ensure_carrier_config (MMBroadbandModemSamsungSit *self,
                                                       GAsyncReadyCallback          callback,
                                                       gpointer                     user_data)
{
    g_autoptr(MMBaseSim) sim = NULL;
    g_autofree gchar    *database = NULL;
    MMSitCarrierSimInfo  sit_info = { 0 };
    const gchar         *imsi = NULL;
    GTask               *task;

    g_return_if_fail (MM_IS_BROADBAND_MODEM_SAMSUNG_SIT (self));

    g_object_get (self,
                  MM_IFACE_MODEM_SIM, &sim,
                  MM_IFACE_MODEM_CARRIER_CONFIG_MAPPING, &database,
                  NULL);
    if (sim)
        imsi = mm_gdbus_sim_get_imsi (MM_GDBUS_SIM (sim));
    if (!imsi && self->priv->client &&
        mm_sit_client_get_carrier_sim_info (self->priv->client, &sit_info))
        imsi = sit_info.imsi;

    /*
     * Re-push only for a subscription we can see has changed.
     *
     * An IMSI we cannot read right now is not evidence that it changed -- the
     * SIM object's load loses the CP's park/wake race often enough that the
     * comment below exists for it -- and the old test fell through to
     * invalidate on exactly that, throwing away a configuration the CP had
     * already accepted.
     *
     * The cost is not one wasted frame. Re-pushing the manifest makes the CP
     * reload its carrier policy, which means re-reading every carrier-config
     * file over the oem channel: measured on hardware, one redundant push on
     * stack 1 at t=26.3s drew 47 of the boot's 89 file reads, and the CP
     * answered "already had the carrier configuration" both times. Stock
     * pushes once per stack and never again.
     */
    if (self->priv->carrier_config_ready) {
        if (!imsi || !imsi[0] ||
            !self->priv->carrier_config_imsi ||
            g_str_equal (self->priv->carrier_config_imsi, imsi)) {
            task = g_task_new (self, NULL, callback, user_data);
            g_task_return_boolean (task, TRUE);
            g_object_unref (task);
            return;
        }
        invalidate_carrier_config (self);
    }

    if (!database || !database[0]) {
        task = g_task_new (self, NULL, callback, user_data);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "Samsung carrier configuration requires a database");
        g_object_unref (task);
        return;
    }

    /* The SIM object's IMSI load can lose the CP's park/wake race during
     * enabling, so it may be empty even with a provisioned SIM.  The CP still
     * holds the identity -- ask it directly before giving up. */
    if (!imsi || !imsi[0]) {
        EnsureCarrierConfigContext *ctx;

        ctx = g_new0 (EnsureCarrierConfigContext, 1);
        ctx->callback = callback;
        ctx->user_data = user_data;
        ctx->database = g_steal_pointer (&database);

        task = g_task_new (self, NULL, NULL, NULL);
        g_task_set_task_data (task, ctx,
                              (GDestroyNotify) ensure_carrier_config_context_free);
        mm_sit_client_query_carrier_sim_info (self->priv->client, NULL,
                                              (GAsyncReadyCallback) carrier_sim_info_queried,
                                              task);
        return;
    }

    setup_carrier_config (MM_IFACE_MODEM (self), imsi, database, callback, user_data);
}

gboolean
mm_broadband_modem_samsung_sit_ensure_carrier_config_finish (MMBroadbandModemSamsungSit  *self,
                                                              GAsyncResult                 *res,
                                                              GError                      **error)
{
    g_return_val_if_fail (MM_IS_BROADBAND_MODEM_SAMSUNG_SIT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static gint
compare_sit_ports (gconstpointer a,
                   gconstpointer b)
{
    return g_strcmp0 (mm_port_get_device (MM_PORT ((gpointer) a)),
                      mm_port_get_device (MM_PORT ((gpointer) b)));
}

MMSitClient *
mm_broadband_modem_samsung_sit_peek_client (MMBroadbandModemSamsungSit  *self,
                                             GError                     **error)
{
    GList *ports;
    GList *l;

    g_return_val_if_fail (MM_IS_BROADBAND_MODEM_SAMSUNG_SIT (self), NULL);

    if (self->priv->client)
        return self->priv->client;

    ports = mm_base_modem_find_ports (MM_BASE_MODEM (self),
                                      MM_PORT_SUBSYS_UNKNOWN,
                                      MM_PORT_TYPE_SIT);
    if (!ports) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                             "No Samsung SIT control port available");
        return NULL;
    }

    ports = g_list_sort (ports, compare_sit_ports);
    if (self->priv->logical_stack >= 0) {
        MMPort *port = MM_PORT (ports->data);
        g_autofree gchar *expected_suffix = g_strdup_printf ("sit%d", self->priv->logical_stack);

        if (g_list_length (ports) != 1 ||
            !g_str_has_suffix (mm_port_get_device (port), expected_suffix)) {
            g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "DSDS modem must own exactly its configured SIT port");
            g_list_free_full (ports, g_object_unref);
            return NULL;
        }
        self->priv->cp = mm_sit_cp_get (mm_base_modem_get_physdev (MM_BASE_MODEM (self)),
                                       mm_port_get_device (port), self->priv->use_proxy, error);
        if (self->priv->cp) {
            self->priv->client = mm_sit_cp_peek_client (self->priv->cp, self->priv->logical_stack);
            mm_sit_client_set_device_info (
                self->priv->client,
                mm_kernel_device_get_property (mm_port_peek_kernel_device (port), "ID_MM_SIT_DEVICE_MODEL"),
                mm_kernel_device_get_property (mm_port_peek_kernel_device (port), "ID_MM_SIT_DEVICE_BUILD"),
                mm_kernel_device_get_property (mm_port_peek_kernel_device (port), "ID_MM_SIT_DEVICE_CODENAME"),
                mm_kernel_device_get_property (mm_port_peek_kernel_device (port), "ID_MM_SIT_DEVICE_SW_VERSION"),
                mm_kernel_device_get_property (mm_port_peek_kernel_device (port), "ID_MM_SIT_CP_SW_VERSION"));
            self->priv->escalate_attach_to_ims = escalate_property (port);
            mm_obj_info (self, "DSDS logical stack %d on %s", self->priv->logical_stack,
                         mm_port_get_device (port));
        }
        g_list_free_full (ports, g_object_unref);
        if (!self->priv->client)
            return NULL;
        goto client_ready;
    }
    self->priv->clients = g_ptr_array_new_with_free_func (g_object_unref);
    for (l = ports; l; l = g_list_next (l)) {
        MMPort      *port = MM_PORT (l->data);
        MMSitClient *client;

        /* An explicitly ignored port is grabbed with its detected type, so
         * it still shows up in this lookup; it must not be opened. */
        if (mm_port_get_port_group (port) == MM_PORT_GROUP_IGNORED) {
            mm_obj_dbg (self, "skipping ignored Samsung SIT port %s",
                        mm_port_get_device (port));
            continue;
        }

        client = self->priv->use_proxy
            ? mm_sit_client_new_proxy (mm_port_get_device (port))
            : mm_sit_client_new (mm_port_get_device (port));
        /* What the boot-config batch tells the CP the AP is. NULL lets the
         * running system answer; the properties exist so a device package can
         * pin exact strings without a code change. */
        mm_sit_client_set_device_info (
            client,
            mm_kernel_device_get_property (mm_port_peek_kernel_device (port),
                                           "ID_MM_SIT_DEVICE_MODEL"),
            mm_kernel_device_get_property (mm_port_peek_kernel_device (port),
                                           "ID_MM_SIT_DEVICE_BUILD"),
            mm_kernel_device_get_property (mm_port_peek_kernel_device (port),
                                           "ID_MM_SIT_DEVICE_CODENAME"),
            mm_kernel_device_get_property (mm_port_peek_kernel_device (port),
                                           "ID_MM_SIT_DEVICE_SW_VERSION"),
            mm_kernel_device_get_property (mm_port_peek_kernel_device (port),
                                           "ID_MM_SIT_CP_SW_VERSION"));
        if (escalate_property (port))
            self->priv->escalate_attach_to_ims = TRUE;
        if (!mm_sit_client_open (client, error)) {
            g_object_unref (client);
            self->priv->client = NULL;
            g_clear_pointer (&self->priv->clients, g_ptr_array_unref);
            g_list_free_full (ports, g_object_unref);
            return NULL;
        }
        g_ptr_array_add (self->priv->clients, client);
        mm_obj_info (self, "opened Samsung SIT logical stack %u on %s%s",
                     self->priv->clients->len - 1, mm_port_get_device (port),
                     mm_sit_client_is_proxied (client) ? " (via sit-proxy)" : "");
    }
    g_list_free_full (ports, g_object_unref);

    if (!self->priv->clients->len) {
        g_clear_pointer (&self->priv->clients, g_ptr_array_unref);
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                             "No usable Samsung SIT control port available");
        return NULL;
    }

    self->priv->client = g_ptr_array_index (self->priv->clients, 0);
client_ready:
    g_signal_connect_object (self->priv->client, "indication", G_CALLBACK (sit_indication), self, 0);
    {
        /* ims.db sits beside the carrier-config database rather than being
         * configured separately: both are per-carrier data for the same modem,
         * found through the same directory, so packaging them together keeps
         * one knob instead of two that can disagree. */
        g_autofree gchar *mapping = NULL;
        g_autofree gchar *directory = NULL;
        g_autofree gchar *ims_database = NULL;

        g_object_get (self, MM_IFACE_MODEM_CARRIER_CONFIG_MAPPING, &mapping, NULL);
        if (mapping) {
            directory = g_path_get_dirname (mapping);
            ims_database = g_build_filename (directory, "ims.db", NULL);
        }
        self->priv->ims = mm_sit_ims_new (self->priv->client, mm_sit_client_get_stack (self->priv->client), ims_database);
        g_signal_connect (self->priv->ims, MM_SIT_IMS_SIGNAL_PDN_REFUSED,
                          G_CALLBACK (ims_pdn_refused), self);
        g_signal_connect (self->priv->client, MM_SIT_CLIENT_SIGNAL_DATA_PDN_REFUSED,
                          G_CALLBACK (data_pdn_refused), self);
        self->priv->ims_database = g_strdup (ims_database);
    }
    g_signal_connect_swapped (self->priv->ims, MM_SIT_IMS_SIGNAL_CALLS_CHANGED,
                              G_CALLBACK (ims_calls_changed), self);
    /* IMS waits for the default context: the enable path power-cycles the radio
     * to apply the attach APN and that drops every PDN, so a stack provisioned
     * before then loses the one it was given. */
    g_signal_connect (self, "notify::" MM_IFACE_MODEM_STATE,
                      G_CALLBACK (modem_state_changed_for_ims), NULL);
    return self->priv->client;
}

MMSitIms *
mm_broadband_modem_samsung_sit_peek_ims (MMBroadbandModemSamsungSit *self)
{
    g_return_val_if_fail (MM_IS_BROADBAND_MODEM_SAMSUNG_SIT (self), NULL);
    return self->priv->ims;
}

/*****************************************************************************/
/* Modem interface */

/* Capabilities and supported modes
 *
 * Both come from 0x0718 GET_RC_NETWORK_TYPE -- the modem's radio capability,
 * i.e. which generations it *has*. Not to be confused with 0x0750, the allowed
 * bitmap, which is a setting: restricting the modem to 2G moves 0x0750 to
 * 0x00000206 and 0x070b to 0x00000001 while 0x0718 stays 0x00040288. Sourcing
 * capabilities from the allowed set would make the modem disown LTE whenever
 * someone restricted the modes, and the core turns off EPS and 5GS
 * registration checks based on capabilities.
 *
 * ModemManager's generic path would infer capabilities from `ATI`, `+GCAP` and
 * `+WS46=?`, and on this modem that lands on `gsm-umts` alone: `+GCAP` names
 * only the circuit-switched capability and `+WS46=?` is answered malformed,
 * so the packet-switched generations never appear.
 *
 * One capability combination, not a list: the generations are not switchable,
 * only maskable, and the maskable part is the Modes property.
 *
 * Both capability steps are *fatal* to modem initialization
 * (`mm-iface-modem.c`), so a failed read falls back to reporting everything
 * rather than refusing to bring the modem up; the warning says so. */

static MMModemCapability
load_current_capabilities_finish (MMIfaceModem  *self,
                                  GAsyncResult  *res,
                                  GError       **error)
{
    GError *inner_error = NULL;
    gssize  value;

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_CAPABILITY_NONE;
    }
    return (MMModemCapability) value;
}

static MMModemCapability
generations_to_capabilities (const MMSitNetworkGenerations *g)
{
    MMModemCapability caps = MM_MODEM_CAPABILITY_NONE;

    if (g->gsm || g->umts)
        caps |= MM_MODEM_CAPABILITY_GSM_UMTS;
    if (g->lte)
        caps |= MM_MODEM_CAPABILITY_LTE;
    if (g->nr)
        caps |= MM_MODEM_CAPABILITY_5GNR;
    return caps;
}

/* What a modem that answers nothing is assumed to have. Only reached when the
 * capability read fails, which is fatal to initialization otherwise. */
static const MMSitNetworkGenerations all_generations = {
    .gsm = TRUE, .umts = TRUE, .lte = TRUE, .nr = TRUE
};

static void
radio_capability_ready (MMSitClient  *client,
                        GAsyncResult *res,
                        GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    g_autoptr(GError)           error = NULL;

    if (!mm_sit_client_get_radio_capability_finish (client, res, &self->priv->radio_capability,
                                                    &error)) {
        mm_obj_warn (self, "couldn't read the modem's radio capability (%s); assuming it has all "
                     "the generations it could have", error->message);
        self->priv->radio_capability = all_generations;
    }
    self->priv->radio_capability_valid = TRUE;

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* Read 0x0718 once and serve the capability and mode loaders from it. */
static void
ensure_radio_capability (MMBroadbandModemSamsungSit *self,
                         GAsyncReadyCallback         callback,
                         gpointer                    user_data)
{
    MMSitClient *client;
    GError      *error = NULL;
    GTask       *task;

    task = g_task_new (self, NULL, callback, user_data);
    if (self->priv->radio_capability_valid) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        mm_obj_warn (self, "couldn't reach the CP to read its radio capability (%s); assuming it "
                     "has all the generations it could have", error->message);
        g_clear_error (&error);
        self->priv->radio_capability = all_generations;
        self->priv->radio_capability_valid = TRUE;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_radio_capability (client, NULL,
                                        (GAsyncReadyCallback) radio_capability_ready, task);
}

static void
current_capabilities_ready (MMBroadbandModemSamsungSit *self,
                            GAsyncResult               *res,
                            GTask                      *task)
{
    g_task_return_int (task, generations_to_capabilities (&self->priv->radio_capability));
    g_object_unref (task);
}

static void
load_current_capabilities (MMIfaceModem        *self,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    ensure_radio_capability (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                             (GAsyncReadyCallback) current_capabilities_ready,
                             g_task_new (self, NULL, callback, user_data));
}

static GArray *
load_supported_capabilities_finish (MMIfaceModem  *self,
                                    GAsyncResult  *res,
                                    GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
supported_capabilities_ready (MMBroadbandModemSamsungSit *self,
                              GAsyncResult               *res,
                              GTask                      *task)
{
    GArray            *capabilities;
    MMModemCapability  capability;

    capability = generations_to_capabilities (&self->priv->radio_capability);
    capabilities = g_array_sized_new (FALSE, FALSE, sizeof (MMModemCapability), 1);
    g_array_append_val (capabilities, capability);

    g_task_return_pointer (task, capabilities, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_supported_capabilities (MMIfaceModem        *self,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
    ensure_radio_capability (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                             (GAsyncReadyCallback) supported_capabilities_ready,
                             g_task_new (self, NULL, callback, user_data));
}

/* Identity
 *
 * All of it comes off the control plane. 0x0401 GET_IMEI holds the equipment
 * identifier, and 0x0901 GET_BASE_BAND_VERSION holds the rest: 'sw_ver' is the
 * firmware build (the same string the AT service answers +CGMR with), 'hw_ver'
 * the hardware revision, and 'cp_chipset' the baseband part.
 *
 * The model is built from the chipset rather than stated, because this plugin
 * is not specific to one part -- the S5400 speaks the same protocol, and the
 * downstream RIL these layouts came from is built from an s5400 tree. Anything
 * the CP does not report is left unset rather than guessed.
 *
 * Manufacturer stays a constant: it is what the plugin is, not something the
 * modem could disagree about. */
#define SAMSUNG_SIT_MANUFACTURER "Samsung Electronics"

static gchar *
load_identity_string_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_manufacturer (MMIfaceModem        *self,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_pointer (task, g_strdup (SAMSUNG_SIT_MANUFACTURER), g_free);
    g_object_unref (task);
}

/* Which field of the version block a loader wants. */
typedef enum {
    VERSION_FIELD_MODEL,
    VERSION_FIELD_REVISION,
    VERSION_FIELD_HARDWARE_REVISION,
} VersionField;

static gchar *
version_field_dup (MMBroadbandModemSamsungSit *self,
                   VersionField                field)
{
    const MMSitBaseBandVersion *v = &self->priv->version;

    switch (field) {
    case VERSION_FIELD_MODEL:
        /* "S" + the part, e.g. S5300. +CGMM answers the same with a firmware
         * variant suffix we have no way to read back. */
        return v->cp_chipset[0] ? g_strdup_printf ("S%s", v->cp_chipset) : NULL;
    case VERSION_FIELD_REVISION:
        return v->sw_ver[0] ? g_strdup (v->sw_ver) : NULL;
    case VERSION_FIELD_HARDWARE_REVISION:
        return v->hw_ver[0] ? g_strdup (v->hw_ver) : NULL;
    default:
        g_assert_not_reached ();
    }
}

static void
load_version_field_ready (MMSitClient  *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    VersionField                field = GPOINTER_TO_UINT (g_task_get_task_data (task));
    g_autoptr(GError)           error = NULL;
    gchar                      *value;

    if (!mm_sit_client_get_base_band_version_finish (client, res, &self->priv->version, &error)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "couldn't read the CP version block: %s", error->message);
        g_object_unref (task);
        return;
    }
    self->priv->version_valid = TRUE;

    value = version_field_dup (self, field);
    if (!value)
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "the CP does not report this");
    else
        g_task_return_pointer (task, value, g_free);
    g_object_unref (task);
}

/* Read the block once and serve all three loaders from it: the core calls them
 * in sequence during initialization and the values cannot change while the CP
 * is up. */
static void
load_version_field (MMIfaceModem        *iface,
                    VersionField         field,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (field), NULL);

    if (self->priv->version_valid) {
        gchar *value = version_field_dup (self, field);

        if (!value)
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                     "the CP does not report this");
        else
            g_task_return_pointer (task, value, g_free);
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_base_band_version (client, NULL,
                                         (GAsyncReadyCallback) load_version_field_ready, task);
}

static void
load_model (MMIfaceModem        *self,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
    load_version_field (self, VERSION_FIELD_MODEL, callback, user_data);
}

static void
load_revision (MMIfaceModem        *self,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
    load_version_field (self, VERSION_FIELD_REVISION, callback, user_data);
}

static void
load_hardware_revision (MMIfaceModem        *self,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    load_version_field (self, VERSION_FIELD_HARDWARE_REVISION, callback, user_data);
}

static void
load_imei_ready (MMSitClient  *client,
                 GAsyncResult *res,
                 GTask        *task)
{
    GError *error = NULL;
    gchar  *imei;

    imei = mm_sit_client_get_imei_finish (client, res, &error);
    if (!imei)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, imei, g_free);
    g_object_unref (task);
}

static void
load_equipment_identifier (MMIfaceModem        *iface,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_imei (client, NULL, (GAsyncReadyCallback) load_imei_ready, task);
}

/* The generic implementation runs an AT sequence purely to feed ATI/ATI1 into
 * the hash. The helper accepts empty ones -- QMI and MBIM pass "" here for the
 * same reason -- and the identity loaded above carries the uniqueness. */
static void
load_device_identifier (MMIfaceModem        *self,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    GTask  *task;
    GError *error = NULL;
    gchar  *device_identifier;

    task = g_task_new (self, NULL, callback, user_data);
    device_identifier = mm_broadband_modem_create_device_identifier (
        MM_BROADBAND_MODEM (self), "", "", &error);
    if (!device_identifier)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, device_identifier, g_free);
    g_object_unref (task);
}

static guint
load_signal_quality_finish (MMIfaceModem  *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    gssize value;

    value = g_task_propagate_int (G_TASK (res), error);
    return value < 0 ? 0 : value;
}

static void
load_signal_quality_ready (MMSitClient *client,
                           GAsyncResult *res,
                           GTask        *task)
{
    MMSitSignal signal;
    GError     *error = NULL;
    guint       quality;

    if (!mm_sit_client_get_signal_finish (client, res, &signal, &error))
        g_task_return_error (task, error);
    else if (!sit_signal_get_quality (&signal, &quality))
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "SIT response did not contain a usable signal value");
    else
        g_task_return_int (task, quality);
    g_object_unref (task);
}

static void
load_signal_quality (MMIfaceModem        *iface,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_signal (client, NULL,
                              (GAsyncReadyCallback) load_signal_quality_ready,
                              task);
}

static gboolean
load_access_technologies_finish (MMIfaceModem            *self,
                                 GAsyncResult            *res,
                                 MMModemAccessTechnology *access_technologies,
                                 guint                   *mask,
                                 GError                 **error)
{
    GTask *task = G_TASK (res);

    if (!g_task_propagate_boolean (task, error))
        return FALSE;
    *access_technologies = (MMModemAccessTechnology) GPOINTER_TO_UINT (g_task_get_task_data (task));
    *mask = MM_MODEM_ACCESS_TECHNOLOGY_ANY;
    return TRUE;
}

static void
load_access_technologies_ready (MMSitClient  *client,
                                GAsyncResult *res,
                                GTask        *task)
{
    MMBroadbandModemSamsungSit *self;
    MMSitSignal signal;
    GError     *error = NULL;

    if (!mm_sit_client_get_signal_finish (client, res, &signal, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /*
     * This reports with a mask of ANY, so whatever it returns replaces the
     * whole value -- including the 5G the registration offered. Decorate it
     * the same way publish_access_tech() does, or the poll silently undoes the
     * indication a few seconds later.
     */
    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    self->priv->signal_act = sit_rat_mask_to_access_technology (signal.rat_mask);
    g_task_set_task_data (task,
                          GUINT_TO_POINTER ((guint) decorate_access_tech (self, self->priv->signal_act)),
                          NULL);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* The indication keeps this current; the poll covers the gap before the first one
 * arrives and any period where the CP goes quiet. Same source either way. */
static void
load_access_technologies (MMIfaceModem       *iface,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_signal (client, NULL,
                              (GAsyncReadyCallback) load_access_technologies_ready, task);
}

/*****************************************************************************/
/* Own numbers
 *
 * The generic path asks AT+CNUM. EF_MSISDN holds the same thing: linear-fixed
 * records ending in the 14-byte dialling-number structure of TS 31.102 -- the
 * length of the BCD contents, the type of number, and the digits in swapped
 * BCD padded with 0xf. What precedes it is the alpha identifier, whose size is
 * whatever the record has left over.
 */

#define MSISDN_DIALLING_LEN 14
#define MSISDN_NUMBER_LEN   10
/* Type-of-number field, TS 24.008 10.5.4.7: 001 in bits 6..4 is international. */
#define TON_MASK            0x70
#define TON_INTERNATIONAL   0x10

static GStrv
load_own_numbers_finish (MMIfaceModem *self,
                         GAsyncResult *res,
                         GError      **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

/* Decode one record's dialling number, or NULL when the record is unused. */
static gchar *
parse_msisdn_record (const guint8 *record,
                     gsize         record_len)
{
    g_autofree gchar *digits = NULL;
    const guint8     *dialling;
    guint8            bcd_len;
    guint8            ton_npi;
    gchar            *end;

    if (record_len < MSISDN_DIALLING_LEN)
        return NULL;

    dialling = record + record_len - MSISDN_DIALLING_LEN;
    bcd_len  = dialling[0];
    ton_npi  = dialling[1];
    /* An unused record leaves the length at 0 or 0xff; the byte counts the
     * type-of-number octet as well as the digits. */
    if (!bcd_len || bcd_len == 0xff || bcd_len > MSISDN_NUMBER_LEN + 1)
        return NULL;

    digits = mm_bcd_to_string (dialling + 2, bcd_len - 1, TRUE);
    if (!digits || !digits[0])
        return NULL;
    /* An odd digit count leaves a padding nibble at the end. */
    end = strchr (digits, 'f');
    if (end)
        *end = '\0';
    if (!digits[0])
        return NULL;

    /* +CNUM dropped the type of number; with the byte in hand the
     * international form can be reported as such. */
    if ((ton_npi & TON_MASK) == TON_INTERNATIONAL)
        return g_strdup_printf ("+%s", digits);
    return g_steal_pointer (&digits);
}

static void
load_own_numbers_ready (MMSitClient  *client,
                        GAsyncResult *res,
                        GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    g_autoptr(GBytes)           file = NULL;
    g_autoptr(GPtrArray)        numbers = NULL;
    MMSitEfInfo                 info;
    const guint8               *data;
    gsize                       len = 0;
    gsize                       offset;
    GError                     *error = NULL;

    file = mm_sit_client_read_ef_finish (client, res, &info, &error);
    if (!file) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!info.record_len) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "EF_MSISDN is not a record file");
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    numbers = g_ptr_array_new ();
    for (offset = 0; offset + info.record_len <= len; offset += info.record_len) {
        gchar *number;

        number = parse_msisdn_record (data + offset, info.record_len);
        if (number)
            g_ptr_array_add (numbers, number);
    }

    if (!numbers->len) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "EF_MSISDN holds no number");
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "read %u subscriber number(s) from EF_MSISDN", numbers->len);
    g_ptr_array_add (numbers, NULL);
    g_task_return_pointer (task, g_ptr_array_free (g_steal_pointer (&numbers), FALSE),
                           (GDestroyNotify) g_strfreev);
    g_object_unref (task);
}

static void
load_own_numbers (MMIfaceModem       *iface,
                  GAsyncReadyCallback callback,
                  gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    static const guint16        adf_path[] = { 0x3f00, 0x7fff };
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;
    g_autoptr(MMBaseSim)         sim = NULL;

    task = g_task_new (iface, NULL, callback, user_data);
    g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
    if (sim && mm_base_sim_is_esim_without_profiles (sim)) {
        g_task_return_pointer (task, NULL, NULL);
        g_object_unref (task);
        return;
    }
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_read_ef (client, 0x6f40, adf_path, G_N_ELEMENTS (adf_path), NULL,
                           (GAsyncReadyCallback) load_own_numbers_ready, task);
}

/*****************************************************************************/
/* SIM object
 *
 * The plugin's own, so every SIM read goes over SIT instead of AT. Only the
 * active SIM gets one: the other slots are built already-initialized from the
 * slot-status inventory and never call a load vfunc.
 */

static MMBaseSim *
create_sim_finish (MMIfaceModem *self,
                   GAsyncResult *res,
                   GError      **error)
{
    return mm_sim_samsung_sit_new_finish (res, error);
}

static void
create_sim (MMIfaceModem       *self,
            GAsyncReadyCallback callback,
            gpointer            user_data)
{
    mm_sim_samsung_sit_new (MM_BASE_MODEM (self), NULL, callback, user_data);
}

/*****************************************************************************/
/* SIM lock state and retry counters
 *
 * The generic AT path asked AT+CPIN? for the lock state and then spent four
 * AT+CSIM VERIFY/CHANGE APDUs with an empty body to read the retry counters.
 * Both come out of the SIM-status response instead: the lock state from the
 * byte holding the RIL PinState enum -- confirmed against the capture where
 * enabling a PIN, hot-swapping the card and verifying it walked that byte
 * through disabled -> not-verified -> verified, and pulling the card reported
 * unknown -- and the counters from the first application's record.
 */
static MMModemLock
load_unlock_required_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError      **error)
{
    GError *inner = NULL;
    gssize  value;

    value = g_task_propagate_int (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return MM_MODEM_LOCK_UNKNOWN;
    }
    return (MMModemLock) value;
}

static void
load_unlock_required_ready (MMSitClient  *client,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitSimStatus              status;
    MMModemLock                 lock;
    GError                     *error = NULL;

    if (!mm_sit_client_get_sim_status_finish (client, res, &status, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (status.card_state == 0) {
        g_task_return_new_error (task, MM_MOBILE_EQUIPMENT_ERROR,
                                 MM_MOBILE_EQUIPMENT_ERROR_SIM_NOT_INSERTED,
                                 "SIM card is not inserted");
        g_object_unref (task);
        return;
    }

    switch (status.universal_pin_state) {
    case MM_SIT_PIN_STATE_ENABLED_NOT_VERIFIED:
        lock = MM_MODEM_LOCK_SIM_PIN;
        break;
    case MM_SIT_PIN_STATE_ENABLED_BLOCKED:
        lock = MM_MODEM_LOCK_SIM_PUK;
        break;
    case MM_SIT_PIN_STATE_ENABLED_PERM_BLOCKED:
        lock = MM_MODEM_LOCK_PH_SIM_PIN;
        break;
    case MM_SIT_PIN_STATE_ENABLED_VERIFIED:
    case MM_SIT_PIN_STATE_DISABLED:
        lock = MM_MODEM_LOCK_NONE;
        break;
    case MM_SIT_PIN_STATE_UNKNOWN:
    default:
        /* The card is present but its PIN state is not known yet. */
        lock = MM_MODEM_LOCK_UNKNOWN;
        break;
    }

    mm_obj_dbg (self, "SIM lock state: pin state %u -> %s",
                status.universal_pin_state, mm_modem_lock_get_string (lock));
    g_task_return_int (task, lock);
    g_object_unref (task);
}

static void
load_unlock_required (MMIfaceModem       *iface,
                      gboolean            last_attempt,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, cancellable, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_sim_status (client, cancellable,
                                 (GAsyncReadyCallback) load_unlock_required_ready, task);
}

static MMUnlockRetries *
load_unlock_retries_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError      **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_unlock_retries_ready (MMSitClient  *client,
                           GAsyncResult *res,
                           GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    g_autoptr(MMUnlockRetries)  retries = NULL;
    MMSitSimStatus              status;
    const MMSitSimApp          *app;
    GError                     *error = NULL;

    if (!mm_sit_client_get_sim_status_finish (client, res, &status, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!status.app_count) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "the SIM reports no application to count retries for");
        g_object_unref (task);
        return;
    }

    /* The subscription application is the first record on every captured card
     * (the second is PKCS-15, which has no subscriber PIN). */
    app = &status.apps[0];
    retries = mm_unlock_retries_new ();
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PIN,  app->pin1_retries);
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PUK,  app->puk1_retries);
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PIN2, app->pin2_retries);
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PUK2, app->puk2_retries);

    mm_obj_dbg (self, "SIM unlock retries: pin %u, puk %u, pin2 %u, puk2 %u",
                app->pin1_retries, app->puk1_retries,
                app->pin2_retries, app->puk2_retries);
    g_task_return_pointer (task, g_steal_pointer (&retries), g_object_unref);
    g_object_unref (task);
}

static void
load_unlock_retries (MMIfaceModem       *iface,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_sim_status (client, NULL,
                                  (GAsyncReadyCallback) load_unlock_retries_ready, task);
}

/* Only the SIM PIN facility has a known code, so only that one is reported. The
 * generic AT path probed six facilities with AT+CLCK; the others need their codes
 * identified before they can move. */
static MMModem3gppFacility
load_enabled_facility_locks_finish (MMIfaceModem3gpp *self,
                                    GAsyncResult     *res,
                                    GError          **error)
{
    GError *inner = NULL;
    gssize  value;

    value = g_task_propagate_int (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return MM_MODEM_3GPP_FACILITY_NONE;
    }
    return (MMModem3gppFacility) value;
}

static void
load_enabled_facility_locks_ready (MMSitClient  *client,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    g_autoptr(GError)           error = NULL;
    gboolean                    locked = FALSE;

    if (!mm_sit_client_get_facility_lock_finish (client, res, &locked, &error)) {
        mm_obj_dbg (self, "could not read the SIM PIN facility lock: %s", error->message);
        g_task_return_int (task, MM_MODEM_3GPP_FACILITY_NONE);
    } else {
        mm_obj_dbg (self, "SIM PIN facility lock is %s", locked ? "enabled" : "disabled");
        g_task_return_int (task, locked ? MM_MODEM_3GPP_FACILITY_SIM : MM_MODEM_3GPP_FACILITY_NONE);
    }
    g_object_unref (task);
}

static void
load_enabled_facility_locks (MMIfaceModem3gpp   *iface,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_facility_lock (client, MM_SIT_FACILITY_SIM_PIN, NULL,
                                     (GAsyncReadyCallback) load_enabled_facility_locks_ready,
                                     task);
}

static GList *
get_cell_info_finish (MMIfaceModem  *self,
                      GAsyncResult  *res,
                      GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
sit_cell_info_list_free (GList *list)
{
    g_list_free_full (list, g_object_unref);
}

static void
get_cell_info_ready (MMSitClient *client,
                     GAsyncResult *res,
                     GTask        *task)
{
    g_autoptr(GArray) cells = NULL;
    g_autoptr(GArray) nr_cells = NULL;
    GList            *result = NULL;
    GError           *error = NULL;
    guint             i;

    cells = mm_sit_client_get_cell_info_finish (client, res, &nr_cells, &error);
    if (!cells) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    for (i = 0; i < cells->len; i++) {
        const MMSitCellInfoLte *cell;
        MMCellInfoLte          *info;
        g_autofree gchar       *tac = NULL;
        g_autofree gchar       *ci = NULL;
        g_autofree gchar       *pci = NULL;

        cell = &g_array_index (cells, MMSitCellInfoLte, i);
        info = MM_CELL_INFO_LTE (mm_cell_info_lte_new_from_dictionary (NULL));
        if (cell->operator_id[0])
            mm_cell_info_lte_set_operator_id (info, cell->operator_id);
        if (cell->tac != G_MAXINT32) {
            tac = g_strdup_printf ("%X", cell->tac);
            mm_cell_info_lte_set_tac (info, tac);
        }
        if (cell->ci != G_MAXINT32) {
            ci = g_strdup_printf ("%X", cell->ci);
            mm_cell_info_lte_set_ci (info, ci);
        }
        if (cell->physical_ci != G_MAXINT32) {
            pci = g_strdup_printf ("%X", cell->physical_ci);
            mm_cell_info_lte_set_physical_ci (info, pci);
        }
        if (cell->earfcn != G_MAXINT32)
            mm_cell_info_lte_set_earfcn (info, cell->earfcn);
        if (cell->bandwidth != G_MAXINT32)
            mm_cell_info_lte_set_bandwidth (info, cell->bandwidth);
        if (cell->rsrp >= 44 && cell->rsrp <= 140)
            mm_cell_info_lte_set_rsrp (info, -(gdouble) cell->rsrp);
        if (cell->rsrq >= 3 && cell->rsrq <= 34)
            mm_cell_info_lte_set_rsrq (info, -(gdouble) cell->rsrq);
        if (cell->registered &&
            cell->timing_advance >= 0 && cell->timing_advance < G_MAXINT32)
            mm_cell_info_lte_set_timing_advance (info, cell->timing_advance);
        if (cell->registered) {
            mm_cell_info_set_serving (MM_CELL_INFO (info), TRUE);
            mm_cell_info_lte_set_serving_cell_type (info, MM_SERVING_CELL_TYPE_PCELL);
        }
        result = g_list_append (result, info);
    }

    /* NR records only show up once the CP has an NR cell in reach; under EN-DC
     * they accompany the LTE ones rather than replacing them. */
    for (i = 0; nr_cells && i < nr_cells->len; i++) {
        const MMSitCellInfoNr *cell;
        MMCellInfoNr5g        *info;
        g_autofree gchar      *tac = NULL;
        g_autofree gchar      *nci = NULL;
        g_autofree gchar      *pci = NULL;

        cell = &g_array_index (nr_cells, MMSitCellInfoNr, i);
        info = MM_CELL_INFO_NR5G (mm_cell_info_nr5g_new_from_dictionary (NULL));
        if (cell->operator_id[0])
            mm_cell_info_nr5g_set_operator_id (info, cell->operator_id);
        if (cell->tac != G_MAXUINT32) {
            tac = g_strdup_printf ("%X", cell->tac);
            mm_cell_info_nr5g_set_tac (info, tac);
        }
        if (cell->nci) {
            nci = g_strdup_printf ("%" G_GINT64_MODIFIER "X", cell->nci);
            mm_cell_info_nr5g_set_ci (info, nci);
        }
        if (cell->physical_ci != G_MAXUINT32) {
            pci = g_strdup_printf ("%X", cell->physical_ci);
            mm_cell_info_nr5g_set_physical_ci (info, pci);
        }
        if (cell->nrarfcn != G_MAXUINT32)
            mm_cell_info_nr5g_set_nrarfcn (info, cell->nrarfcn);
        /* Same encoding as the LTE records: a positive magnitude in dB(m). */
        if (cell->ss_rsrp >= 44 && cell->ss_rsrp <= 140)
            mm_cell_info_nr5g_set_rsrp (info, -(gdouble) cell->ss_rsrp);
        if (cell->ss_rsrq >= 3 && cell->ss_rsrq <= 43)
            mm_cell_info_nr5g_set_rsrq (info, -(gdouble) cell->ss_rsrq);
        if (cell->ss_sinr != G_MAXINT32)
            mm_cell_info_nr5g_set_sinr (info, (gdouble) cell->ss_sinr);
        if (cell->registered) {
            mm_cell_info_set_serving (MM_CELL_INFO (info), TRUE);
            /* Under EN-DC the NR leg is the secondary cell; LTE keeps the PCell. */
            mm_cell_info_nr5g_set_serving_cell_type (info, MM_SERVING_CELL_TYPE_SCELL);
        }
        result = g_list_append (result, info);
    }

    g_task_return_pointer (task, result, (GDestroyNotify) sit_cell_info_list_free);
    g_object_unref (task);
}

static void
get_cell_info (MMIfaceModem        *iface,
               GAsyncReadyCallback callback,
               gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_cell_info (client, NULL,
                                 (GAsyncReadyCallback) get_cell_info_ready,
                                 task);
}

static MMModemPowerState
load_power_state_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    GError          *inner_error = NULL;
    MMSitRadioState  state;

    state = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_POWER_STATE_UNKNOWN;
    }
    if (state == MM_SIT_RADIO_STATE_ON)
        return MM_MODEM_POWER_STATE_ON;
    if (state == MM_SIT_RADIO_STATE_OFF)
        return MM_MODEM_POWER_STATE_LOW;
    return MM_MODEM_POWER_STATE_UNKNOWN;
}

static void
load_power_state_ready (MMSitClient *client,
                        GAsyncResult *res,
                        GTask *task)
{
    GError          *error = NULL;
    MMSitRadioState  state;

    state = mm_sit_client_get_radio_state_finish (client, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* A freshly booted CP claims the radio is on while it is actually
     * parked (it never starts a PLMN search).  Report low power until this
     * client has driven an explicit radio transition, so the core runs the
     * power-up sequence instead of trusting the hollow "on". */
    if (state == MM_SIT_RADIO_STATE_ON && !mm_sit_client_radio_state_trusted (client))
        state = MM_SIT_RADIO_STATE_OFF;
    g_task_return_int (task, state);
    g_object_unref (task);
}

static void
load_power_state (MMIfaceModem        *iface,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_radio_state (client, NULL,
                                   (GAsyncReadyCallback) load_power_state_ready,
                                   task);
}

static gboolean
modem_set_power_finish (MMIfaceModem  *self,
                        GAsyncResult  *res,
                        GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void power_up_radio (GTask *task);
static void power_up_boot_config_ready (MMSitClient  *client,
                                        GAsyncResult *res,
                                        GTask        *task);

static void
default_attach_apn_ready (MMSitClient  *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBroadbandModemSamsungSit *self;
    g_autoptr(GError)           error = NULL;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    if (!mm_sit_client_set_initial_attach_apn_finish (client, res, &error))
        mm_obj_warn (self, "couldn't set a default initial attach APN: %s", error->message);
    power_up_radio (task);
}

static void
modem_set_power_ready (MMSitClient *client,
                       GAsyncResult *res,
                       GTask *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_radio_state_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* The CP refuses the radio-on until its own init finishes; retry the chain
 * until it accepts. Never through OFF: taking the radio down inside that
 * window leaves the CP not searching at all, and it does not recover. */
#define POWER_UP_RETRY_ATTEMPTS 5
#define POWER_UP_RETRY_SECS     3

/*
 * A CP that comes up with the AP already talking to it takes the radio-on
 * (status 0) and then does nothing with it: GET_DATA_REGISTRATION reads
 * state 0 with rat 0 -- not even a radio technology, so it is not searching.
 * It stays that way indefinitely; only a radio cycle once its own init is
 * done makes it camp (rat 14, then a serving PLMN a few seconds later).
 * That cycle is safe here and only here: it happens a good while after the
 * power-up, not inside the startup window where an OFF wedges the CP.
 */
#define SEARCH_WATCHDOG_SECS    20
#define SEARCH_WATCHDOG_RETRIES 2

static void power_up_chain_start (GTask *task);
static void arm_search_watchdog  (MMBroadbandModemSamsungSit *self);

static void
search_watchdog_radio_off_ready (MMSitClient  *client,
                                 GAsyncResult *res,
                                 GTask        *task)
{
    MMBroadbandModemSamsungSit *self;
    g_autoptr(GError)           error = NULL;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    if (!mm_sit_client_set_radio_state_finish (client, res, &error))
        mm_obj_dbg (self, "couldn't take the radio down before re-sending the bring-up: %s",
                    error->message);
    power_up_chain_start (task);
}

static void
search_watchdog_registration_ready (MMSitClient                *client,
                                    GAsyncResult               *res,
                                    MMBroadbandModemSamsungSit *self)
{
    MMSitRegistration  registration = { 0 };
    g_autoptr(GError)  error = NULL;
    GTask             *task;

    if (!mm_sit_client_get_registration_finish (client, res, &registration, &error)) {
        mm_obj_dbg (self, "couldn't check whether the CP is searching: %s", error->message);
        goto out;
    }
    /* Anything but "no radio technology at all" means the CP is doing
     * something; leave it alone and let registration take its time. */
    if (registration.registered || registration.radio_technology) {
        mm_obj_dbg (self, "CP is searching (rat %u); no radio cycle needed",
                    registration.radio_technology);
        self->priv->search_retries = 0;
        goto out;
    }

    self->priv->search_retries++;
    mm_obj_msg (self, "CP took the radio-on but never started searching; cycling the radio (%u/%u)",
                self->priv->search_retries, SEARCH_WATCHDOG_RETRIES);
    task = g_task_new (self, NULL, NULL, NULL);
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_OFF, NULL,
                                   (GAsyncReadyCallback) search_watchdog_radio_off_ready,
                                   task);

out:
    g_object_unref (self);
}

static gboolean
search_watchdog_expired (MMBroadbandModemSamsungSit *self)
{
    MMSitClient *client;

    self->priv->search_watchdog_id = 0;
    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    if (!client)
        return G_SOURCE_REMOVE;

    mm_sit_client_get_registration (client, NULL,
                                    (GAsyncReadyCallback) search_watchdog_registration_ready,
                                    g_object_ref (self));
    return G_SOURCE_REMOVE;
}

static void
arm_search_watchdog (MMBroadbandModemSamsungSit *self)
{
    if (self->priv->search_watchdog_id)
        g_source_remove (self->priv->search_watchdog_id);
    if (self->priv->search_retries >= SEARCH_WATCHDOG_RETRIES) {
        self->priv->search_watchdog_id = 0;
        return;
    }
    self->priv->search_watchdog_id = g_timeout_add_seconds (SEARCH_WATCHDOG_SECS,
                                                            (GSourceFunc) search_watchdog_expired,
                                                            self);
}

/*
 * A CP that is still coming up refuses SET_RADIO_POWER and keeps reporting the
 * radio as already "on", so the bring-up chain above runs against a CP that
 * took none of it: it attaches with no APN and every SETUP_DATA_CALL then fails
 * with cause 133.  A refusal leaves the state untrusted (see mm-sit-client.c),
 * which is the signal to re-send the whole chain a moment later.
 */
static gboolean
power_up_retry_chain (GTask *task)
{
    power_up_chain_start (task);
    return G_SOURCE_REMOVE;
}

/*
 * Replay one phase of mm_sit_parity_requests[]: the traffic this modem's own
 * Android puts on a control stack before it asks for an IMS PDN, and this
 * plugin did not.
 *
 * It is replayed rather than modelled. Every field of the IMS context and of
 * the whole carrier-configuration exchange was matched to Android byte for
 * byte -- the 983-byte SETUP_DATA_CALL differs in zero bytes -- and the CP
 * still refused every PDN but the one it attached with. What differed was the
 * volume and order of everything around them: 419 requests in Android's first
 * 22 seconds against this plugin's 89. The CP builds its state from that
 * traffic, so the traffic is reproduced instead of guessed at.
 *
 * A refusal is informational. Most of these are advisory settings or plain
 * reads, the CP answers some with "wrong state" depending on where it is in
 * its own init, and none of them is worth failing a power-up over.
 */
static void apply_attach_apn (GTask *task);
static void parity_step (GTask *task, MMSitParityPhase phase);

static GByteArray *
parity_build (MMBroadbandModemSamsungSit *self,
              MMSitClient                *client,
              const MMSitParityRequest   *req,
              GError                    **error)
{
    g_autofree guint8 *body = NULL;
    gsize              len = 4 + req->len;

    body = g_malloc0 (len);
    if (req->payload)
        memcpy (body + 4, req->payload, req->len);

    switch (req->sub) {
    case MM_SIT_PARITY_SUB_MCCMNC: {
        g_autoptr(MMBaseSim)  sim = NULL;
        const gchar          *mccmnc;

        g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
        mccmnc = sim ? mm_gdbus_sim_get_operator_identifier (MM_GDBUS_SIM (sim)) : NULL;
        /* Nothing to name the subscription with yet; the CP gets it next time. */
        if (!mccmnc || !mccmnc[0] || strlen (mccmnc) >= req->len)
            return NULL;
        memset (body + 4, 0, req->len);
        memcpy (body + 4, mccmnc, strlen (mccmnc));
        break;
    }
    case MM_SIT_PARITY_SUB_STACK:
        if (req->len < 1)
            return NULL;
        body[4] = (guint8) mm_sit_client_get_stack (client);
        break;
    case MM_SIT_PARITY_SUB_NONE:
    default:
        break;
    }

    /*
     * The AIMS stack requests name their stack in payload byte 1, 1-based, and
     * the table does not cover it.
     *
     * mm-sit-boot-parity.h is a verbatim replay of one cold boot captured on
     * **one stack**, and its own header says so -- it substitutes the two rows
     * it knew named a subscription (SET_OPEN_CARRIER_INFO's MCCMNC and
     * SET_MOBILE_DATA_STATE's stack) and leaves the rest byte-for-byte. But
     * STACK_START (0x0d39), STACK_STOP (0x0d3a), 0x0d3b and 0x0d3c also carry
     * the stack, so replaying them unmodified sent stack 0's index on both
     * stacks: on ch 0xf6 we were starting, stopping and configuring the IMS
     * stack of the *other* subscription.
     *
     * Confirmed against komodo's own Android, 2026-11-03: across every AP->CP
     * request in the capture, exactly these four ids have payload byte 1 equal
     * to 1 on ch 0xf5 and 2 on ch 0xf6, and no others do. In particular
     * **0x0d0b is 2 on both channels** -- it is not a stack index and must not
     * be touched. Our own wire showed the damage as a mixture, because the IMS
     * sequence in mm-sit-ims.c gets it right (port_index + 1) while the parity
     * replay did not: `01 02` from one path and `01 01` from the other on the
     * same channel.
     *
     * Done here rather than in the table so it survives regeneration.
     */
    switch (req->id) {
    case 0x0d39:
    case 0x0d3a:
    case 0x0d3b:
    case 0x0d3c:
        if (req->len < 2)
            return NULL;
        body[4 + 1] = (guint8) (mm_sit_client_get_stack (client) + 1);
        break;
    default:
        break;
    }

    return mm_sit_request_new (req->id, mm_sit_client_next_token (client),
                               body, len, error);
}

static void
parity_ready (MMSitClient  *client,
              GAsyncResult *res,
              GTask        *task)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    g_autoptr(MMSitMessage)     message = NULL;
    g_autoptr(GError)           error = NULL;
    MMSitParityPhase            phase;

    phase = mm_sit_parity_requests[self->priv->parity_index].phase;
    message = mm_sit_client_command_finish (client, res, &error);
    if (!message)
        mm_obj_dbg (self, "boot parity: 0x%04x refused: %s",
                    mm_sit_parity_requests[self->priv->parity_index].id,
                    error->message);

    self->priv->parity_index++;
    parity_step (task, phase);
}

static void
parity_step (GTask *task, MMSitParityPhase phase)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    g_autoptr(GByteArray)       request = NULL;
    g_autoptr(GError)           error = NULL;
    MMSitClient                *client;

    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    while (client && self->priv->parity_index < G_N_ELEMENTS (mm_sit_parity_requests)) {
        const MMSitParityRequest *req = &mm_sit_parity_requests[self->priv->parity_index];

        if (req->phase != phase) {
            self->priv->parity_index++;
            continue;
        }
        request = parity_build (self, client, req, &error);
        if (request)
            break;
        if (error) {
            mm_obj_dbg (self, "boot parity: couldn't build 0x%04x: %s",
                        req->id, error->message);
            g_clear_error (&error);
        }
        self->priv->parity_index++;
    }

    if (!request) {
        if (phase == MM_SIT_PARITY_PHASE_BOOT) {
            apply_attach_apn (task);
        } else {
            arm_search_watchdog (self);
            g_task_return_boolean (task, TRUE);
            g_object_unref (task);
        }
        return;
    }

    mm_sit_client_command (client, request, 10, NULL,
                           (GAsyncReadyCallback) parity_ready, task);
}

static void
modem_power_up_ready (MMSitClient  *client,
                      GAsyncResult *res,
                      GTask        *task)
{
    MMBroadbandModemSamsungSit *self;
    GError                     *error = NULL;
    guint                       attempts;

    if (!mm_sit_client_set_radio_state_finish (client, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    attempts = GPOINTER_TO_UINT (g_task_get_task_data (task));
    if (!mm_sit_client_radio_state_trusted (client) &&
        attempts + 1 < POWER_UP_RETRY_ATTEMPTS) {
        g_task_set_task_data (task, GUINT_TO_POINTER (attempts + 1), NULL);
        mm_obj_dbg (self, "CP refused the radio-on; re-sending the bring-up in %us (attempt %u)",
                    POWER_UP_RETRY_SECS, attempts + 2);
        g_timeout_add_seconds (POWER_UP_RETRY_SECS, (GSourceFunc) power_up_retry_chain, task);
        return;
    }

    if (!mm_sit_client_radio_state_trusted (client)) {
        mm_obj_warn (self, "CP kept refusing the radio-on; taking its reported state at face value");
        mm_sit_client_assume_radio_state (client);
    }

    /* The radio is on; give the CP the rest of what Android tells it before
     * anything asks for a second PDN. arm_search_watchdog() and the task's
     * result move to the end of that walk. */
    self->priv->parity_index = 0;
    parity_step (task, MM_SIT_PARITY_PHASE_RADIO);
}

static void
power_up_chain_start (GTask *task)
{
    MMBroadbandModemSamsungSit *self;
    MMSitClient                *client;
    GError                     *error = NULL;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_send_boot_config (client, NULL,
                                    (GAsyncReadyCallback) power_up_boot_config_ready,
                                    task);
}

static void
power_up_radio (GTask *task)
{
    MMBroadbandModemSamsungSit *self;
    MMSitClient                *client;
    GError                     *error = NULL;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_ON, NULL,
                                   (GAsyncReadyCallback) modem_power_up_ready,
                                   task);
}

/*
 * The carrier's APN list, for the SIM in this modem.
 *
 * Resolved here rather than cached from ensure_carrier_config() because that
 * builds its identity out of SIM properties it does not own, and copying them
 * to keep them alive buys nothing: this runs once per power-up.
 */
static GPtrArray *
resolve_carrier_apns (MMBroadbandModemSamsungSit *self)
{
    g_autoptr(MMBaseSim)  sim = NULL;
    g_autoptr(GError)     error = NULL;
    g_autofree gchar     *gid1 = NULL;
    g_autofree gchar     *gid2 = NULL;
    MMSitCarrierIdentity  identity = { 0 };
    GPtrArray            *apns;

    if (!self->priv->ims_database)
        return NULL;

    g_object_get (self, MM_IFACE_MODEM_SIM, &sim, NULL);
    if (!sim)
        return NULL;

    identity.imsi = self->priv->carrier_config_imsi;
    identity.iccid = mm_gdbus_sim_get_sim_identifier (MM_GDBUS_SIM (sim));
    identity.mccmnc = mm_gdbus_sim_get_operator_identifier (MM_GDBUS_SIM (sim));
    identity.spn = mm_gdbus_sim_get_operator_name (MM_GDBUS_SIM (sim));
    gid1 = variant_to_hex (mm_gdbus_sim_get_gid1 (MM_GDBUS_SIM (sim)));
    gid2 = variant_to_hex (mm_gdbus_sim_get_gid2 (MM_GDBUS_SIM (sim)));
    identity.gid1 = gid1;
    identity.gid2 = gid2;

    apns = mm_sit_carrier_apns_resolve (self->priv->ims_database, &identity, &error);
    if (!apns)
        mm_obj_dbg (self, "no carrier APN list for this SIM%s%s",
                    error ? ": " : "", error ? error->message : "");
    return apns;
}

static void apply_attach_apn (GTask *task);
static void register_next_apn_profile (GTask *task);

static void
apn_profile_ready (MMSitClient  *client,
                   GAsyncResult *res,
                   GTask        *task)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    g_autoptr(GError)           error = NULL;
    const MMSitApn             *apn = NULL;

    if (self->priv->boot_apns && self->priv->boot_apn_index < self->priv->boot_apns->len)
        apn = g_ptr_array_index (self->priv->boot_apns, self->priv->boot_apn_index);

    /* The CP answers status 2 for a profile it has no use for, and Android's
     * own list draws that answer for several entries -- DUN, emergency, XCAP.
     * So this is informational, never a reason to stop. */
    if (!mm_sit_client_set_data_profile_finish (client, res, &error))
        mm_obj_dbg (self, "data profile '%s' not taken: %s",
                    apn ? apn->apn : "?", error->message);
    else
        mm_obj_dbg (self, "data profile '%s' registered", apn ? apn->apn : "?");

    self->priv->boot_apn_index++;
    register_next_apn_profile (task);
}

/*
 * Register the carrier's whole APN list with the CP, one profile per round
 * trip, before the radio goes on.
 *
 * The CP will not match a URSP rule for an APN it was never given a profile
 * for, and refuses the context with MATCH_ALL_RULE_NOT_ALLOWED (2253). The
 * connect path registers only the profile it is about to use, which is not
 * enough on a network whose policy forbids the catch-all -- the IMS PDN was
 * refused for a month on exactly this. Registering the list and then attaching
 * on the entry marked IA is what Android does, confirmed against this CP on the
 * wire: profiles, attach APN, then the ims context comes up with its P-CSCF
 * addresses.
 */
static void
register_next_apn_profile (GTask *task)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    MMSitApnAuth                auth0 = { 0 };
    const MMSitApn             *apn;
    MMSitClient                *client;

    if (!self->priv->boot_apns || self->priv->boot_apn_index >= self->priv->boot_apns->len) {
        self->priv->parity_index = 0;
        parity_step (task, MM_SIT_PARITY_PHASE_BOOT);
        return;
    }
    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    if (!client) {
        apply_attach_apn (task);
        return;
    }

    apn = g_ptr_array_index (self->priv->boot_apns, self->priv->boot_apn_index);
    mm_sit_client_set_data_profile (client, apn->apn, (MMSitPdpType) apn->protocol,
                                    apn->types, apn->bearers << 1, &auth0, NULL,
                                    (GAsyncReadyCallback) apn_profile_ready, task);
}

static void
attach_apn_from_settings_ready (MMBroadbandModemSamsungSit *self,
                                GAsyncResult               *res,
                                GTask                      *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_broadband_modem_samsung_sit_ensure_carrier_config_finish (self, res, &error)) {
        mm_obj_warn (self, "couldn't provision carrier configuration before radio-on: %s",
                     error->message);
        g_clear_error (&error);
    }

    g_clear_pointer (&self->priv->boot_apns, g_ptr_array_unref);
    self->priv->boot_apns = resolve_carrier_apns (self);
    self->priv->boot_apn_index = 0;
    /* The CP loses its profile table with the radio, so the batch has to run
     * again on the next registration. */

    /* The IMS stack takes its PDN's APN from the same list, so it is resolved
     * once here rather than twice. */
    if (self->priv->ims) {
        const MMSitApn *ims = mm_sit_carrier_apns_find (self->priv->boot_apns,
                                                        MM_SIT_APN_TYPE_IMS);

        if (ims)
            /* The database holds Android's framework encoding, bit (n-1) per
             * radio technology n; the CP's field is bit n. */
            mm_sit_ims_set_apn (self->priv->ims, ims->apn, ims->protocol,
                                ims->bearers << 1);
    }

    register_next_apn_profile (task);
}

static void
apply_attach_apn (GTask *task)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    MMSitClient                *client;
    MMSitApnAuth                auth;
    MMSitPdpType                pdp;
    const gchar                *apn;

    /*
     * The CP needs *an* attach APN or it never attaches, and it will not ask the
     * network for one itself. SetInitialEpsBearerSettings replaces this, so it
     * is a floor and not a policy -- attach_apn_resolve() applies that order.
     *
     * The floor is the carrier's own attach entry rather than empty. An ATTACH
     * REQUEST naming no APN does ask the HSS for the subscribed default, which
     * is enough to carry data -- but the CP then has no context of its own to
     * hang a second PDN off, and the IMS PDN that follows is refused with
     * MATCH_ALL_RULE_NOT_ALLOWED (2253) on a network whose policy forbids the
     * catch-all URSP rule, or with cause 133 where the ESM piggyback failed.
     * Either way there is no VoLTE, which previously had to be worked around by
     * setting gsm.initial-eps-bearer-apn on a NetworkManager profile -- i.e.
     * the user had to add an APN from a terminal before the phone could call.
     *
     * Both stacks of this modem's own Android were captured mid-attach: Telus
     * sends its IA entry 'sp.telus.com' (types 0x117, pdp 3) and Fizz, which
     * has no IA entry, sends its DEFAULT entry 'mobile.bm' (types 0x7, pdp 1)
     * -- the same APN either stack carries data on. AT&T likewise marks no IA,
     * so it attaches on 'enhancedphone' (types 0x27).
     *
     * This must happen here, before radio-on, and not from the IMS sequence:
     * changing the attach APN once the CP has already attached forces a
     * re-attach, and the IMS PDN request then lands mid-flight and is refused
     * with cause 133. It also has to run on every power-up rather than only the
     * first: the CP keeps the APN until told otherwise but loses it with every
     * re-init, and the modem can be re-enabled with the radio already on.
     */
    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    if (!client) {
        power_up_radio (task);
        return;
    }

    apn = attach_apn_resolve (self, &pdp, &auth);
    mm_obj_dbg (self, "applying attach APN '%s' (pdp %u) before radio-on",
                apn, (guint) pdp);
    mm_sit_client_set_initial_attach_apn (client, apn, pdp, attach_pcscf_request (self), &auth, NULL,
                                          (GAsyncReadyCallback) default_attach_apn_ready,
                                          task);
}

static void
power_up_boot_config_ready (MMSitClient  *client,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBroadbandModemSamsungSit *self;
    g_autoptr(GError)           error = NULL;

    self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    if (!mm_sit_client_send_boot_config_finish (client, res, &error))
        mm_obj_warn (self, "couldn't send boot configuration before radio-on: %s",
                     error->message);
    mm_broadband_modem_samsung_sit_ensure_carrier_config (
        self, (GAsyncReadyCallback) attach_apn_from_settings_ready, task);
}

static void
modem_set_power (MMIfaceModem        *iface,
                 MMSitRadioState      state,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* The CP parks a radio-on that arrives before its boot configuration
     * and carrier manifest (radio reports "on" but never searches), so
     * mirror the downstream bring-up order before requesting the radio. */
    if (state == MM_SIT_RADIO_STATE_ON) {
        power_up_chain_start (task);
        return;
    }
    mm_sit_client_set_radio_state (client, state, NULL,
                                   (GAsyncReadyCallback) modem_set_power_ready,
                                   task);
}

static void
modem_power_up (MMIfaceModem        *self,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    modem_set_power (self, MM_SIT_RADIO_STATE_ON, callback, user_data);
}

static void
modem_power_down (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    modem_set_power (self, MM_SIT_RADIO_STATE_OFF, callback, user_data);
}

/*****************************************************************************/
/* SIM slots */

/* Right after CP boot the slot inventory is published before the CP maps a
 * port to logical stack 0; the mapping lands a few seconds later (announced
 * with SIM_SLOT_STATUS_CHANGED, which we do not consume during init). */
#define LOAD_SIM_SLOTS_ATTEMPTS   10
#define LOAD_SIM_SLOTS_RETRY_SECS 1

typedef struct {
    GPtrArray *sim_slots;
    guint      primary_sim_slot;
    guint      attempts_left;
} LoadSimSlotsContext;

static void
sim_slot_free (MMBaseSim *sim)
{
    if (sim)
        g_object_unref (sim);
}

static void
load_sim_slots_context_free (LoadSimSlotsContext *ctx)
{
    g_clear_pointer (&ctx->sim_slots, g_ptr_array_unref);
    g_free (ctx);
}

static gboolean
load_sim_slots_finish (MMIfaceModem  *self,
                       GAsyncResult  *res,
                       GPtrArray    **sim_slots,
                       guint         *primary_sim_slot,
                       GError       **error)
{
    LoadSimSlotsContext *ctx;

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;

    ctx = g_task_get_task_data (G_TASK (res));
    if (sim_slots)
        *sim_slots = g_steal_pointer (&ctx->sim_slots);
    if (primary_sim_slot)
        *primary_sim_slot = ctx->primary_sim_slot;
    return TRUE;
}

static void load_sim_slots_ready (MMSitClient *client,
                                  GAsyncResult *res,
                                  GTask        *task);

static gboolean
load_sim_slots_retry (GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitClient                *client;
    GError                     *error = NULL;

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }
    mm_sit_client_get_sim_slot_status (client, NULL,
                                       (GAsyncReadyCallback) load_sim_slots_ready,
                                       task);
    return G_SOURCE_REMOVE;
}

static void
load_sim_slots_ready (MMSitClient *client,
                      GAsyncResult *res,
                      GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    LoadSimSlotsContext        *ctx = g_task_get_task_data (task);
    MMSitSimSlotStatus          status;
    GError                     *error = NULL;
    guint                       i;

    if (!mm_sit_client_get_sim_slot_status_finish (client, res, &status, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->sim_slots = g_ptr_array_new_full (status.slot_count, (GDestroyNotify) sim_slot_free);
    for (i = 0; i < status.slot_count; i++) {
        const MMSitSimSlot *slot = &status.slots[i];
        const MMSitSimPort *primary_port = NULL;
        const MMSitSimPort *active_port = NULL;
        const MMSitSimPort *fallback_port = NULL;
        const MMSitSimPort *identity_port;
        g_autofree gchar   *raw_iccid = NULL;
        g_autofree gchar   *iccid = NULL;
        g_autofree gchar   *eid = NULL;
        g_autoptr(GError)   inner_error = NULL;
        gboolean            active = FALSE;
        guint               j;

        for (j = 0; j < slot->port_count; j++) {
            const MMSitSimPort *port = &slot->ports[j];

            if (port->logical_slot_id == (guint) MAX (self->priv->logical_stack, 0)) {
                ctx->primary_sim_slot = self->priv->logical_stack >= 0 ? 1 : i + 1;
                primary_port = port;
            }
            if (!active_port && port->state == 1)
                active_port = port;
            if (!fallback_port && port->iccid_len)
                fallback_port = port;
        }
        /* Only the physical slot mapped to logical stack 0 is the active
         * one in ModemManager's slot model; a secondary or stale active
         * port must not mark the non-primary SIM as active. */
        active = primary_port != NULL;
        if (self->priv->logical_stack >= 0 && !active)
            continue;
        identity_port = (primary_port && primary_port->state == 1) ? primary_port :
                        active_port ? active_port :
                        (primary_port && primary_port->iccid_len) ? primary_port :
                        fallback_port;

        if (slot->card_state != 1) {
            g_ptr_array_add (ctx->sim_slots, NULL);
            continue;
        }

        if (identity_port && identity_port->iccid_len) {
            raw_iccid = mm_utils_bin2hexstr (identity_port->iccid,
                                             identity_port->iccid_len);
            if (raw_iccid)
                iccid = mm_3gpp_parse_iccid (raw_iccid, &inner_error);
            if (!iccid)
                mm_obj_warn (self, "couldn't decode ICCID for physical SIM slot %u: %s",
                             i + 1, inner_error ? inner_error->message : "conversion failed");
        }
        if (slot->eid_len)
            eid = mm_decode_eid ((const gchar *) slot->eid, slot->eid_len);

        /* The active slot gets the plugin's own SIM so its remaining
         * properties are read over SIT; an inactive slot is never read from,
         * everything it shows comes from this inventory. */
        g_ptr_array_add (ctx->sim_slots,
                         active ?
                         mm_sim_samsung_sit_new_initialized (MM_BASE_MODEM (self),
                                                             G_OBJECT (self),
                                                             self->priv->logical_stack >= 0 ? 1 : i + 1,
                                                             active,
                                                             iccid,
                                                             eid) :
                         mm_base_sim_new_initialized (MM_BASE_MODEM (self),
                                                      G_OBJECT (self),
                                                      i + 1,
                                                      active,
                                                      iccid,
                                                      NULL,
                                                      eid,
                                                      NULL,
                                                      NULL,
                                                      NULL));
    }

    if (!ctx->primary_sim_slot) {
        if (ctx->attempts_left > 0) {
            ctx->attempts_left--;
            g_clear_pointer (&ctx->sim_slots, g_ptr_array_unref);
            mm_obj_dbg (self, "SIT slot status has no mapping for logical stack %d yet; retrying",
                        MAX (self->priv->logical_stack, 0));
            g_timeout_add_seconds (LOAD_SIM_SLOTS_RETRY_SECS,
                                   (GSourceFunc) load_sim_slots_retry, task);
            return;
        }
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "SIT slot status has no mapping for logical stack %d",
                                 MAX (self->priv->logical_stack, 0));
        g_object_unref (task);
        return;
    }

    mm_obj_info (self, "loaded %u SIM slots; slot %u maps to logical stack %d",
                 ctx->sim_slots->len, ctx->primary_sim_slot, MAX (self->priv->logical_stack, 0));
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
load_sim_slots (MMIfaceModem        *iface,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    LoadSimSlotsContext        *ctx;
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx = g_new0 (LoadSimSlotsContext, 1);
    ctx->attempts_left = LOAD_SIM_SLOTS_ATTEMPTS;
    g_task_set_task_data (task, ctx, (GDestroyNotify) load_sim_slots_context_free);
    mm_sit_client_get_sim_slot_status (client, NULL,
                                       (GAsyncReadyCallback) load_sim_slots_ready,
                                       task);
}

static gboolean
set_primary_sim_slot_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/* The CP acknowledges SIM_SET_LOGICAL_SLOT_PORT_MAPPING before it re-runs SIM
 * initialization on the new mapping; the updated table only becomes visible in
 * SIM_GET_SLOT_STATUS once that completes.  Returning success right away made
 * the core's reprobe read the old mapping, so poll the inventory until the
 * requested slot actually owns logical stack 0. */
#define SET_SLOT_VERIFY_ATTEMPTS 20
#define SET_SLOT_VERIFY_MS       500

typedef struct {
    guint requested_slot;
    guint verify_attempts_left;
} SetPrimarySimSlotContext;

static void set_primary_sim_slot_verify_ready (MMSitClient  *client,
                                               GAsyncResult *res,
                                               GTask        *task);

static gboolean
set_primary_sim_slot_verify (GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitClient                *client;
    GError                     *error = NULL;

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return G_SOURCE_REMOVE;
    }
    mm_sit_client_get_sim_slot_status (client, NULL,
                                       (GAsyncReadyCallback) set_primary_sim_slot_verify_ready,
                                       task);
    return G_SOURCE_REMOVE;
}

static void
set_primary_sim_slot_verify_ready (MMSitClient  *client,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    SetPrimarySimSlotContext   *ctx = g_task_get_task_data (task);
    MMSitSimSlotStatus          status;
    g_autoptr(GError)           error = NULL;

    if (mm_sit_client_get_sim_slot_status_finish (client, res, &status, &error) &&
        ctx->requested_slot <= status.slot_count) {
        const MMSitSimSlot *slot = &status.slots[ctx->requested_slot - 1];
        guint               j;

        for (j = 0; j < slot->port_count; j++) {
            if (slot->ports[j].logical_slot_id == 0) {
                mm_obj_info (self, "physical SIM slot %u now maps to logical stack 0",
                             ctx->requested_slot);
                g_task_return_boolean (task, TRUE);
                g_object_unref (task);
                return;
            }
        }
    }

    if (ctx->verify_attempts_left == 0) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "SIT slot mapping change was accepted but has not been applied");
        g_object_unref (task);
        return;
    }
    ctx->verify_attempts_left--;
    g_timeout_add (SET_SLOT_VERIFY_MS, (GSourceFunc) set_primary_sim_slot_verify, task);
}

static void
set_primary_sim_slot_mapping_ready (MMSitClient *client,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_logical_slot_mapping_finish (client, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    set_primary_sim_slot_verify (task);
}

static void
set_primary_sim_slot_status_ready (MMSitClient *client,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    SetPrimarySimSlotContext *ctx = g_task_get_task_data (task);
    MMSitSimSlotStatus      status;
    MMSitLogicalSlotMapping mapping;
    GError                 *error = NULL;
    guint                   requested_slot = ctx->requested_slot;
    guint                   selected_port = 0;
    gboolean                selected_port_found = FALSE;
    guint                   i;
    guint                   j;

    if (!mm_sit_client_get_sim_slot_status_finish (client, res, &status, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!requested_slot || requested_slot > status.slot_count) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "Physical SIM slot %u is out of range", requested_slot);
        g_object_unref (task);
        return;
    }

    for (i = 0; i < status.slot_count; i++) {
        const MMSitSimSlot *slot = &status.slots[i];

        for (j = 0; j < slot->port_count; j++) {
            const MMSitSimPort *port = &slot->ports[j];

            if (port->logical_slot_id == 0 && i + 1 == requested_slot) {
                g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_EXISTS,
                                         "Physical SIM slot %u already maps to logical stack 0",
                                         requested_slot);
                g_object_unref (task);
                return;
            }
            if (port->logical_slot_id != 0 && port->logical_slot_id != 0xff) {
                g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                         "Cannot switch the primary slot while logical stack %u is mapped",
                                         port->logical_slot_id);
                g_object_unref (task);
                return;
            }
        }
    }

    if (status.slots[requested_slot - 1].card_state != 1) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "No SIM is present in physical slot %u", requested_slot);
        g_object_unref (task);
        return;
    }

    for (j = 0; j < status.slots[requested_slot - 1].port_count; j++) {
        const MMSitSimPort *port = &status.slots[requested_slot - 1].ports[j];

        if (port->state == 1) {
            selected_port = j;
            selected_port_found = TRUE;
            break;
        }
    }
    for (j = 0;
         !selected_port_found && j < status.slots[requested_slot - 1].port_count;
         j++) {
        if (status.slots[requested_slot - 1].ports[j].iccid_len) {
            selected_port = j;
            selected_port_found = TRUE;
        }
    }
    if (!selected_port_found && status.slots[requested_slot - 1].port_count)
        selected_port_found = TRUE;
    if (!selected_port_found) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "Physical SIM slot %u has no ports", requested_slot);
        g_object_unref (task);
        return;
    }

    mapping.physical_slot_id = requested_slot - 1;
    mapping.port_id = selected_port;
    mm_sit_client_set_logical_slot_mapping (client, &mapping, 1, NULL,
                                            (GAsyncReadyCallback) set_primary_sim_slot_mapping_ready,
                                            task);
}

static void
set_primary_sim_slot (MMIfaceModem        *iface,
                      guint                sim_slot,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    SetPrimarySimSlotContext   *ctx;
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    if (self->priv->logical_stack >= 0) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "DSDS exposes one modem per SIM; select the other modem instead of remapping slots");
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx = g_new0 (SetPrimarySimSlotContext, 1);
    ctx->requested_slot = sim_slot;
    ctx->verify_attempts_left = SET_SLOT_VERIFY_ATTEMPTS;
    g_task_set_task_data (task, ctx, g_free);
    mm_sit_client_get_sim_slot_status (client, NULL,
                                       (GAsyncReadyCallback) set_primary_sim_slot_status_ready,
                                       task);
}

static MMBaseBearer *
create_bearer_finish (MMIfaceModem  *self,
                      GAsyncResult  *res,
                      GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
create_bearer (MMIfaceModem        *iface,
               MMBearerProperties *properties,
               GAsyncReadyCallback callback,
               gpointer            user_data)
{
    MMBaseBearer *bearer;
    GTask        *task;

    bearer = mm_bearer_samsung_sit_new (MM_BROADBAND_MODEM_SAMSUNG_SIT (iface),
                                        properties);
    task = g_task_new (iface, NULL, callback, user_data);
    g_task_return_pointer (task, bearer, g_object_unref);
    g_object_unref (task);
}

/*****************************************************************************/
/* 3GPP interface */

/* The same value the Modem interface publishes as the equipment identifier,
 * from the same 0x0401 read; the two properties are separate on D-Bus and both
 * would otherwise fall back to +CGSN. */
static gchar *
modem_3gpp_load_imei_finish (MMIfaceModem3gpp  *self,
                             GAsyncResult      *res,
                             GError           **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
modem_3gpp_load_imei (MMIfaceModem3gpp    *iface,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_imei (client, NULL, (GAsyncReadyCallback) load_imei_ready, task);
}

static gboolean
registration_checks_finish (MMIfaceModem3gpp  *self,
                            GAsyncResult      *res,
                            GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
voice_registration_ready (MMSitClient  *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitRegistration           registration;
    g_autoptr(GError)           error = NULL;

    /* A CS query that fails is not fatal: the packet-switched state has already
     * been applied and is what the core needs to connect. */
    if (mm_sit_client_get_voice_registration_finish (client, res, &registration, &error))
        update_cs_registration (self, &registration);
    else
        mm_obj_dbg (self, "CS registration check failed: %s", error->message);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
registration_ready (MMSitClient *client,
                    GAsyncResult *res,
                    GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitRegistration           registration;
    GError                     *error = NULL;

    if (!mm_sit_client_get_registration_finish (client, res, &registration, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    update_ps_registration (self, &registration);

    if (!GPOINTER_TO_UINT (g_task_get_task_data (task))) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_voice_registration (client, NULL,
                                          (GAsyncReadyCallback) voice_registration_ready, task);
}

static void
registration_checks (MMIfaceModem3gpp   *iface,
                     gboolean            is_cs_supported,
                     gboolean            is_ps_supported,
                     gboolean            is_eps_supported,
                     gboolean            is_5gs_supported,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task, GUINT_TO_POINTER (is_cs_supported), NULL);
    mm_sit_client_get_registration (client, NULL,
                                    (GAsyncReadyCallback) registration_ready, task);
}

static gchar *
load_operator_code_finish (MMIfaceModem3gpp  *self,
                           GAsyncResult      *res,
                           GError           **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_operator_code_ready (MMSitClient *client,
                          GAsyncResult *res,
                          GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitOperator               operator_info;
    GError                     *error = NULL;

    if (!mm_sit_client_get_operator_finish (client, res, &operator_info, &error))
        g_task_return_error (task, error);
    else if (!operator_info.plmn[0])
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "SIT operator response did not include a PLMN");
    else {
        update_operator (self, &operator_info);
        g_task_return_pointer (task, g_strdup (operator_info.plmn), g_free);
    }
    g_object_unref (task);
}

static void
load_operator_code (MMIfaceModem3gpp   *iface,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    if (self->priv->operator_code) {
        g_task_return_pointer (task, g_strdup (self->priv->operator_code), g_free);
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_operator (client, NULL,
                                (GAsyncReadyCallback) load_operator_code_ready, task);
}

static gchar *
load_operator_name_finish (MMIfaceModem3gpp  *self,
                           GAsyncResult      *res,
                           GError           **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_operator_name_ready (MMSitClient *client,
                          GAsyncResult *res,
                          GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitOperator               operator_info;
    GError                     *error = NULL;

    if (!mm_sit_client_get_operator_finish (client, res, &operator_info, &error))
        g_task_return_error (task, error);
    else if (!operator_info.plmn[0])
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "SIT operator response did not include a PLMN");
    else {
        update_operator (self, &operator_info);
        if (self->priv->operator_name)
            g_task_return_pointer (task, g_strdup (self->priv->operator_name), g_free);
        else
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                     "SIT operator response did not include a name");
    }
    g_object_unref (task);
}

static void
load_operator_name (MMIfaceModem3gpp   *iface,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    if (self->priv->operator_name) {
        g_task_return_pointer (task, g_strdup (self->priv->operator_name), g_free);
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_operator (client, NULL,
                                (GAsyncReadyCallback) load_operator_name_ready, task);
}

static gboolean
register_in_network_finish (MMIfaceModem3gpp  *self,
                            GAsyncResult      *res,
                            GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void register_in_network_ready (MMSitClient  *client,
                                       GAsyncResult *res,
                                       GTask        *task);

/* The CP already picks a network on its own; only pin it when it says it is in
 * manual mode. Registration itself is reported by the SERVING_NETWORK
 * indication and the registration polls the core already runs, so returning
 * success here without sending anything loses nothing. */
static void
register_selection_mode_ready (MMSitClient  *client,
                               GAsyncResult *res,
                               GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitNetworkSelectionMode   mode;
    g_autoptr(GError)           error = NULL;

    if (!mm_sit_client_get_network_selection_mode_finish (client, res, &mode, &error)) {
        /* Not knowing is not a reason to fail the registration; fall back to
         * asking, which is what this did unconditionally before. */
        mm_obj_dbg (self, "couldn't read the network selection mode (%s); asking for automatic anyway",
                    error->message);
        mode = MM_SIT_NETWORK_SELECTION_MANUAL;
    }

    if (mode == MM_SIT_NETWORK_SELECTION_AUTOMATIC) {
        mm_obj_dbg (self, "the CP is already selecting a network automatically; letting it search");
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "asking the CP to select a network automatically");
    mm_sit_client_set_network_selection_auto (client, g_task_get_cancellable (task),
                                              (GAsyncReadyCallback) register_in_network_ready,
                                              task);
}

static void
register_in_network_ready (MMSitClient  *client,
                           GAsyncResult *res,
                           GTask        *task)
{
    gboolean manual = GPOINTER_TO_UINT (g_task_get_task_data (task));
    GError  *error = NULL;
    gboolean ok;

    ok = manual ?
        mm_sit_client_set_network_selection_manual_finish (client, res, &error) :
        mm_sit_client_set_network_selection_auto_finish (client, res, &error);
    if (!ok)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*
 * The CP only starts an automatic search of its own accord when radio power is
 * first enabled. If it later drops out of service -- a failed reselection, an
 * attach reject -- it stays put, and this used to return success without sending
 * anything, so nothing ever asked it to look again and the modem stayed
 * unregistered until it was power-cycled. Drive the search explicitly instead,
 * the way downstream does.
 *
 * Manual selection needs the RAT to try alongside the PLMN; the currently
 * reported access technology is the best available guess, and LTE is the
 * fallback since it is the only RAT this modem is configured for.
 */
static void
register_in_network (MMIfaceModem3gpp   *self,
                     const gchar        *operator_id,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    MMSitClient *client;
    GError      *error = NULL;
    GTask       *task;
    gboolean     manual;

    task = g_task_new (self, cancellable, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                                                        &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    manual = (operator_id && operator_id[0]);
    g_task_set_task_data (task, GUINT_TO_POINTER (manual), NULL);

    if (manual) {
        mm_obj_dbg (self, "selecting network '%s' manually", operator_id);
        mm_sit_client_set_network_selection_manual (client, operator_id,
                                                   MM_SIT_REGISTRATION_RAT_LTE, cancellable,
                                                   (GAsyncReadyCallback) register_in_network_ready,
                                                   task);
        return;
    }

    /* Ask what the CP is doing before telling it to do it. It selects
     * automatically out of reset and starts searching the moment radio power
     * comes up, so an automatic-selection request is usually redundant -- and
     * it is not free: the ack lands when the search *finishes*, and the port
     * carries nothing else until then. */
    mm_sit_client_get_network_selection_mode (client, cancellable,
                                              (GAsyncReadyCallback) register_selection_mode_ready,
                                              task);
}

static GList *
scan_networks_finish (MMIfaceModem3gpp *self,
                      GAsyncResult     *res,
                      GError          **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

/* EF_FPLMN (TS 31.102 4.2.16): the forbidden-PLMN list the card keeps, three
 * packed bytes per entry, unused slots all ones.  This is how a phone knows a
 * network will refuse it without trying -- the network rewrites the card's list
 * when it rejects a registration. */
#define EF_FPLMN        0x6f7b
#define FPLMN_ENTRY_LEN 3

static void start_network_scan (GTask *task);

static gboolean
plmn_is_forbidden (MMBroadbandModemSamsungSit *self,
                   const gchar                *plmn)
{
    guint i;

    if (!self->priv->forbidden_plmns)
        return FALSE;
    for (i = 0; i < self->priv->forbidden_plmns->len; i++) {
        if (g_strcmp0 (g_ptr_array_index (self->priv->forbidden_plmns, i), plmn) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
load_forbidden_plmns_ready (MMSitClient  *client,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    g_autoptr(GBytes)           data = NULL;
    g_autoptr(GError)           error = NULL;
    const guint8               *bytes;
    gsize                       len = 0;
    gsize                       offset;

    data = mm_sit_client_read_ef_finish (client, res, NULL, &error);
    if (!data) {
        /* Not fatal: without the list a scan simply cannot mark anything
         * forbidden, which is where we were before. */
        mm_obj_dbg (self, "couldn't read the forbidden-PLMN list: %s", error->message);
    } else {
        g_clear_pointer (&self->priv->forbidden_plmns, g_ptr_array_unref);
        self->priv->forbidden_plmns = g_ptr_array_new_with_free_func (g_free);
        bytes = g_bytes_get_data (data, &len);
        for (offset = 0; offset + FPLMN_ENTRY_LEN <= len; offset += FPLMN_ENTRY_LEN) {
            gchar *plmn = mm_sit_plmn_from_packed (&bytes[offset]);

            if (plmn)
                g_ptr_array_add (self->priv->forbidden_plmns, plmn);
        }
        mm_obj_dbg (self, "%u forbidden PLMN(s) on the card",
                    self->priv->forbidden_plmns->len);
    }

    start_network_scan (task);
}

static void
scan_networks_ready (MMSitClient  *client,
                     GAsyncResult *res,
                     GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    g_autoptr(GArray)           networks = NULL;
    g_autoptr(GHashTable)       seen = NULL;
    GError                     *error = NULL;
    GList                      *results = NULL;
    guint                       i;

    networks = mm_sit_client_scan_networks_finish (client, res, &error);
    if (!networks) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* The CP lists one record per cell, so an operator repeats once per cell of
     * it that was found; the D-Bus scan result wants one entry per operator. */
    seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (i = 0; i < networks->len; i++) {
        const MMSitScannedNetwork *network = &g_array_index (networks, MMSitScannedNetwork, i);
        MM3gppNetworkInfo         *info;
        MMModemAccessTechnology    access_tech;
        g_autofree gchar          *key = NULL;

        if (!network->plmn[0])
            continue;

        access_tech = network->rat == MM_SIT_SCAN_RAT_LTE ?
            MM_MODEM_ACCESS_TECHNOLOGY_LTE : MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        key = g_strdup_printf ("%s/%u", network->plmn, (guint) access_tech);

        info = g_hash_table_lookup (seen, key);
        if (info) {
            /* Any cell of the serving operator makes the whole entry current. */
            if (network->serving)
                info->status = MM_MODEM_3GPP_NETWORK_AVAILABILITY_CURRENT;
            continue;
        }

        info = g_new0 (MM3gppNetworkInfo, 1);
        info->operator_code = g_strdup (network->plmn);
        if (network->serving)
            info->status = MM_MODEM_3GPP_NETWORK_AVAILABILITY_CURRENT;
        else if (plmn_is_forbidden (self, network->plmn))
            info->status = MM_MODEM_3GPP_NETWORK_AVAILABILITY_FORBIDDEN;
        else
            info->status = MM_MODEM_3GPP_NETWORK_AVAILABILITY_AVAILABLE;
        info->access_tech = access_tech;
        /*
         * Scan records carry no operator names -- the CP leaves the whole 316
         * byte record blank apart from the PLMN -- and nothing else here can
         * name a *foreign* network: the card's own EF_PNN/EF_OPL describe the
         * home operator, and a numeric lookup in the provider database is
         * ambiguous because MVNOs share a network code (five providers list
         * 26203).  So fall back to the code itself, which at least gives the UI
         * something to show instead of a blank row.  The serving network is the
         * one name we have on authority, from GET_OPERATOR.
         */
        if (network->serving && self->priv->operator_name &&
            g_strcmp0 (self->priv->operator_code, network->plmn) == 0)
            info->operator_long = g_strdup (self->priv->operator_name);
        else
            info->operator_long = g_strdup (network->plmn);

        g_hash_table_insert (seen, g_steal_pointer (&key), info);
        results = g_list_prepend (results, info);
    }

    mm_obj_dbg (self, "network scan: %u cell record(s) -> %u operator(s)",
                networks->len, g_hash_table_size (seen));
    g_task_return_pointer (task, g_list_reverse (results),
                           (GDestroyNotify) mm_3gpp_network_info_list_free);
    g_object_unref (task);
}

/* The CP scans natively, so nothing here needs the AT port. Results carry the
 * PLMN and which one the CP is camped on, but no operator names. */
static void
start_network_scan (GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitClient                *client;
    GError                     *error = NULL;

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_scan_networks (client, NULL,
                                 (GAsyncReadyCallback) scan_networks_ready, task);
}

static void
scan_networks (MMIfaceModem3gpp   *self,
               GAsyncReadyCallback callback,
               gpointer            user_data)
{
    MMSitClient          *client;
    GError               *error = NULL;
    GTask                *task;
    static const guint16  adf_path[] = { 0x3f00, 0x7fff };

    task = g_task_new (self, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                                                        &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Refresh the forbidden list first: the network rewrites it on the card
     * when it rejects a registration, so a stale one would mislabel exactly the
     * network the user is about to try. */
    mm_sit_client_read_ef (client, EF_FPLMN, adf_path, G_N_ELEMENTS (adf_path), NULL,
                           (GAsyncReadyCallback) load_forbidden_plmns_ready, task);
}

static void
set_screen_state_ready (MMSitClient *client,
                        GAsyncResult *res,
                        GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_screen_state_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_indication_filter_ready (MMSitClient *client,
                             GAsyncResult *res,
                             GTask *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_indication_filter_finish (client, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /*
     * Android pairs the indication filter with the display state and reports
     * the screen ON: `01 00 00 00` on both stacks, every time. We reported OFF.
     *
     * That one byte is also why we polled: with the screen reported off the CP
     * withholds unsolicited measurement indications, so the plugin fell back to
     * explicit queries. Measured on identical firmware (Android B-16035863 vs
     * pmOS r32, per-boot AP->CP request counts):
     *
     *     GET_SIGNAL_STRENGTH      android 1    pmOS 58
     *     GET_VOICE_REGISTRATION   android 15   pmOS 62
     *     GET_DATA_REGISTRATION    android 17   pmOS 34
     *
     * So report ON, like stock, and let the CP push 0x0906/0x070e instead --
     * the plugin already consumes both. ModemManager still has no display-state
     * integration; this is a constant, which is exactly what stock sends.
     */
    mm_sit_client_set_screen_state (client, TRUE, NULL,
                                    (GAsyncReadyCallback) set_screen_state_ready,
                                    task);
}

static void
set_unsolicited_events (MMIfaceModem3gpp   *iface,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_set_indication_filter (client, MM_SIT_INDICATION_FILTER_ALL, NULL,
                                         (GAsyncReadyCallback) set_indication_filter_ready,
                                         task);
}

static gboolean
set_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                               GAsyncResult      *res,
                               GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
clear_unsolicited_events (MMIfaceModem3gpp   *self,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    MMBroadbandModemSamsungSit *sit = MM_BROADBAND_MODEM_SAMSUNG_SIT (self);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (self, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (sit, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Android keeps registration-failure reporting enabled with the screen
     * off. Calls, SMS, SIM changes and network time are outside this mask and
     * remain available as wake events. Keep the control port open so active
     * data calls survive quick suspend. */
    mm_sit_client_set_indication_filter (
        client, MM_SIT_INDICATION_FILTER_REGISTRATION_FAILURE, NULL,
        (GAsyncReadyCallback) set_indication_filter_ready, task);
}

/*
 * Registration state comes from SIT alone. Leaving the generic AT handlers in
 * place had them enable +CGREG/+CEREG/+C5GREG on the AT port and feed whatever
 * arrived straight into the core, in parallel with -- and overriding -- the SIT
 * path. This CP delivers those URCs in bursts of several values at once with
 * byte-swapped LAC/TAC fields, so the last line of a burst won, and a burst
 * ending in "registration denied" left the core stuck in a state it treats as
 * terminal even though the CP went on to camp normally. Never subscribe to them.
 */
static void
enable_registration_events_noop (MMIfaceModem3gpp   *self,
                          gboolean            cs_supported,
                          gboolean            ps_supported,
                          gboolean            eps_supported,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
enable_registration_events_noop_finish (MMIfaceModem3gpp  *self,
                                 GAsyncResult      *res,
                                 GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
setup_registration_events_noop (MMIfaceModem3gpp   *self,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
setup_registration_events_noop_finish (MMIfaceModem3gpp  *self,
                                       GAsyncResult      *res,
                                       GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static gboolean
clear_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                 GAsyncResult      *res,
                                 GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*****************************************************************************/
/* Allowed radio generations (modes)
 *
 * The generic AT path used to handle this, badly: this CP answers AT+WS46=? with
 * a malformed list ("+WS46: 12, 22, 25, 28, 29, 30, 31)" -- a closing paren, no
 * opening one), MM's parser rejected it, and the fallback left the modem
 * advertising 4G as its only supported mode. The CP has a native bitmap for
 * exactly this.
 */

static MMModemMode
generations_to_modes (const MMSitNetworkGenerations *generations)
{
    MMModemMode modes = MM_MODEM_MODE_NONE;

    if (generations->gsm)
        modes |= MM_MODEM_MODE_2G;
    if (generations->umts)
        modes |= MM_MODEM_MODE_3G;
    if (generations->lte)
        modes |= MM_MODEM_MODE_4G;
    if (generations->nr)
        modes |= MM_MODEM_MODE_5G;
    return modes;
}

static void
modes_to_generations (MMModemMode              modes,
                      MMSitNetworkGenerations *generations)
{
    generations->gsm  = !!(modes & MM_MODEM_MODE_2G);
    generations->umts = !!(modes & MM_MODEM_MODE_3G);
    generations->lte  = !!(modes & MM_MODEM_MODE_4G);
    generations->nr   = !!(modes & MM_MODEM_MODE_5G);
}

static GArray *
load_supported_modes_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError      **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

/* Every combination the bitmap can express, since its four generation groups are
 * independent. No preferred mode: the bitmap says what the CP may use, not what
 * it should favour, and nothing in the corpus expresses a preference. */
static void
supported_modes_ready (MMBroadbandModemSamsungSit *self,
                       GAsyncResult               *res,
                       GTask                      *task)
{
    const MMSitNetworkGenerations *g = &self->priv->radio_capability;
    GArray                        *combinations;
    MMModemModeCombination         combination;
    MMModemMode                    all = MM_MODEM_MODE_NONE;
    guint                          mask;

    /* Every subset of what the modem has. 0x074f takes any of them, since the
     * allowed bitmap masks generations independently. */
    if (g->gsm)
        all |= MM_MODEM_MODE_2G;
    if (g->umts)
        all |= MM_MODEM_MODE_3G;
    if (g->lte)
        all |= MM_MODEM_MODE_4G;
    if (g->nr)
        all |= MM_MODEM_MODE_5G;

    combinations = g_array_sized_new (FALSE, FALSE, sizeof (MMModemModeCombination), 15);
    for (mask = 1; mask <= (guint) all; mask++) {
        if (mask & ~((guint) all))
            continue;
        combination.allowed = (MMModemMode) mask;
        combination.preferred = MM_MODEM_MODE_NONE;
        g_array_append_val (combinations, combination);
    }

    g_task_return_pointer (task, combinations, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_supported_modes (MMIfaceModem       *self,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    ensure_radio_capability (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                             (GAsyncReadyCallback) supported_modes_ready,
                             g_task_new (self, NULL, callback, user_data));
}

static MMBearerIpFamily
load_supported_ip_families_finish (MMIfaceModem *self,
                                   GAsyncResult *res,
                                   GError      **error)
{
    GError *inner = NULL;
    gssize  value;

    value = g_task_propagate_int (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return MM_BEARER_IP_FAMILY_NONE;
    }
    return (MMBearerIpFamily) value;
}

/* The CP takes a PDP type per context and answers with the families the network
 * granted, so all three are supported here. Without this the generic
 * implementation probes `+CGDCONT=?` on the AT port, which this CP answers in a
 * way that leaves ModemManager reporting IPv4 only -- and a v4-only modem never
 * gets asked for anything else. */
static void
load_supported_ip_families (MMIfaceModem       *self,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_int (task, (MM_BEARER_IP_FAMILY_IPV4 |
                              MM_BEARER_IP_FAMILY_IPV6 |
                              MM_BEARER_IP_FAMILY_IPV4V6));
    g_object_unref (task);
}

static gboolean
load_current_modes_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           MMModemMode  *allowed,
                           MMModemMode  *preferred,
                           GError      **error)
{
    GTask *task = G_TASK (res);

    if (!g_task_propagate_boolean (task, error))
        return FALSE;
    *allowed = (MMModemMode) GPOINTER_TO_UINT (g_task_get_task_data (task));
    *preferred = MM_MODEM_MODE_NONE;
    return TRUE;
}

static void
load_current_modes_ready (MMSitClient  *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMSitNetworkGenerations generations;
    GError                 *error = NULL;

    if (!mm_sit_client_get_allowed_network_types_finish (client, res, &generations, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task,
                          GUINT_TO_POINTER ((guint) generations_to_modes (&generations)),
                          NULL);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
load_current_modes (MMIfaceModem       *self,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    MMSitClient *client;
    GError      *error = NULL;
    GTask       *task;

    task = g_task_new (self, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                                                        &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_allowed_network_types (client, NULL,
                                            (GAsyncReadyCallback) load_current_modes_ready, task);
}

static gboolean
set_current_modes_finish (MMIfaceModem *self,
                          GAsyncResult *res,
                          GError      **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_current_modes_ready (MMSitClient  *client,
                         GAsyncResult *res,
                         GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_allowed_network_types_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_current_modes (MMIfaceModem       *self,
                   MMModemMode         modes,
                   MMModemMode         preferred,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    MMSitClient             *client;
    GError                  *error = NULL;
    GTask                   *task;
    MMSitNetworkGenerations  generations;

    task = g_task_new (self, NULL, callback, user_data);

    if (preferred != MM_MODEM_MODE_NONE) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "The CP's allowed-network-type bitmap cannot express a "
                                 "preferred mode");
        g_object_unref (task);
        return;
    }

    modes_to_generations (modes, &generations);
    if (!generations.gsm && !generations.umts && !generations.lte && !generations.nr) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "At least one radio generation must be allowed");
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (self),
                                                        &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_set_allowed_network_types (client, &generations, NULL,
                                            (GAsyncReadyCallback) set_current_modes_ready, task);
}

/*****************************************************************************/
/* Bands
 *
 * GET_MANUAL_BAND_MODE (0x0730) is the only message in the SIT inventory that
 * carries a real band list -- 0x0709/0x0708 are Android's coarse regional
 * preset and 0x0746 reports the serving band. Its reply holds two blocks, what
 * the part supports and what it is currently restricted to, plus a manual-mode
 * flag; see docs/subsystems/modem/reference/sit-protocol.md.
 *
 * Only LTE and UMTS are mapped. The reply's third bitmap is GSM but its
 * enumeration is not the 3GPP band number the other two use (two set bits on a
 * device with four GSM bands), and the 22 bytes that are presumably NR read
 * zero even under a lock, so neither is guessed at here.
 */

/* E-UTRAN is a straight offset in MM's enum; UTRAN is not, so it needs a
 * table. Both are checked against MM_MODEM_BAND_* rather than open-coded. */
static MMModemBand
sit_lte_band_to_mm (guint16 band)
{
    if (band < 1 || band > 71)
        return MM_MODEM_BAND_UNKNOWN;
    return (MMModemBand) (MM_MODEM_BAND_EUTRAN_1 + (band - 1));
}

static MMModemBand
sit_wcdma_band_to_mm (guint16 band)
{
    static const struct { guint16 sit; MMModemBand mm; } map[] = {
        {  1, MM_MODEM_BAND_UTRAN_1  }, {  2, MM_MODEM_BAND_UTRAN_2  },
        {  3, MM_MODEM_BAND_UTRAN_3  }, {  4, MM_MODEM_BAND_UTRAN_4  },
        {  5, MM_MODEM_BAND_UTRAN_5  }, {  6, MM_MODEM_BAND_UTRAN_6  },
        {  7, MM_MODEM_BAND_UTRAN_7  }, {  8, MM_MODEM_BAND_UTRAN_8  },
        {  9, MM_MODEM_BAND_UTRAN_9  }, { 10, MM_MODEM_BAND_UTRAN_10 },
        { 11, MM_MODEM_BAND_UTRAN_11 }, { 12, MM_MODEM_BAND_UTRAN_12 },
        { 13, MM_MODEM_BAND_UTRAN_13 }, { 14, MM_MODEM_BAND_UTRAN_14 },
        { 19, MM_MODEM_BAND_UTRAN_19 }, { 20, MM_MODEM_BAND_UTRAN_20 },
        { 21, MM_MODEM_BAND_UTRAN_21 }, { 22, MM_MODEM_BAND_UTRAN_22 },
        { 25, MM_MODEM_BAND_UTRAN_25 }, { 26, MM_MODEM_BAND_UTRAN_26 },
        { 32, MM_MODEM_BAND_UTRAN_32 },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (map); i++) {
        if (map[i].sit == band)
            return map[i].mm;
    }
    return MM_MODEM_BAND_UNKNOWN;
}

/* A band the CP names and MM has no enum for is dropped with a note rather than
 * reported as UNKNOWN, which would show up in mmcli as a bogus entry. */
static GArray *
sit_bands_to_mm (MMBroadbandModemSamsungSit *self,
                 const guint16              *lte,
                 guint8                      n_lte,
                 const guint16              *wcdma,
                 guint8                      n_wcdma)
{
    GArray *out = g_array_sized_new (FALSE, FALSE, sizeof (MMModemBand), n_lte + n_wcdma);
    guint   i;

    for (i = 0; i < n_lte; i++) {
        MMModemBand b = sit_lte_band_to_mm (lte[i]);

        if (b == MM_MODEM_BAND_UNKNOWN)
            mm_obj_dbg (self, "no ModemManager band for LTE B%u", lte[i]);
        else
            g_array_append_val (out, b);
    }
    for (i = 0; i < n_wcdma; i++) {
        MMModemBand b = sit_wcdma_band_to_mm (wcdma[i]);

        if (b == MM_MODEM_BAND_UNKNOWN)
            mm_obj_dbg (self, "no ModemManager band for UTRAN B%u", wcdma[i]);
        else
            g_array_append_val (out, b);
    }
    return out;
}

static GArray *
load_bands_finish (MMIfaceModem  *self,
                   GAsyncResult  *res,
                   GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_supported_bands_ready (MMSitClient  *client,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitBandMode               bands;
    GError                     *error = NULL;

    if (!mm_sit_client_get_bands_finish (client, res, &bands, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_return_pointer (task,
                           sit_bands_to_mm (self,
                                            bands.supported_lte, bands.supported_lte_len,
                                            bands.supported_wcdma, bands.supported_wcdma_len),
                           (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_current_bands_ready (MMSitClient  *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitBandMode               bands;
    GError                     *error = NULL;

    if (!mm_sit_client_get_bands_finish (client, res, &bands, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_return_pointer (task,
                           sit_bands_to_mm (self,
                                            bands.current_lte, bands.current_lte_len,
                                            bands.current_wcdma, bands.current_wcdma_len),
                           (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_bands (MMIfaceModem        *iface,
            GAsyncReadyCallback  callback,
            gpointer             user_data,
            gboolean             current)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_bands (client, NULL,
                             current ? (GAsyncReadyCallback) load_current_bands_ready
                                     : (GAsyncReadyCallback) load_supported_bands_ready,
                             task);
}

static void
load_supported_bands (MMIfaceModem       *iface,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    load_bands (iface, callback, user_data, FALSE);
}

static void
load_current_bands (MMIfaceModem       *iface,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    load_bands (iface, callback, user_data, TRUE);
}

static gboolean
set_current_bands_finish (MMIfaceModem  *self,
                          GAsyncResult  *res,
                          GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_current_bands_ready (MMSitClient  *client,
                         GAsyncResult *res,
                         GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_bands_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* Reverse of sit_wcdma_band_to_mm. */
static guint16
mm_band_to_sit_wcdma (MMModemBand band)
{
    guint16 sit;

    for (sit = 1; sit <= 32; sit++) {
        if (sit_wcdma_band_to_mm (sit) == band)
            return sit;
    }
    return 0;
}

/* MM's contract is "use exactly these bands", so everything not named here is
 * cleared -- including the RATs the caller did not mention. Reporting back a
 * superset makes the core fail the operation, which is what happened when this
 * kept UTRAN at its supported value while restricting LTE.
 *
 * MM_MODEM_BAND_ANY means "stop restricting", which is the CP's manual-mode
 * flag cleared rather than a selection of everything. */
static void
set_current_bands (MMIfaceModem        *iface,
                   GArray              *bands_array,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;
    g_autoptr(GArray)           lte = NULL;
    g_autoptr(GArray)           wcdma = NULL;
    guint                       i;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    lte = g_array_new (FALSE, FALSE, sizeof (guint16));
    wcdma = g_array_new (FALSE, FALSE, sizeof (guint16));
    for (i = 0; i < bands_array->len; i++) {
        MMModemBand b = g_array_index (bands_array, MMModemBand, i);
        guint16     sit;

        if (b == MM_MODEM_BAND_ANY) {
            g_array_set_size (lte, 0);
            g_array_set_size (wcdma, 0);
            break;
        }
        if (b >= MM_MODEM_BAND_EUTRAN_1 && b <= MM_MODEM_BAND_EUTRAN_71) {
            sit = (guint16) (b - MM_MODEM_BAND_EUTRAN_1 + 1);
            g_array_append_val (lte, sit);
            continue;
        }
        sit = mm_band_to_sit_wcdma (b);
        if (sit) {
            g_array_append_val (wcdma, sit);
            continue;
        }
        /* GSM has no mapping: the CP's third bitmap does not use 3GPP band
         * numbers (see the note above this section), so a GSM band cannot be
         * requested without guessing. */
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "band %s cannot be selected on this modem",
                                 mm_modem_band_get_string (b));
        g_object_unref (task);
        return;
    }

    mm_sit_client_set_bands (client, lte, wcdma, NULL,
                             (GAsyncReadyCallback) set_current_bands_ready, task);
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface->load_current_capabilities = load_current_capabilities;
    iface->load_current_capabilities_finish = load_current_capabilities_finish;
    iface->load_supported_capabilities = load_supported_capabilities;
    iface->load_supported_capabilities_finish = load_supported_capabilities_finish;
    iface->load_manufacturer = load_manufacturer;
    iface->load_manufacturer_finish = load_identity_string_finish;
    iface->load_model = load_model;
    iface->load_model_finish = load_identity_string_finish;
    iface->load_revision = load_revision;
    iface->load_revision_finish = load_identity_string_finish;
    iface->load_hardware_revision = load_hardware_revision;
    iface->load_hardware_revision_finish = load_identity_string_finish;
    iface->load_equipment_identifier = load_equipment_identifier;
    iface->load_equipment_identifier_finish = load_identity_string_finish;
    iface->load_device_identifier = load_device_identifier;
    iface->load_device_identifier_finish = load_identity_string_finish;
    iface->load_power_state = load_power_state;
    iface->load_power_state_finish = load_power_state_finish;
    iface->load_signal_quality = load_signal_quality;
    iface->load_signal_quality_finish = load_signal_quality_finish;
    iface->load_access_technologies = load_access_technologies;
    iface->load_access_technologies_finish = load_access_technologies_finish;
    iface->load_supported_bands = load_supported_bands;
    iface->load_supported_bands_finish = load_bands_finish;
    iface->load_current_bands = load_current_bands;
    iface->load_current_bands_finish = load_bands_finish;
    iface->set_current_bands = set_current_bands;
    iface->set_current_bands_finish = set_current_bands_finish;
    iface->get_cell_info = get_cell_info;
    iface->get_cell_info_finish = get_cell_info_finish;
    iface->modem_power_up = modem_power_up;
    iface->modem_power_up_finish = modem_set_power_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_set_power_finish;
    iface->create_sim = create_sim;
    iface->create_sim_finish = create_sim_finish;
    iface->load_own_numbers = load_own_numbers;
    iface->load_own_numbers_finish = load_own_numbers_finish;
    iface->load_unlock_required = load_unlock_required;
    iface->load_unlock_required_finish = load_unlock_required_finish;
    iface->load_unlock_retries = load_unlock_retries;
    iface->load_unlock_retries_finish = load_unlock_retries_finish;
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_supported_ip_families = load_supported_ip_families;
    iface->load_supported_ip_families_finish = load_supported_ip_families_finish;
    iface->load_current_modes = load_current_modes;
    iface->load_current_modes_finish = load_current_modes_finish;
    iface->set_current_modes = set_current_modes;
    iface->set_current_modes_finish = set_current_modes_finish;
    iface->setup_sim_hot_swap = setup_sim_hot_swap;
    iface->setup_sim_hot_swap_finish = setup_sim_hot_swap_finish;
    iface->cleanup_sim_hot_swap = cleanup_sim_hot_swap;
    iface->load_sim_slots = load_sim_slots;
    iface->load_sim_slots_finish = load_sim_slots_finish;
    iface->set_primary_sim_slot = set_primary_sim_slot;
    iface->set_primary_sim_slot_finish = set_primary_sim_slot_finish;
    iface->create_bearer = create_bearer;
    iface->create_bearer_finish = create_bearer_finish;
    iface->setup_carrier_config = setup_carrier_config;
    iface->setup_carrier_config_finish = setup_carrier_config_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface)
{
    iface->load_imei = modem_3gpp_load_imei;
    iface->load_imei_finish = modem_3gpp_load_imei_finish;
    iface->set_initial_eps_bearer_settings = set_initial_eps_bearer_settings;
    iface->set_initial_eps_bearer_settings_finish = set_initial_eps_bearer_settings_finish;
    iface->load_initial_eps_bearer_settings = load_initial_eps_bearer_settings;
    iface->load_initial_eps_bearer_settings_finish = load_initial_eps_bearer_settings_finish;
    iface->load_initial_eps_bearer = load_initial_eps_bearer;
    iface->load_initial_eps_bearer_finish = load_initial_eps_bearer_finish;
    iface->run_registration_checks = registration_checks;
    iface->run_registration_checks_finish = registration_checks_finish;
    iface->register_in_network = register_in_network;
    iface->register_in_network_finish = register_in_network_finish;
    iface->load_enabled_facility_locks = load_enabled_facility_locks;
    iface->load_enabled_facility_locks_finish = load_enabled_facility_locks_finish;
    iface->scan_networks = scan_networks;
    iface->scan_networks_finish = scan_networks_finish;
    /* SIT owns registration state; never subscribe to the AT +CxREG URCs. */
    iface->setup_unsolicited_registration_events = setup_registration_events_noop;
    iface->setup_unsolicited_registration_events_finish = setup_registration_events_noop_finish;
    iface->cleanup_unsolicited_registration_events = setup_registration_events_noop;
    iface->cleanup_unsolicited_registration_events_finish = setup_registration_events_noop_finish;
    iface->enable_unsolicited_registration_events = enable_registration_events_noop;
    iface->enable_unsolicited_registration_events_finish = enable_registration_events_noop_finish;
    iface->disable_unsolicited_registration_events = enable_registration_events_noop;
    iface->disable_unsolicited_registration_events_finish = enable_registration_events_noop_finish;
    iface->load_operator_code = load_operator_code;
    iface->load_operator_code_finish = load_operator_code_finish;
    iface->load_operator_name = load_operator_name;
    iface->load_operator_name_finish = load_operator_name_finish;
    iface->setup_unsolicited_events = set_unsolicited_events;
    iface->setup_unsolicited_events_finish = set_unsolicited_events_finish;
    iface->enable_unsolicited_events = set_unsolicited_events;
    iface->enable_unsolicited_events_finish = set_unsolicited_events_finish;
    iface->disable_unsolicited_events = clear_unsolicited_events;
    iface->disable_unsolicited_events_finish = clear_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = clear_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = clear_unsolicited_events_finish;
}

/*****************************************************************************/
/* Signal interface */

static gboolean
signal_check_support_finish (MMIfaceModemSignal  *self,
                             GAsyncResult        *res,
                             GError             **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
signal_check_support (MMIfaceModemSignal  *iface,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    GTask *task;

    task = g_task_new (iface, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
signal_load_values_finish (MMIfaceModemSignal  *self,
                           GAsyncResult        *res,
                           MMSignal           **cdma,
                           MMSignal           **evdo,
                           MMSignal           **gsm,
                           MMSignal           **umts,
                           MMSignal           **lte,
                           MMSignal           **nr5g,
                           GError             **error)
{
    MMSitSignal *signal;

    signal = g_task_propagate_pointer (G_TASK (res), error);
    if (!signal)
        return FALSE;

    if (cdma)
        *cdma = NULL;
    if (evdo)
        *evdo = NULL;
    if (gsm)
        *gsm = NULL;
    if (umts)
        *umts = NULL;
    if (lte)
        *lte = sit_signal_build_lte (signal);
    if (nr5g)
        *nr5g = sit_signal_build_nr5g (signal);
    g_free (signal);
    return TRUE;
}

static void
signal_load_values_ready (MMSitClient *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMSitSignal *signal;
    GError      *error = NULL;

    signal = g_new0 (MMSitSignal, 1);
    if (!mm_sit_client_get_signal_finish (client, res, signal, &error)) {
        g_free (signal);
        g_task_return_error (task, error);
    } else
        g_task_return_pointer (task, signal, g_free);
    g_object_unref (task);
}

static void
signal_load_values (MMIfaceModemSignal  *iface,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, cancellable, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_signal (client, cancellable,
                              (GAsyncReadyCallback) signal_load_values_ready,
                              task);
}

/*****************************************************************************/
/* Time interface
 *
 * The CP runs no clock the AP can read.  GET_NITZ_TIME (0x0955) hands back the
 * last NITZ the network sent -- a UTC timestamp, the network's offset from it
 * and the DST part of that offset -- together with an age in milliseconds, so
 * the current network time is the timestamp plus the age.  A fresh NITZ lands
 * on every (re-)attach, which is what keeps the cached value honest.
 *
 * Both vfuncs read that one message on demand.
 *
 * The CP *does* also push each NITZ as it arrives, as NITZ_TIME_RECEIVED
 * (0x0905), and this comment used to say it did not.  That was wrong: the
 * indications were arriving all along and being dropped, because nothing looked
 * for that id.  They carry the response payload without its status word (33
 * bytes rather than 37) and always with a zero age, since they *are* the
 * arrival.  Identified from the corpus -- the only 33-byte indication in 20 746
 * frames, 26 of them, every field decoding and the dates matching each
 * capture's own day -- then confirmed live, where the pushed timestamps match
 * their arrival times to the second.
 *
 * So the interface no longer depends on its post-registration poll to notice a
 * new time.  0x1007 NETWORK_TIME remains the EMBMS query, which reports
 * unavailable here and is unrelated.
 */

static gboolean
modem_time_check_support_finish (MMIfaceModemTime *self,
                                 GAsyncResult     *res,
                                 GError          **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_time_check_support (MMIfaceModemTime    *self,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    GTask *task;

    /* Every CP that speaks SIT answers this message; whether it has a time to
     * report is a per-call question, not a capability. Probing here would turn
     * "not attached yet" into "no time support for this session". */
    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gchar *
modem_time_load_network_time_finish (MMIfaceModemTime *self,
                                     GAsyncResult     *res,
                                     GError          **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_network_time_ready (MMSitClient  *client,
                         GAsyncResult *res,
                         GTask        *task)
{
    MMSitNitzTime  time;
    gchar         *iso8601;
    GError        *error = NULL;

    if (!mm_sit_client_get_network_time_finish (client, res, &time, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    if (!iso8601)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, iso8601, g_free);
    g_object_unref (task);
}

static void
modem_time_load_network_time (MMIfaceModemTime    *iface,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_network_time (client, NULL,
                                    (GAsyncReadyCallback) load_network_time_ready,
                                    task);
}

static MMNetworkTimezone *
modem_time_load_network_timezone_finish (MMIfaceModemTime *self,
                                         GAsyncResult     *res,
                                         GError          **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_network_timezone_ready (MMSitClient  *client,
                             GAsyncResult *res,
                             GTask        *task)
{
    MMNetworkTimezone *tz;
    MMSitNitzTime      time;
    GError            *error = NULL;

    if (!mm_sit_client_get_network_time_finish (client, res, &time, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    tz = mm_network_timezone_new ();
    mm_network_timezone_set_offset (tz, time.timezone_quarter_hours * 15);
    mm_network_timezone_set_dst_offset (tz, time.daylight_saving_hours * 60);
    g_task_return_pointer (task, tz, g_object_unref);
    g_object_unref (task);
}

static void
modem_time_load_network_timezone (MMIfaceModemTime    *iface,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_network_time (client, NULL,
                                    (GAsyncReadyCallback) load_network_timezone_ready,
                                    task);
}

/*****************************************************************************/
/* Voice interface (CS calls)
 *
 * The CS call group is native SIT (0x00xx), completely independent of the
 * AIMS/IMS group: DIAL/ANSWER/HANGUP are typed requests and GET_CALL_LIST is
 * the poll.  The CP does not push call state; it pokes an empty
 * CALL_STATE_CHANGED indication and the AP re-reads the list, which is the
 * classic RIL pattern and maps directly onto load_call_list.
 *
 * In-call audio is deliberately absent here.  The media is a modem <-> AoC
 * DMA that never reaches the AP, so there is no audio channel for MM to
 * describe: setup_in_call_audio_channel stays unimplemented and the audio
 * binding is done by the audio layer (UCM + tegu-call-audio) off the call
 * state MM publishes.
 *
 * Nothing subscribes to unsolicited events either. The generic enable arms
 * +CLIP, +CRC and +CCWA on the AT port, and all three announce things this
 * plugin already learns from CALL_STATE_CHANGED and the call list -- including
 * a waiting call, which the list reports as RIL state 5. Arming them is
 * therefore pure AT traffic with no reader, so the whole family is a no-op.
 *
 * Supplementary services are unimplemented on purpose, call waiting included.
 * The CP has 0x050b GET_CALL_WAITING and 0x050c SET_CALL_WAITING, but a query
 * is an interrogation that goes to the network -- downstream budgets 60 s for
 * it -- and over AT it took 6.8 s here and dropped the CP's serving network for
 * about ten seconds afterwards. What it buys is a settings toggle: whether the
 * service is active is the subscription's business, and a waiting call arrives
 * and is reported regardless. The part worth having is hold/swap
 * (0x0500-0x0504), without which a reported waiting call cannot be taken; that
 * is where this should be picked up again.
 */

static void
modem_voice_unsolicited_events_noop (MMIfaceModemVoice   *self,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
modem_voice_unsolicited_events_noop_finish (MMIfaceModemVoice  *self,
                                            GAsyncResult       *res,
                                            GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static gboolean
modem_voice_check_support_finish (MMIfaceModemVoice  *self,
                                  GAsyncResult       *res,
                                  GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
modem_voice_check_support (MMIfaceModemVoice   *self,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask *task;

    /* Every SIT CP speaks the CS call group; there is no capability query
     * for it, and the port is the same one already in use. */
    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static MMBaseCall *
modem_voice_create_call (MMIfaceModemVoice *self,
                         MMCallDirection    direction,
                         const gchar       *number,
                         const guint        dtmf_tone_duration)
{
    return mm_call_samsung_sit_new (MM_BASE_MODEM (self),
                                    direction,
                                    number,
                                    dtmf_tone_duration);
}

static MMCallState
sit_call_state_to_mm (guint32 state)
{
    /* Raw RIL_CallState, as carried in the GET_CALL_LIST records. */
    switch (state) {
    case 0: return MM_CALL_STATE_ACTIVE;
    case 1: return MM_CALL_STATE_HELD;
    case 2: return MM_CALL_STATE_DIALING;
    case 3: return MM_CALL_STATE_RINGING_OUT;
    case 4: return MM_CALL_STATE_RINGING_IN;
    case 5: return MM_CALL_STATE_WAITING;
    default: return MM_CALL_STATE_UNKNOWN;
    }
}

static gboolean
modem_voice_load_call_list_finish (MMIfaceModemVoice  *self,
                                   GAsyncResult       *res,
                                   GList             **out_call_info_list,
                                   GError            **error)
{
    GList *list;

    list = g_task_propagate_pointer (G_TASK (res), error);
    if (!list && error && *error)
        return FALSE;

    *out_call_info_list = list;
    return TRUE;
}

/* The core's call_info_free() is static in mm-modem-helpers.c, so plugins
 * carry their own (cinterion does the same). */
static void
sit_call_info_free (MMCallInfo *info)
{
    g_free (info->number);
    g_slice_free (MMCallInfo, info);
}

static void
call_info_list_free (GList *list)
{
    g_list_free_full (list, (GDestroyNotify) sit_call_info_free);
}

static void
get_call_list_ready (MMSitClient  *client,
                     GAsyncResult *res,
                     GTask        *task)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (g_task_get_source_object (task));
    g_autoptr(GArray)  calls = NULL;
    GError            *error = NULL;
    GList             *call_info_list = NULL;
    guint              i;

    calls = mm_sit_client_get_call_list_finish (client, res, &error);
    if (!calls) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /*
     * One list, whichever stack carries the call. The CS group is polled --
     * GET_CALL_LIST is the only way to read it -- while AIMS has no list at all
     * and is tracked from its indications, so the two halves are gathered
     * differently and joined here. A dialer should not have to know which.
     */
    if (self->priv->ims)
        call_info_list = mm_sit_ims_get_call_list (self->priv->ims);

    for (i = 0; i < calls->len; i++) {
        const MMSitCall *call;
        MMCallInfo      *info;
        MMCallState      state;

        call = &g_array_index (calls, MMSitCall, i);

        /* Both index and state are mandatory when reporting the full list --
         * mm_iface_modem_voice_report_all_calls() g_assert()s on them, so a
         * bad entry aborts the daemon rather than being ignored. */
        state = sit_call_state_to_mm (call->state);
        if (state == MM_CALL_STATE_UNKNOWN) {
            mm_obj_dbg (self, "ignoring call %u with unmodelled SIT state %u",
                        call->index, call->state);
            continue;
        }

        info = g_slice_new0 (MMCallInfo);
        /* The CP numbers calls from 0, but 0 means "no index assigned" in
         * ModemManager. Shift by one; mm-call-samsung-sit.c shifts back when
         * it sends HANGUP. */
        info->index     = call->index + 1;
        info->state     = state;
        info->direction = call->incoming ? MM_CALL_DIRECTION_INCOMING : MM_CALL_DIRECTION_OUTGOING;
        info->number    = call->number[0] ? g_strdup (call->number) : NULL;

        call_info_list = g_list_append (call_info_list, info);
    }

    /* An empty list is a valid answer -- it is how a cleared call is
     * reported -- so return the (possibly NULL) list as success. */
    g_task_return_pointer (task, call_info_list, (GDestroyNotify) call_info_list_free);
    g_object_unref (task);
}

static void
modem_voice_load_call_list (MMIfaceModemVoice   *iface,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;
    GTask                      *task;

    task = g_task_new (iface, NULL, callback, user_data);
    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_call_list (client, NULL,
                                 (GAsyncReadyCallback) get_call_list_ready,
                                 task);
}

static void
iface_modem_voice_init (MMIfaceModemVoiceInterface *iface)
{
    iface->check_support           = modem_voice_check_support;
    iface->check_support_finish    = modem_voice_check_support_finish;
    iface->create_call             = modem_voice_create_call;
    iface->load_call_list          = modem_voice_load_call_list;
    iface->load_call_list_finish   = modem_voice_load_call_list_finish;
    /* Every supplementary service is inherited from the generic AT
     * implementation and none of them can work here, so say so rather than
     * letting a caller get "No AT port available to run command" -- an
     * unimplemented vfunc is reported as Unsupported, which is the truth. */
    iface->hold_and_accept                = NULL;
    iface->hold_and_accept_finish         = NULL;
    iface->hangup_and_accept              = NULL;
    iface->hangup_and_accept_finish       = NULL;
    iface->hangup_all                     = NULL;
    iface->hangup_all_finish              = NULL;
    iface->join_multiparty                = NULL;
    iface->join_multiparty_finish         = NULL;
    iface->leave_multiparty               = NULL;
    iface->leave_multiparty_finish        = NULL;
    iface->transfer                       = NULL;
    iface->transfer_finish                = NULL;
    iface->call_waiting_setup             = NULL;
    iface->call_waiting_setup_finish      = NULL;
    iface->call_waiting_query             = NULL;
    iface->call_waiting_query_finish      = NULL;
    /* SIT owns call state; never arm the AT +CLIP/+CRC/+CCWA URCs. */
    iface->setup_unsolicited_events           = modem_voice_unsolicited_events_noop;
    iface->setup_unsolicited_events_finish    = modem_voice_unsolicited_events_noop_finish;
    iface->cleanup_unsolicited_events         = modem_voice_unsolicited_events_noop;
    iface->cleanup_unsolicited_events_finish  = modem_voice_unsolicited_events_noop_finish;
    iface->enable_unsolicited_events          = modem_voice_unsolicited_events_noop;
    iface->enable_unsolicited_events_finish   = modem_voice_unsolicited_events_noop_finish;
    iface->disable_unsolicited_events         = modem_voice_unsolicited_events_noop;
    iface->disable_unsolicited_events_finish  = modem_voice_unsolicited_events_noop_finish;
    /* Likewise the in-call pair, which otherwise opens the AT port for the
     * duration of every call to watch URCs nothing here consumes. */
    iface->setup_in_call_unsolicited_events          = modem_voice_unsolicited_events_noop;
    iface->setup_in_call_unsolicited_events_finish   = modem_voice_unsolicited_events_noop_finish;
    iface->cleanup_in_call_unsolicited_events        = modem_voice_unsolicited_events_noop;
    iface->cleanup_in_call_unsolicited_events_finish = modem_voice_unsolicited_events_noop_finish;
}

static void
iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface)
{
    iface->check_support = messaging_check_support;
    iface->check_support_finish = messaging_check_support_finish;
    iface->load_supported_storages = messaging_load_supported_storages;
    iface->load_supported_storages_finish = messaging_load_supported_storages_finish;
    iface->setup_sms_format = NULL;
    iface->setup_sms_format_finish = NULL;
    iface->set_default_storage = NULL;
    iface->set_default_storage_finish = NULL;
    iface->init_current_storages = NULL;
    iface->init_current_storages_finish = NULL;
    iface->load_initial_sms_parts = messaging_load_initial_sms_parts;
    iface->load_initial_sms_parts_finish = messaging_load_initial_sms_parts_finish;
    iface->setup_unsolicited_events = messaging_setup_cleanup_unsolicited_events;
    iface->setup_unsolicited_events_finish = messaging_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = messaging_setup_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = messaging_setup_cleanup_unsolicited_events_finish;
    iface->enable_unsolicited_events = messaging_setup_cleanup_unsolicited_events;
    iface->enable_unsolicited_events_finish = messaging_setup_cleanup_unsolicited_events_finish;
    iface->disable_unsolicited_events = messaging_setup_cleanup_unsolicited_events;
    iface->disable_unsolicited_events_finish = messaging_setup_cleanup_unsolicited_events_finish;
}

static void
iface_modem_signal_init (MMIfaceModemSignalInterface *iface)
{
    iface->check_support = signal_check_support;
    iface->check_support_finish = signal_check_support_finish;
    iface->load_values = signal_load_values;
    iface->load_values_finish = signal_load_values_finish;
}

static void
iface_modem_time_init (MMIfaceModemTimeInterface *iface)
{
    iface->check_support               = modem_time_check_support;
    iface->check_support_finish        = modem_time_check_support_finish;
    iface->load_network_time           = modem_time_load_network_time;
    iface->load_network_time_finish    = modem_time_load_network_time_finish;
    iface->load_network_timezone       = modem_time_load_network_timezone;
    iface->load_network_timezone_finish = modem_time_load_network_timezone_finish;
    /* NITZ_TIME_RECEIVED arrives on the one control port every other indication
     * comes in on, and the plugin's single handler is already attached for the
     * life of the port. There is no per-interface subscription to make and no
     * arming command to send, so these stay unimplemented -- unlike before, not
     * because nothing is pushed, but because nothing has to be turned on. */
    iface->setup_unsolicited_events    = NULL;
    iface->setup_unsolicited_events_finish = NULL;
    iface->cleanup_unsolicited_events  = NULL;
    iface->cleanup_unsolicited_events_finish = NULL;
    iface->enable_unsolicited_events   = NULL;
    iface->enable_unsolicited_events_finish = NULL;
    iface->disable_unsolicited_events  = NULL;
    iface->disable_unsolicited_events_finish = NULL;
}

/*****************************************************************************/
/* Initialization and enabling, with no AT port
 *
 * MMBroadbandModem opens the primary AT port before anything else runs, in both
 * the initialization and the enabling sequence, and treats its absence as fatal
 * -- "Couldn't get primary port". On this modem there is no primary AT port to
 * open: MMBaseModem's "primary port" is specifically the primary *AT* port, the
 * target of mm_base_modem_at_command, and every interface here is driven over
 * SIT instead. mm_base_modem_organize_ports already tolerates that (it allows a
 * modem with no AT port when a SIT port is present); the two hooks below are
 * what is left.
 *
 * Chain to the parent and demote its failure to a debug line, which is exactly
 * what the QMI and MBIM modems do for the same reason. The parent's context
 * pointer is passed straight through, NULL included -- initialization_stopped
 * takes a NULL context.
 *
 * There is deliberately no SIT-side "primary port" concept introduced here.
 * The primary logical stack is the plugin's own (peek_client), which keeps that
 * choice inside the plugin and out of the core -- and means a per-port
 * ID_MM_PHYSDEV_UID split, which would give each logical stack its own MMDevice
 * with exactly one SIT port, needs nothing changed here. */

static gpointer
initialization_started_finish (MMBroadbandModem  *self,
                               GAsyncResult      *res,
                               GError           **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
cp_initialization_ready (MMSitCp *cp, GAsyncResult *res, GTask *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_cp_prepare_finish (cp, res, &error))
        g_task_return_error (task, g_steal_pointer (&error));
    else
        g_task_return_pointer (task, g_task_get_task_data (task), NULL);
    g_object_unref (task);
}

static void
parent_initialization_started_ready (MMBroadbandModem *self,
                                     GAsyncResult     *res,
                                     GTask            *task)
{
    g_autoptr(GError)  error = NULL;
    gpointer           parent_ctx;

    parent_ctx = MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_samsung_sit_parent_class)->initialization_started_finish (
        self, res, &error);
    if (error)
        mm_obj_dbg (self, "couldn't start parent initialization: %s", error->message);

    if (MM_BROADBAND_MODEM_SAMSUNG_SIT (self)->priv->logical_stack >= 0) {
        MMBroadbandModemSamsungSit *sit = MM_BROADBAND_MODEM_SAMSUNG_SIT (self);

        g_clear_error (&error);
        if (!mm_broadband_modem_samsung_sit_peek_client (sit, &error)) {
            g_task_return_error (task, g_steal_pointer (&error));
            g_object_unref (task);
            return;
        }
        g_task_set_task_data (task, parent_ctx, NULL);
        mm_sit_cp_prepare (sit->priv->cp, (GAsyncReadyCallback) cp_initialization_ready, task);
        return;
    }
    g_task_return_pointer (task, parent_ctx, NULL);
    g_object_unref (task);
}

static void
initialization_started (MMBroadbandModem    *self,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_samsung_sit_parent_class)->initialization_started (
        self, (GAsyncReadyCallback) parent_initialization_started_ready, task);
}

static gboolean
enabling_started_finish (MMBroadbandModem  *self,
                         GAsyncResult      *res,
                         GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_enabling_started_ready (MMBroadbandModem *self,
                               GAsyncResult     *res,
                               GTask            *task)
{
    g_autoptr(GError) error = NULL;

    if (!MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_samsung_sit_parent_class)->enabling_started_finish (self, res, &error))
        mm_obj_dbg (self, "couldn't start parent enabling: %s", error->message);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
enabling_started (MMBroadbandModem    *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_samsung_sit_parent_class)->enabling_started (
        self, (GAsyncReadyCallback) parent_enabling_started_ready, task);
}

/*****************************************************************************/

MMBroadbandModemSamsungSit *
mm_broadband_modem_samsung_sit_new (const gchar  *device,
                                    const gchar  *physdev,
                                    const gchar **drivers,
                                    const gchar  *plugin,
                                    guint16       vendor_id,
                                    guint16       product_id,
                                    const gchar  *carrier_config_database,
                                    gboolean      use_proxy,
                                    gint          logical_stack)
{
    MMBroadbandModemSamsungSit *self;

    self = g_object_new (MM_TYPE_BROADBAND_MODEM_SAMSUNG_SIT,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_PHYSDEV, physdev,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         /*
                          * The CS domain is real on this modem: with IMS down
                          * it drops to UMTS and places circuit-switched calls,
                          * and the CP answers GET_VOICE_REGISTRATION (0x0700)
                          * with a full registration payload -- which the stock
                          * stack re-reads after every serving-network and
                          * physical-channel indication, where we stopped after
                          * boot.
                          *
                          * Declaring it unsupported left run_registration_checks
                          * logging "CS: 'no'" and skipping the voice query
                          * entirely, so update_cs_registration() below never ran
                          * and the core had no circuit-switched state at all.
                          * Nothing is lost by asking: the consolidated state
                          * takes the best of the four domains, so a CS answer
                          * can only add to what PS and EPS already report.
                          */
                         MM_IFACE_MODEM_3GPP_CS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_3GPP_PS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_3GPP_EPS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_3GPP_5GS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_CARRIER_CONFIG_MAPPING, carrier_config_database,
                         NULL);

    /* Not a construct property: nothing reads it until peek_client, which is
     * well after construction. */
    self->priv->use_proxy = use_proxy;
    self->priv->logical_stack = logical_stack;
    return self;
}

static void
dispose (GObject *object)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (object);

    cleanup_sim_hot_swap (MM_IFACE_MODEM (self));
    if (self->priv->reject_retry_id) {
        g_source_remove (self->priv->reject_retry_id);
        self->priv->reject_retry_id = 0;
    }
    if (self->priv->search_watchdog_id) {
        g_source_remove (self->priv->search_watchdog_id);
        self->priv->search_watchdog_id = 0;
    }
    g_clear_pointer (&self->priv->boot_apns, g_ptr_array_unref);
    g_clear_pointer (&self->priv->ims_database, g_free);
    g_clear_object (&self->priv->initial_eps_bearer_settings);
    g_clear_object (&self->priv->ims);
    if (self->priv->client)
        g_signal_handlers_disconnect_by_data (self->priv->client, self);
    self->priv->client = NULL;
    g_clear_pointer (&self->priv->clients, g_ptr_array_unref);
    g_clear_object (&self->priv->cp);
    G_OBJECT_CLASS (mm_broadband_modem_samsung_sit_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (object);

    g_free (self->priv->operator_code);
    g_free (self->priv->operator_name);
    g_free (self->priv->carrier_config_imsi);
    g_clear_pointer (&self->priv->forbidden_plmns, g_ptr_array_unref);
    G_OBJECT_CLASS (mm_broadband_modem_samsung_sit_parent_class)->finalize (object);
}

static void
mm_broadband_modem_samsung_sit_init (MMBroadbandModemSamsungSit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_MODEM_SAMSUNG_SIT,
                                              MMBroadbandModemSamsungSitPrivate);
    self->priv->logical_stack = -1;
}

static void
mm_broadband_modem_samsung_sit_class_init (MMBroadbandModemSamsungSitClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemSamsungSitPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;

    /* The Shannon AT service rejects ATZ with +CME ERROR: 100. The generic
     * initialization sequence has already configured the AT port, so skip the
     * reset normally issued by the first-enable hook. */
    broadband_modem_class->create_sms = create_sms;
    broadband_modem_class->enabling_modem_init = NULL;
    broadband_modem_class->enabling_modem_init_finish = NULL;
    broadband_modem_class->initialization_started = initialization_started;
    broadband_modem_class->initialization_started_finish = initialization_started_finish;
    broadband_modem_class->enabling_started = enabling_started;
    broadband_modem_class->enabling_started_finish = enabling_started_finish;
}
