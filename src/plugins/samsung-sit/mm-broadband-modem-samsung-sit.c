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
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-sit-carrier-config.h"
#include "mm-utils.h"

static void iface_modem_init      (MMIfaceModemInterface     *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface);
static void iface_modem_signal_init (MMIfaceModemSignalInterface *iface);

struct _MMBroadbandModemSamsungSitPrivate {
    MMSitClient *client;
    gchar       *operator_code;
    gchar       *operator_name;
    gboolean     carrier_config_ready;
    gchar       *carrier_config_imsi;
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
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_SIGNAL, iface_modem_signal_init))

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

static gboolean
sit_signal_get_quality (const MMSitSignal *signal,
                        guint             *out_quality)
{
    gdouble value;

    if (sit_signal_get_lte_rsrp (signal, &value)) {
        *out_quality = MM_RSRP_TO_QUALITY (value);
        return TRUE;
    }
    if (sit_signal_get_lte_rssi (signal, &value)) {
        *out_quality = MM_RSSI_TO_QUALITY (value);
        return TRUE;
    }
    return FALSE;
}

static void
update_registration (MMBroadbandModemSamsungSit *self,
                     const MMSitRegistration    *registration)
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

    mm_iface_modem_3gpp_update_ps_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_update_eps_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_update_5gs_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_apply_deferred_registration_state (MM_IFACE_MODEM_3GPP (self));
}

static void
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
    if (operator_info->plmn[0] && operator_name) {
        g_free (self->priv->operator_name);
        self->priv->operator_name = g_strdup (operator_name);
    }
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

static void
sit_indication (MMSitClient              *client,
                const MMSitMessage       *message,
                MMBroadbandModemSamsungSit *self)
{
    MMSitOperator operator_info;

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
        update_operator (self, &operator_info);
    }

    if (message->id == MM_SIT_MESSAGE_SIGNAL_STRENGTH_CHANGED) {
        MMSitSignal        signal;
        g_autoptr(MMSignal) lte = NULL;
        guint              quality;

        if (!mm_sit_message_parse_signal (message, &signal, NULL))
            return;

        mm_obj_dbg (self, "SIT signal changed (schema: V%u, RAT mask: 0x%04x)",
                    signal.version, signal.rat_mask);
        lte = sit_signal_build_lte (&signal);
        if (lte)
            mm_iface_modem_signal_update (MM_IFACE_MODEM_SIGNAL (self),
                                          NULL, NULL, NULL, NULL, lte, NULL);
        if (sit_signal_get_quality (&signal, &quality))
            mm_iface_modem_update_signal_quality (MM_IFACE_MODEM (self), quality);
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

MMSitClient *
mm_broadband_modem_samsung_sit_peek_client (MMBroadbandModemSamsungSit  *self,
                                             GError                     **error)
{
    GList  *ports;
    MMPort *port;

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

    port = MM_PORT (ports->data);
    self->priv->client = mm_sit_client_new (mm_port_get_device (port));
    if (!mm_sit_client_open (self->priv->client, error)) {
        g_clear_object (&self->priv->client);
        g_list_free_full (ports, g_object_unref);
        return NULL;
    }

    g_signal_connect (self->priv->client, "indication", G_CALLBACK (sit_indication), self);
    mm_obj_info (self, "opened Samsung SIT control port %s", mm_port_get_device (port));
    g_list_free_full (ports, g_object_unref);
    return self->priv->client;
}

/*****************************************************************************/
/* Modem interface */

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
    GList            *result = NULL;
    GError           *error = NULL;
    guint             i;

    cells = mm_sit_client_get_cell_info_finish (client, res, &error);
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
        tac = g_strdup_printf ("%X", cell->tac);
        ci = g_strdup_printf ("%X", cell->ci);
        pci = g_strdup_printf ("%X", cell->physical_ci);
        mm_cell_info_lte_set_operator_id (info, cell->operator_id);
        mm_cell_info_lte_set_tac (info, tac);
        mm_cell_info_lte_set_ci (info, ci);
        mm_cell_info_lte_set_physical_ci (info, pci);
        mm_cell_info_lte_set_earfcn (info, cell->earfcn);
        mm_cell_info_lte_set_bandwidth (info, cell->bandwidth);
        if (cell->rsrp >= 44 && cell->rsrp <= 140)
            mm_cell_info_lte_set_rsrp (info, -(gdouble) cell->rsrp);
        if (cell->rsrq >= 3 && cell->rsrq <= 34)
            mm_cell_info_lte_set_rsrq (info, -(gdouble) cell->rsrq);
        if (cell->timing_advance >= 0 && cell->timing_advance < G_MAXINT32)
            mm_cell_info_lte_set_timing_advance (info, cell->timing_advance);
        if (cell->registered) {
            mm_cell_info_set_serving (MM_CELL_INFO (info), TRUE);
            mm_cell_info_lte_set_serving_cell_type (info, MM_SERVING_CELL_TYPE_PCELL);
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
    if (error)
        g_task_return_error (task, error);
    else
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

static gboolean
registration_checks_finish (MMIfaceModem3gpp  *self,
                            GAsyncResult      *res,
                            GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
registration_ready (MMSitClient *client,
                    GAsyncResult *res,
                    GTask *task)
{
    MMBroadbandModemSamsungSit *self = g_task_get_source_object (task);
    MMSitRegistration           registration;
    GError                     *error = NULL;

    if (!mm_sit_client_get_registration_finish (client, res, &registration, &error))
        g_task_return_error (task, error);
    else {
        update_registration (self, &registration);
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
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
register_in_network (MMIfaceModem3gpp   *self,
                     const gchar        *operator_id,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);

    if (operator_id && operator_id[0])
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "Manual network selection is not implemented by the SIT client");
    else
        /* The Shannon modem starts automatic selection when radio power is enabled. */
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
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

static gboolean
clear_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                 GAsyncResult      *res,
                                 GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface->load_power_state = load_power_state;
    iface->load_power_state_finish = load_power_state_finish;
    iface->load_signal_quality = load_signal_quality;
    iface->load_signal_quality_finish = load_signal_quality_finish;
    iface->get_cell_info = get_cell_info;
    iface->get_cell_info_finish = get_cell_info_finish;
    iface->modem_power_up = modem_power_up;
    iface->modem_power_up_finish = modem_set_power_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_set_power_finish;
    iface->create_bearer = create_bearer;
    iface->create_bearer_finish = create_bearer_finish;
    iface->setup_carrier_config = setup_carrier_config;
    iface->setup_carrier_config_finish = setup_carrier_config_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface)
{
    iface->run_registration_checks = registration_checks;
    iface->run_registration_checks_finish = registration_checks_finish;
    iface->register_in_network = register_in_network;
    iface->register_in_network_finish = register_in_network_finish;
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
        *nr5g = NULL;
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

static void
iface_modem_signal_init (MMIfaceModemSignalInterface *iface)
{
    iface->check_support = signal_check_support;
    iface->check_support_finish = signal_check_support_finish;
    iface->load_values = signal_load_values;
    iface->load_values_finish = signal_load_values_finish;
}

MMBroadbandModemSamsungSit *
mm_broadband_modem_samsung_sit_new (const gchar  *device,
                                    const gchar  *physdev,
                                    const gchar **drivers,
                                    const gchar  *plugin,
                                    guint16       vendor_id,
                                    guint16       product_id,
                                    const gchar  *carrier_config_database)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_SAMSUNG_SIT,
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
}

static void
dispose (GObject *object)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (object);

    g_clear_object (&self->priv->client);
    G_OBJECT_CLASS (mm_broadband_modem_samsung_sit_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (object);

    g_free (self->priv->operator_code);
    g_free (self->priv->operator_name);
    g_free (self->priv->carrier_config_imsi);
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
    broadband_modem_class->enabling_modem_init = NULL;
    broadband_modem_class->enabling_modem_init_finish = NULL;
}
