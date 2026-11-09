/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_BROADBAND_MODEM_SAMSUNG_SIT_H
#define MM_BROADBAND_MODEM_SAMSUNG_SIT_H

#include "mm-broadband-modem.h"
#include "mm-sit-client.h"
#include "mm-sit-ims.h"

/*
 * Two different directories, and they are deliberately not the same string.
 *
 * ANNOUNCED is what goes on the wire in CARRIER_CONFIG_MANIFEST. The CP opens
 * paths under it over RFS, and it is downstream's location so that the frame
 * matches the vendor RIL byte for byte. There is no /vendor on this rootfs;
 * exynos-modem's RFS server re-roots whatever the CP opens onto the real
 * files, so nothing has to exist at this path.
 *
 * Where the plugin opens cfg.db is deliberately not here. That is a property
 * of how a particular device packages its vendor blobs, so it comes from the
 * port's ID_MM_SIT_CARRIER_CONFIG_DIRECTORY and nowhere else; the aport that
 * ships the blobs is the one that knows where they landed. Without it the
 * modem still comes up, and the carrier configuration reports that it needs a
 * database.
 */
#define MM_SIT_ANNOUNCED_CARRIER_CONFIG_DIRECTORY "/vendor/firmware/carrierconfig"

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
                                                                 const gchar  *carrier_config_database,
                                                                 gboolean      use_proxy,
                                                                 gint          logical_stack);

MMSitClient *mm_broadband_modem_samsung_sit_peek_client (MMBroadbandModemSamsungSit  *self,
                                                          GError                     **error);

/* The CP's IMS stack, or NULL before a control port is up. Borrowed. */
/*
 * Hold every active call. The CP refuses a new call while one is active, so
 * the dial path runs this first; see the implementation for the capture.
 */
void     mm_broadband_modem_samsung_sit_hold_active_calls        (MMBroadbandModemSamsungSit  *self,
                                                                  GAsyncReadyCallback          callback,
                                                                  gpointer                     user_data);
gboolean mm_broadband_modem_samsung_sit_hold_active_calls_finish (MMBroadbandModemSamsungSit  *self,
                                                                  GAsyncResult                *res,
                                                                  GError                     **error);

MMSitIms *mm_broadband_modem_samsung_sit_peek_ims (MMBroadbandModemSamsungSit *self);

void     mm_broadband_modem_samsung_sit_ensure_carrier_config        (MMBroadbandModemSamsungSit *self,
                                                                      GAsyncReadyCallback          callback,
                                                                      gpointer                     user_data);
gboolean mm_broadband_modem_samsung_sit_ensure_carrier_config_finish (MMBroadbandModemSamsungSit *self,
                                                                      GAsyncResult                *res,
                                                                      GError                     **error);

#endif /* MM_BROADBAND_MODEM_SAMSUNG_SIT_H */
