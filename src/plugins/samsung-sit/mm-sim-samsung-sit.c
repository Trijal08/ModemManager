/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

/* The SIM object for the Samsung SIT plugin.
 *
 * MMBaseSim reads the card over AT: the ICCID, EF_AD, EF_SPN, EF_GID1/2 and
 * EF_ECC with +CRSM, the IMSI with +CIMI, and the preferred networks with
 * +CPLS/+CPOL. The CP serves all of it natively -- the identifiers come out of
 * the SIM status it already reports, and every elementary file is one
 * mm_sit_client_read_ef().
 *
 * What is *in* the files is plain 3GPP, so the decoding here is ModemManager's
 * own (mm_3gpp_parse_iccid, mm_sim_validate_mnc_length,
 * mm_sim_convert_spn_to_utf8, mm_3gpp_parse_emergency_numbers) rather than a
 * second implementation living in the plugin.
 *
 * Writes stay where they are: set_preferred_networks keeps the inherited +CPOL
 * path. Nothing calls it on its own, and this card has no EF_PLMNwAcT to write
 * -- the list it reports comes from the operator-controlled EF_OPLMNwAcT, which
 * a user-facing setter has no business rewriting.
 */

#include <config.h>
#include <string.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-bind.h"
#include "mm-broadband-modem-samsung-sit.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-sim-samsung-sit.h"

G_DEFINE_TYPE (MMSimSamsungSit, mm_sim_samsung_sit, MM_TYPE_BASE_SIM)

/* Every file read here lives under the USIM ADF; the one MF-level file the
 * generic path reads, EF_ICCID, is not read at all -- the CP reports the ICCID
 * in the SIM status. */
static const guint16 adf_path[] = { 0x3f00, 0x7fff };

#define EF_IMSI       0x6f07
#define EF_AD         0x6fad
#define EF_SPN        0x6f46
#define EF_GID1       0x6f3e
#define EF_GID2       0x6f3f
#define EF_ECC        0x6fb7
#define EF_PLMN_W_ACT 0x6f60
#define EF_OPLMN_W_ACT 0x6f61

/*****************************************************************************/

static MMSitClient *
peek_client (MMBaseSim *self,
             GError   **error)
{
    g_autoptr(MMBaseModem) modem = NULL;

    g_object_get (self, MM_BASE_SIM_MODEM, &modem, NULL);
    if (!modem || !MM_IS_BROADBAND_MODEM_SAMSUNG_SIT (modem)) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                             "the SIM is not owned by a Samsung SIT modem");
        return NULL;
    }
    return mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (modem),
                                                       error);
}

/* Start one elementary-file read, failing the task if the client is gone. */
static void
read_ef (MMBaseSim          *self,
         guint16             file_id,
         const guint16      *path,
         guint               path_len,
         GAsyncReadyCallback callback,
         GTask              *task)
{
    MMSitClient *client;
    GError      *error = NULL;

    client = peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_read_ef (client, file_id, path, path_len, NULL, callback, task);
}

/* Fetch the SIM status, which carries the identifiers the CP knows without any
 * file access. */
static void
get_sim_status (MMBaseSim          *self,
                GAsyncReadyCallback callback,
                GTask              *task)
{
    MMSitClient *client;
    GError      *error = NULL;

    client = peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_sim_status (client, NULL, callback, task);
}

/*****************************************************************************/
/* SIM identifier (ICCID)
 *
 * In the SIM-status reply, as the BCD the card returns; the generic path read
 * EF_ICCID with +CRSM and decoded the same bytes.
 */

static gchar *
load_sim_identifier_finish (MMBaseSim     *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_sim_identifier_ready (MMSitClient  *client,
                           GAsyncResult *res,
                           GTask        *task)
{
    g_autofree gchar *raw = NULL;
    gchar            *iccid;
    MMSitSimStatus    status;
    GError           *error = NULL;

    if (!mm_sit_client_get_sim_status_finish (client, res, &status, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!status.iccid_len) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "the SIM status carries no ICCID");
        g_object_unref (task);
        return;
    }

    raw = mm_utils_bin2hexstr (status.iccid, status.iccid_len);
    iccid = mm_3gpp_parse_iccid (raw, &error);
    if (!iccid)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, iccid, g_free);
    g_object_unref (task);
}

static void
load_sim_identifier (MMBaseSim          *self,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    get_sim_status (self, (GAsyncReadyCallback) load_sim_identifier_ready,
                    g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* IMSI
 *
 * EF_IMSI is a length byte then the digits in BCD with swapped nibbles, the
 * first of which is the odd/even parity flag rather than part of the identity.
 * This replaces +CIMI.
 */

static gchar *
load_imsi_finish (MMBaseSim     *self,
                  GAsyncResult  *res,
                  GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_imsi_ready (MMSitClient  *client,
                 GAsyncResult *res,
                 GTask        *task)
{
    g_autoptr(GBytes) file = NULL;
    g_autofree gchar *digits = NULL;
    const guint8     *data;
    gsize             len = 0;
    GError           *error = NULL;

    file = mm_sit_client_read_ef_finish (client, res, NULL, &error);
    if (!file) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    /* The length byte counts the BCD bytes that follow. */
    if (len < 2 || data[0] < 1 || data[0] > len - 1) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "EF_IMSI is malformed (%" G_GSIZE_FORMAT " bytes)", len);
        g_object_unref (task);
        return;
    }

    /* Swapped nibbles: the low one of each byte comes first. */
    digits = mm_bcd_to_string (data + 1, data[0], TRUE);
    if (!digits || strlen (digits) < 2) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "EF_IMSI holds no digits");
        g_object_unref (task);
        return;
    }

    /* Drop the parity nibble that precedes the identity. */
    g_task_return_pointer (task, g_strdup (digits + 1), g_free);
    g_object_unref (task);
}

static void
load_imsi (MMBaseSim          *self,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    read_ef (self, EF_IMSI, adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_imsi_ready,
             g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* EID
 *
 * Only an eUICC has one, and only in the SIM status: BCD, swapped nibbles, no
 * padding. The generic path has no way to read it at all.
 */

static gchar *
load_eid_finish (MMBaseSim     *self,
                 GAsyncResult  *res,
                 GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_eid_ready (MMSitClient  *client,
                GAsyncResult *res,
                GTask        *task)
{
    MMSitSimStatus  status;
    gchar          *eid;
    GError         *error = NULL;

    if (!mm_sit_client_get_sim_status_finish (client, res, &status, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!status.eid_len) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "the card reports no EID");
        g_object_unref (task);
        return;
    }

    eid = mm_decode_eid ((const gchar *) status.eid, status.eid_len);
    if (!eid)
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "the card reports a %u-byte EID", status.eid_len);
    else
        g_task_return_pointer (task, eid, g_free);
    g_object_unref (task);
}

static void
load_eid (MMBaseSim          *self,
          GAsyncReadyCallback callback,
          gpointer            user_data)
{
    get_sim_status (self, (GAsyncReadyCallback) load_eid_ready,
                    g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Operator identifier
 *
 * EF_AD says how many digits of the IMSI are the MNC; the operator id is that
 * prefix. Replaces the EF_AD +CRSM read.
 */

static gchar *
load_operator_identifier_finish (MMBaseSim     *self,
                                 GAsyncResult  *res,
                                 GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_operator_identifier_ready (MMSitClient  *client,
                                GAsyncResult *res,
                                GTask        *task)
{
    MMBaseSim         *self = g_task_get_source_object (task);
    g_autoptr(GBytes)  file = NULL;
    const guint8      *data;
    gsize              len = 0;
    guint              mnc_length;
    const gchar       *imsi;
    GError            *error = NULL;

    file = mm_sit_client_read_ef_finish (client, res, NULL, &error);
    if (!file) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    mnc_length = mm_sim_validate_mnc_length (data, len, &error);
    if (!mnc_length) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    imsi = mm_gdbus_sim_get_imsi (MM_GDBUS_SIM (self));
    if (!imsi) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "cannot build the operator id without the IMSI");
        g_object_unref (task);
        return;
    }

    g_task_return_pointer (task, g_strndup (imsi, 3 + mnc_length), g_free);
    g_object_unref (task);
}

static void
load_operator_identifier (MMBaseSim          *self,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    read_ef (self, EF_AD, adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_operator_identifier_ready,
             g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Operator name (service provider name), EF_SPN. */

static gchar *
load_operator_name_finish (MMBaseSim     *self,
                           GAsyncResult  *res,
                           GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_operator_name_ready (MMSitClient  *client,
                          GAsyncResult *res,
                          GTask        *task)
{
    g_autoptr(GBytes)  file = NULL;
    const guint8      *data;
    gsize              len = 0;
    gchar             *spn;
    GError            *error = NULL;

    file = mm_sit_client_read_ef_finish (client, res, NULL, &error);
    if (!file) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    spn = mm_sim_convert_spn_to_utf8 (data, len, &error);
    if (!spn)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, spn, g_free);
    g_object_unref (task);
}

static void
load_operator_name (MMBaseSim          *self,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    read_ef (self, EF_SPN, adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_operator_name_ready,
             g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Group identifiers, EF_GID1 and EF_GID2. Both are transparent files handed
 * back as they are. */

static GByteArray *
load_gid_finish (MMBaseSim     *self,
                 GAsyncResult  *res,
                 GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_gid_ready (MMSitClient  *client,
                GAsyncResult *res,
                GTask        *task)
{
    g_autoptr(GBytes)  file = NULL;
    const guint8      *data;
    gsize              len = 0;
    GByteArray        *gid;
    GError            *error = NULL;

    file = mm_sit_client_read_ef_finish (client, res, NULL, &error);
    if (!file) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    gid = g_byte_array_sized_new (len);
    g_byte_array_append (gid, data, len);
    g_task_return_pointer (task, gid, (GDestroyNotify) g_byte_array_unref);
    g_object_unref (task);
}

static void
load_gid1 (MMBaseSim          *self,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    read_ef (self, EF_GID1, adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_gid_ready,
             g_task_new (self, NULL, callback, user_data));
}

static void
load_gid2 (MMBaseSim          *self,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    read_ef (self, EF_GID2, adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_gid_ready,
             g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Emergency numbers, EF_ECC
 *
 * The generic path reads this as a transparent file, which this card refuses
 * (SW 6981): on a USIM, EF_ECC is a record file whose first three bytes are the
 * number and whose tail is an alpha identifier plus the service category. Those
 * first three bytes per record are exactly the layout ModemManager's parser
 * wants, so they are handed over on their own.
 */

#define EF_ECC_NUMBER_LEN 3

static GStrv
load_emergency_numbers_finish (MMBaseSim     *self,
                               GAsyncResult  *res,
                               GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_emergency_numbers_ready (MMSitClient  *client,
                              GAsyncResult *res,
                              GTask        *task)
{
    MMBaseSim         *self = g_task_get_source_object (task);
    g_autoptr(GBytes)  file = NULL;
    g_autoptr(GString) numbers = NULL;
    g_autofree gchar  *hex = NULL;
    MMSitEfInfo        info;
    const guint8      *data;
    gsize              len = 0;
    gsize              stride;
    gsize              offset;
    guint              i;
    GStrv              parsed;
    GError            *error = NULL;

    file = mm_sit_client_read_ef_finish (client, res, &info, &error);
    if (!file) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    stride = (info.structure == MM_SIT_EF_TRANSPARENT) ? EF_ECC_NUMBER_LEN : info.record_len;
    if (stride < EF_ECC_NUMBER_LEN) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "EF_ECC records are too short (%" G_GSIZE_FORMAT " bytes)",
                                 stride);
        g_object_unref (task);
        return;
    }

    numbers = g_string_new (NULL);
    for (offset = 0; offset + EF_ECC_NUMBER_LEN <= len; offset += stride) {
        g_autofree gchar *entry = NULL;

        entry = mm_utils_bin2hexstr (data + offset, EF_ECC_NUMBER_LEN);
        g_string_append (numbers, entry);
    }

    hex = g_string_free (g_steal_pointer (&numbers), FALSE);
    parsed = mm_3gpp_parse_emergency_numbers (hex, &error);
    if (!parsed) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    for (i = 0; parsed[i]; i++)
        mm_obj_dbg (self, "loaded emergency number: %s", parsed[i]);
    g_task_return_pointer (task, parsed, (GDestroyNotify) g_strfreev);
    g_object_unref (task);
}

static void
load_emergency_numbers (MMBaseSim          *self,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    read_ef (self, EF_ECC, adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_emergency_numbers_ready,
             g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Preferred networks
 *
 * EF_PLMNwAcT and EF_OPLMNwAcT hold five-byte entries: three bytes of PLMN in
 * the usual swapped BCD, then a big-endian access-technology bitmap. The
 * generic path read them indirectly with +CPLS/+CPOL.
 *
 * The user-controlled list is tried first, exactly as +CPLS=0 asks for, and the
 * operator-controlled one is the fallback: this card has no EF_PLMNwAcT at all
 * (SW 6A82) and everything it reports comes from EF_OPLMNwAcT.
 */

#define PLMN_W_ACT_ENTRY_LEN 5

/* Bits of the access-technology half of an entry, TS 31.102 4.2.5. Only UTRAN,
 * E-UTRAN and GSM appear in the corpus and on this card -- an entry reads
 * C0 80 and ModemManager reported it as gsm/umts/lte over +CPOL, which is what
 * pins those three down. NG-RAN is taken from the spec and has no entry here to
 * confirm it. */
#define PLMN_W_ACT_UTRAN       0x8000
#define PLMN_W_ACT_EUTRAN      0x4000
#define PLMN_W_ACT_NGRAN       0x2000
#define PLMN_W_ACT_GSM         0x0080
#define PLMN_W_ACT_GSM_COMPACT 0x0040

static GList *
load_preferred_networks_finish (MMBaseSim     *self,
                                GAsyncResult  *res,
                                GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static GList *
parse_plmn_w_act (const guint8 *data,
                  gsize         len)
{
    GList *list = NULL;
    gsize  offset;

    for (offset = 0; offset + PLMN_W_ACT_ENTRY_LEN <= len; offset += PLMN_W_ACT_ENTRY_LEN) {
        const guint8            *entry = data + offset;
        g_autofree gchar        *plmn = NULL;
        MMSimPreferredNetwork   *preferred;
        MMModemAccessTechnology  act = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        guint16                  act_bits;

        /* An unused entry is all ones, which fails the digit check. */
        plmn = mm_sit_plmn_from_packed (entry);
        if (!plmn)
            continue;

        act_bits = (entry[3] << 8) | entry[4];
        if (act_bits & PLMN_W_ACT_GSM)
            act |= MM_MODEM_ACCESS_TECHNOLOGY_GSM;
        if (act_bits & PLMN_W_ACT_GSM_COMPACT)
            act |= MM_MODEM_ACCESS_TECHNOLOGY_GSM_COMPACT;
        if (act_bits & PLMN_W_ACT_UTRAN)
            act |= MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
        if (act_bits & PLMN_W_ACT_EUTRAN)
            act |= MM_MODEM_ACCESS_TECHNOLOGY_LTE;
        if (act_bits & PLMN_W_ACT_NGRAN)
            act |= MM_MODEM_ACCESS_TECHNOLOGY_5GNR;

        preferred = mm_sim_preferred_network_new ();
        mm_sim_preferred_network_set_operator_code (preferred, plmn);
        mm_sim_preferred_network_set_access_technology (preferred, act);
        list = g_list_append (list, preferred);
    }

    return list;
}

static void load_preferred_networks_read (GTask *task, gboolean operator_list);

static void
load_preferred_networks_ready (MMSitClient  *client,
                               GAsyncResult *res,
                               GTask        *task)
{
    MMBaseSim         *self = g_task_get_source_object (task);
    gboolean           operator_list = GPOINTER_TO_UINT (g_task_get_task_data (task));
    g_autoptr(GBytes)  file = NULL;
    g_autoptr(GError)  error = NULL;
    const guint8      *data;
    gsize              len = 0;
    GList             *list;

    file = mm_sit_client_read_ef_finish (client, res, NULL, &error);
    if (!file) {
        if (!operator_list) {
            /* No user-controlled list on this card; the operator's is what the
             * modem itself reports. */
            mm_obj_dbg (self, "no user preferred-network list (%s); trying the "
                        "operator-controlled one", error->message);
            load_preferred_networks_read (task, TRUE);
            return;
        }
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    data = g_bytes_get_data (file, &len);
    list = parse_plmn_w_act (data, len);
    g_task_return_pointer (task, list, (GDestroyNotify) mm_sim_preferred_network_list_free);
    g_object_unref (task);
}

static void
load_preferred_networks_read (GTask    *task,
                              gboolean  operator_list)
{
    MMBaseSim *self = g_task_get_source_object (task);

    g_task_set_task_data (task, GUINT_TO_POINTER (operator_list), NULL);
    read_ef (self, operator_list ? EF_OPLMN_W_ACT : EF_PLMN_W_ACT,
             adf_path, G_N_ELEMENTS (adf_path),
             (GAsyncReadyCallback) load_preferred_networks_ready, task);
}

static void
load_preferred_networks (MMBaseSim          *self,
                         GAsyncReadyCallback callback,
                         gpointer            user_data)
{
    load_preferred_networks_read (g_task_new (self, NULL, callback, user_data), FALSE);
}

/*****************************************************************************/

/* The slot inventory provides the active SIM object, bypassing create_sim.
 * Publish eSIM readiness before core initialization checks unlock status and
 * tries subscription file reads. A mapped eUICC without an ICCID is still
 * present and accessible to an LPA. */
MMBaseSim *
mm_sim_samsung_sit_new_initialized (MMBaseModem *modem,
                                    GObject     *bind_to,
                                    guint        slot_number,
                                    gboolean     active,
                                    const gchar *sim_identifier,
                                    const gchar *eid)
{
    MMBaseSim *sim;

    sim = MM_BASE_SIM (g_object_new (MM_TYPE_SIM_SAMSUNG_SIT,
                                     MM_BASE_SIM_MODEM,       modem,
                                     MM_BIND_TO,              bind_to,
                                     MM_BASE_SIM_SLOT_NUMBER, slot_number,
                                     "active",                active,
                                     "sim-identifier",        sim_identifier,
                                     "eid",                   eid,
                                     "sim-type",              eid ? MM_SIM_TYPE_ESIM : MM_SIM_TYPE_PHYSICAL,
                                     "esim-status",           eid ? (sim_identifier ? MM_SIM_ESIM_STATUS_WITH_PROFILES :
                                                                                       MM_SIM_ESIM_STATUS_NO_PROFILES) :
                                                                    MM_SIM_ESIM_STATUS_UNKNOWN,
                                     NULL));
    mm_base_sim_export (sim);
    return sim;
}

MMBaseSim *
mm_sim_samsung_sit_new_finish (GAsyncResult  *res,
                               GError       **error)
{
    g_autoptr(GObject) source = NULL;
    GObject           *sim;

    source = g_async_result_get_source_object (res);
    sim = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    if (!sim)
        return NULL;

    mm_base_sim_export (MM_BASE_SIM (sim));
    return MM_BASE_SIM (sim);
}

void
mm_sim_samsung_sit_new (MMBaseModem         *modem,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    g_async_initable_new_async (MM_TYPE_SIM_SAMSUNG_SIT,
                                G_PRIORITY_DEFAULT,
                                cancellable,
                                callback,
                                user_data,
                                MM_BASE_SIM_MODEM, modem,
                                MM_BIND_TO, G_OBJECT (modem),
                                "active", TRUE, /* the SIT stack has one active SIM */
                                NULL);
}

static void
mm_sim_samsung_sit_init (MMSimSamsungSit *self)
{
}

static void
mm_sim_samsung_sit_class_init (MMSimSamsungSitClass *klass)
{
    MMBaseSimClass *base_sim_class = MM_BASE_SIM_CLASS (klass);

    base_sim_class->load_sim_identifier = load_sim_identifier;
    base_sim_class->load_sim_identifier_finish = load_sim_identifier_finish;
    base_sim_class->load_imsi = load_imsi;
    base_sim_class->load_imsi_finish = load_imsi_finish;
    base_sim_class->load_eid = load_eid;
    base_sim_class->load_eid_finish = load_eid_finish;
    base_sim_class->load_operator_identifier = load_operator_identifier;
    base_sim_class->load_operator_identifier_finish = load_operator_identifier_finish;
    base_sim_class->load_operator_name = load_operator_name;
    base_sim_class->load_operator_name_finish = load_operator_name_finish;
    base_sim_class->load_gid1 = load_gid1;
    base_sim_class->load_gid1_finish = load_gid_finish;
    base_sim_class->load_gid2 = load_gid2;
    base_sim_class->load_gid2_finish = load_gid_finish;
    base_sim_class->load_emergency_numbers = load_emergency_numbers;
    base_sim_class->load_emergency_numbers_finish = load_emergency_numbers_finish;
    base_sim_class->load_preferred_networks = load_preferred_networks;
    base_sim_class->load_preferred_networks_finish = load_preferred_networks_finish;
}
