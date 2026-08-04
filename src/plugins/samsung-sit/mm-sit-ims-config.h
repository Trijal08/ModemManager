/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_IMS_CONFIG_H
#define MM_SIT_IMS_CONFIG_H

#include <glib.h>

#include <samsung_sit_ffi.h>

/*
 * The configuration the CP's IMS stack needs before it can register, resolved
 * for one carrier.
 *
 * This is the seam the converted ims.db plugs into. Today the values are
 * compiled in -- AOSP defaults plus the overrides decoded from the captured
 * o2_de frames -- which is right for exactly one carrier and honest about it.
 * The real source is Android's per-carrier CarrierSettings protobuf; see
 * research/decisions/0005-ims-architecture.md. Nothing outside this file knows
 * where the values came from, so replacing them does not reach the lifecycle.
 */
typedef struct _MMSitImsConfig MMSitImsConfig;

/*
 * The substitutions for the SIP User-Agent. The carrier owns the template's
 * shape and its certification suffix; the device owns these four values.
 *
 * What to put in them is a policy question this module deliberately does not
 * answer: the string that registered on o2 claims to be the stock Android
 * build, and whether the network cares is untested. The caller decides, and
 * this only assembles.
 */
typedef struct {
    const gchar *manufacturer;
    const gchar *model;
    /* Operating system, as the User-Agent names it, plus its version. */
    const gchar *os_name;
    const gchar *os_version;
    /* Build/software identifier. */
    const gchar *software_version;
} MMSitImsDeviceIdentity;

/*
 * Resolve the configuration for `mccmnc` (NULL or unknown yields the defaults),
 * with `device` substituted into the User-Agent. Never fails: an unknown
 * carrier is a set of defaults, not an error, because a CP with default timers
 * and a valid User-Agent is worth trying.
 */
MMSitImsConfig *mm_sit_ims_config_resolve (const MMSitImsDeviceIdentity *device,
                                           const gchar                  *mccmnc);

void mm_sit_ims_config_free (MMSitImsConfig *self);

/* The entries, in the order they should reach the CP. Owned by `self`. */
const SamsungAimsConfigEntry *mm_sit_ims_config_peek_entries (const MMSitImsConfig *self,
                                                              gsize                *out_n_entries);

/* Which carrier's overrides were applied, for logging. "default" if none. */
const gchar *mm_sit_ims_config_get_carrier (const MMSitImsConfig *self);

/* The assembled User-Agent, for logging -- it is the one value a network is
 * most likely to reject us over, so it belongs in the log. */
const gchar *mm_sit_ims_config_get_user_agent (const MMSitImsConfig *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitImsConfig, mm_sit_ims_config_free)

#endif /* MM_SIT_IMS_CONFIG_H */
