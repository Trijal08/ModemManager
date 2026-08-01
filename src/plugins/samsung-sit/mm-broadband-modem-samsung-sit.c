/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

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
#include "mm-utils.h"

static void iface_modem_init      (MMIfaceModemInterface     *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface);
static void iface_modem_messaging_init (MMIfaceModemMessagingInterface *iface);
static void iface_modem_signal_init (MMIfaceModemSignalInterface *iface);
static void iface_modem_time_init (MMIfaceModemTimeInterface *iface);
static void iface_modem_voice_init (MMIfaceModemVoiceInterface *iface);

struct _MMBroadbandModemSamsungSitPrivate {
    GPtrArray   *clients;
    MMSitClient *client; /* borrowed primary logical-stack client */
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
    /* Reach the SIT ports through sit-proxy rather than owning the device
     * nodes, so mm-sit-tool and the IMS helper can share them. Set from
     * ID_MM_SIT_PROXY on the port. */
    gboolean     use_proxy;
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

/* 0x0701 covers the packet-switched domain only. The three states move together
 * because the CP reports one packet-service registration for whichever RAT it is
 * on, rather than separate GPRS/EPS/5GS views. */
static void
update_ps_registration (MMBroadbandModemSamsungSit *self,
                        const MMSitRegistration    *registration)
{
    MMModem3gppRegistrationState state = sit_registration_state (registration);

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

static void
handle_new_sms (MMBroadbandModemSamsungSit *self,
                const MMSitMessage         *message)
{
    g_autoptr(GError)  error = NULL;
    MMSitSmsDeliver    deliver;
    MMSmsPart         *part;
    MMSitClient       *client;

    if (!mm_sit_message_parse_new_sms (message, &deliver, &error)) {
        mm_obj_warn (self, "couldn't parse a delivered SMS: %s", error->message);
        return;
    }

    mm_obj_dbg (self, "%s delivered over %s, %u bytes",
                deliver.status_report ? "SMS delivery report" : "SMS",
                deliver.over_ims ? "IMS" : "the CS group", deliver.pdu_len);

    part = mm_sms_part_3gpp_new_from_binary_pdu (SMS_PART_INVALID_INDEX,
                                                 deliver.pdu, deliver.pdu_len,
                                                 self, FALSE, &error);
    if (!part)
        mm_obj_warn (self, "couldn't parse the delivered SMS PDU: %s", error->message);
    /* A delivery report goes through the same door: the core matches it to the
     * message it belongs to by its reference and marks that one delivered. */
    else if (!mm_iface_modem_messaging_take_part (MM_IFACE_MODEM_MESSAGING (self),
                                                  mm_broadband_modem_create_sms (MM_BROADBAND_MODEM (self)),
                                                  part,
                                                  MM_SMS_STATE_RECEIVED,
                                                  MM_SMS_STORAGE_ME,
                                                  &error))
        mm_obj_warn (self, "couldn't store the delivered SMS: %s", error->message);

    /* Acknowledge either way: a message the core could not take is still one
     * the CP is holding, and leaving it unacknowledged stalls delivery. */
    client = mm_broadband_modem_samsung_sit_peek_client (self, NULL);
    if (client)
        mm_sit_client_ack_sms (client, deliver.over_ims, NULL,
                               (GAsyncReadyCallback) ack_sms_ready, g_object_ref (self));
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

    apn = mm_bearer_properties_get_apn (properties);
    if (!apn || !apn[0]) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "The initial EPS bearer needs an APN");
        g_object_unref (task);
        return;
    }

    if (!mm_sit_apn_auth_from_properties (properties, &auth, &inner_error)) {
        g_task_return_error (task, g_steal_pointer (&inner_error));
        g_object_unref (task);
        return;
    }

    g_task_set_task_data (task, g_object_ref (properties), g_object_unref);

    mm_sit_client_set_initial_attach_apn (client, apn,
                                          mm_sit_pdp_type_from_ip_family (mm_bearer_properties_get_ip_type (properties)),
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

    delay = reject_retry_delays_secs[self->priv->reject_retries];
    mm_obj_dbg (self, "scheduling attach retry in %u s", delay);
    self->priv->reject_retry_id = g_timeout_add_seconds (delay,
                                                         (GSourceFunc) reject_retry_cb,
                                                         self);
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

    if (message->id == MM_SIT_MESSAGE_SIM_STATUS_CHANGED)
        invalidate_carrier_config (self);
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
    }

    if (message->id == MM_SIT_MESSAGE_SIGNAL_STRENGTH_CHANGED) {
        MMSitSignal        signal;
        g_autoptr(MMSignal) lte = NULL;
        g_autoptr(MMSignal) nr5g = NULL;
        guint              quality;

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
        mm_iface_modem_update_access_technologies (
            MM_IFACE_MODEM (self), sit_rat_mask_to_access_technology (signal.rat_mask),
            MM_MODEM_ACCESS_TECHNOLOGY_ANY);
    }
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

    if (!mm_sit_carrier_config_resolve (carrier_config_database, &identity, &config, &error)) {
        invalidate_carrier_config (self);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_free (self->priv->carrier_config_imsi);
    self->priv->carrier_config_imsi = g_strdup (imsi);
    directory = g_path_get_dirname (carrier_config_database);
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

    if (self->priv->carrier_config_ready && imsi &&
        self->priv->carrier_config_imsi &&
        g_str_equal (self->priv->carrier_config_imsi, imsi)) {
        task = g_task_new (self, NULL, callback, user_data);
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    if (self->priv->carrier_config_ready)
        invalidate_carrier_config (self);

    if (!database || !database[0] || !imsi || !imsi[0]) {
        task = g_task_new (self, NULL, callback, user_data);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "Samsung carrier configuration requires a database and SIM identity");
        g_object_unref (task);
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
    g_signal_connect (self->priv->client, "indication", G_CALLBACK (sit_indication), self);
    return self->priv->client;
}

/*****************************************************************************/
/* Modem interface */

/* Capabilities
 *
 * The CP has no capability query and the downstream RIL never asks for one --
 * which generations a baseband speaks is a property of the part, not runtime
 * state. ModemManager's generic path infers it from `ATI`, `+GCAP` and
 * `+WS46=?`, and on this modem that lands on `gsm-umts` alone: `+GCAP` names
 * only the circuit-switched capability and `+WS46=?` is answered malformed (a
 * closing parenthesis with no opening one), so the packet-switched generations
 * never appear. The modem then reports itself 2G/3G-only while registered on
 * LTE.
 *
 * So answer for the part. Both capability steps are also *fatal* to modem
 * initialization (`mm-iface-modem.c`, `load_{current,supported}_capabilities_
 * ready`), which makes this the one thing that has to exist before the AT port
 * can go away at all.
 *
 * One combination, not a list: the generations are not switchable here. What
 * *is* switchable is which of them the CP may use, and that is the Modes
 * property, which 0x074f/0x0750 already drive. */
#define SAMSUNG_SIT_CAPABILITIES (MM_MODEM_CAPABILITY_GSM_UMTS | \
                                  MM_MODEM_CAPABILITY_LTE |      \
                                  MM_MODEM_CAPABILITY_5GNR)

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

static void
load_current_capabilities (MMIfaceModem        *self,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_int (task, SAMSUNG_SIT_CAPABILITIES);
    g_object_unref (task);
}

static GArray *
load_supported_capabilities_finish (MMIfaceModem  *self,
                                    GAsyncResult  *res,
                                    GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_supported_capabilities (MMIfaceModem        *self,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
    GArray            *capabilities;
    MMModemCapability  capability = SAMSUNG_SIT_CAPABILITIES;
    GTask             *task;

    capabilities = g_array_sized_new (FALSE, FALSE, sizeof (MMModemCapability), 1);
    g_array_append_val (capabilities, capability);

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_pointer (task, capabilities, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

/* Identity
 *
 * Only the equipment identifier has a source on this control plane: the CP
 * holds its own IMEI and 0x0401 GET_IMEI reads it back. Everything else the
 * generic path gets from +CGMI/+CGMM/+CGMR has no SIT equivalent --
 * 0x0901 GET_BASE_BAND_VERSION answers a near-empty frame naming only an IMS
 * capability level, and 0x0931 AT_COMMAND, which would have tunnelled the same
 * three commands, is refused with status 2 on this firmware.
 *
 * So the two that are properties of the part are stated, and the revision is
 * left unset rather than filled with something that is not one. That changes
 * Modem.DeviceIdentifier, which is hashed from these together with the
 * equipment identifier -- nothing here keys a NetworkManager connection on it,
 * but it does not survive the switch. */
#define SAMSUNG_SIT_MANUFACTURER "Samsung Electronics"
/* The baseband part, which is what +CGMM names too (as "S5300" plus a firmware
 * variant suffix we have no way to read back). */
#define SAMSUNG_SIT_MODEL "S5300"

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

static void
load_model (MMIfaceModem        *self,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_pointer (task, g_strdup (SAMSUNG_SIT_MODEL), g_free);
    g_object_unref (task);
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
    MMSitSignal signal;
    GError     *error = NULL;

    if (!mm_sit_client_get_signal_finish (client, res, &signal, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_set_task_data (task,
                          GUINT_TO_POINTER ((guint) sit_rat_mask_to_access_technology (signal.rat_mask)),
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

    task = g_task_new (iface, NULL, callback, user_data);
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
        /* No card, or a card the CP has not finished reading. Reporting a lock
         * here would strand the core waiting for a PIN that is not wanted, so
         * say unknown and let it ask again. */
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
                                   (GAsyncReadyCallback) modem_set_power_ready,
                                   task);
}

static void
power_up_carrier_config_ready (MMBroadbandModemSamsungSit *self,
                               GAsyncResult               *res,
                               GTask                      *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_broadband_modem_samsung_sit_ensure_carrier_config_finish (self, res, &error))
        mm_obj_warn (self, "couldn't provision carrier configuration before radio-on: %s",
                     error->message);
    power_up_radio (task);
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
        self, (GAsyncReadyCallback) power_up_carrier_config_ready, task);
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
        mm_sit_client_send_boot_config (client, NULL,
                                        (GAsyncReadyCallback) power_up_boot_config_ready,
                                        task);
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

            if (port->logical_slot_id == 0) {
                ctx->primary_sim_slot = i + 1;
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
                                                             i + 1,
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
            mm_obj_dbg (self, "SIT slot status has no mapping for logical stack 0 yet; retrying");
            g_timeout_add_seconds (LOAD_SIM_SLOTS_RETRY_SECS,
                                   (GSourceFunc) load_sim_slots_retry, task);
            return;
        }
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "SIT slot status has no mapping for logical stack 0");
        g_object_unref (task);
        return;
    }

    mm_obj_info (self, "loaded %u physical SIM slots; slot %u maps to logical stack 0",
                 status.slot_count, ctx->primary_sim_slot);
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
    } else {
        mm_obj_dbg (self, "asking the CP to select a network automatically");
        mm_sit_client_set_network_selection_auto (client, cancellable,
                                                 (GAsyncReadyCallback) register_in_network_ready,
                                                 task);
    }
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
set_unsolicited_events_ready (MMSitClient *client,
                              GAsyncResult *res,
                              GTask *task)
{
    GError *error = NULL;

    if (!mm_sit_client_set_indication_filter_finish (client, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Android maps the signal indication-filter bit to SET_SCREEN_STATE=1,
     * which makes this modem emit a large SIGNAL_STRENGTH_IND every second.
     * ModemManager has no display-state integration, so keep the modem in its
     * low-rate screen-off policy and use explicit signal queries instead. */
    mm_sit_client_set_screen_state (client, FALSE, NULL,
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
    mm_sit_client_set_indication_filter (client, 0x7f, NULL,
                                         (GAsyncReadyCallback) set_unsolicited_events_ready,
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
    GTask *task = g_task_new (self, NULL, callback, user_data);

    /* Keep the control port open: closing it may release an active data call. */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
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
load_supported_modes (MMIfaceModem       *self,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    GArray                  *combinations;
    MMModemModeCombination   combination;
    MMModemMode              all;
    GTask                   *task;
    guint                    mask;

    all = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G;
    combinations = g_array_sized_new (FALSE, FALSE, sizeof (MMModemModeCombination), 15);
    for (mask = 1; mask <= (guint) all; mask++) {
        if (mask & ~((guint) all))
            continue;
        combination.allowed = (MMModemMode) mask;
        combination.preferred = MM_MODEM_MODE_NONE;
        g_array_append_val (combinations, combination);
    }

    task = g_task_new (self, NULL, callback, user_data);
    g_task_return_pointer (task, combinations, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
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
 * Both vfuncs read that one message on demand.  There is no unsolicited path to
 * hook: the CP emits no NITZ indication -- checked with the 0x0928 indication
 * filter set to 0xffffffff across a re-attach that did land a fresh NITZ, and
 * 0x1007 NETWORK_TIME is the EMBMS query, which reports unavailable here.  So
 * setup/enable_unsolicited_events stay unimplemented and the interface's own
 * post-registration timezone poll does the refreshing.
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
    /* The CP pushes no NITZ indication, so there is nothing to subscribe to. */
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
                                    gboolean      use_proxy)
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
                         MM_IFACE_MODEM_3GPP_CS_NETWORK_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_3GPP_PS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_3GPP_EPS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_3GPP_5GS_NETWORK_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_CARRIER_CONFIG_MAPPING, carrier_config_database,
                         NULL);

    /* Not a construct property: nothing reads it until peek_client, which is
     * well after construction. */
    self->priv->use_proxy = use_proxy;
    return self;
}

static void
dispose (GObject *object)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (object);

    if (self->priv->reject_retry_id) {
        g_source_remove (self->priv->reject_retry_id);
        self->priv->reject_retry_id = 0;
    }
    g_clear_object (&self->priv->initial_eps_bearer_settings);
    self->priv->client = NULL;
    g_clear_pointer (&self->priv->clients, g_ptr_array_unref);
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
