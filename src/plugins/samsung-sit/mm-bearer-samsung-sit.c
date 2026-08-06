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

/* One family's static configuration out of the granted data call. The CP hands
 * back a full address rather than a prefix, so the v4 side takes the /32 a
 * raw-IP bearer wants and the v6 side the /64 a 3GPP PDP context is assigned. */
static MMBearerIpConfig *
build_ip_config (const gchar *address,
                 guint        prefix,
                 const gchar *dns1,
                 const gchar *dns2,
                 guint16      mtu)
{
    MMBearerIpConfig *config;
    const gchar      *dns[3] = { NULL, NULL, NULL };
    guint             n_dns = 0;

    config = mm_bearer_ip_config_new ();
    mm_bearer_ip_config_set_method (config, MM_BEARER_IP_METHOD_STATIC);
    mm_bearer_ip_config_set_address (config, address);
    mm_bearer_ip_config_set_prefix (config, prefix);
    if (dns1[0])
        dns[n_dns++] = dns1;
    if (dns2[0])
        dns[n_dns++] = dns2;
    if (n_dns)
        mm_bearer_ip_config_set_dns (config, dns);
    if (mtu)
        mm_bearer_ip_config_set_mtu (config, mtu);
    return config;
}

/*
 * A context's network interface follows from its context id, and nothing in the
 * response says so: the CP carries context n on PDP channel 180 + n, which the
 * kernel exposes as rmnet(n-1) -- rmnet0..rmnet29 over channels 181..210.
 *
 * That mapping is read out of the vendor stack rather than guessed.
 * GetDataCallFromDataCallList_v11 (research/re/sitprot/datacall.c:1396) stores
 * the response item's byte 2 -- the cid, per docs/subsystems/modem/reference/sit-protocol.md -- at +0x10
 * of its parsed record, and libsitril names the interface from that same field:
 * `if (TextUtils::IsEmpty(supplied_name)) snprintf(dst, "%s%d", "rmnet", cid-1);
 * else strncpy(dst, supplied_name)`, at 0x1e5ca0 and 0x1e7d74 (the two RIL HAL
 * struct generations, each reached from both SetDataCall entry points).
 *
 * The synthesis is therefore the fallback, not the only rule -- but the other
 * arm only copies through a name the caller supplied, and whatever it says still
 * has to name an interface cpif created, which is only ever rmnet<i> on channel
 * base+i. So there is no competing formula, and this is the same arithmetic the
 * vendor RIL does rather than one that happens to fit.
 *
 * Two independent observations agree: Android's first setupDataCall takes cid 1
 * and is handed rmnet0, and its second stack -- which the RIL names from rmnet15
 * up -- was seen using cids 16 and 17. Only cid 1 is directly observed, though;
 * for cid >= 2 the CP-side fact is inferred from the vendor stack being right.
 *
 * It also means rmnet0 is the initial attach context's (cid 1, set by
 * SET_INITIAL_ATTACH_APN), which is why ordinary contexts allocate from 2 and
 * land on rmnet1 up.
 */
static MMPort *
get_context_data_port (MMBaseModem  *modem,
                       guint8        cid,
                       GError      **error)
{
    g_autofree gchar *name = NULL;
    MMPort           *port;

    if (!cid) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                             "SIT data call reported no context id");
        return NULL;
    }

    name = g_strdup_printf ("rmnet%u", (guint) cid - 1);
    port = mm_base_modem_peek_port (modem, name);
    if (!port) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                     "No network port '%s' for SIT context %u", name, cid);
        return NULL;
    }
    if (mm_port_get_port_type (port) != MM_PORT_TYPE_NET) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Port '%s' for SIT context %u is not a network port",
                     name, cid);
        return NULL;
    }
    return g_object_ref (port);
}

static void
orphan_dropped (MMSitClient  *client,
                GAsyncResult *res,
                gpointer      user_data)
{
    MMBearerSamsungSit *self = user_data;
    g_autoptr(GError)   error = NULL;

    if (!mm_sit_client_disconnect_data_finish (client, res, &error))
        mm_obj_warn (self, "Could not drop orphaned SIT data call: %s", error->message);
    g_object_unref (self);
}

/*
 * Fail a connect that has already been granted. Reporting the error is not
 * enough once the CP has the context up: MMBaseBearer only disconnects on a
 * cancelled connect, priv->cid is not recorded until the connect succeeds so the
 * bearer's own disconnect() would be a no-op, and cid_acquire() hands the same
 * cid back for the same APN -- so the context would stay up unreachable and
 * every retry would fail on it identically until ModemManager restarted.
 * Deactivating it here also releases the cid, which the deactivate path does
 * only once the CP agrees the context is gone.
 */
static void
connect_failed_after_call (GTask       *task,
                           MMSitClient *client,
                           guint8       cid,
                           GError      *error)
{
    MMBearerSamsungSit *self = g_task_get_source_object (task);

    mm_obj_dbg (self, "dropping granted SIT data call %u: %s", cid, error->message);
    mm_sit_client_disconnect_data (client, cid, NULL,
                                   (GAsyncReadyCallback) orphan_dropped,
                                   g_object_ref (self));
    g_task_return_error (task, error);
    g_object_unref (task);
}

static void
connect_ready (MMSitClient *client,
               GAsyncResult *res,
               GTask *task)
{
    MMBearerSamsungSit       *self = g_task_get_source_object (task);
    g_autoptr(MMBearerIpConfig) ipv4 = NULL;
    g_autoptr(MMBearerIpConfig) ipv6 = NULL;
    g_autoptr(MMPort)         data = NULL;
    MMSitDataCall             call;
    GError                   *error = NULL;

    if (!mm_sit_client_connect_data_finish (client, res, &call, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = get_context_data_port (MM_BASE_MODEM (g_task_get_task_data (task)),
                                  call.cid, &error);
    if (!data) {
        connect_failed_after_call (task, client, call.cid, error);
        return;
    }

    /* A dual-stack context is one call carrying both families, so both configs
     * come out of the same record. The CP grants what the network allows, which
     * can be narrower than what was asked for. */
    if (call.has_ipv4)
        ipv4 = build_ip_config (call.ipv4, 32, call.dns1, call.dns2, call.mtu);
    if (call.has_ipv6) {
        ipv6 = build_ip_config (call.ipv6, 64, call.dns1_v6, call.dns2_v6, call.mtu);
        /*
         * The CP never reports a next hop -- its gateway slots are zero in every
         * captured response, for both families, and no link-local address
         * appears anywhere in the frame. It terminates neighbour discovery
         * itself: router solicitations sent from the AP on the data interface
         * go out and are never answered, which is also how the CP knows the
         * prefix it handed us. A point-to-point PDP link needs no next hop
         * anyway, and Android does the same thing -- an empty gateway list
         * there becomes an interface-scoped route.
         *
         * NetworkManager treats a missing gateway as "not a hard failure" for
         * IPv4 and installs a default route with a zero next hop, but its IPv6
         * path rejects a static configuration without one outright. Reporting
         * "::" gets us that same interface-scoped default route. Revisit if
         * NetworkManager ever grows the IPv4 fallback for IPv6.
         */
        mm_bearer_ip_config_set_gateway (ipv6, "::");
    }

    if (!ipv4 && !ipv6) {
        connect_failed_after_call (task, client, call.cid,
                                   g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                                "SIT data call %u carries no address",
                                                call.cid));
        return;
    }

    self->priv->cid = call.cid;
    self->priv->connected = TRUE;
    mm_obj_info (self, "SIT data call %u connected on %s with %s configuration",
                 call.cid, mm_port_get_device (data),
                 (ipv4 && ipv6) ? "IPv4 and IPv6" : (ipv4 ? "IPv4" : "IPv6"));
    g_task_return_pointer (task,
                           mm_bearer_connect_result_new (data, ipv4, ipv6),
                           (GDestroyNotify) mm_bearer_connect_result_unref);
    g_object_unref (task);
}

static void
carrier_config_ready (MMBroadbandModemSamsungSit *modem,
                      GAsyncResult                *res,
                      GTask                       *task)
{
    MMBaseBearer       *bearer;
    MMBearerProperties *properties;
    MMSitClient        *client;
    const gchar        *apn;
    MMSitApnAuth        auth;
    GError             *error = NULL;

    if (!mm_broadband_modem_samsung_sit_ensure_carrier_config_finish (modem, res, &error)) {
        g_prefix_error (&error, "Could not provision Samsung carrier profile: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (modem, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    bearer = g_task_get_source_object (task);
    properties = mm_base_bearer_peek_config (bearer);
    apn = mm_bearer_properties_get_apn (properties);
    if (!mm_sit_apn_auth_from_properties (properties, &auth, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_connect_data (client, apn,
                                mm_sit_pdp_type_from_ip_family (mm_bearer_properties_get_ip_type (properties)),
                                mm_sit_apn_types_from_properties (properties),
                                &auth,
                                g_task_get_cancellable (task),
                                (GAsyncReadyCallback) connect_ready, task);
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
    const gchar                     *apn;
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
    mm_broadband_modem_samsung_sit_ensure_carrier_config (
        modem, (GAsyncReadyCallback) carrier_config_ready, task);
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

gboolean
mm_bearer_samsung_sit_get_cid (MMBearerSamsungSit *self,
                               guint8             *out_cid)
{
    g_return_val_if_fail (MM_IS_BEARER_SAMSUNG_SIT (self), FALSE);
    g_return_val_if_fail (out_cid != NULL, FALSE);

    if (!self->priv->connected)
        return FALSE;
    *out_cid = self->priv->cid;
    return TRUE;
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

/* The CP can drop a PDN on its own -- a network-initiated deactivation, or a RAT
 * change forced by narrowing the allowed network types. Forget the context so a
 * later connect is not refused as "already connected". */
static void
report_connection_status (MMBaseBearer             *bearer,
                          MMBearerConnectionStatus  status,
                          const GError             *connection_error)
{
    MMBearerSamsungSit *self = MM_BEARER_SAMSUNG_SIT (bearer);

    if (status == MM_BEARER_CONNECTION_STATUS_DISCONNECTED) {
        self->priv->cid = 0;
        self->priv->connected = FALSE;
    }

    MM_BASE_BEARER_CLASS (mm_bearer_samsung_sit_parent_class)->report_connection_status (
        bearer, status, connection_error);
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
    bearer_class->report_connection_status = report_connection_status;
}
