/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_CARRIER_CONFIG_H
#define MM_SIT_CARRIER_CONFIG_H

#include <gio/gio.h>
#include <string.h>

#include <samsung_sit_ffi.h>

typedef struct {
    const gchar *imsi;
    const gchar *iccid;
    const gchar *mccmnc;
    const gchar *spn;
    const gchar *gid1;
    const gchar *gid2;
    const gchar *privilege_access_rule;
    /* Whether the card carries an ISIM application. Only the IMS settings
     * read it; the manifest resolver ignores it. */
    gboolean     has_isim;
    /* EF_DOMAIN as the ISIM states it, or NULL to derive the 3GPP form. */
    const gchar *isim_domain;
} MMSitCarrierIdentity;

typedef struct {
    guint   carrier_id;
    gchar  *carrier_name;
    gchar  *manifest_sha1;
    gchar  *manifest_path;
    guint8  manifest_sha1_bytes[20];
} MMSitCarrierConfig;

void     mm_sit_carrier_config_free    (MMSitCarrierConfig          *self);
gboolean mm_sit_carrier_config_resolve (const gchar                 *database_path,
                                        const MMSitCarrierIdentity  *identity,
                                        MMSitCarrierConfig         **out_config,
                                        GError                     **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitCarrierConfig, mm_sit_carrier_config_free)

#endif /* MM_SIT_CARRIER_CONFIG_H */
