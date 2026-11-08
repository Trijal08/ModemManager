/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-sit-ims-config.h"

/*
 * A GLib face on the vendor data resolver in libsamsung_sit_ffi.
 *
 * The database behind it is the vendor's: its schema, its tag space, the value
 * transforms Android's carrier configuration goes through on the way in, and
 * the match rule that tells two carriers sharing an MCCMNC apart. None of that
 * is ModemManager's business, so none of it is here -- this file converts
 * arguments and errors, and nothing else. What the CP is sent is decided by
 * the library, which is also what builds the frames from these entries.
 */

struct _MMSitImsConfig {
    SamsungImsConfig *handle;
};

static void
set_ffi_error (GError **error, const gchar *what)
{
    const gchar *message = samsung_sit_last_error_message ();

    g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "%s: %s", what,
                 message ? message : "unknown error");
}

static SamsungCarrierIdentity
ffi_identity (const MMSitCarrierIdentity *identity)
{
    SamsungCarrierIdentity out = { 0 };

    if (identity) {
        out.mccmnc = identity->mccmnc;
        out.spn    = identity->spn;
        out.imsi   = identity->imsi;
        out.gid1   = identity->gid1;
        out.gid2   = identity->gid2;
        out.iccid  = identity->iccid;
        out.has_isim = identity->has_isim ? 1 : 0;
        out.isim_domain = identity->isim_domain;
    }
    return out;
}

MMSitImsConfig *
mm_sit_ims_config_resolve (const gchar                  *database_path,
                           const MMSitImsDeviceIdentity *device,
                           const MMSitCarrierIdentity   *identity,
                           GError                      **error)
{
    SamsungCarrierIdentity  sim;
    SamsungImsDeviceIdentity   dev;
    SamsungImsConfig          *handle = NULL;
    MMSitImsConfig            *self;
    SamsungSitStatus           status;

    g_return_val_if_fail (device != NULL, NULL);

    dev.manufacturer     = device->manufacturer;
    dev.model            = device->model;
    dev.os_name          = device->os_name;
    dev.os_version       = device->os_version;
    dev.software_version = device->software_version;
    sim = ffi_identity (identity);

    status = samsung_ims_config_resolve (database_path, &dev, &sim, &handle);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, "could not resolve the IMS configuration");
        return NULL;
    }

    self = g_new0 (MMSitImsConfig, 1);
    self->handle = handle;
    return self;
}

void
mm_sit_ims_config_free (MMSitImsConfig *self)
{
    if (!self)
        return;
    samsung_ims_config_free (self->handle);
    g_free (self);
}

const SamsungAimsConfigEntry *
mm_sit_ims_config_peek_entries (const MMSitImsConfig *self,
                                gsize                *out_n_entries)
{
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (out_n_entries != NULL, NULL);

    return samsung_ims_config_entries (self->handle, out_n_entries);
}

const SamsungAimsConfigEntry *
mm_sit_ims_config_peek_group (const MMSitImsConfig *self,
                              MMSitImsConfigGroup   group,
                              gsize                *out_n_entries)
{
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (out_n_entries != NULL, NULL);
    g_return_val_if_fail (group < MM_SIT_IMS_CONFIG_GROUP_LAST, NULL);

    return samsung_ims_config_group (self->handle,
                                     (SamsungImsConfigGroup) group,
                                     out_n_entries);
}

const gchar *
mm_sit_ims_config_get_carrier (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);

    return samsung_ims_config_carrier (self->handle);
}

const gchar *
mm_sit_ims_config_get_user_agent (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);

    return samsung_ims_config_user_agent (self->handle);
}

const gchar *
mm_sit_ims_config_get_geo_entity (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return samsung_ims_config_geo_entity (self->handle);
}

const gchar *
mm_sit_ims_config_get_geo_country (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return samsung_ims_config_geo_country (self->handle);
}

static void
apn_free (MMSitApn *apn)
{
    g_free (apn->name);
    g_free (apn->apn);
    g_free (apn);
}

GPtrArray *
mm_sit_carrier_apns_resolve (const gchar                *database_path,
                             const MMSitCarrierIdentity *identity,
                             GError                    **error)
{
    SamsungCarrierIdentity  sim = ffi_identity (identity);
    SamsungImsApnList         *list = NULL;
    const SamsungImsApn       *rows;
    GPtrArray                 *apns;
    gsize                      n = 0, i;
    SamsungSitStatus           status;

    status = samsung_ims_apns_resolve (database_path, &sim, &list);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, "could not resolve the carrier's APNs");
        return NULL;
    }

    rows = samsung_ims_apn_list_peek (list, &n);
    /* No APNs is not a failure: it is a SIM whose carrier ships none, and the
     * caller's answer to that is the same as its answer to an unknown SIM. */
    if (!n) {
        samsung_ims_apn_list_free (list);
        return NULL;
    }

    apns = g_ptr_array_new_full (n, (GDestroyNotify) apn_free);
    for (i = 0; i < n; i++) {
        MMSitApn *item = g_new0 (MMSitApn, 1);

        item->name     = g_strdup (rows[i].name);
        item->apn      = g_strdup (rows[i].apn);
        item->types    = rows[i].types;
        item->bearers  = rows[i].bearers;
        item->protocol = rows[i].protocol;
        item->roaming  = rows[i].roaming;
        g_ptr_array_add (apns, item);
    }
    samsung_ims_apn_list_free (list);
    return apns;
}

const MMSitApn *
mm_sit_carrier_apns_find (GPtrArray *apns,
                          guint32    types)
{
    guint i;

    if (!apns || !types)
        return NULL;
    for (i = 0; i < apns->len; i++) {
        const MMSitApn *apn = g_ptr_array_index (apns, i);

        if ((apn->types & types) == types)
            return apn;
    }
    return NULL;
}
