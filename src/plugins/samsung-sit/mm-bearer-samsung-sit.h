/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_BEARER_SAMSUNG_SIT_H
#define MM_BEARER_SAMSUNG_SIT_H

#include "mm-base-bearer.h"

typedef struct _MMBroadbandModemSamsungSit MMBroadbandModemSamsungSit;

#define MM_TYPE_BEARER_SAMSUNG_SIT            (mm_bearer_samsung_sit_get_type ())
#define MM_BEARER_SAMSUNG_SIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BEARER_SAMSUNG_SIT, MMBearerSamsungSit))
#define MM_IS_BEARER_SAMSUNG_SIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BEARER_SAMSUNG_SIT))

typedef struct _MMBearerSamsungSit        MMBearerSamsungSit;
typedef struct _MMBearerSamsungSitClass   MMBearerSamsungSitClass;
typedef struct _MMBearerSamsungSitPrivate MMBearerSamsungSitPrivate;

struct _MMBearerSamsungSit {
    MMBaseBearer parent;
    MMBearerSamsungSitPrivate *priv;
};

struct _MMBearerSamsungSitClass {
    MMBaseBearerClass parent;
};

GType         mm_bearer_samsung_sit_get_type (void);
MMBaseBearer *mm_bearer_samsung_sit_new      (MMBroadbandModemSamsungSit *modem,
                                               MMBearerProperties         *properties);

/* The CP's context id for the PDN this bearer carries, or 0 when not connected;
 * used to match it against a data-call list the CP reports. */
gboolean      mm_bearer_samsung_sit_get_cid  (MMBearerSamsungSit         *self,
                                               guint8                     *out_cid);

#endif /* MM_BEARER_SAMSUNG_SIT_H */
