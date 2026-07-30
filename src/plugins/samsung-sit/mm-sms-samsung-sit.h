/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SMS_SAMSUNG_SIT_H
#define MM_SMS_SAMSUNG_SIT_H

#include <glib-object.h>

#include "mm-base-sms.h"

#define MM_TYPE_SMS_SAMSUNG_SIT            (mm_sms_samsung_sit_get_type ())
#define MM_SMS_SAMSUNG_SIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SMS_SAMSUNG_SIT, MMSmsSamsungSit))
#define MM_IS_SMS_SAMSUNG_SIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SMS_SAMSUNG_SIT))

typedef struct _MMSmsSamsungSit      MMSmsSamsungSit;
typedef struct _MMSmsSamsungSitClass MMSmsSamsungSitClass;

struct _MMSmsSamsungSit {
    MMBaseSms parent;
};

struct _MMSmsSamsungSitClass {
    MMBaseSmsClass parent;
};

GType mm_sms_samsung_sit_get_type (void);

MMBaseSms *mm_sms_samsung_sit_new (MMBaseModem *modem);

#endif /* MM_SMS_SAMSUNG_SIT_H */
