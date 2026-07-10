/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_BROADBAND_MODEM_SAMSUNG_SIT_H
#define MM_BROADBAND_MODEM_SAMSUNG_SIT_H

#include "mm-broadband-modem.h"
#include "mm-sit-client.h"

#define MM_SIT_DEFAULT_CARRIER_CONFIG_DIRECTORY "/vendor/firmware/carrierconfig"

#define MM_TYPE_BROADBAND_MODEM_SAMSUNG_SIT            (mm_broadband_modem_samsung_sit_get_type ())
#define MM_BROADBAND_MODEM_SAMSUNG_SIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_SAMSUNG_SIT, MMBroadbandModemSamsungSit))
#define MM_IS_BROADBAND_MODEM_SAMSUNG_SIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_SAMSUNG_SIT))

typedef struct _MMBroadbandModemSamsungSit        MMBroadbandModemSamsungSit;
typedef struct _MMBroadbandModemSamsungSitClass   MMBroadbandModemSamsungSitClass;
typedef struct _MMBroadbandModemSamsungSitPrivate MMBroadbandModemSamsungSitPrivate;

struct _MMBroadbandModemSamsungSit {
    MMBroadbandModem parent;
    MMBroadbandModemSamsungSitPrivate *priv;
};

struct _MMBroadbandModemSamsungSitClass {
    MMBroadbandModemClass parent;
};

GType mm_broadband_modem_samsung_sit_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMBroadbandModemSamsungSit, g_object_unref)

MMBroadbandModemSamsungSit *mm_broadband_modem_samsung_sit_new (const gchar  *device,
                                                                 const gchar  *physdev,
                                                                 const gchar **drivers,
                                                                 const gchar  *plugin,
                                                                 guint16       vendor_id,
                                                                 guint16       product_id,
                                                                 const gchar  *carrier_config_database);

MMSitClient *mm_broadband_modem_samsung_sit_peek_client (MMBroadbandModemSamsungSit  *self,
                                                          GError                     **error);

void     mm_broadband_modem_samsung_sit_ensure_carrier_config        (MMBroadbandModemSamsungSit *self,
                                                                      GAsyncReadyCallback          callback,
                                                                      gpointer                     user_data);
gboolean mm_broadband_modem_samsung_sit_ensure_carrier_config_finish (MMBroadbandModemSamsungSit *self,
                                                                      GAsyncResult                *res,
                                                                      GError                     **error);

#endif /* MM_BROADBAND_MODEM_SAMSUNG_SIT_H */
