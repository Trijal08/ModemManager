/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIM_SAMSUNG_SIT_H
#define MM_SIM_SAMSUNG_SIT_H

#include <glib-object.h>

#include "mm-base-sim.h"

#define MM_TYPE_SIM_SAMSUNG_SIT            (mm_sim_samsung_sit_get_type ())
#define MM_SIM_SAMSUNG_SIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SIM_SAMSUNG_SIT, MMSimSamsungSit))
#define MM_IS_SIM_SAMSUNG_SIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SIM_SAMSUNG_SIT))

typedef struct _MMSimSamsungSit      MMSimSamsungSit;
typedef struct _MMSimSamsungSitClass MMSimSamsungSitClass;

struct _MMSimSamsungSit {
    MMBaseSim parent;
};

struct _MMSimSamsungSitClass {
    MMBaseSimClass parent;
};

GType mm_sim_samsung_sit_get_type (void);

/* For the slot inventory: a SIM whose identifiers are already known, which is
 * how the active slot has to be built, since an existing active SIM bypasses
 * create_sim. Empty physical slots must fail the unlock check before it. */
MMBaseSim *mm_sim_samsung_sit_new_initialized (MMBaseModem *modem,
                                               GObject     *bind_to,
                                               guint        slot_number,
                                               gboolean     active,
                                               const gchar *sim_identifier,
                                               const gchar *eid);

void       mm_sim_samsung_sit_new        (MMBaseModem         *modem,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data);
MMBaseSim *mm_sim_samsung_sit_new_finish (GAsyncResult        *res,
                                          GError             **error);

#endif /* MM_SIM_SAMSUNG_SIT_H */
