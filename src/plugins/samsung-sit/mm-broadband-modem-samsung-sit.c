/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-bearer-samsung-sit.h"
#include "mm-broadband-modem-samsung-sit.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log-object.h"

static void iface_modem_init      (MMIfaceModemInterface     *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface);

struct _MMBroadbandModemSamsungSitPrivate {
    MMSitClient *client;
    gchar       *operator_code;
};

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemSamsungSit,
                        mm_broadband_modem_samsung_sit,
                        MM_TYPE_BROADBAND_MODEM,
                        0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init))

static void
update_registration (MMBroadbandModemSamsungSit *self,
                     const MMSitRegistration    *registration)
{
    MMModem3gppRegistrationState state;

    state = registration->registered ? MM_MODEM_3GPP_REGISTRATION_STATE_HOME :
                                       MM_MODEM_3GPP_REGISTRATION_STATE_IDLE;
    g_free (self->priv->operator_code);
    self->priv->operator_code = registration->registered ? g_strdup (registration->plmn) : NULL;

    mm_iface_modem_3gpp_update_ps_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_update_eps_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_update_5gs_registration_state (MM_IFACE_MODEM_3GPP (self), state, TRUE);
    mm_iface_modem_3gpp_apply_deferred_registration_state (MM_IFACE_MODEM_3GPP (self));
}

static void
sit_indication (MMSitClient              *client,
                const MMSitMessage       *message,
                MMBroadbandModemSamsungSit *self)
{
    MMSitRegistration registration;

    if (message->id == MM_SIT_MESSAGE_SERVING_NETWORK &&
        mm_sit_message_parse_registration (message, &registration, NULL)) {
        mm_obj_dbg (self, "SIT serving network changed (registered: %s)",
                    registration.registered ? "yes" : "no");
        update_registration (self, &registration);
    }
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
load_power_state (MMIfaceModem        *iface,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_report_error (self, callback, user_data, load_power_state, error);
        return;
    }
    mm_sit_client_get_radio_state (client, NULL, callback, user_data);
}

static gboolean
modem_set_power_finish (MMIfaceModem  *self,
                        GAsyncResult  *res,
                        GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
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

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_report_error (self, callback, user_data, modem_set_power, error);
        return;
    }
    mm_sit_client_set_radio_state (client, state, NULL, callback, user_data);
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
    MMSitRegistration           registration;
    GError                     *error = NULL;

    if (!mm_sit_client_get_registration_finish (client, res, &registration, &error))
        g_task_return_error (task, error);
    else if (!registration.registered)
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "SIT modem is not registered");
    else {
        update_registration (self, &registration);
        g_task_return_pointer (task, g_strdup (registration.plmn), g_free);
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
    mm_sit_client_get_registration (client, NULL,
                                    (GAsyncReadyCallback) load_operator_code_ready, task);
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
set_unsolicited_events (MMIfaceModem3gpp   *iface,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    MMBroadbandModemSamsungSit *self = MM_BROADBAND_MODEM_SAMSUNG_SIT (iface);
    MMSitClient                *client;
    GError                     *error = NULL;

    client = mm_broadband_modem_samsung_sit_peek_client (self, &error);
    if (!client) {
        g_task_report_error (self, callback, user_data, set_unsolicited_events, error);
        return;
    }
    mm_sit_client_set_indication_filter (client, 0x7f, NULL, callback, user_data);
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
    iface->modem_power_up = modem_power_up;
    iface->modem_power_up_finish = modem_set_power_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_set_power_finish;
    iface->create_bearer = create_bearer;
    iface->create_bearer_finish = create_bearer_finish;
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
    iface->setup_unsolicited_events = set_unsolicited_events;
    iface->setup_unsolicited_events_finish = set_unsolicited_events_finish;
    iface->enable_unsolicited_events = set_unsolicited_events;
    iface->enable_unsolicited_events_finish = set_unsolicited_events_finish;
    iface->disable_unsolicited_events = clear_unsolicited_events;
    iface->disable_unsolicited_events_finish = clear_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = clear_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = clear_unsolicited_events_finish;
}

MMBroadbandModemSamsungSit *
mm_broadband_modem_samsung_sit_new (const gchar  *device,
                                    const gchar  *physdev,
                                    const gchar **drivers,
                                    const gchar  *plugin,
                                    guint16       vendor_id,
                                    guint16       product_id)
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

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemSamsungSitPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;
}
