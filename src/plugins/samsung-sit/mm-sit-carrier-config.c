/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <sqlite3.h>
#include <string.h>

#include "mm-sit-carrier-config.h"

typedef struct {
    guint   carrier_id;
    gint64  rowid;
    gchar  *name;
    gchar  *hash;
    gchar  *mccmnc;
    gchar  *imsi_pattern;
    gchar  *spn;
    gchar  *gid1;
    gchar  *gid2;
    gchar  *iccid_prefix;
    guint   rank;
    guint   imsi_strength;
} CarrierCandidate;

static void
carrier_candidate_clear (CarrierCandidate *candidate)
{
    g_free (candidate->name);
    g_free (candidate->hash);
    g_free (candidate->mccmnc);
    g_free (candidate->imsi_pattern);
    g_free (candidate->spn);
    g_free (candidate->gid1);
    g_free (candidate->gid2);
    g_free (candidate->iccid_prefix);
    memset (candidate, 0, sizeof (*candidate));
}

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

static gchar *
column_dup (sqlite3_stmt *statement,
            gint          column)
{
    const guchar *value;

    value = sqlite3_column_text (statement, column);
    return value ? g_strdup ((const gchar *) value) : NULL;
}

static gboolean
is_wildcard (const gchar *value)
{
    return (!value || !value[0] || g_str_equal (value, "%"));
}

static gboolean
like_match (const gchar *value,
            const gchar *pattern)
{
    const gchar *v;
    const gchar *p;
    const gchar *star = NULL;
    const gchar *retry = NULL;

    if (is_wildcard (pattern))
        return TRUE;
    if (!value)
        return FALSE;

    v = value;
    p = pattern;
    while (*v) {
        if (*p == '_') {
            p++;
            v++;
        } else if (*p == '%') {
            star = ++p;
            retry = v;
        } else if (*p == *v) {
            p++;
            v++;
        } else if (star) {
            p = star;
            v = ++retry;
        } else
            return FALSE;
    }
    while (*p == '%')
        p++;
    return (*p == '\0');
}

static gboolean
candidate_matches (CarrierCandidate          *candidate,
                   const MMSitCarrierIdentity *identity)
{
    gboolean gid1_matches;
    gboolean gid2_matches;
    gboolean orange_france_iin;
    gboolean spn_matches;

    if (!identity->imsi || !like_match (identity->imsi, candidate->imsi_pattern))
        return FALSE;

    /* The vendor query applies this ICCID constraint only for this IIN/MCCMNC. */
    orange_france_iin = (identity->iccid &&
                         g_str_has_prefix (identity->iccid, "894936") &&
                         candidate->mccmnc &&
                         g_str_equal (candidate->mccmnc, "20801"));
    if (orange_france_iin &&
        (!candidate->iccid_prefix || !like_match ("894936", candidate->iccid_prefix)))
        return FALSE;

    spn_matches = (identity->spn && candidate->spn &&
                   g_str_equal (identity->spn, candidate->spn));
    gid1_matches = (identity->gid1 && candidate->gid1 &&
                    g_ascii_strcasecmp (identity->gid1, candidate->gid1) == 0);
    gid2_matches = (identity->gid2 && candidate->gid2 &&
                    g_ascii_strcasecmp (identity->gid2, candidate->gid2) == 0);

    /*
     * Each vendor UNION branch deliberately ignores the other discriminators;
     * e.g. a row with a mismatching SPN may still win its GID1+GID2 branch.
     */
    if (spn_matches && gid1_matches && gid2_matches)
        candidate->rank = 0;
    else if (gid1_matches && gid2_matches)
        candidate->rank = 1;
    else if (spn_matches && gid1_matches)
        candidate->rank = 2;
    else if (spn_matches && gid2_matches)
        candidate->rank = 3;
    else if (gid1_matches)
        candidate->rank = 4;
    else if (gid2_matches)
        candidate->rank = 5;
    else if (spn_matches)
        candidate->rank = 6;
    else if (g_strcmp0 (candidate->spn, "%") == 0 &&
             g_strcmp0 (candidate->gid1, "%") == 0 &&
             g_strcmp0 (candidate->gid2, "%") == 0)
        candidate->rank = 7;
    else
        return FALSE;

    candidate->imsi_strength = is_wildcard (candidate->imsi_pattern) ? 0 : 1;
    return TRUE;
}

static gboolean
candidate_is_better (const CarrierCandidate *candidate,
                     const CarrierCandidate *current)
{
    gsize candidate_mccmnc_len;
    gsize current_mccmnc_len;

    if (!current->hash)
        return TRUE;
    if (candidate->rank != current->rank)
        return candidate->rank < current->rank;
    if (candidate->imsi_strength != current->imsi_strength)
        return candidate->imsi_strength > current->imsi_strength;

    candidate_mccmnc_len = candidate->mccmnc ? strlen (candidate->mccmnc) : 0;
    current_mccmnc_len = current->mccmnc ? strlen (current->mccmnc) : 0;
    if (candidate_mccmnc_len != current_mccmnc_len)
        return candidate_mccmnc_len > current_mccmnc_len;
    return candidate->rowid < current->rowid;
}

static gboolean
prepare (sqlite3       *database,
         const gchar   *sql,
         sqlite3_stmt **out_statement,
         GError       **error)
{
    gint result;

    result = sqlite3_prepare_v2 (database, sql, -1, out_statement, NULL);
    if (result == SQLITE_OK)
        return TRUE;

    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "Invalid Samsung carrier database schema: %s",
                 sqlite3_errmsg (database));
    return FALSE;
}

static gboolean
resolve_privilege_rule (sqlite3                     *database,
                        const MMSitCarrierIdentity  *identity,
                        CarrierCandidate            *selected,
                        GError                     **error)
{
    static const gchar sql[] =
        "SELECT ci.rowid,ci.carrier_id,cm.confman,"
        "COALESCE(n.name,pn.name,'') "
        "FROM carrier_info ci "
        "INNER JOIN confmap cm ON cm.carrier_id=CAST(ci.carrier_id AS TEXT) "
        "LEFT JOIN confnames n ON n.carrier_id=ci.carrier_id "
        "LEFT JOIN confnames pn ON pn.carrier_id=CAST(cm.parent_id AS INTEGER) "
        "WHERE UPPER(ci.privelege_access_rule)=UPPER(?1) "
        "ORDER BY ci.rowid LIMIT 1";
    sqlite3_stmt *statement = NULL;
    gint          result;

    if (!identity->privilege_access_rule || !identity->privilege_access_rule[0])
        return TRUE;
    if (!prepare (database, sql, &statement, error))
        return FALSE;

    sqlite3_bind_text (statement, 1, identity->privilege_access_rule, -1, SQLITE_TRANSIENT);
    result = sqlite3_step (statement);
    if (result == SQLITE_ROW) {
        selected->rowid = sqlite3_column_int64 (statement, 0);
        selected->carrier_id = sqlite3_column_int (statement, 1);
        selected->hash = column_dup (statement, 2);
        selected->name = column_dup (statement, 3);
    } else if (result != SQLITE_DONE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to query Samsung carrier privilege rule: %s",
                     sqlite3_errmsg (database));
        sqlite3_finalize (statement);
        return FALSE;
    }

    sqlite3_finalize (statement);
    return TRUE;
}

static gboolean
resolve_ranked_carrier (sqlite3                     *database,
                        const MMSitCarrierIdentity  *identity,
                        CarrierCandidate            *selected,
                        GError                     **error)
{
    static const gchar sql[] =
        "SELECT ci.rowid,ci.carrier_id,ci.mccmnc,ci.imsi_prefix_xpattern,"
        "ci.spn,ci.gid1,ci.gid2,ci.iccid_prefix,cm.confman,"
        "COALESCE(n.name,pn.name,'') "
        "FROM carrier_info ci "
        "INNER JOIN confmap cm ON cm.carrier_id=CAST(ci.carrier_id AS TEXT) "
        "LEFT JOIN confnames n ON n.carrier_id=ci.carrier_id "
        "LEFT JOIN confnames pn ON pn.carrier_id=CAST(cm.parent_id AS INTEGER) "
        "WHERE ((?1 IS NOT NULL AND ci.mccmnc=?1) OR "
        "       (?1 IS NULL AND SUBSTR(?2,1,LENGTH(ci.mccmnc))=ci.mccmnc)) "
        "ORDER BY ci.rowid";
    sqlite3_stmt    *statement = NULL;
    CarrierCandidate candidate = { 0 };
    gint             result;

    /* A matching SIM privilege rule is the vendor resolver's first choice. */
    if (selected->hash || !identity->imsi || !identity->imsi[0])
        return TRUE;
    if (!prepare (database, sql, &statement, error))
        return FALSE;

    if (identity->mccmnc && identity->mccmnc[0])
        sqlite3_bind_text (statement, 1, identity->mccmnc, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null (statement, 1);
    sqlite3_bind_text (statement, 2, identity->imsi, -1, SQLITE_TRANSIENT);

    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        candidate.rowid = sqlite3_column_int64 (statement, 0);
        candidate.carrier_id = sqlite3_column_int (statement, 1);
        candidate.mccmnc = column_dup (statement, 2);
        candidate.imsi_pattern = column_dup (statement, 3);
        candidate.spn = column_dup (statement, 4);
        candidate.gid1 = column_dup (statement, 5);
        candidate.gid2 = column_dup (statement, 6);
        candidate.iccid_prefix = column_dup (statement, 7);
        candidate.hash = column_dup (statement, 8);
        candidate.name = column_dup (statement, 9);

        if (candidate_matches (&candidate, identity) &&
            candidate_is_better (&candidate, selected)) {
            carrier_candidate_clear (selected);
            *selected = candidate;
            memset (&candidate, 0, sizeof (candidate));
        }
        carrier_candidate_clear (&candidate);
    }

    if (result != SQLITE_DONE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to query Samsung carrier database: %s",
                     sqlite3_errmsg (database));
        sqlite3_finalize (statement);
        carrier_candidate_clear (&candidate);
        return FALSE;
    }

    sqlite3_finalize (statement);
    return TRUE;
}

static gboolean
resolve_iccid_fallback (sqlite3                     *database,
                        const MMSitCarrierIdentity  *identity,
                        CarrierCandidate            *selected,
                        GError                     **error)
{
    static const gchar sql[] =
        "SELECT i.carrier_id,i.iccid_prefix,cm.confman,"
        "COALESCE(n.name,pn.name,'') "
        "FROM iin i "
        "INNER JOIN confmap cm ON cm.carrier_id=i.carrier_id "
        "LEFT JOIN confnames n ON n.carrier_id=CAST(i.carrier_id AS INTEGER) "
        "LEFT JOIN confnames pn ON pn.carrier_id=CAST(cm.parent_id AS INTEGER)";
    sqlite3_stmt *statement = NULL;
    gint          result;
    gsize         selected_strength = 0;

    if (selected->hash || !identity->iccid || !identity->iccid[0])
        return TRUE;
    if (!prepare (database, sql, &statement, error))
        return FALSE;

    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        g_autofree gchar *pattern = column_dup (statement, 1);
        gsize             strength;

        if (!like_match (identity->iccid, pattern))
            continue;
        strength = pattern ? strcspn (pattern, "%_") : 0;
        if (selected->hash && strength <= selected_strength)
            continue;

        carrier_candidate_clear (selected);
        selected->carrier_id = (guint) g_ascii_strtoull ((const gchar *) sqlite3_column_text (statement, 0), NULL, 10);
        selected->hash = column_dup (statement, 2);
        selected->name = column_dup (statement, 3);
        selected_strength = strength;
    }

    if (result != SQLITE_DONE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to query Samsung carrier ICCID fallback: %s",
                     sqlite3_errmsg (database));
        sqlite3_finalize (statement);
        return FALSE;
    }

    sqlite3_finalize (statement);
    return TRUE;
}

static gboolean
resolve_global_fallback (sqlite3           *database,
                         CarrierCandidate  *selected,
                         GError           **error)
{
    static const gchar sql[] =
        "SELECT r.carrier_id,cm.confman,COALESCE(n.name,'') "
        "FROM regional_fallback r "
        "INNER JOIN confmap cm ON cm.carrier_id=r.carrier_id "
        "LEFT JOIN confnames n ON n.carrier_id=CAST(r.carrier_id AS INTEGER) "
        "WHERE CAST(r.country_code AS INTEGER)=0 "
        "ORDER BY CAST(r.carrier_id AS INTEGER) LIMIT 1";
    sqlite3_stmt *statement = NULL;
    gint          result;

    if (selected->hash)
        return TRUE;
    if (!prepare (database, sql, &statement, error))
        return FALSE;

    result = sqlite3_step (statement);
    if (result == SQLITE_ROW) {
        selected->carrier_id = (guint) g_ascii_strtoull ((const gchar *) sqlite3_column_text (statement, 0), NULL, 10);
        selected->hash = column_dup (statement, 1);
        selected->name = column_dup (statement, 2);
    } else if (result != SQLITE_DONE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to query Samsung global carrier fallback: %s",
                     sqlite3_errmsg (database));
        sqlite3_finalize (statement);
        return FALSE;
    }

    sqlite3_finalize (statement);
    return TRUE;
}

static gboolean
decode_sha1 (const gchar  *hex,
             guint8        output[20],
             GError      **error)
{
    guint i;

    if (!hex || strlen (hex) != 40) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "Carrier manifest identifier is not a 40-character SHA1");
        return FALSE;
    }

    for (i = 0; i < 20; i++) {
        gint high;
        gint low;

        high = g_ascii_xdigit_value (hex[i * 2]);
        low = g_ascii_xdigit_value (hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Carrier manifest identifier contains non-hexadecimal data");
            return FALSE;
        }
        output[i] = (high << 4) | low;
    }
    return TRUE;
}

gboolean
mm_sit_carrier_config_resolve (const gchar                 *database_path,
                               const MMSitCarrierIdentity  *identity,
                               MMSitCarrierConfig         **out_config,
                               GError                     **error)
{
    sqlite3                    *database = NULL;
    CarrierCandidate            selected = { 0 };
    g_autofree gchar           *directory = NULL;
    g_autoptr(MMSitCarrierConfig) config = NULL;
    gint                        result;

    g_return_val_if_fail (database_path != NULL, FALSE);
    g_return_val_if_fail (identity != NULL, FALSE);
    g_return_val_if_fail (out_config != NULL, FALSE);

    *out_config = NULL;
    if (!identity->imsi || !identity->imsi[0]) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Cannot resolve a Samsung carrier profile without an IMSI");
        return FALSE;
    }

    result = sqlite3_open_v2 (database_path, &database,
                              SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    if (result != SQLITE_OK) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Could not open Samsung carrier database %s: %s",
                     database_path, database ? sqlite3_errmsg (database) : "unknown error");
        if (database)
            sqlite3_close (database);
        return FALSE;
    }

    if (!resolve_privilege_rule (database, identity, &selected, error) ||
        !resolve_ranked_carrier (database, identity, &selected, error) ||
        !resolve_iccid_fallback (database, identity, &selected, error) ||
        !resolve_global_fallback (database, &selected, error)) {
        carrier_candidate_clear (&selected);
        sqlite3_close (database);
        return FALSE;
    }
    sqlite3_close (database);

    if (!selected.hash) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "No matching Samsung carrier manifest was found");
        carrier_candidate_clear (&selected);
        return FALSE;
    }

    config = g_new0 (MMSitCarrierConfig, 1);
    config->carrier_id = selected.carrier_id;
    config->carrier_name = g_steal_pointer (&selected.name);
    config->manifest_sha1 = g_ascii_strdown (selected.hash, -1);
    if (!decode_sha1 (config->manifest_sha1, config->manifest_sha1_bytes, error)) {
        carrier_candidate_clear (&selected);
        return FALSE;
    }

    directory = g_path_get_dirname (database_path);
    config->manifest_path = g_build_filename (directory, "manifests", config->manifest_sha1, NULL);
    if (!g_file_test (config->manifest_path, G_FILE_TEST_EXISTS)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     "Selected Samsung carrier manifest does not exist: %s",
                     config->manifest_path);
        carrier_candidate_clear (&selected);
        return FALSE;
    }

    carrier_candidate_clear (&selected);
    *out_config = g_steal_pointer (&config);
    return TRUE;
}
