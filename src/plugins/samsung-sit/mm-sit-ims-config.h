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

/*
 * Which of the vendor's three HIDDEN_MENU pushes to take. Stock sends its
 * configuration as three messages and the CP does not emit its registration
 * indication while the link is idle, so the three writes are part of the
 * behaviour rather than a detail of packaging.
 */
typedef enum {
    MM_SIT_IMS_CONFIG_GROUP_FULL     = 0,
    MM_SIT_IMS_CONFIG_GROUP_IDENTITY = 1,
    MM_SIT_IMS_CONFIG_GROUP_CARRIER  = 2,
    MM_SIT_IMS_CONFIG_GROUP_LAST,
} MMSitImsConfigGroup;

/* The entries of one push, in the order they should reach the CP. */
const SamsungAimsConfigEntry *mm_sit_ims_config_peek_group (const MMSitImsConfig *self,
                                                            MMSitImsConfigGroup   group,
                                                            gsize                *out_n_entries);

/* The entries, in the order they should reach the CP. Owned by `self`. */
const SamsungAimsConfigEntry *mm_sit_ims_config_peek_entries (const MMSitImsConfig *self,
                                                              gsize                *out_n_entries);

/* Which carrier's overrides were applied, for logging. "default" if none. */
const gchar *mm_sit_ims_config_get_carrier (const MMSitImsConfig *self);

/* The assembled User-Agent, for logging -- it is the one value a network is
 * most likely to reject us over, so it belongs in the log. */
const gchar *mm_sit_ims_config_get_user_agent (const MMSitImsConfig *self);

/* The emergency location's presence URI and ISO country, for
 * SET_GEO_LOCATION_INFO. Either may be empty. Owned by `self`. */
const gchar *mm_sit_ims_config_get_geo_entity  (const MMSitImsConfig *self);
const gchar *mm_sit_ims_config_get_geo_country (const MMSitImsConfig *self);

/*
 * The carrier's APN list, from the same database and resolved for the same SIM.
 *
 * It lives here rather than in a file of its own because resolving *which*
 * carrier a SIM belongs to is Android's scoring rule over carrier_list, and
 * that is already implemented here -- duplicating it to keep the file names
 * tidy would be the worse trade.
 *
 * The list is not an IMS detail even though it shares this database: the CP
 * will not match a URSP rule for an APN it was never given a data profile for,
 * and answers MATCH_ALL_RULE_NOT_ALLOWED (2253) instead. So the whole list has
 * to be registered before any one context is asked for, and the entry marked
 * IA is the APN to attach with. That is what Android does, confirmed on the
 * wire against this CP.
 *
 * Values are already in the CP's own numbering; the database build does the
 * conversion from CarrierSettings' enums.
 */
typedef struct {
    gchar   *name;      /* the vendor's label, for logging */
    gchar   *apn;
    guint32  types;     /* MM_SIT_APN_TYPE_* bitmask */
    guint32  bearers;   /* 1 << (radio tech - 1), Android's framework
                         * encoding. The CP's SET_DATA_PROFILE field at @0xee
                         * is bit n instead, so shift left by one for the wire.
                         * Zero is not "any": it declares a profile valid on no
                         * radio technology, and the IMS PDN is then refused
                         * with MATCH_ALL_RULE_NOT_ALLOWED (2253). */
    guint8   protocol;  /* MM_SIT_PDP_TYPE_* */
    guint8   roaming;
} MMSitApn;

/*
 * Resolve the list, in the vendor's own order, which is the order it should
 * reach the CP in. NULL with no error set when the SIM resolves to a carrier
 * that ships no APNs -- a normal outcome, not a failure.
 */
GPtrArray *mm_sit_carrier_apns_resolve (const gchar                *database_path,
                                        const MMSitCarrierIdentity *identity,
                                        GError                    **error);

/* The first entry carrying every bit of @types, or NULL. */
const MMSitApn *mm_sit_carrier_apns_find (GPtrArray *apns,
                                          guint32    types);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitImsConfig, mm_sit_ims_config_free)

#endif /* MM_SIT_IMS_CONFIG_H */
