/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <string.h>

#include "mm-sit-protocol.h"

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

GByteArray *
mm_sit_request_new_set_logical_slot_mapping (guint16                       token,
                                             const MMSitLogicalSlotMapping *mappings,
                                             guint                         mapping_count,
                                             GError                      **error)
{
    guint8 body[13] = { 0 };
    guint  i;

    if (!mappings || mapping_count == 0 || mapping_count > MM_SIT_MAX_SIM_SLOTS) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Invalid SIT logical-slot mapping count %u", mapping_count);
        return NULL;
    }

    body[4] = mapping_count;
    for (i = 0; i < mapping_count; i++) {
        body[5 + (2 * i)] = mappings[i].physical_slot_id;
        body[6 + (2 * i)] = mappings[i].port_id;
    }
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_LOGICAL_SLOT_MAPPING,
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
    return mm_sit_request_new_query (MM_SIT_MESSAGE_GET_DATA_CALL_LIST, token);
}

GByteArray *
mm_sit_request_new_set_fast_dormancy_info (guint16 token)
{
    const guint8 body[8] = { 0, 0, 0, 0, 0x0a, 0x0a, 0x0a, 0x0a };

    return mm_sit_request_new (MM_SIT_MESSAGE_SET_FAST_DORMANCY_INFO,
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
mm_sit_request_new_set_screen_state (guint16  token,
                                     gboolean screen_on)
{
    guint8 body[8] = { 0 };

    write_le32 (&body[4], screen_on ? 1 : 0);
    return mm_sit_request_new (MM_SIT_MESSAGE_SET_SCREEN_STATE,
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

    /* Android's SIT adapter normalizes GET_RADIO_POWER differently from
     * RADIO_STATE_CHANGED: GET values 2..10 all mean RIL_RADIO_ON, while
     * unsolicited values 0/1 mean unavailable and 2 means on. */
    if (message->id == MM_SIT_MESSAGE_GET_RADIO_POWER) {
        if (state > 10) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "Unknown SIT GET radio state %u", state);
            return FALSE;
        }
        *out_state = (state == 0) ? MM_SIT_RADIO_STATE_UNKNOWN :
                     (state == 1) ? MM_SIT_RADIO_STATE_OFF :
                                    MM_SIT_RADIO_STATE_ON;
        return TRUE;
    }

    if (message->id == MM_SIT_MESSAGE_RADIO_STATE_CHANGED) {
        if (state == 0 || state == 1)
            *out_state = MM_SIT_RADIO_STATE_UNKNOWN;
        else if (state == 2)
            *out_state = MM_SIT_RADIO_STATE_ON;
        else
            *out_state = MM_SIT_RADIO_STATE_OFF;
        return TRUE;
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

gboolean
mm_sit_message_parse_sim_slot_status (const MMSitMessage *message,
                                      MMSitSimSlotStatus *out_status,
                                      GError            **error)
{
    const guint8 *payload;
    const guint8 *records;
    gsize         payload_len;
    gsize         prefix_len;
    gsize         record_len;
    guint         i;

    g_return_val_if_fail (out_status != NULL, FALSE);

    if (!message ||
        !((message->type == MM_SIT_MESSAGE_TYPE_RESPONSE &&
           message->id == MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS) ||
          (message->type == MM_SIT_MESSAGE_TYPE_INDICATION &&
           message->id == MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED))) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry SIT SIM slot status");
        return FALSE;
    }
    if (message->type == MM_SIT_MESSAGE_TYPE_RESPONSE &&
        (!message->has_status || message->status != 0)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "GET_SIM_SLOT_STATUS did not return a successful response");
        return FALSE;
    }

    payload = mm_sit_message_get_payload (message, &payload_len);
    prefix_len = message->type == MM_SIT_MESSAGE_TYPE_RESPONSE ? MM_SIT_STATUS_SIZE : 0;
    if (payload_len < prefix_len + 1) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT SIM slot status is truncated");
        return FALSE;
    }

    memset (out_status, 0, sizeof (*out_status));
    out_status->slot_count = payload[prefix_len];
    if (out_status->slot_count > MM_SIT_MAX_SIM_SLOTS) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT SIM slot status reports %u slots", out_status->slot_count);
        return FALSE;
    }

    record_len = payload_len - prefix_len - 1;
    if (record_len == MM_SIT_MAX_SIM_SLOTS * 105)
        record_len = 105;
    else if (record_len == MM_SIT_MAX_SIM_SLOTS * 65) {
        record_len = 65;
        out_status->legacy = TRUE;
    } else {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Unsupported SIT SIM slot status layout (%" G_GSIZE_FORMAT " bytes)",
                     payload_len);
        return FALSE;
    }

    records = &payload[prefix_len + 1];
    for (i = 0; i < out_status->slot_count; i++) {
        const guint8 *record = &records[i * record_len];
        MMSitSimSlot *slot = &out_status->slots[i];

        slot->card_state = record[0];
        if (out_status->legacy) {
            MMSitSimPort *port = &slot->ports[0];

            slot->atr_len = record[2];
            slot->eid_len = record[48];
            port->logical_slot_id = record[36];
            port->iccid_len = record[37];
            port->state = record[1];
            slot->port_count = 1;
            if (slot->atr_len > MM_SIT_MAX_ATR_LENGTH ||
                slot->eid_len > MM_SIT_MAX_EID_LENGTH ||
                port->iccid_len > MM_SIT_MAX_ICCID_LENGTH) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT legacy SIM slot %u has invalid field lengths", i);
                return FALSE;
            }
            memcpy (slot->atr, &record[3], slot->atr_len);
            memcpy (port->iccid, &record[38], port->iccid_len);
            memcpy (slot->eid, &record[49], slot->eid_len);
        } else {
            guint j;

            slot->atr_len = record[1];
            slot->eid_len = record[35];
            slot->port_count = record[52];
            if (slot->atr_len > MM_SIT_MAX_ATR_LENGTH ||
                slot->eid_len > MM_SIT_MAX_EID_LENGTH ||
                slot->port_count > MM_SIT_MAX_SIM_PORTS) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT SIM slot %u has invalid field lengths", i);
                return FALSE;
            }
            memcpy (slot->atr, &record[2], slot->atr_len);
            memcpy (slot->eid, &record[36], slot->eid_len);
            for (j = 0; j < slot->port_count; j++) {
                const guint8 *port_record = &record[53 + (13 * j)];
                MMSitSimPort *port = &slot->ports[j];

                port->iccid_len = port_record[0];
                if (port->iccid_len > MM_SIT_MAX_ICCID_LENGTH) {
                    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "SIT SIM slot %u port %u has an invalid ICCID length",
                                 i, j);
                    return FALSE;
                }
                memcpy (port->iccid, &port_record[1], port->iccid_len);
                port->logical_slot_id = port_record[11];
                port->state = port_record[12];
            }
        }
    }
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

static void
copy_operator_name (gchar        *destination,
                    gsize         destination_len,
                    const guint8 *source,
                    gsize         source_len)
{
    const guint8 *terminator;
    gsize         len;

    g_assert (destination_len > 0);

    len = MIN (source_len, destination_len - 1);
    terminator = memchr (source, '\0', len);
    if (terminator)
        len = terminator - source;
    memcpy (destination, source, len);
    destination[len] = '\0';
    g_strstrip (destination);
    if (!g_utf8_validate (destination, -1, NULL))
        destination[0] = '\0';
}

gboolean
mm_sit_message_parse_registration (const MMSitMessage *message,
                                   MMSitRegistration  *out_registration,
                                   GError            **error)
{
    const guint8 *data;
    gsize         len;

    g_return_val_if_fail (out_registration != NULL, FALSE);

    if (!message || message->id != MM_SIT_MESSAGE_GET_DATA_REGISTRATION) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry supported SIT registration data");
        return FALSE;
    }

    data = mm_sit_message_get_data (message, &len);
    if (message->type != MM_SIT_MESSAGE_TYPE_RESPONSE || len < 16) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT data-registration response is malformed");
        return FALSE;
    }
    if (!message->has_status || message->status != 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "SIT registration query failed");
        return FALSE;
    }

    memset (out_registration, 0, sizeof (*out_registration));
    out_registration->state = data[12];
    out_registration->radio_technology = data[15];
    out_registration->registered = (out_registration->state == 1 ||
                                    out_registration->state == 5);
    return TRUE;
}

gboolean
mm_sit_message_parse_operator (const MMSitMessage *message,
                               MMSitOperator      *out_operator,
                               GError            **error)
{
    const guint8 *data;
    gsize         len;
    gsize         plmn_offset;

    g_return_val_if_fail (out_operator != NULL, FALSE);

    if (!message || (message->id != MM_SIT_MESSAGE_GET_OPERATOR &&
                     message->id != MM_SIT_MESSAGE_SERVING_NETWORK)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry supported SIT operator data");
        return FALSE;
    }

    data = mm_sit_message_get_data (message, &len);
    plmn_offset = (message->id == MM_SIT_MESSAGE_SERVING_NETWORK) ? 9 : 12;
    if (len <= plmn_offset) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT operator message has no operator payload");
        return FALSE;
    }
    if (message->type == MM_SIT_MESSAGE_TYPE_RESPONSE &&
        (!message->has_status || message->status != 0)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "SIT operator query failed");
        return FALSE;
    }

    memset (out_operator, 0, sizeof (*out_operator));
    copy_plmn (out_operator->plmn, &data[plmn_offset], len - plmn_offset);
    if (message->id == MM_SIT_MESSAGE_GET_OPERATOR) {
        if (len > 18)
            copy_operator_name (out_operator->operator_short,
                                sizeof (out_operator->operator_short),
                                &data[18], len - 18);
        if (len > 50)
            copy_operator_name (out_operator->operator_long,
                                sizeof (out_operator->operator_long),
                                &data[50], len - 50);
    } else {
        if (len > 35)
            copy_operator_name (out_operator->operator_short,
                                sizeof (out_operator->operator_short),
                                &data[35], len - 35);
        if (len > 67)
            copy_operator_name (out_operator->operator_long,
                                sizeof (out_operator->operator_long),
                                &data[67], len - 67);
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_signal (const MMSitMessage *message,
                             MMSitSignal        *out_signal,
                             GError            **error)
{
    const guint8 *data;
    const guint8 *values;
    gsize         len;
    gsize         mask_offset;
    gsize         values_len;

    g_return_val_if_fail (out_signal != NULL, FALSE);

    if (!message || (message->id != MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH &&
                     message->id != MM_SIT_MESSAGE_SIGNAL_STRENGTH_CHANGED)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message does not carry supported SIT signal data");
        return FALSE;
    }

    if (message->id == MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH) {
        if (message->type != MM_SIT_MESSAGE_TYPE_RESPONSE ||
            !message->has_status || message->status != 0) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "SIT signal-strength query failed");
            return FALSE;
        }
        mask_offset = 12;
    } else {
        if (message->type != MM_SIT_MESSAGE_TYPE_INDICATION) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "SIT signal-strength indication has an invalid type");
            return FALSE;
        }
        mask_offset = 8;
    }

    data = mm_sit_message_get_data (message, &len);
    if (len < mask_offset + 2) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT signal-strength message is truncated");
        return FALSE;
    }

    values = &data[mask_offset + 2];
    values_len = len - mask_offset - 2;
    memset (out_signal, 0, sizeof (*out_signal));
    out_signal->rat_mask = read_le16 (&data[mask_offset]);
    out_signal->gsm_timing_advance = G_MAXINT32;
    out_signal->lte_cqi_table_index = G_MAXINT32;
    out_signal->nr5g_timing_advance = G_MAXINT32;

#define SIGNAL_VALUE(index) ((gint32) read_le32 (&values[(index) * 4]))

    if (values_len == 60) {
        out_signal->version = 1;
        if (out_signal->rat_mask & (1 << 0)) {
            out_signal->has_gsm = TRUE;
            out_signal->gsm_signal_strength = SIGNAL_VALUE (0);
            out_signal->gsm_bit_error_rate = SIGNAL_VALUE (1);
            out_signal->has_wcdma = TRUE;
            out_signal->wcdma_signal_strength = SIGNAL_VALUE (0);
            out_signal->wcdma_bit_error_rate = SIGNAL_VALUE (1);
        }
        if (out_signal->rat_mask & (1 << 3)) {
            out_signal->has_lte = TRUE;
            out_signal->lte_signal_strength = SIGNAL_VALUE (7);
            out_signal->lte_rsrp = SIGNAL_VALUE (8);
            out_signal->lte_rsrq = SIGNAL_VALUE (9);
            out_signal->lte_rssnr = SIGNAL_VALUE (10);
            out_signal->lte_cqi = SIGNAL_VALUE (11);
            out_signal->lte_timing_advance = SIGNAL_VALUE (12);
        }
    } else if (values_len == 108 || values_len == 196 || values_len == 200) {
        out_signal->version = values_len == 108 ? 2 : values_len == 196 ? 4 : 5;
        if (out_signal->rat_mask & (1 << 0)) {
            out_signal->has_gsm = TRUE;
            out_signal->gsm_signal_strength = SIGNAL_VALUE (0);
            out_signal->gsm_bit_error_rate = SIGNAL_VALUE (1);
            out_signal->gsm_timing_advance = SIGNAL_VALUE (2);
        }
        if (out_signal->rat_mask & (1 << 1)) {
            out_signal->has_wcdma = TRUE;
            out_signal->wcdma_signal_strength = SIGNAL_VALUE (3);
            out_signal->wcdma_bit_error_rate = SIGNAL_VALUE (4);
            out_signal->wcdma_rscp = SIGNAL_VALUE (5);
            out_signal->wcdma_ecno = SIGNAL_VALUE (6);
        }
        if (out_signal->rat_mask & (1 << 2)) {
            out_signal->has_lte = TRUE;
            out_signal->lte_signal_strength = SIGNAL_VALUE (15);
            out_signal->lte_rsrp = SIGNAL_VALUE (16);
            out_signal->lte_rsrq = SIGNAL_VALUE (17);
            out_signal->lte_rssnr = SIGNAL_VALUE (18);
            if (values_len == 108) {
                out_signal->lte_cqi = SIGNAL_VALUE (19);
                out_signal->lte_timing_advance = SIGNAL_VALUE (20);
            } else {
                out_signal->lte_cqi_table_index = SIGNAL_VALUE (19);
                out_signal->lte_cqi = SIGNAL_VALUE (20);
                out_signal->lte_timing_advance = SIGNAL_VALUE (21);
            }
        }
        if (out_signal->rat_mask & (1 << 6)) {
            guint nr_offset = values_len == 108 ? 21 : 22;

            out_signal->has_nr5g = TRUE;
            out_signal->nr5g_ss_rsrp = SIGNAL_VALUE (nr_offset + 0);
            out_signal->nr5g_ss_rsrq = SIGNAL_VALUE (nr_offset + 1);
            out_signal->nr5g_ss_sinr = SIGNAL_VALUE (nr_offset + 2);
            out_signal->nr5g_csi_rsrp = SIGNAL_VALUE (nr_offset + 3);
            out_signal->nr5g_csi_rsrq = SIGNAL_VALUE (nr_offset + 4);
            out_signal->nr5g_csi_sinr = SIGNAL_VALUE (nr_offset + 5);
            if (values_len == 200)
                out_signal->nr5g_timing_advance = SIGNAL_VALUE (49);
        }
    } else {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     "Unsupported SIT signal-strength layout (%" G_GSIZE_FORMAT " bytes)",
                     values_len);
        return FALSE;
    }

#undef SIGNAL_VALUE
    return TRUE;
}

gboolean
mm_sit_message_parse_cell_info_list (const MMSitMessage *message,
                                     GArray            **out_cells,
                                     GError            **error)
{
    static const guint record_sizes[] = { 0xa4, 0x2b, 0x13c, 0x114, 0x110, 0x18c };
    const guint8      *data;
    gsize              len;
    gsize              offset = 16;
    guint32            count;
    guint32            i;
    g_autoptr(GArray)   cells = NULL;

    g_return_val_if_fail (out_cells != NULL, FALSE);
    *out_cells = NULL;
    if (!message || message->id != MM_SIT_MESSAGE_GET_CELL_INFO_LIST ||
        message->type != MM_SIT_MESSAGE_TYPE_RESPONSE) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Message is not a SIT cell-info-list response");
        return FALSE;
    }
    data = mm_sit_message_get_data (message, &len);
    if (len < offset) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "SIT cell-info-list response is truncated");
        return FALSE;
    }

    count = read_le32 (&data[12]);
    cells = g_array_new (FALSE, FALSE, sizeof (MMSitCellInfoLte));
    for (i = 0; i < count; i++) {
        const guint8 *record;
        const guint8 *lte;
        guint         type;
        guint         record_size;

        if (offset + 3 > len) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "SIT cell-info record header is truncated");
            return FALSE;
        }
        record = &data[offset];
        type = record[0];
        if (type >= G_N_ELEMENTS (record_sizes)) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "Unknown SIT V5 cell-info type %u", type);
            return FALSE;
        }
        record_size = record_sizes[type];
        if (offset + record_size > len) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "SIT V5 cell-info type %u is truncated", type);
            return FALSE;
        }

        /* Raw type 2 is RIL_CELL_INFO_TYPE_LTE. Skip other valid V5 records
         * until their ModemManager mappings are implemented. */
        if (type == 2) {
            MMSitCellInfoLte cell = { 0 };
            guint            mnc_len;

            lte = &record[3];
            memcpy (cell.operator_id, lte, 3);
            mnc_len = lte[5] == '#' ? 2 : 3;
            memcpy (&cell.operator_id[3], &lte[3], mnc_len);
            cell.operator_id[3 + mnc_len] = '\0';
            cell.registered = record[1] == 1;
            cell.connection_status = record[2];
            cell.ci = read_le32 (&lte[6]);
            cell.physical_ci = read_le32 (&lte[10]);
            cell.tac = read_le32 (&lte[14]);
            cell.earfcn = read_le32 (&lte[18]);
            cell.bandwidth = read_le32 (&lte[22]);
            cell.rsrp = (gint32) read_le32 (&lte[0x121]);
            cell.rsrq = (gint32) read_le32 (&lte[0x125]);
            cell.rssnr = (gint32) read_le32 (&lte[0x129]);
            cell.timing_advance = (gint32) read_le32 (&lte[0x135]);
            g_array_append_val (cells, cell);
        }
        offset += record_size;
    }
    if (offset != len) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT cell-info-list has %" G_GSIZE_FORMAT " trailing bytes", len - offset);
        return FALSE;
    }
    *out_cells = g_steal_pointer (&cells);
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
