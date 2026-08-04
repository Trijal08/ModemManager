/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_IMS_H
#define MM_SIT_IMS_H

#include <glib-object.h>
#include <ModemManager.h>

#include "mm-sit-client.h"
#include "mm-sit-ims-config.h"

#define MM_TYPE_SIT_IMS            (mm_sit_ims_get_type ())
#define MM_SIT_IMS(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SIT_IMS, MMSitIms))
#define MM_IS_SIT_IMS(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SIT_IMS))

typedef struct _MMSitIms        MMSitIms;
typedef struct _MMSitImsClass   MMSitImsClass;
typedef struct _MMSitImsPrivate MMSitImsPrivate;

struct _MMSitIms {
    GObject          parent;
    MMSitImsPrivate *priv;
};

struct _MMSitImsClass {
    GObjectClass parent;
};

GType mm_sit_ims_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitIms, g_object_unref)

/*
 * Drives the CP's own IMS stack: bring up the ims PDN on its own context,
 * start the stack, push the configuration it cannot invent, hand it the address
 * and proxies the network granted, and follow what it reports back.
 *
 * `port_index` selects the stack, which on this CP *is* the SIT port -- there is
 * no in-band selector, so one of these belongs to one port.
 */
MMSitIms *mm_sit_ims_new (MMSitClient *client,
                          guint        port_index);

/* What to substitute into the SIP User-Agent. Must be set before IMS can start;
 * the CP will not register without one. */
void mm_sit_ims_set_device_identity (MMSitIms                     *self,
                                     const MMSitImsDeviceIdentity *device);

/*
 * The trigger. IMS needs a packet-switched RAT, so this starts the sequence
 * when one appears and tears it down when it goes -- but whether a call can be
 * placed is [`mm_sit_ims_is_registered`], never this, because the network can
 * refuse IMS on a perfectly good LTE cell.
 */
void mm_sit_ims_update_access_tech (MMSitIms                   *self,
                                    MMModemAccessTechnology     act);

/* The serving network's MCCMNC, which selects the carrier configuration. */
void mm_sit_ims_set_operator_code (MMSitIms    *self,
                                   const gchar *mccmnc);

/* Whether the CP has told us it is registered. The gate for routing a call to
 * IMS rather than CS. */
gboolean mm_sit_ims_is_registered (MMSitIms *self);

/* The P-Associated-URI list from the last successful registration, or NULL. */
const gchar *mm_sit_ims_get_associated_uris (MMSitIms *self);

/* Stop the stack and drop the PDN; used on radio-off and at dispose. */
void mm_sit_ims_stop (MMSitIms *self);

#endif /* MM_SIT_IMS_H */
