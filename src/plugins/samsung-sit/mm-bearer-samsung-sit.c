/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-bearer-samsung-sit.h"
#include "mm-bind.h"
#include "mm-broadband-modem-samsung-sit.h"
#include "mm-log-object.h"

struct _MMBearerSamsungSitPrivate {
    guint8   cid;
    gboolean connected;
};

G_DEFINE_TYPE (MMBearerSamsungSit, mm_bearer_samsung_sit, MM_TYPE_BASE_BEARER)

static MMBearerConnectResult *
connect_finish (MMBaseBearer  *bearer,
                GAsyncResult  *res,
                GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
connect_ready (MMSitClient *client,
               GAsyncResult *res,
               GTask *task)
{
    MMBearerSamsungSit       *self = g_task_get_source_object (task);
    g_autoptr(MMBearerIpConfig) ipv4 = NULL;
    g_autoptr(MMPort)         data = NULL;
    MMSitDataCall             call;
    GError                   *error = NULL;
    const gchar              *dns[3] = { NULL, NULL, NULL };
    guint                     n_dns = 0;

    if (!mm_sit_client_connect_data_finish (client, res, &call, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = mm_base_modem_get_best_data_port (
        MM_BASE_MODEM (g_task_get_task_data (task)), MM_PORT_TYPE_NET);
    if (!data) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "No Samsung SIT network data port available");
        g_object_unref (task);
        return;
    }

    ipv4 = mm_bearer_ip_config_new ();
    mm_bearer_ip_config_set_method (ipv4, MM_BEARER_IP_METHOD_STATIC);
    mm_bearer_ip_config_set_address (ipv4, call.ipv4);
    mm_bearer_ip_config_set_prefix (ipv4, 32);
    if (call.dns1[0])
        dns[n_dns++] = call.dns1;
    if (call.dns2[0])
        dns[n_dns++] = call.dns2;
    if (n_dns)
        mm_bearer_ip_config_set_dns (ipv4, dns);
    if (call.mtu)
        mm_bearer_ip_config_set_mtu (ipv4, call.mtu);

    self->priv->cid = call.cid;
    self->priv->connected = TRUE;
    mm_obj_info (self, "SIT data call %u connected with IPv4 configuration", call.cid);
    g_task_return_pointer (task,
                           mm_bearer_connect_result_new (data, ipv4, NULL),
                           (GDestroyNotify) mm_bearer_connect_result_unref);
    g_object_unref (task);
}

static void
connect (MMBaseBearer        *bearer,
         GCancellable        *cancellable,
         GAsyncReadyCallback  callback,
         gpointer             user_data)
{
    MMBearerSamsungSit              *self = MM_BEARER_SAMSUNG_SIT (bearer);
    g_autoptr(MMBroadbandModemSamsungSit) modem = NULL;
    MMBearerProperties              *properties;
    MMSitClient                     *client;
    const gchar                     *apn;
    GError                          *error = NULL;
    GTask                           *task;

    task = g_task_new (bearer, cancellable, callback, user_data);
    if (self->priv->connected) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_CONNECTED,
                                 "Samsung SIT bearer is already connected");
        g_object_unref (task);
        return;
    }

    g_object_get (bearer, MM_BASE_BEARER_MODEM, &modem, NULL);
    g_assert (modem);
    client = mm_broadband_modem_samsung_sit_peek_client (modem, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    properties = mm_base_bearer_peek_config (bearer);
    apn = mm_bearer_properties_get_apn (properties);
    if (!apn || !apn[0]) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "Samsung SIT data calls require an APN");
        g_object_unref (task);
        return;
    }

    /* Keep the modem alive until the asynchronous setup sequence completes. */
    g_task_set_task_data (task, g_object_ref (modem), g_object_unref);
    mm_sit_client_connect_data (client, apn, cancellable,
                                (GAsyncReadyCallback) connect_ready, task);
}

static gboolean
disconnect_finish (MMBaseBearer  *bearer,
                   GAsyncResult  *res,
                   GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disconnect_ready (MMSitClient *client,
                  GAsyncResult *res,
                  GTask *task)
{
    MMBearerSamsungSit *self = g_task_get_source_object (task);
    GError             *error = NULL;

    if (!mm_sit_client_disconnect_data_finish (client, res, &error))
        g_task_return_error (task, error);
    else {
        mm_obj_info (self, "SIT data call %u disconnected", self->priv->cid);
        self->priv->cid = 0;
        self->priv->connected = FALSE;
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
disconnect (MMBaseBearer        *bearer,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
    MMBearerSamsungSit              *self = MM_BEARER_SAMSUNG_SIT (bearer);
    g_autoptr(MMBroadbandModemSamsungSit) modem = NULL;
    MMSitClient                     *client;
    GError                          *error = NULL;
    GTask                           *task;

    task = g_task_new (bearer, NULL, callback, user_data);
    if (!self->priv->connected) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_object_get (bearer, MM_BASE_BEARER_MODEM, &modem, NULL);
    g_assert (modem);
    client = mm_broadband_modem_samsung_sit_peek_client (modem, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_sit_client_disconnect_data (client, self->priv->cid, NULL,
                                   (GAsyncReadyCallback) disconnect_ready, task);
}

MMBaseBearer *
mm_bearer_samsung_sit_new (MMBroadbandModemSamsungSit *modem,
                           MMBearerProperties         *properties)
{
    MMBaseBearer *bearer;

    bearer = g_object_new (MM_TYPE_BEARER_SAMSUNG_SIT,
                           MM_BASE_BEARER_MODEM, modem,
                           MM_BIND_TO, modem,
                           MM_BASE_BEARER_CONFIG, properties,
                           NULL);
    mm_base_bearer_export (bearer);
    return bearer;
}

static void
mm_bearer_samsung_sit_init (MMBearerSamsungSit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BEARER_SAMSUNG_SIT,
                                              MMBearerSamsungSitPrivate);
}

static void
mm_bearer_samsung_sit_class_init (MMBearerSamsungSitClass *klass)
{
    GObjectClass      *object_class = G_OBJECT_CLASS (klass);
    MMBaseBearerClass *bearer_class = MM_BASE_BEARER_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBearerSamsungSitPrivate));
    bearer_class->connect = connect;
    bearer_class->connect_finish = connect_finish;
    bearer_class->disconnect = disconnect;
    bearer_class->disconnect_finish = disconnect_finish;
}
