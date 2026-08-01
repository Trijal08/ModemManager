/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <gmodule.h>

#include "mm-broadband-modem-samsung-sit.h"
#include "mm-kernel-device.h"
#include "mm-log-object.h"
#include "mm-plugin-common.h"
#include "mm-private-boxed-types.h"

#define MM_TYPE_PLUGIN_SAMSUNG_SIT mm_plugin_samsung_sit_get_type ()
MM_DEFINE_PLUGIN (SAMSUNG_SIT, samsung_sit, SamsungSit)

static MMBaseModem *
create_modem (MMPlugin      *self,
              const gchar   *uid,
              const gchar   *physdev,
              const gchar  **drivers,
              guint16        vendor,
              guint16        product,
              guint16        subsystem_vendor,
              guint16        subsystem_device,
              GList         *probes,
              GError       **error)
{
    g_autofree gchar *database = NULL;
    const gchar      *directory = MM_SIT_DEFAULT_CARRIER_CONFIG_DIRECTORY;
    gboolean          use_proxy = FALSE;
    GList            *l;

    if (!mm_port_probe_list_has_sit_port (probes)) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "Samsung s5300 device has no SIT control port");
        return NULL;
    }

    for (l = probes; l; l = g_list_next (l)) {
        MMPortProbe    *probe = MM_PORT_PROBE (l->data);
        MMKernelDevice *port;
        const gchar    *configured_directory;

        if (!mm_port_probe_is_sit (probe))
            continue;
        port = mm_port_probe_peek_port (probe);
        configured_directory = mm_kernel_device_get_property (
            port, "ID_MM_SIT_CARRIER_CONFIG_DIRECTORY");
        if (configured_directory && configured_directory[0])
            directory = configured_directory;
        /* Declared by the udev rule that ships with sit-proxy, so the transport
         * is a packaging decision rather than something probed at runtime. */
        use_proxy = mm_kernel_device_get_property_as_boolean (port, "ID_MM_SIT_PROXY");
        break;
    }

    database = g_build_filename (directory, "cfg.db", NULL);
    return MM_BASE_MODEM (mm_broadband_modem_samsung_sit_new (
        uid, physdev, drivers, mm_plugin_get_name (self), vendor, product, database,
        use_proxy));
}

/* The CP exposes an AT service on its own WWAN port, and it is a veneer: the
 * contexts it lists are not the ones configured over SIT, +CGDCONT offers only
 * IP and PPP where SIT negotiates IPv4v6, +CEMODE and +WS46=? are answered
 * malformed, and +CSCB is refused outright. Anything ModemManager does through
 * it is therefore either a no-op or wrong -- and one of them is actively
 * harmful: a call-waiting query (+CCWA=1,2) is a supplementary-service
 * interrogation that goes to the network, and on this CP it costs the serving
 * network for about ten seconds.
 *
 * Every interface here is driven over SIT, so grab the port as ignored rather
 * than leave it usable. Doing it here and not only in a udev rule makes it a
 * property of the plugin: nothing about this modem needs an AT port, on any
 * system, whatever the packaging ships. peek_client already skips ignored
 * ports, and MMBaseModem tolerates a modem whose only control port is SIT. */
static gboolean
grab_port (MMPlugin     *self,
           MMBaseModem  *modem,
           MMPortProbe  *probe,
           GError      **error)
{
    MMPortType  ptype;
    MMPortGroup pgroup;

    ptype  = mm_port_probe_get_port_type  (probe);
    pgroup = mm_port_probe_get_port_group (probe);

    if (ptype == MM_PORT_TYPE_AT && pgroup != MM_PORT_GROUP_IGNORED) {
        mm_obj_dbg (self, "ignoring AT port %s: this modem is driven over SIT",
                    mm_port_probe_get_port_name (probe));
        pgroup = MM_PORT_GROUP_IGNORED;
    }

    return mm_base_modem_grab_port (modem,
                                    mm_port_probe_peek_port (probe),
                                    pgroup,
                                    ptype,
                                    MM_PORT_SERIAL_AT_FLAG_NONE,
                                    error);
}

MM_PLUGIN_NAMED_CREATOR_SCOPE MMPlugin *
mm_plugin_create_samsung_sit (void)
{
    static const gchar *subsystems[] = { "wwan", "net", NULL };
    static const gchar *drivers[] = { "s5300-modem", NULL };

    return MM_PLUGIN (
        g_object_new (MM_TYPE_PLUGIN_SAMSUNG_SIT,
                      MM_PLUGIN_NAME,               MM_MODULE_NAME,
                      MM_PLUGIN_ALLOWED_SUBSYSTEMS, subsystems,
                      MM_PLUGIN_ALLOWED_DRIVERS,    drivers,
                      MM_PLUGIN_ALLOWED_AT,         TRUE,
                      NULL));
}

static void
mm_plugin_samsung_sit_init (MMPluginSamsungSit *self)
{
}

static void
mm_plugin_samsung_sit_class_init (MMPluginSamsungSitClass *klass)
{
    MM_PLUGIN_CLASS (klass)->create_modem = create_modem;
    MM_PLUGIN_CLASS (klass)->grab_port = grab_port;
}
