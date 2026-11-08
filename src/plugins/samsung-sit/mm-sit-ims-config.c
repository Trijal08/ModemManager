/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <sqlite3.h>
#include <string.h>

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-sit-ims-config.h"

/*
 * The tags this file names for itself. Everything else arrives from ims.db as a
 * number and is passed through without being understood here, which is the
 * point: the tag space, the AOSP keys behind it and the value transforms
 * between them all belong to the offline converter that builds the database,
 * not to ModemManager.
 */
enum {
    TAG_USER_AGENT                   = 519,
    /* Hardcoded by the vendor's RegistrationAdaptor, all five to 30 ms. */
    TAG_SIP_TIMER_TA_MS              = 523,
    TAG_SIP_TIMER_TE_MS              = 527,
    TAG_SIP_TIMER_TG_MS              = 529,
    TAG_SIP_TIMER_TI_MS              = 531,
    TAG_SIP_TIMER_TK_MS              = 533,
    TAG_UA_MANUFACTURER              = 772,
    TAG_UA_MODEL                     = 773,
    TAG_UA_SOFTWARE_VERSION          = 774,
    TAG_UA_PRODUCT_LINE              = 775,
    TAG_UA_OS_VERSION                = 776,
    TAG_GEOLOCATION_VOWIFI_EMERGENCY = 1044,
    TAG_GEOLOCATION_VOLTE_EMERGENCY  = 1045,
};

/*
 * Tags the CP is sent that are not carrier configuration and are in no
 * database, because the vendor does not read them from one either: the five SIP
 * timers are literals in RegistrationAdaptor, and the geolocation pair comes
 * from ShannonIms' built-in defaults after the AOSP key that would have fed
 * them is discarded. All seven were confirmed on the wire in a captured
 * HIDDEN_MENU frame.
 */
static const struct {
    guint16 tag;
    guint32 value;
} vendor_constants[] = {
    { TAG_SIP_TIMER_TA_MS, 30 },
    { TAG_SIP_TIMER_TE_MS, 30 },
    { TAG_SIP_TIMER_TG_MS, 30 },
    { TAG_SIP_TIMER_TI_MS, 30 },
    { TAG_SIP_TIMER_TK_MS, 30 },
    { TAG_GEOLOCATION_VOWIFI_EMERGENCY, 1 },
    { TAG_GEOLOCATION_VOLTE_EMERGENCY, 0 },
};

struct _MMSitImsConfig {
    GArray    *entries;   /* SamsungAimsConfigEntry, pointers into `owned` */
    GPtrArray *owned;     /* the buffers those entries borrow */
    gchar     *carrier;
    gchar     *user_agent;
};

/* Replace an already-emitted tag, or append. The database cannot produce a
 * duplicate -- (carrier, tag) is its primary key -- but the constants and the
 * device identity are applied on top of it and must not leave two entries for
 * one tag: the CP would take the second, which happens to be right, but only by
 * accident. */
static void
set_entry (MMSitImsConfig               *self,
           const SamsungAimsConfigEntry *entry)
{
    guint i;

    for (i = 0; i < self->entries->len; i++) {
        SamsungAimsConfigEntry *existing;

        existing = &g_array_index (self->entries, SamsungAimsConfigEntry, i);
        if (existing->tag == entry->tag) {
            *existing = *entry;
            return;
        }
    }
    g_array_append_vals (self->entries, entry, 1);
}

static void
set_int (MMSitImsConfig *self,
         guint16         tag,
         guint32         value)
{
    SamsungAimsConfigEntry e = { 0 };

    e.tag = tag;
    e.value_type = SAMSUNG_AIMS_VALUE_TYPE_INT;
    e.int_value = value;
    set_entry (self, &e);
}

static void
set_string (MMSitImsConfig *self,
            guint16         tag,
            const gchar    *value)
{
    SamsungAimsConfigEntry e = { 0 };
    gchar                 *owned;

    owned = g_strdup (value ? value : "");
    g_ptr_array_add (self->owned, owned);
    e.tag = tag;
    e.value_type = SAMSUNG_AIMS_VALUE_TYPE_STR;
    e.str_value = owned;
    set_entry (self, &e);
}

static void
set_bytes (MMSitImsConfig *self,
           guint16         tag,
           const guint8   *value,
           gsize           len)
{
    SamsungAimsConfigEntry e = { 0 };
    guint8                *owned;

    if (!len)
        return;

    owned = g_memdup2 (value, len);
    g_ptr_array_add (self->owned, owned);
    e.tag = tag;
    e.value_type = SAMSUNG_AIMS_VALUE_TYPE_BYTES;
    e.bytes_value = owned;
    e.bytes_len = len;
    set_entry (self, &e);
}

/*
 * One User-Agent field.
 *
 * Space is the field separator, so a value containing one silently becomes two
 * fields and the string stops matching the shape the CP was built for -- which
 * is what kept it from registering at all. Every value the vendor sends is a
 * single token ("Pixel9a", "CP2A.260705.006"), so ours become one too, joined
 * rather than run together.
 */
static gchar *
ua_token (const gchar *value)
{
    GString *out;

    out = g_string_new (value ?: "");
    g_string_replace (out, " ", "-", 0);
    return g_string_free (out, FALSE);
}

/*
 * Expand the carrier's User-Agent template.
 *
 * The template is the carrier's, placeholders and all
 * ("#MANUFACTURE# #MODEL# Android #AV# #BUILD# EU01454"), because its *shape*
 * is what the network is sensitive to -- the separators differ between
 * carriers, and one carrier's is underscores. Only the placeholders are ours to
 * fill.
 *
 * The literal "Android" is replaced too, and deliberately: we are not Android,
 * the field is one token either way so the shape is preserved, and the string
 * that registered on o2 named postmarketOS. It is substituted first, while the
 * template still holds no device values, so a device value containing the word
 * cannot be corrupted by it.
 */
static gchar *
expand_user_agent (const gchar                  *template,
                   const MMSitImsDeviceIdentity *device)
{
    const gchar      *model = device->model ?: "";
    const gchar      *vendor = device->manufacturer ?: "";
    gsize             vendor_len = strlen (vendor);
    g_autofree gchar *model_token = NULL;
    g_autofree gchar *build_token = NULL;
    g_autofree gchar *os_token = NULL;
    GString          *out;

    /* The device tree names the model with its vendor already in it ("Google
     * Pixel 9a") while the template wants them separately, so a repeated vendor
     * is dropped rather than emitted twice. */
    if (vendor_len && g_ascii_strncasecmp (model, vendor, vendor_len) == 0 &&
        model[vendor_len] == ' ')
        model += vendor_len + 1;

    model_token = ua_token (model);
    build_token = ua_token (device->software_version);
    os_token = ua_token (device->os_name);

    out = g_string_new (template);
    g_string_replace (out, "Android", os_token, 0);
    g_string_replace (out, "#MANUFACTURE#", vendor, 0);
    g_string_replace (out, "#MODEL#", model_token, 0);
    g_string_replace (out, "#AV#", device->os_version ?: "", 0);
    g_string_replace (out, "#BUILD#", build_token, 0);
    return g_string_free (out, FALSE);
}

/* A record's field either says nothing, or has to agree with the SIM. */
static gboolean
field_matches (const gchar *required,
               const gchar *actual,
               gboolean     prefix)
{
    if (!required || !required[0])
        return TRUE;
    if (!actual || !actual[0])
        return FALSE;
    return prefix ? g_str_has_prefix (actual, required)
                  : (g_ascii_strcasecmp (required, actual) == 0);
}

/*
 * Which carrier's row set to read.
 *
 * ims.db carries the SIM identities from Android's carrier_list, where a record
 * is an MCCMNC plus any of SPN, IMSI prefix, GID1/2 or ICCID prefix. Several
 * carriers share an MCCMNC and are told apart only by those, so the rule is
 * Android's: every field a record *does* specify must agree with the SIM, and
 * among the records that survive, the one specifying the most wins. A record
 * asking for nothing beyond the MCCMNC is therefore the fallback for that
 * MCCMNC rather than a competitor to a more specific one.
 */
static gint64
find_carrier (sqlite3                    *db,
              const MMSitCarrierIdentity *identity,
              gchar                     **out_name)
{
    static const gchar *sql =
        "SELECT c.id, c.canonical_name, i.spn, i.imsi, i.gid1, i.gid2, i.iccid "
        "FROM carrier c JOIN carrier_id i ON i.carrier = c.id "
        "WHERE i.mccmnc = ?1";
    sqlite3_stmt *statement = NULL;
    gint64        best_id = -1;
    gint          best_score = -1;

    if (!identity || !identity->mccmnc || !identity->mccmnc[0])
        return -1;
    if (sqlite3_prepare_v2 (db, sql, -1, &statement, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text (statement, 1, identity->mccmnc, -1, SQLITE_STATIC);
    while (sqlite3_step (statement) == SQLITE_ROW) {
        const gchar *spn = (const gchar *) sqlite3_column_text (statement, 2);
        const gchar *imsi = (const gchar *) sqlite3_column_text (statement, 3);
        const gchar *gid1 = (const gchar *) sqlite3_column_text (statement, 4);
        const gchar *gid2 = (const gchar *) sqlite3_column_text (statement, 5);
        const gchar *iccid = (const gchar *) sqlite3_column_text (statement, 6);
        gint         score = 0;
        guint        i;
        const struct {
            const gchar *required;
            const gchar *actual;
            gboolean     prefix;
        } fields[] = {
            { spn,   identity->spn,   FALSE },
            { imsi,  identity->imsi,  TRUE  },
            { gid1,  identity->gid1,  TRUE  },
            { gid2,  identity->gid2,  TRUE  },
            { iccid, identity->iccid, TRUE  },
        };

        for (i = 0; i < G_N_ELEMENTS (fields); i++) {
            if (!field_matches (fields[i].required, fields[i].actual, fields[i].prefix)) {
                score = -1;
                break;
            }
            if (fields[i].required && fields[i].required[0])
                score++;
        }
        if (score > best_score) {
            best_score = score;
            best_id = sqlite3_column_int64 (statement, 0);
            g_free (*out_name);
            *out_name = g_strdup ((const gchar *) sqlite3_column_text (statement, 1));
        }
    }
    sqlite3_finalize (statement);
    return best_id;
}

static gint64
find_default_carrier (sqlite3  *db,
                      gchar   **out_name)
{
    static const gchar *sql = "SELECT id FROM carrier WHERE canonical_name = 'default'";
    sqlite3_stmt       *statement = NULL;
    gint64              id = -1;

    if (sqlite3_prepare_v2 (db, sql, -1, &statement, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step (statement) == SQLITE_ROW) {
        id = sqlite3_column_int64 (statement, 0);
        *out_name = g_strdup ("default");
    }
    sqlite3_finalize (statement);
    return id;
}

/* Returns the carrier's User-Agent template, or NULL: it is the one value the
 * caller has to assemble rather than pass through. */
static gchar *
load_settings (MMSitImsConfig *self,
               sqlite3        *db,
               gint64          carrier)
{
    static const gchar *sql =
        "SELECT tag, kind, int_value, text_value, blob_value FROM setting "
        "WHERE carrier = ?1 ORDER BY tag";
    sqlite3_stmt *statement = NULL;
    gchar        *user_agent = NULL;

    if (sqlite3_prepare_v2 (db, sql, -1, &statement, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64 (statement, 1, carrier);
    while (sqlite3_step (statement) == SQLITE_ROW) {
        guint16      tag = (guint16) sqlite3_column_int (statement, 0);
        const gchar *kind = (const gchar *) sqlite3_column_text (statement, 1);

        if (!kind)
            continue;
        if (tag == TAG_USER_AGENT) {
            user_agent = g_strdup ((const gchar *) sqlite3_column_text (statement, 3));
            continue;
        }
        if (g_str_equal (kind, "int"))
            set_int (self, tag, (guint32) sqlite3_column_int64 (statement, 2));
        else if (g_str_equal (kind, "str"))
            set_string (self, tag, (const gchar *) sqlite3_column_text (statement, 3));
        else if (g_str_equal (kind, "bytes"))
            set_bytes (self, tag, sqlite3_column_blob (statement, 4),
                       sqlite3_column_bytes (statement, 4));
    }
    sqlite3_finalize (statement);
    return user_agent;
}

MMSitImsConfig *
mm_sit_ims_config_resolve (const gchar                  *database_path,
                           const MMSitImsDeviceIdentity *device,
                           const MMSitCarrierIdentity   *identity,
                           GError                      **error)
{
    MMSitImsConfig   *self;
    sqlite3          *db = NULL;
    gint64            carrier;
    g_autofree gchar *template = NULL;
    guint             i;

    g_return_val_if_fail (device != NULL, NULL);

    if (!database_path || !database_path[0]) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                             "no IMS configuration database configured");
        return NULL;
    }
    if (sqlite3_open_v2 (database_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "could not open the IMS configuration database '%s': %s",
                     database_path, sqlite3_errmsg (db));
        sqlite3_close (db);
        return NULL;
    }

    self = g_new0 (MMSitImsConfig, 1);
    self->entries = g_array_new (FALSE, TRUE, sizeof (SamsungAimsConfigEntry));
    self->owned = g_ptr_array_new_with_free_func (g_free);

    /* An unrecognised SIM is not an error: the database carries the same
     * carrier-neutral layer every per-carrier record is built on, and a CP with
     * those settings is worth trying. */
    carrier = find_carrier (db, identity, &self->carrier);
    if (carrier < 0)
        carrier = find_default_carrier (db, &self->carrier);
    if (carrier < 0) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "the IMS configuration database '%s' has no record for '%s' "
                     "and no default", database_path,
                     (identity && identity->mccmnc) ? identity->mccmnc : "(unknown operator)");
        sqlite3_close (db);
        mm_sit_ims_config_free (self);
        return NULL;
    }

    template = load_settings (self, db, carrier);
    sqlite3_close (db);

    for (i = 0; i < G_N_ELEMENTS (vendor_constants); i++)
        set_int (self, vendor_constants[i].tag, vendor_constants[i].value);

    /*
     * The device identity, and the User-Agent assembled from it. The CP wants
     * both: the parts, and the finished string it puts in REGISTER -- it does
     * not assemble one for itself, which is why an unprovisioned CP cannot
     * register at all.
     */
    set_string (self, TAG_UA_MANUFACTURER, device->manufacturer);
    set_string (self, TAG_UA_MODEL, device->model);
    set_string (self, TAG_UA_SOFTWARE_VERSION, device->software_version);
    set_string (self, TAG_UA_PRODUCT_LINE, "SH000000");
    set_string (self, TAG_UA_OS_VERSION, device->os_version);

    self->user_agent = expand_user_agent (
        template ?: "#MANUFACTURE# #MODEL# Android #AV# #BUILD#", device);
    set_string (self, TAG_USER_AGENT, self->user_agent);

    return self;
}

void
mm_sit_ims_config_free (MMSitImsConfig *self)
{
    if (!self)
        return;
    g_array_unref (self->entries);
    g_ptr_array_unref (self->owned);
    g_free (self->carrier);
    g_free (self->user_agent);
    g_free (self);
}

const SamsungAimsConfigEntry *
mm_sit_ims_config_peek_entries (const MMSitImsConfig *self,
                                gsize                *out_n_entries)
{
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (out_n_entries != NULL, NULL);

    *out_n_entries = self->entries->len;
    return (const SamsungAimsConfigEntry *) self->entries->data;
}

const gchar *
mm_sit_ims_config_get_carrier (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->carrier;
}

const gchar *
mm_sit_ims_config_get_user_agent (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->user_agent;
}

/*****************************************************************************/
/* The carrier's APN list */

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
    static const gchar *sql =
        "SELECT name, apn, types, bearers, protocol, roaming "
        "FROM apn WHERE carrier = ?1 ORDER BY seq";
    sqlite3          *db = NULL;
    sqlite3_stmt     *statement = NULL;
    g_autofree gchar *carrier_name = NULL;
    GPtrArray        *apns;
    gint64            carrier;

    if (!database_path || !database_path[0]) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                             "no IMS configuration database configured");
        return NULL;
    }
    if (sqlite3_open_v2 (database_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "could not open the IMS configuration database '%s': %s",
                     database_path, sqlite3_errmsg (db));
        sqlite3_close (db);
        return NULL;
    }

    /* No default fallback here, unlike the settings: the carrier-neutral layer
     * has no APNs of its own, and registering another carrier's APNs would be
     * worse than registering none. */
    carrier = find_carrier (db, identity, &carrier_name);
    if (carrier < 0) {
        sqlite3_close (db);
        return NULL;
    }

    if (sqlite3_prepare_v2 (db, sql, -1, &statement, NULL) != SQLITE_OK) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "could not read the APN list for '%s': %s",
                     carrier_name, sqlite3_errmsg (db));
        sqlite3_close (db);
        return NULL;
    }
    sqlite3_bind_int64 (statement, 1, carrier);

    apns = g_ptr_array_new_with_free_func ((GDestroyNotify) apn_free);
    while (sqlite3_step (statement) == SQLITE_ROW) {
        const gchar *name = (const gchar *) sqlite3_column_text (statement, 0);
        const gchar *apn  = (const gchar *) sqlite3_column_text (statement, 1);
        MMSitApn    *item;

        if (!apn || !apn[0])
            continue;
        item = g_new0 (MMSitApn, 1);
        item->name     = g_strdup (name);
        item->apn      = g_strdup (apn);
        item->types    = (guint32) sqlite3_column_int64 (statement, 2);
        item->bearers  = (guint32) sqlite3_column_int64 (statement, 3);
        item->protocol = (guint8) sqlite3_column_int (statement, 4);
        item->roaming  = (guint8) sqlite3_column_int (statement, 5);
        g_ptr_array_add (apns, item);
    }
    sqlite3_finalize (statement);
    sqlite3_close (db);

    if (!apns->len) {
        g_ptr_array_unref (apns);
        return NULL;
    }
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
