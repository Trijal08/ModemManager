/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <string.h>

#include "mm-sit-protocol.h"

static guint16
read_le16 (const guint8 *data)
{
    return ((guint16) data[0]) | (((guint16) data[1]) << 8);
}

static guint32
read_le32 (const guint8 *data)
{
    return ((guint32) data[0]) |
           (((guint32) data[1]) << 8) |
           (((guint32) data[2]) << 16) |
           (((guint32) data[3]) << 24);
}

static void
write_le16 (guint8 *data,
            guint16 value)
{
    data[0] = value & 0xff;
    data[1] = value >> 8;
}

static void
write_le32 (guint8 *data,
            guint32 value)
{
    data[0] = value & 0xff;
    data[1] = (value >> 8) & 0xff;
    data[2] = (value >> 16) & 0xff;
    data[3] = value >> 24;
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
    switch (id) {
    case MM_SIT_MESSAGE_GET_SIM_STATUS:          return "GET_SIM_STATUS";
    case MM_SIT_MESSAGE_SIM_IO:                  return "SIM_IO";
    case MM_SIT_MESSAGE_SIM_STATUS_CHANGED:      return "SIM_STATUS_CHANGED";
    case MM_SIT_MESSAGE_SETUP_DATA_CALL:         return "SETUP_DATA_CALL";
    case MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL:    return "DEACTIVATE_DATA_CALL";
    case MM_SIT_MESSAGE_SET_INITIAL_ATTACH_APN:  return "SET_INITIAL_ATTACH_APN";
    case MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED:  return "DATA_CALL_LIST_CHANGED";
    case MM_SIT_MESSAGE_GET_DATA_CALL_LIST:      return "GET_DATA_CALL_LIST";
    case MM_SIT_MESSAGE_SET_DATA_PROFILE:        return "SET_DATA_PROFILE";
    case MM_SIT_MESSAGE_GET_VOICE_REGISTRATION:  return "GET_VOICE_REGISTRATION";
    case MM_SIT_MESSAGE_GET_NETWORK_REGISTRATION:return "GET_NETWORK_REGISTRATION";
    case MM_SIT_MESSAGE_GET_DATA_REGISTRATION:   return "GET_DATA_REGISTRATION";
    case MM_SIT_MESSAGE_SERVING_NETWORK:         return "SERVING_NETWORK";
    case MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM:return "SET_PREFERRED_DATA_MODEM";
    case MM_SIT_MESSAGE_SET_RADIO_POWER:         return "SET_RADIO_POWER";
    case MM_SIT_MESSAGE_GET_RADIO_POWER:         return "GET_RADIO_POWER";
    case MM_SIT_MESSAGE_RADIO_STATE_CHANGED:     return "RADIO_STATE_CHANGED";
    case MM_SIT_MESSAGE_DEVICE_INFO:             return "DEVICE_INFO";
    case MM_SIT_MESSAGE_SET_INDICATION_FILTER:   return "SET_INDICATION_FILTER";
    case MM_SIT_MESSAGE_SET_MODEM_CONFIG:        return "SET_MODEM_CONFIG";
    case MM_SIT_MESSAGE_DEVICE_CAPABILITIES:     return "DEVICE_CAPABILITIES";
    case MM_SIT_MESSAGE_CARRIER_CONFIG_MANIFEST: return "CARRIER_CONFIG_MANIFEST";
    case MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO: return "CARRIER_CONFIG_SIM_INFO";
    case MM_SIT_MESSAGE_OEM_CONFIG:              return "OEM_CONFIG";
    default:                                     return "UNKNOWN";
    }
}

GByteArray *
mm_sit_request_new (guint16        id,
                    guint16        token,
                    const guint8  *body,
                    gsize          body_len,
                    GError       **error)
{
    GByteArray *request;
    gsize       total_len;

    if (body_len && !body) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "SIT request body is NULL");
        return NULL;
    }

    total_len = MM_SIT_HEADER_SIZE + body_len;
    if (total_len > G_MAXUINT16) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "SIT request is too large (%" G_GSIZE_FORMAT " bytes)",
                     total_len);
        return NULL;
    }

    request = g_byte_array_sized_new (total_len);
    g_byte_array_set_size (request, total_len);
    request->data[0] = MM_SIT_MESSAGE_TYPE_REQUEST;
    request->data[1] = 0;
    write_le16 (&request->data[2], id);
    write_le16 (&request->data[4], total_len);
    write_le16 (&request->data[6], token);
    if (body_len)
        memcpy (&request->data[MM_SIT_HEADER_SIZE], body, body_len);
    return request;
}

GByteArray *
mm_sit_request_new_query (guint16 id,
                          guint16 token)
{
    const guint8 reserved[4] = { 0 };

    return mm_sit_request_new (id, token, reserved, sizeof (reserved), NULL);
}

GByteArray *
mm_sit_request_new_set_radio_power (guint16         token,
                                    MMSitRadioState state)
{
    guint8 body[10] = { 0 };

    write_le32 (&body[4], state);
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_RADIO_POWER,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_set_preferred_modem (guint16 token,
                                        guint8  modem_index)
{
    guint8 body[5] = { 0 };

    body[4] = modem_index;
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM,
                               token, body, sizeof (body), NULL);
}

static gboolean
copy_apn (guint8       *destination,
          gsize         destination_len,
          const gchar  *apn,
          GError      **error)
{
    gsize apn_len;

    if (!apn || !apn[0]) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "An APN is required");
        return FALSE;
    }

    apn_len = strlen (apn);
    if (apn_len > MM_SIT_MAX_APN_LENGTH || apn_len >= destination_len) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "APN is too long (%" G_GSIZE_FORMAT ", maximum %u)",
                     apn_len, MM_SIT_MAX_APN_LENGTH);
        return FALSE;
    }

    memcpy (destination, apn, apn_len);
    return TRUE;
}

GByteArray *
mm_sit_request_new_set_data_profile (guint16      token,
                                     const gchar *apn,
                                     GError     **error)
{
    guint8 body[238] = { 0 };

    memset (&body[4], 0xff, 4); /* profile id: automatic */
    if (!copy_apn (&body[8], 101, apn, error))
        return NULL;
    body[109] = 1;
    body[110] = 1;
    body[225] = 1;
    body[226] = 7;
    body[227] = 1;
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_DATA_PROFILE,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_set_initial_attach_apn (guint16      token,
                                           const gchar *apn,
                                           GError     **error)
{
    guint8 body[243] = { 0 };

    body[4] = 1;
    body[5] = 0x0e;
    if (!copy_apn (&body[8], 202, apn, error))
        return NULL;
    body[210] = 1;
    body[240] = 1;
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_INITIAL_ATTACH_APN,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_setup_data_call (guint16      token,
                                    const gchar *apn,
                                    GError     **error)
{
    guint8 body[975] = { 0 };

    body[4] = 1;
    body[5] = 0x0e;
    if (!copy_apn (&body[8], 202, apn, error))
        return NULL;
    body[210] = 1;
    body[233] = 7;
    body[234] = 1;
    body[237] = 2;
    memset (&body[244], 0xff, 4);
    memset (&body[249], 0xff, 4);
    return mm_sit_request_new (MM_SIT_MESSAGE_SETUP_DATA_CALL,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_deactivate_data_call (guint16 token,
                                         guint8  cid,
                                         guint8  reason)
{
    guint8 body[6] = { 0 };

    body[4] = cid;
    body[5] = reason;
    return mm_sit_request_new (MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_get_data_call_list (guint16 token)
{
    const guint8 body[8] = { 0, 0, 0, 0, 0x0a, 0x0a, 0x0a, 0x0a };

    return mm_sit_request_new (MM_SIT_MESSAGE_GET_DATA_CALL_LIST,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_set_indication_filter (guint16 token,
                                          guint32 filter)
{
    guint8 body[8] = { 0 };

    write_le32 (&body[4], filter);
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_INDICATION_FILTER,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_device_capabilities (guint16      token,
                                        const guint8 capabilities[6])
{
    guint8 body[10] = { 0 };

    g_return_val_if_fail (capabilities != NULL, NULL);
    memcpy (&body[4], capabilities, 6);
    return mm_sit_request_new (MM_SIT_MESSAGE_DEVICE_CAPABILITIES,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_device_info (guint16      token,
                                const gchar *model,
                                const gchar *build,
                                GError     **error)
{
    guint8 body[132] = { 0 };

    if (!model || strlen (model) > 31) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Device model must contain at most 31 bytes");
        return NULL;
    }
    if (!build || strlen (build) > 95) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Device build must contain at most 95 bytes");
        return NULL;
    }

    /* Captured layout: reserved @8, model @12, build @44. */
    memcpy (&body[4], model, strlen (model));
    memcpy (&body[36], build, strlen (build));
    return mm_sit_request_new (MM_SIT_MESSAGE_DEVICE_INFO,
                               token, body, sizeof (body), NULL);
}

GByteArray *
mm_sit_request_new_carrier_manifest (guint16      token,
                                     const gchar *directory,
                                     const guint8 sha1[MM_SIT_SHA1_LENGTH],
                                     GError     **error)
{
    guint8 body[524] = { 0 };
    gsize  directory_len;

    if (!directory || !sha1) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Carrier-config directory and SHA1 are required");
        return NULL;
    }

    directory_len = strlen (directory);
    if (!directory_len || directory_len > 255) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Carrier-config directory must contain 1 to 255 bytes");
        return NULL;
    }

    write_le32 (&body[4], directory_len);
    memcpy (&body[8], directory, directory_len);
    write_le32 (&body[264], MM_SIT_SHA1_LENGTH);
    memcpy (&body[268], sha1, MM_SIT_SHA1_LENGTH);
    return mm_sit_request_new (MM_SIT_MESSAGE_CARRIER_CONFIG_MANIFEST,
                               token, body, sizeof (body), NULL);
}

static gboolean
require_message (const MMSitMessage *message,
                 guint16             id,
                 gsize               minimum_length,
                 GError            **error)
{
    if (!message || message->id != id) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Expected SIT message 0x%04x", id);
        return FALSE;
    }
    if (message->length < minimum_length) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT %s message is too short (%u, need %" G_GSIZE_FORMAT ")",
                     mm_sit_message_id_get_string (id), message->length, minimum_length);
        return FALSE;
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_radio_state (const MMSitMessage *message,
                                  MMSitRadioState    *out_state,
                                  GError            **error)
{
    const guint8 *data;
    gsize         len;
    guint32       state;

    g_return_val_if_fail (out_state != NULL, FALSE);

    if (!message || (message->id != MM_SIT_MESSAGE_RADIO_STATE_CHANGED &&
                     message->id != MM_SIT_MESSAGE_GET_RADIO_POWER &&
                     message->id != MM_SIT_MESSAGE_SET_RADIO_POWER)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry a SIT radio state");
        return FALSE;
    }

    data = mm_sit_message_get_data (message, &len);
    if (message->id == MM_SIT_MESSAGE_SET_RADIO_POWER && len == 12)
        state = read_le16 (&data[10]);
    else {
        if (len < 12 + (message->id == MM_SIT_MESSAGE_GET_RADIO_POWER ? 4 : 0)) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "SIT radio-state message is too short");
            return FALSE;
        }
        state = read_le32 (&data[message->id == MM_SIT_MESSAGE_GET_RADIO_POWER ? 12 : 8]);
    }

    if (state > MM_SIT_RADIO_STATE_ON) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Unknown SIT radio state %u", state);
        return FALSE;
    }

    *out_state = state;
    return TRUE;
}

gboolean
mm_sit_message_parse_sim_status (const MMSitMessage *message,
                                 MMSitSimStatus     *out_status,
                                 GError            **error)
{
    const guint8 *data;

    g_return_val_if_fail (out_status != NULL, FALSE);

    if (!require_message (message, MM_SIT_MESSAGE_GET_SIM_STATUS, 18, error))
        return FALSE;
    if (message->type != MM_SIT_MESSAGE_TYPE_RESPONSE ||
        !message->has_status || message->status != 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "GET_SIM_STATUS did not return a successful response");
        return FALSE;
    }

    data = mm_sit_message_get_data (message, NULL);
    memset (out_status, 0, sizeof (*out_status));
    out_status->card_state = data[12];
    out_status->universal_pin_state = data[13];
    out_status->gsm_umts_app_index = data[14];
    out_status->cdma_app_index = data[15];
    out_status->ims_app_index = data[16];
    out_status->app_count = data[17];
    out_status->present = (out_status->card_state == 1);
    return TRUE;
}

static gboolean
copy_decimal_identity (gchar        *destination,
                       gsize         destination_size,
                       const guint8 *source,
                       gsize         source_size,
                       const gchar  *field,
                       GError      **error)
{
    gsize i;

    for (i = 0; i < source_size && source[i]; i++) {
        if (!g_ascii_isdigit (source[i])) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "SIT carrier SIM information has an invalid %s", field);
            return FALSE;
        }
    }
    if (!i || i >= destination_size || (i == source_size && source[source_size - 1])) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT carrier SIM information has an invalid %s length", field);
        return FALSE;
    }

    memcpy (destination, source, i);
    destination[i] = '\0';
    return TRUE;
}

gboolean
mm_sit_message_parse_carrier_sim_info (const MMSitMessage  *message,
                                       MMSitCarrierSimInfo *out_info,
                                       GError             **error)
{
    const guint8 *payload;
    gsize         payload_len;
    guint32       declared_len;

    g_return_val_if_fail (out_info != NULL, FALSE);

    if (!require_message (message, MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO, 49, error))
        return FALSE;
    if (message->type != MM_SIT_MESSAGE_TYPE_INDICATION) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Carrier SIM information is not an indication");
        return FALSE;
    }

    payload = mm_sit_message_get_payload (message, &payload_len);
    declared_len = read_le32 (payload);
    if (declared_len != payload_len) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT carrier SIM information declares %u bytes but contains %" G_GSIZE_FORMAT,
                     declared_len, payload_len);
        return FALSE;
    }

    memset (out_info, 0, sizeof (*out_info));
    /* Captured 0x4602 layout: u32 body length, char ICCID[21], char IMSI[16]. */
    if (!copy_decimal_identity (out_info->iccid, sizeof (out_info->iccid),
                                &payload[4], 21, "ICCID", error) ||
        !copy_decimal_identity (out_info->imsi, sizeof (out_info->imsi),
                                &payload[25], 16, "IMSI", error))
        return FALSE;
    return TRUE;
}

static void
copy_plmn (gchar        destination[7],
           const guint8 *source,
           gsize         source_len)
{
    gsize i;

    for (i = 0; i < source_len && i < 6 && g_ascii_isdigit (source[i]); i++)
        destination[i] = source[i];
    destination[i] = '\0';
}

gboolean
mm_sit_message_parse_registration (const MMSitMessage *message,
                                   MMSitRegistration  *out_registration,
                                   GError            **error)
{
    const guint8 *data;
    gsize         len;
    gsize         plmn_offset;

    g_return_val_if_fail (out_registration != NULL, FALSE);

    if (!message || (message->id != MM_SIT_MESSAGE_GET_DATA_REGISTRATION &&
                     message->id != MM_SIT_MESSAGE_SERVING_NETWORK)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry supported SIT registration data");
        return FALSE;
    }

    data = mm_sit_message_get_data (message, &len);
    plmn_offset = (message->id == MM_SIT_MESSAGE_SERVING_NETWORK) ? 9 : 12;
    if (len <= plmn_offset) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT registration message has no registration payload");
        return FALSE;
    }
    if (message->type == MM_SIT_MESSAGE_TYPE_RESPONSE &&
        (!message->has_status || message->status != 0)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "SIT registration query failed");
        return FALSE;
    }

    memset (out_registration, 0, sizeof (*out_registration));
    copy_plmn (out_registration->plmn, &data[plmn_offset], len - plmn_offset);
    out_registration->registered = out_registration->plmn[0] != '\0';
    return TRUE;
}

static void
format_ipv4 (gchar        destination[16],
             const guint8 address[4])
{
    g_snprintf (destination, 16, "%u.%u.%u.%u",
                address[0], address[1], address[2], address[3]);
}

gboolean
mm_sit_message_parse_data_call (const MMSitMessage *message,
                                MMSitDataCall      *out_call,
                                GError            **error)
{
    const guint8 *data;
    gsize         len;
    gsize         record_offset;
    gsize         valid_offset;
    gsize         address_offset;
    gsize         dns1_offset;
    gsize         dns2_offset;
    gsize         mtu_offset;

    g_return_val_if_fail (out_call != NULL, FALSE);

    if (!message || (message->id != MM_SIT_MESSAGE_SETUP_DATA_CALL &&
                     message->id != MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry a supported SIT data call");
        return FALSE;
    }

    data = mm_sit_message_get_data (message, &len);
    memset (out_call, 0, sizeof (*out_call));

    if (message->id == MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED) {
        if (len == 9 && data[8] == 0)
            return TRUE;
        if (len < 14 || data[8] == 0) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Malformed SIT data-call-list indication");
            return FALSE;
        }
        record_offset = 9;
        valid_offset = 13;
        address_offset = 14;
        dns1_offset = 35;
        dns2_offset = 55;
        mtu_offset = 136;
    } else {
        if (message->type != MM_SIT_MESSAGE_TYPE_RESPONSE || len < 17) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Malformed SIT setup-data-call response");
            return FALSE;
        }
        if (!message->has_status || message->status != 0) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "SETUP_DATA_CALL returned a transport error");
            return FALSE;
        }
        record_offset = 12;
        valid_offset = 16;
        address_offset = 17;
        dns1_offset = 38;
        dns2_offset = 58;
        mtu_offset = 139;
    }

    if (len < record_offset + 4) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT data-call record is truncated");
        return FALSE;
    }

    out_call->cause = read_le16 (&data[record_offset]);
    out_call->cid = data[record_offset + 2];
    out_call->active = data[record_offset + 3];

    if (len > valid_offset && data[valid_offset]) {
        if (len < address_offset + 4) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "SIT IPv4 address is truncated");
            return FALSE;
        }
        out_call->has_ipv4 = TRUE;
        format_ipv4 (out_call->ipv4, &data[address_offset]);
    }
    if (len >= dns1_offset + 4 && read_le32 (&data[dns1_offset]))
        format_ipv4 (out_call->dns1, &data[dns1_offset]);
    if (len >= dns2_offset + 4 && read_le32 (&data[dns2_offset]))
        format_ipv4 (out_call->dns2, &data[dns2_offset]);
    if (len >= mtu_offset + 2)
        out_call->mtu = read_le16 (&data[mtu_offset]);

    return TRUE;
}
