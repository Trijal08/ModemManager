/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

/*
 * Thin adapter over the samsung-sit codec (Rust, libsamsung_sit_ffi). The wire
 * format lives in that crate; this file only bridges its GLib-free C ABI
 * (byte-buffer out-params, status codes, #[repr(C)] structs) to the
 * GByteArray/GError/GDateTime shapes the rest of the plugin expects. The
 * MMSitMessage container and the message-id table stay here because they are
 * pure GLib glue with no codec equivalent.
 */

#include <string.h>

#include "mm-sit-protocol.h"

#include <samsung_sit_ffi.h>

typedef struct {
    guint16      id;
    const gchar *name;
} MMSitMessageIdInfo;

static const MMSitMessageIdInfo message_id_info[] = {
#include "mm-sit-message-ids.inc"
};

static guint16
read_le16 (const guint8 *data)
{
    return ((guint16) data[0]) | (((guint16) data[1]) << 8);
}

gboolean
mm_sit_frame_peek_length (const guint8  *data,
                          gsize          data_len,
                          guint16       *out_length,
                          GError       **error)
{
    guint16 frame_len;

    g_return_val_if_fail (out_length != NULL, FALSE);

    if (!data || data_len < MM_SIT_HEADER_SIZE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                     "SIT frame header is incomplete (%" G_GSIZE_FORMAT " bytes)",
                     data_len);
        return FALSE;
    }

    frame_len = read_le16 (&data[4]);
    if (frame_len < MM_SIT_HEADER_SIZE) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Invalid SIT frame length %u", frame_len);
        return FALSE;
    }

    *out_length = frame_len;
    return TRUE;
}

MMSitMessage *
mm_sit_message_parse (const guint8  *data,
                      gsize          data_len,
                      GError       **error)
{
    MMSitMessage *self;
    guint16       frame_len;

    if (!mm_sit_frame_peek_length (data, data_len, &frame_len, error))
        return NULL;

    if (frame_len != data_len) {
        g_set_error (error, G_IO_ERROR,
                     frame_len > data_len ? G_IO_ERROR_PARTIAL_INPUT : G_IO_ERROR_INVALID_DATA,
                     "SIT frame declares %u bytes but %" G_GSIZE_FORMAT " supplied",
                     frame_len, data_len);
        return NULL;
    }

    if (data[0] > MM_SIT_MESSAGE_TYPE_INDICATION) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Invalid SIT message type %u", data[0]);
        return NULL;
    }

    self = g_new0 (MMSitMessage, 1);
    self->type = data[0];
    self->reserved = data[1];
    self->id = read_le16 (&data[2]);
    self->length = frame_len;
    self->token = read_le16 (&data[6]);
    self->raw = g_bytes_new (data, data_len);

    if (self->type == MM_SIT_MESSAGE_TYPE_RESPONSE && data_len >= 10) {
        /*
         * RCM errors are 16-bit.  Most responses reserve four bytes, but the
         * 12-byte SET_RADIO_POWER response overlays the resulting state in
         * the upper half.  Reading a u32 would turn successful state=2 into
         * the false error 0x00020000.
         */
        self->has_status = TRUE;
        self->status = read_le16 (&data[8]);
    }

    return self;
}

void
mm_sit_message_free (MMSitMessage *self)
{
    if (!self)
        return;
    g_clear_pointer (&self->raw, g_bytes_unref);
    g_free (self);
}

const guint8 *
mm_sit_message_get_data (const MMSitMessage *self,
                         gsize              *out_len)
{
    g_return_val_if_fail (self != NULL, NULL);
    return g_bytes_get_data (self->raw, out_len);
}

const guint8 *
mm_sit_message_get_payload (const MMSitMessage *self,
                            gsize              *out_len)
{
    const guint8 *data;
    gsize         len;

    g_return_val_if_fail (self != NULL, NULL);

    data = g_bytes_get_data (self->raw, &len);
    if (out_len)
        *out_len = len - MM_SIT_HEADER_SIZE;
    return data + MM_SIT_HEADER_SIZE;
}

const gchar *
mm_sit_message_id_get_string (guint16 id)
{
    guint lower = 0;
    guint upper = G_N_ELEMENTS (message_id_info);

    while (lower < upper) {
        guint middle = lower + (upper - lower) / 2;

        if (message_id_info[middle].id < id)
            lower = middle + 1;
        else
            upper = middle;
    }

    if (lower < G_N_ELEMENTS (message_id_info) && message_id_info[lower].id == id)
        return message_id_info[lower].name;
    return "UNKNOWN";
}

/*****************************************************************************/
/* FFI bridging helpers                                                      */

static GIOErrorEnum
ffi_status_to_gio (SamsungSitStatus status)
{
    /* Map codec status codes to the GError codes callers expect; cast away the
     * enum type so -Wswitch-enum does not demand every value. */
    switch ((int) status) {
    case SAMSUNG_SIT_STATUS_APN_REQUIRED:
    case SAMSUNG_SIT_STATUS_ARG_TOO_LONG:
    case SAMSUNG_SIT_STATUS_ARG_OUT_OF_RANGE:
    case SAMSUNG_SIT_STATUS_UNEXPECTED_MESSAGE:
    case SAMSUNG_SIT_STATUS_NOT_A_RESPONSE:
    case SAMSUNG_SIT_STATUS_NOT_AN_INDICATION:
    case SAMSUNG_SIT_STATUS_NULL_POINTER:
    case SAMSUNG_SIT_STATUS_UTF8:
        return G_IO_ERROR_INVALID_ARGUMENT;
    case SAMSUNG_SIT_STATUS_TRANSPORT_STATUS:
        return G_IO_ERROR_FAILED;
    case SAMSUNG_SIT_STATUS_UNSUPPORTED:
        return G_IO_ERROR_NOT_SUPPORTED;
    default:
        /* Truncated, TooManySlots, FieldTooLong, Malformed, Binrw. */
        return G_IO_ERROR_INVALID_DATA;
    }
}

static void
set_ffi_error (GError          **error,
               SamsungSitStatus  status)
{
    const char *message = samsung_sit_last_error_message ();

    g_set_error (error, G_IO_ERROR, ffi_status_to_gio (status), "%s",
                 message ? message : samsung_sit_status_name (status));
}

/* Wrap a codec-built frame in a GByteArray, or set @error and return NULL. */
static GByteArray *
finish_build (SamsungSitStatus  status,
              guint8           *buffer,
              gsize             len,
              GError          **error)
{
    GByteArray *request;

    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return NULL;
    }

    request = g_byte_array_sized_new (len);
    g_byte_array_append (request, buffer, len);
    samsung_sit_buf_free (buffer, len);
    return request;
}

static void
format_ipv4 (gchar        destination[16],
             const guint8 address[4])
{
    g_snprintf (destination, 16, "%u.%u.%u.%u",
                address[0], address[1], address[2], address[3]);
}

/*****************************************************************************/
/* Request builders                                                          */

GByteArray *
mm_sit_request_new (guint16        id,
                    guint16        token,
                    const guint8  *body,
                    gsize          body_len,
                    GError       **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_request (id, token, body, body_len, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_query (guint16 id,
                          guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_query (id, token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_radio_power (guint16         token,
                                    MMSitRadioState state)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_radio_power (token, (SamsungSitRadioState) state,
                                                &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_preferred_modem (guint16 token,
                                        guint8  modem_index)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_preferred_modem (token, modem_index, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_logical_slot_mapping (guint16                        token,
                                             const MMSitLogicalSlotMapping *mappings,
                                             guint                          mapping_count,
                                             GError                       **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    /* MMSitLogicalSlotMapping and SamsungSitLogicalSlotMapping share a layout. */
    status = samsung_sit_build_set_logical_slot_mapping (
        token, (const SamsungSitLogicalSlotMapping *) mappings, mapping_count,
        &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_set_data_profile (guint16      token,
                                     const gchar *apn,
                                     GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_data_profile (token, apn, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_set_initial_attach_apn (guint16      token,
                                           const gchar *apn,
                                           GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_initial_attach_apn (token, apn, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_setup_data_call (guint16      token,
                                    const gchar *apn,
                                    GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_setup_data_call (token, apn, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_deactivate_data_call (guint16 token,
                                         guint8  cid,
                                         guint8  reason)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_deactivate_data_call (token, cid, reason, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_get_data_call_list (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_data_call_list (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_dial (guint16       token,
                         const gchar  *number,
                         GError      **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_val_if_fail (number != NULL, NULL);

    status = samsung_sit_build_dial (token, number, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_answer (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_answer (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_hangup (guint16 token,
                           guint32 call_id)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_hangup (token, call_id, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_get_call_list (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_call_list (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_fast_dormancy_info (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_fast_dormancy_info (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_indication_filter (guint16 token,
                                          guint32 filter)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_indication_filter (token, filter, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_screen_state (guint16  token,
                                     gboolean screen_on)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_screen_state (token, screen_on ? 1 : 0, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_ap_system_time (guint16    token,
                                       GDateTime *time,
                                       GError   **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    if (!time) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "A time is required");
        return NULL;
    }

    status = samsung_sit_build_set_ap_system_time (
        token,
        (guint16) g_date_time_get_year (time),
        g_date_time_get_month (time),
        g_date_time_get_day_of_month (time),
        g_date_time_get_hour (time),
        g_date_time_get_minute (time),
        g_date_time_get_second (time),
        &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_oem_config (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_oem_config (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_modem_config (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_modem_config (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_device_info (guint16      token,
                                const gchar *model,
                                const gchar *build,
                                GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_device_info (token, model, build, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_carrier_manifest (guint16      token,
                                     const gchar *directory,
                                     const guint8 sha1[MM_SIT_SHA1_LENGTH],
                                     GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_carrier_manifest (token, directory, sha1, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

/*****************************************************************************/
/* Response/indication parsers                                               */

gboolean
mm_sit_message_parse_radio_state (const MMSitMessage *message,
                                  MMSitRadioState    *out_state,
                                  GError            **error)
{
    const guint8        *data;
    gsize                len;
    SamsungSitRadioState state;
    SamsungSitStatus     status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_state != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_radio_state (data, len, &state);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    *out_state = (MMSitRadioState) state;
    return TRUE;
}

gboolean
mm_sit_message_parse_sim_status (const MMSitMessage *message,
                                 MMSitSimStatus     *out_status,
                                 GError            **error)
{
    const guint8       *data;
    gsize               len;
    SamsungSitSimStatus s;
    SamsungSitStatus    status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_sim_status (data, len, &s);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_status->card_state          = s.card_state;
    out_status->universal_pin_state = s.universal_pin_state;
    out_status->gsm_umts_app_index  = s.gsm_umts_app_index;
    out_status->cdma_app_index      = s.cdma_app_index;
    out_status->ims_app_index       = s.ims_app_index;
    out_status->app_count           = s.app_count;
    out_status->present             = s.present ? TRUE : FALSE;
    return TRUE;
}

gboolean
mm_sit_message_parse_sim_slot_status (const MMSitMessage *message,
                                      MMSitSimSlotStatus *out_status,
                                      GError            **error)
{
    const guint8           *data;
    gsize                   len;
    SamsungSitSimSlotStatus s;
    SamsungSitStatus        status;
    guint                   i, j;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_sim_slot_status (data, len, &s);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_status, 0, sizeof (*out_status));
    out_status->legacy     = s.legacy ? TRUE : FALSE;
    out_status->slot_count = s.slot_count;

    /* The slot/port records are all-u8 and defined identically on both sides. */
    for (i = 0; i < MM_SIT_MAX_SIM_SLOTS; i++) {
        out_status->slots[i].card_state = s.slots[i].card_state;
        out_status->slots[i].atr_len    = s.slots[i].atr_len;
        memcpy (out_status->slots[i].atr, s.slots[i].atr, MM_SIT_MAX_ATR_LENGTH);
        out_status->slots[i].eid_len    = s.slots[i].eid_len;
        memcpy (out_status->slots[i].eid, s.slots[i].eid, MM_SIT_MAX_EID_LENGTH);
        out_status->slots[i].port_count = s.slots[i].port_count;
        for (j = 0; j < MM_SIT_MAX_SIM_PORTS; j++) {
            out_status->slots[i].ports[j].iccid_len = s.slots[i].ports[j].iccid_len;
            memcpy (out_status->slots[i].ports[j].iccid, s.slots[i].ports[j].iccid,
                    MM_SIT_MAX_ICCID_LENGTH);
            out_status->slots[i].ports[j].logical_slot_id = s.slots[i].ports[j].logical_slot_id;
            out_status->slots[i].ports[j].state           = s.slots[i].ports[j].state;
        }
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_carrier_sim_info (const MMSitMessage  *message,
                                       MMSitCarrierSimInfo *out_info,
                                       GError             **error)
{
    const guint8            *data;
    gsize                    len;
    SamsungSitCarrierSimInfo info;
    SamsungSitStatus         status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_info != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_carrier_sim_info (data, len, &info);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memcpy (out_info->iccid, info.iccid, sizeof (out_info->iccid));
    memcpy (out_info->imsi, info.imsi, sizeof (out_info->imsi));
    return TRUE;
}

gboolean
mm_sit_message_parse_registration (const MMSitMessage *message,
                                   MMSitRegistration  *out_registration,
                                   GError            **error)
{
    const guint8          *data;
    gsize                  len;
    SamsungSitRegistration reg;
    SamsungSitStatus       status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_registration != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_registration (data, len, &reg);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_registration->state            = reg.state;
    out_registration->radio_technology = reg.radio_technology;
    out_registration->registered       = reg.registered ? TRUE : FALSE;
    return TRUE;
}

gboolean
mm_sit_message_parse_operator (const MMSitMessage *message,
                               MMSitOperator      *out_operator,
                               GError            **error)
{
    const guint8      *data;
    gsize              len;
    SamsungSitOperator op;
    SamsungSitStatus   status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_operator != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_operator (data, len, &op);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memcpy (out_operator->plmn, op.plmn, sizeof (out_operator->plmn));
    memcpy (out_operator->operator_short, op.operator_short, sizeof (out_operator->operator_short));
    memcpy (out_operator->operator_long, op.operator_long, sizeof (out_operator->operator_long));
    return TRUE;
}

gboolean
mm_sit_message_parse_signal (const MMSitMessage *message,
                             MMSitSignal        *out_signal,
                             GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitSignal s;
    SamsungSitStatus status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_signal != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_signal (data, len, &s);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_signal, 0, sizeof (*out_signal));
    out_signal->version  = s.version;
    out_signal->rat_mask = s.rat_mask;

    out_signal->has_gsm             = s.has_gsm ? TRUE : FALSE;
    out_signal->gsm_signal_strength = s.gsm_signal_strength;
    out_signal->gsm_bit_error_rate  = s.gsm_bit_error_rate;
    out_signal->gsm_timing_advance  = s.gsm_timing_advance;

    out_signal->has_wcdma             = s.has_wcdma ? TRUE : FALSE;
    out_signal->wcdma_signal_strength = s.wcdma_signal_strength;
    out_signal->wcdma_bit_error_rate  = s.wcdma_bit_error_rate;
    out_signal->wcdma_rscp            = s.wcdma_rscp;
    out_signal->wcdma_ecno            = s.wcdma_ecno;

    out_signal->has_lte             = s.has_lte ? TRUE : FALSE;
    out_signal->lte_signal_strength = s.lte_signal_strength;
    out_signal->lte_rsrp            = s.lte_rsrp;
    out_signal->lte_rsrq            = s.lte_rsrq;
    out_signal->lte_rssnr           = s.lte_rssnr;
    out_signal->lte_cqi_table_index = s.lte_cqi_table_index;
    out_signal->lte_cqi             = s.lte_cqi;
    out_signal->lte_timing_advance  = s.lte_timing_advance;

    out_signal->has_nr5g            = s.has_nr5g ? TRUE : FALSE;
    out_signal->nr5g_ss_rsrp        = s.nr5g_ss_rsrp;
    out_signal->nr5g_ss_rsrq        = s.nr5g_ss_rsrq;
    out_signal->nr5g_ss_sinr        = s.nr5g_ss_sinr;
    out_signal->nr5g_csi_rsrp       = s.nr5g_csi_rsrp;
    out_signal->nr5g_csi_rsrq       = s.nr5g_csi_rsrq;
    out_signal->nr5g_csi_sinr       = s.nr5g_csi_sinr;
    out_signal->nr5g_timing_advance = s.nr5g_timing_advance;
    return TRUE;
}

gboolean
mm_sit_message_parse_cell_info_list (const MMSitMessage *message,
                                     GArray            **out_cells,
                                     GError            **error)
{
    const guint8          *data;
    gsize                  len;
    SamsungSitCellInfoLte *cells = NULL;
    gsize                  count = 0;
    SamsungSitStatus       status;
    GArray                *array;
    gsize                  i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_cells != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_cell_info_list (data, len, &cells, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitCellInfoLte), count);
    for (i = 0; i < count; i++) {
        MMSitCellInfoLte cell;

        memset (&cell, 0, sizeof (cell));
        cell.registered        = cells[i].registered ? TRUE : FALSE;
        cell.connection_status = cells[i].connection_status;
        memcpy (cell.operator_id, cells[i].operator_id, sizeof (cell.operator_id));
        cell.ci             = cells[i].ci;
        cell.physical_ci    = cells[i].physical_ci;
        cell.tac            = cells[i].tac;
        cell.earfcn         = cells[i].earfcn;
        cell.bandwidth      = cells[i].bandwidth;
        cell.rsrp           = cells[i].rsrp;
        cell.rsrq           = cells[i].rsrq;
        cell.rssnr          = cells[i].rssnr;
        cell.timing_advance = cells[i].timing_advance;
        g_array_append_val (array, cell);
    }
    samsung_sit_cell_info_list_free (cells, count);

    *out_cells = array;
    return TRUE;
}

gboolean
mm_sit_message_parse_data_call (const MMSitMessage *message,
                                MMSitDataCall      *out_call,
                                GError            **error)
{
    const guint8      *data;
    gsize              len;
    SamsungSitDataCall dc;
    SamsungSitStatus   status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_call != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_data_call (data, len, &dc);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_call, 0, sizeof (*out_call));
    out_call->cause = dc.cause;
    out_call->cid   = dc.cid;
    out_call->active = dc.active;

    /*
     * The codec exposes both families; MMSitDataCall currently only carries the
     * IPv4 strings, so format those. (dc.address.v6 / dc.gateway are available
     * for when the struct grows an IPv6 path.)
     */
    if (dc.address.has_v4) {
        out_call->has_ipv4 = TRUE;
        format_ipv4 (out_call->ipv4, dc.address.v4);
    }
    if (dc.dns[0].has_v4)
        format_ipv4 (out_call->dns1, dc.dns[0].v4);
    if (dc.dns[1].has_v4)
        format_ipv4 (out_call->dns2, dc.dns[1].v4);
    out_call->mtu = (guint16) dc.mtu;
    return TRUE;
}

gboolean
mm_sit_message_parse_data_call_list (const MMSitMessage *message,
                                     GArray            **out_calls,
                                     GError            **error)
{
    const guint8       *data;
    gsize               len;
    SamsungSitDataCall *calls = NULL;
    gsize               count = 0;
    SamsungSitStatus    status;
    GArray             *array;
    gsize               i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_calls != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_data_call_list (data, len, &calls, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitDataCall), count);
    for (i = 0; i < count; i++) {
        MMSitDataCall call;

        memset (&call, 0, sizeof (call));
        call.cause  = calls[i].cause;
        call.cid    = calls[i].cid;
        call.active = calls[i].active;
        if (calls[i].address.has_v4) {
            call.has_ipv4 = TRUE;
            format_ipv4 (call.ipv4, calls[i].address.v4);
        }
        if (calls[i].dns[0].has_v4)
            format_ipv4 (call.dns1, calls[i].dns[0].v4);
        if (calls[i].dns[1].has_v4)
            format_ipv4 (call.dns2, calls[i].dns[1].v4);
        call.mtu = (guint16) calls[i].mtu;
        g_array_append_val (array, call);
    }
    samsung_sit_data_call_list_free (calls, count);

    *out_calls = array;
    return TRUE;
}

gboolean
mm_sit_message_parse_call_list (const MMSitMessage *message,
                                GArray            **out_calls,
                                GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitCall  *calls = NULL;
    gsize            count = 0;
    SamsungSitStatus status;
    GArray          *array;
    gsize            i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_calls != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_call_list (data, len, &calls, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitCall), count);
    for (i = 0; i < count; i++) {
        MMSitCall call;

        memset (&call, 0, sizeof (call));
        call.state    = calls[i].state;
        call.index    = calls[i].index;
        call.incoming = (calls[i].incoming != 0);
        g_strlcpy (call.number, calls[i].number, sizeof (call.number));
        g_array_append_val (array, call);
    }
    samsung_sit_call_list_free (calls, count);

    *out_calls = array;
    return TRUE;
}
