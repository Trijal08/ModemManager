/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <gmodule.h>

#include "mm-broadband-modem-samsung-sit.h"
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
    if (!mm_port_probe_list_has_sit_port (probes)) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "Samsung s5300 device has no SIT control port");
        return NULL;
    }

    return MM_BASE_MODEM (mm_broadband_modem_samsung_sit_new (
        uid, physdev, drivers, mm_plugin_get_name (self), vendor, product));
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
}
