/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_IMS_CONFIG_H
#define MM_SIT_IMS_CONFIG_H

#include <glib.h>

#include "mm-sit-carrier-config.h"

#include <samsung_sit_ffi.h>

/*
 * The configuration the CP's IMS stack needs before it can register, resolved
 * for one carrier out of `ims.db`.
 *
 * That database is built offline from Android's per-carrier CarrierSettings
 * data, merged with AOSP's framework defaults and put through the vendor's own
 * value transforms. Every value it stores is therefore already the value the CP
 * is handed, so this file does one flat SELECT and no merging of its own.
 *
 * Seven tags are *not* in the database because the vendor does not read them
 * from carrier configuration either; they are compiled in here.
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
 * Resolve the configuration for `identity` out of `database_path`, with
 * `device` substituted into the carrier's User-Agent template.
 *
 * `identity` is the SIM's, not the serving network's, and the discriminators in
 * it matter: several carriers share an MCCMNC and are told apart only by SPN,
 * GID or an IMSI prefix.
 *
 * An unknown or absent identity is not an error: the database carries the same
 * carrier-neutral layer every per-carrier record is built on, and a CP with
 * those settings is worth trying. A missing or unreadable database is an error,
 * because the alternative is registering with another carrier's timers.
 */
MMSitImsConfig *mm_sit_ims_config_resolve (const gchar                  *database_path,
                                           const MMSitImsDeviceIdentity *device,
                                           const MMSitCarrierIdentity   *identity,
                                           GError                      **error);

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
