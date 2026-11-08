/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include "mm-sit-carrier-config.h"

/*
 * A GLib face on the carrier-manifest resolver in libsamsung_sit_ffi.
 *
 * Which manifest a SIM gets is the vendor's decision, reproduced from its own
 * resolver: four fallback tiers, a ranking between them, and one special case
 * for a French IIN. Getting it wrong does not fail loudly -- the CP simply
 * applies another carrier's rules -- so the logic is kept next to the database
 * it reads and the frames it feeds, and this file only converts arguments and
 * errors.
 */

void
mm_sit_carrier_config_free (MMSitCarrierConfig *self)
{
    if (!self)
        return;

    g_free (self->carrier_name);
    g_free (self->manifest_sha1);
    g_free (self->manifest_path);
    g_free (self);
}

gboolean
mm_sit_carrier_config_resolve (const gchar                 *database_path,
                               const MMSitCarrierIdentity  *identity,
                               MMSitCarrierConfig         **out_config,
                               GError                     **error)
{
    SamsungCarrierIdentity  sim = { 0 };
    SamsungCarrierConfig   *handle = NULL;
    MMSitCarrierConfig     *config;
    const guint8           *sha1;
    SamsungSitStatus        status;

    g_return_val_if_fail (database_path != NULL, FALSE);
    g_return_val_if_fail (identity != NULL, FALSE);
    g_return_val_if_fail (out_config != NULL, FALSE);

    *out_config = NULL;

    sim.imsi                  = identity->imsi;
    sim.iccid                 = identity->iccid;
    sim.mccmnc                = identity->mccmnc;
    sim.spn                   = identity->spn;
    sim.gid1                  = identity->gid1;
    sim.gid2                  = identity->gid2;
    sim.privilege_access_rule = identity->privilege_access_rule;

    status = samsung_carrier_config_resolve (database_path, &sim, &handle);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        const gchar *message = samsung_sit_last_error_message ();

        /* The caller distinguishes "this SIM has nothing to configure" from a
         * broken installation, so the status has to survive the conversion. */
        g_set_error_literal (error, G_IO_ERROR,
                             status == SAMSUNG_SIT_STATUS_ARG_OUT_OF_RANGE ?
                             G_IO_ERROR_INVALID_ARGUMENT : G_IO_ERROR_NOT_FOUND,
                             message ? message : "Could not resolve a Samsung carrier manifest");
        return FALSE;
    }

    config = g_new0 (MMSitCarrierConfig, 1);
    config->carrier_id    = samsung_carrier_config_id (handle);
    config->carrier_name  = g_strdup (samsung_carrier_config_name (handle));
    config->manifest_sha1 = g_strdup (samsung_carrier_config_sha1 (handle));
    config->manifest_path = g_strdup (samsung_carrier_config_path (handle));
    sha1 = samsung_carrier_config_sha1_bytes (handle);
    memcpy (config->manifest_sha1_bytes, sha1, sizeof (config->manifest_sha1_bytes));
    samsung_carrier_config_free (handle);

    *out_config = config;
    return TRUE;
}
