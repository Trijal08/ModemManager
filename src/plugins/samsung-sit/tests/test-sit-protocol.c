/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mm-port-serial-sit.h"
#include "mm-sit-carrier-config.h"
#include "mm-sit-client.h"
#include "mm-sit-protocol.h"

static void
assert_bytes (GByteArray  *actual,
              const guint8 *expected,
              gsize         expected_len)
{
    g_assert_nonnull (actual);
    g_assert_cmpuint (actual->len, ==, expected_len);
    g_assert_cmpmem (actual->data, actual->len, expected, expected_len);
}

static void
set_le16 (guint8  *data,
          guint16  value)
{
    data[0] = value & 0xff;
    data[1] = value >> 8;
}

static void
set_le32 (guint8 *data,
          guint32 value)
{
    data[0] = value & 0xff;
    data[1] = (value >> 8) & 0xff;
    data[2] = (value >> 16) & 0xff;
    data[3] = value >> 24;
}

static void
test_parse_header (void)
{
    static const guint8 raw[] = {
        0x01, 0x00, 0x00, 0x08, 0x0c, 0x00, 0x34, 0x12,
        0x00, 0x00, 0x02, 0x00,
    };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;

    message = mm_sit_message_parse (raw, sizeof (raw), &error);
    g_assert_no_error (error);
    g_assert_nonnull (message);
    g_assert_cmpuint (message->type, ==, MM_SIT_MESSAGE_TYPE_RESPONSE);
    g_assert_cmphex (message->id, ==, MM_SIT_MESSAGE_SET_RADIO_POWER);
    g_assert_cmpuint (message->length, ==, sizeof (raw));
    g_assert_cmphex (message->token, ==, 0x1234);
    g_assert_true (message->has_status);
    g_assert_cmpuint (message->status, ==, 0);
}

static void
test_parse_invalid (void)
{
    static const guint8 short_header[] = { 1, 0, 0, 8 };
    static const guint8 bad_length[] = { 1, 0, 0, 8, 7, 0, 0, 0 };
    static const guint8 truncated[] = { 1, 0, 0, 8, 12, 0, 0, 0 };
    static const guint8 bad_type[] = { 3, 0, 0, 8, 8, 0, 0, 0 };
    g_autoptr(GError) error = NULL;

    g_assert_null (mm_sit_message_parse (short_header, sizeof (short_header), &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
    g_clear_error (&error);

    g_assert_null (mm_sit_message_parse (bad_length, sizeof (bad_length), &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    g_assert_null (mm_sit_message_parse (truncated, sizeof (truncated), &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
    g_clear_error (&error);

    g_assert_null (mm_sit_message_parse (bad_type, sizeof (bad_type), &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_message_id_names (void)
{
    g_assert_cmpstr (mm_sit_message_id_get_string (0x0000), ==, "GET_CALL_LIST");
    g_assert_cmpstr (mm_sit_message_id_get_string (0x020b), ==,
                     "GET_SIM_AUTHENTICATE / SIM_GET_ISIM_AUTH / SIM_GET_SIM_AUTH");
    g_assert_cmpstr (mm_sit_message_id_get_string (MM_SIT_MESSAGE_GET_DATA_REGISTRATION), ==,
                     "GET_DATA_REGISTRATION_STATE / GET_DATA_REGISTRATION");
    g_assert_cmpstr (mm_sit_message_id_get_string (MM_SIT_MESSAGE_SET_AP_SYSTEM_TIME), ==,
                     "SET_AP_SYSTEM_TIME");
    g_assert_cmpstr (mm_sit_message_id_get_string (0x4605), ==, "SV_NUMBER / OEM_CONFIG");
    g_assert_cmpstr (mm_sit_message_id_get_string (0xffff), ==, "UNKNOWN");
}

static void
test_build_radio (void)
{
    static const guint8 expected_set[] = {
        0x00, 0x00, 0x00, 0x08, 0x12, 0x00, 0x42, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00,
    };
    static const guint8 expected_get[] = {
        0x00, 0x00, 0x01, 0x08, 0x0c, 0x00, 0x43, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_radio_power (0x42, MM_SIT_RADIO_STATE_ON);
    assert_bytes (request, expected_set, sizeof (expected_set));

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_RADIO_POWER, 0x43);
    assert_bytes (request, expected_get, sizeof (expected_get));
}

static void
test_build_slot_mapping (void)
{
    static const guint8 expected[] = {
        0x00, 0x00, 0x50, 0x02, 0x15, 0x00, 0x34, 0x12,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00,
    };
    static const MMSitLogicalSlotMapping mappings[] = {
        { .physical_slot_id = 1, .port_id = 0 },
        { .physical_slot_id = 0, .port_id = 1 },
    };
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_logical_slot_mapping (0x1234,
                                                            mappings,
                                                            G_N_ELEMENTS (mappings),
                                                            &error);
    g_assert_no_error (error);
    assert_bytes (request, expected, sizeof (expected));

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_set_logical_slot_mapping (1, mappings, 0, &error);
    g_assert_null (request);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_build_data_requests (void)
{
    g_autoptr(GByteArray) profile = NULL;
    g_autoptr(GByteArray) attach = NULL;
    g_autoptr(GByteArray) setup = NULL;
    g_autoptr(GByteArray) deactivate = NULL;
    g_autoptr(GByteArray) list = NULL;
    g_autoptr(GByteArray) fast_dormancy = NULL;
    g_autoptr(GError)     error = NULL;

    profile = mm_sit_request_new_set_data_profile (1, "internet", &error);
    g_assert_no_error (error);
    g_assert_cmpuint (profile->len, ==, 246);
    g_assert_cmpmem (&profile->data[16], 8, "internet", 8);
    g_assert_cmphex (profile->data[12], ==, 0xff);
    g_assert_cmpuint (profile->data[0x75], ==, 1);
    g_assert_cmpuint (profile->data[0xe9], ==, 1);

    attach = mm_sit_request_new_set_initial_attach_apn (2, "internet", &error);
    g_assert_no_error (error);
    g_assert_cmpuint (attach->len, ==, 251);
    g_assert_cmpmem (&attach->data[16], 8, "internet", 8);
    g_assert_cmpuint (attach->data[0xda], ==, 1);
    g_assert_cmpuint (attach->data[0xf8], ==, 1);

    setup = mm_sit_request_new_setup_data_call (3, "internet", &error);
    g_assert_no_error (error);
    g_assert_cmpuint (setup->len, ==, 983);
    g_assert_cmpmem (&setup->data[16], 8, "internet", 8);
    g_assert_cmpuint (setup->data[0xf1], ==, 7);
    g_assert_cmphex (setup->data[0xfc], ==, 0xff);
    g_assert_cmphex (setup->data[0x101], ==, 0xff);

    deactivate = mm_sit_request_new_deactivate_data_call (4, 1, 0);
    g_assert_cmpuint (deactivate->len, ==, 14);
    g_assert_cmpuint (deactivate->data[12], ==, 1);

    list = mm_sit_request_new_get_data_call_list (5);
    g_assert_cmpuint (list->len, ==, 12);

    fast_dormancy = mm_sit_request_new_set_fast_dormancy_info (6);
    g_assert_cmpuint (fast_dormancy->len, ==, 16);
    g_assert_cmpmem (&fast_dormancy->data[12], 4, "\x0a\x0a\x0a\x0a", 4);
}

static void
test_build_config_requests (void)
{
    static const guint8 sha1[20] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
    };
    g_autoptr(GByteArray) info = NULL;
    g_autoptr(GByteArray) manifest = NULL;
    g_autoptr(GByteArray) screen_state = NULL;
    g_autoptr(GError)     error = NULL;

    info = mm_sit_request_new_device_info (2, "Pixel 9a", "TEST.BUILD", &error);
    g_assert_no_error (error);
    g_assert_cmpuint (info->len, ==, 140);
    g_assert_cmpmem (&info->data[12], 8, "Pixel 9a", 8);
    g_assert_cmpmem (&info->data[44], 10, "TEST.BUILD", 10);

    manifest = mm_sit_request_new_carrier_manifest (3, "/vendor/firmware/carrierconfig", sha1, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (manifest->len, ==, 532);
    g_assert_cmpuint (manifest->data[12], ==, 30);
    g_assert_cmpuint (manifest->data[0x110], ==, 20);
    g_assert_cmpmem (&manifest->data[0x114], 20, sha1, 20);

    screen_state = mm_sit_request_new_set_screen_state (4, FALSE);
    g_assert_cmpuint (screen_state->len, ==, 16);
    g_assert_cmpuint (screen_state->data[2], ==, 0x02);
    g_assert_cmpuint (screen_state->data[3], ==, 0x09);
    g_assert_cmpmem (&screen_state->data[12], 4, "\0\0\0\0", 4);
}

static void
test_build_boot_config_requests (void)
{
    /* Byte values from the downstream tegu-sit.trace boot capture. */
    static const guint8 expected_time[] = {
        0x00, 0x00, 0x49, 0x09, 0x12, 0x00, 0x42, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x7e, 0x06, 0x08, 0x0f,
        0x26, 0x0c,
    };
    static const guint8 expected_oem[] = {
        0x00, 0x00, 0x05, 0x46, 0x0e, 0x00, 0x43, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x33, 0x33,
    };
    static const guint8 expected_modem[] = {
        0x00, 0x00, 0x3f, 0x09, 0x0d, 0x00, 0x44, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    g_autoptr(GDateTime)  when = NULL;
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    when = g_date_time_new_local (2026, 6, 8, 15, 38, 12.0);
    request = mm_sit_request_new_set_ap_system_time (0x42, when, &error);
    g_assert_no_error (error);
    assert_bytes (request, expected_time, sizeof (expected_time));

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_oem_config (0x43);
    assert_bytes (request, expected_oem, sizeof (expected_oem));

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_set_modem_config (0x44);
    assert_bytes (request, expected_modem, sizeof (expected_modem));
}

static void
test_parse_radio (void)
{
    static const guint8 get_response[] = {
        1, 0, 1, 8, 16, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0,
    };
    static const guint8 get_response_android_on[] = {
        1, 0, 1, 8, 16, 0, 1, 0, 0, 0, 0, 0, 10, 0, 0, 0,
    };
    static const guint8 indication_on[] = {
        2, 0, 2, 8, 12, 0, 0, 0, 2, 0, 0, 0,
    };
    static const guint8 indication[] = {
        2, 0, 2, 8, 12, 0, 0, 0, 1, 0, 0, 0,
    };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitRadioState         state;

    message = mm_sit_message_parse (get_response, sizeof (get_response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_ON);

    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (get_response_android_on,
                                    sizeof (get_response_android_on), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_ON);

    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (indication_on, sizeof (indication_on), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_ON);

    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_UNKNOWN);
}

static void
test_parse_status_and_registration (void)
{
    guint8 sim_response[18] = { 1, 0, 0, 2, 18, 0, 1, 0 };
    guint8 reg_response[86] = { 1, 0, 1, 7, 86, 0, 2, 0 };
    guint8 operator_response[119] = { 1, 0, 2, 7, 119, 0, 3, 0 };
    guint8 operator_indication[131] = { 2, 0, 0x0e, 7, 131, 0, 0, 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitSimStatus          sim;
    MMSitRegistration       registration;
    MMSitOperator           operator_info;

    sim_response[12] = 1;
    sim_response[13] = 3;
    sim_response[14] = 2;
    sim_response[15] = 2;
    sim_response[16] = 5;
    sim_response[17] = 2;
    message = mm_sit_message_parse (sim_response, sizeof (sim_response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_sim_status (message, &sim, &error));
    g_assert_no_error (error);
    g_assert_true (sim.present);
    g_assert_cmpuint (sim.app_count, ==, 2);

    reg_response[12] = 1;
    reg_response[15] = 14;
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (reg_response, sizeof (reg_response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_registration (message, &registration, &error));
    g_assert_no_error (error);
    g_assert_true (registration.registered);
    g_assert_cmpuint (registration.state, ==, 1);
    g_assert_cmpuint (registration.radio_technology, ==, 14);

    memcpy (&operator_response[12], "26203#", 6);
    memcpy (&operator_response[18], "o2-de", 6);
    memcpy (&operator_response[50], "o2 - de", 8);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (operator_response, sizeof (operator_response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_operator (message, &operator_info, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (operator_info.plmn, ==, "26203");
    g_assert_cmpstr (operator_info.operator_short, ==, "o2-de");
    g_assert_cmpstr (operator_info.operator_long, ==, "o2 - de");

    memcpy (&operator_indication[9], "26203#", 6);
    memcpy (&operator_indication[35], "o2-de", 6);
    memcpy (&operator_indication[67], "o2 - de", 8);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (operator_indication, sizeof (operator_indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_operator (message, &operator_info, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (operator_info.plmn, ==, "26203");
    g_assert_cmpstr (operator_info.operator_short, ==, "o2-de");
    g_assert_cmpstr (operator_info.operator_long, ==, "o2 - de");
}

static void
test_parse_sim_slot_status (void)
{
    static const guint8 eid[] = {
        0x89, 0x01, 0x23, 0x45, 0x67, 0x89, 0x01, 0x23,
        0x45, 0x67, 0x89, 0x01, 0x23, 0x45, 0x67, 0x89,
    };
    static const guint8 iccid[] = {
        0x98, 0x10, 0x32, 0x54, 0x76, 0x98, 0x10, 0x32, 0x54, 0xf6,
    };
    guint8 indication[429] = { 0 };
    guint8 response[433] = { 0 };
    guint8 legacy[269] = { 0 };
    guint8 *slot0;
    guint8 *slot1;
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitSimSlotStatus      status;

    indication[0] = MM_SIT_MESSAGE_TYPE_INDICATION;
    set_le16 (&indication[2], MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED);
    set_le16 (&indication[4], sizeof (indication));
    indication[8] = 2;
    slot0 = &indication[9];
    slot0[0] = 1;
    slot0[1] = 2;
    slot0[2] = 0x3b;
    slot0[3] = 0x9f;
    slot0[35] = sizeof (eid);
    memcpy (&slot0[36], eid, sizeof (eid));
    slot0[52] = 2;
    slot0[53 + 11] = 0xff;
    slot0[53 + 12] = 0;
    slot0[66 + 11] = 0xff;
    slot0[66 + 12] = 0;

    slot1 = &indication[9 + 105];
    slot1[0] = 1;
    slot1[1] = 1;
    slot1[2] = 0x3b;
    slot1[52] = 1;
    slot1[53] = sizeof (iccid);
    memcpy (&slot1[54], iccid, sizeof (iccid));
    slot1[53 + 11] = 0;
    slot1[53 + 12] = 1;

    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_sim_slot_status (message, &status, &error));
    g_assert_no_error (error);
    g_assert_false (status.legacy);
    g_assert_cmpuint (status.slot_count, ==, 2);
    g_assert_cmpuint (status.slots[0].eid_len, ==, sizeof (eid));
    g_assert_cmpmem (status.slots[0].eid, status.slots[0].eid_len, eid, sizeof (eid));
    g_assert_cmpuint (status.slots[0].port_count, ==, 2);
    g_assert_cmpuint (status.slots[0].ports[0].logical_slot_id, ==, 0xff);
    g_assert_cmpuint (status.slots[1].port_count, ==, 1);
    g_assert_cmpuint (status.slots[1].ports[0].logical_slot_id, ==, 0);
    g_assert_cmpuint (status.slots[1].ports[0].state, ==, 1);
    g_assert_cmpmem (status.slots[1].ports[0].iccid,
                     status.slots[1].ports[0].iccid_len,
                     iccid, sizeof (iccid));

    response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
    set_le16 (&response[2], MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS);
    set_le16 (&response[4], sizeof (response));
    set_le16 (&response[6], 7);
    response[12] = indication[8];
    memcpy (&response[13], &indication[9], sizeof (indication) - 9);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (response, sizeof (response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_sim_slot_status (message, &status, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (status.slot_count, ==, 2);
    g_assert_cmpuint (status.slots[1].ports[0].logical_slot_id, ==, 0);

    response[12] = MM_SIT_MAX_SIM_SLOTS + 1;
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (response, sizeof (response), &error);
    g_assert_no_error (error);
    g_assert_false (mm_sit_message_parse_sim_slot_status (message, &status, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

    g_clear_error (&error);
    legacy[0] = MM_SIT_MESSAGE_TYPE_INDICATION;
    set_le16 (&legacy[2], MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED);
    set_le16 (&legacy[4], sizeof (legacy));
    legacy[8] = 2;
    slot0 = &legacy[9];
    slot0[0] = 1;
    slot0[1] = 0;
    slot0[2] = 1;
    slot0[3] = 0x3b;
    slot0[36] = 0xff;
    slot0[48] = sizeof (eid);
    memcpy (&slot0[49], eid, sizeof (eid));
    slot1 = &legacy[9 + 65];
    slot1[0] = 1;
    slot1[1] = 1;
    slot1[36] = 0;
    slot1[37] = sizeof (iccid);
    memcpy (&slot1[38], iccid, sizeof (iccid));

    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (legacy, sizeof (legacy), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_sim_slot_status (message, &status, &error));
    g_assert_no_error (error);
    g_assert_true (status.legacy);
    g_assert_cmpuint (status.slot_count, ==, 2);
    g_assert_cmpuint (status.slots[1].ports[0].logical_slot_id, ==, 0);
    g_assert_cmpuint (status.slots[1].ports[0].state, ==, 1);
}

static void
test_parse_signal (void)
{
    guint8 response_v5[214] = { 0 };
    guint8 indication_v5[210] = { 0 };
    guint8 response_v4[210] = { 0 };
    guint8 response_v2[122] = { 0 };
    guint8 response_v1[74] = { 0 };
    guint8 unsupported[114] = { 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitSignal             signal;

#define INIT_SIGNAL_FRAME(frame, message_type, message_id) G_STMT_START { \
    (frame)[0] = (message_type); \
    (frame)[2] = (message_id) & 0xff; \
    (frame)[3] = (message_id) >> 8; \
    (frame)[4] = sizeof (frame) & 0xff; \
    (frame)[5] = sizeof (frame) >> 8; \
} G_STMT_END

    INIT_SIGNAL_FRAME (response_v5, MM_SIT_MESSAGE_TYPE_RESPONSE,
                       MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH);
    response_v5[12] = 1 << 2;
    set_le32 (&response_v5[14 + 15 * 4], 27);
    set_le32 (&response_v5[14 + 16 * 4], 83);
    set_le32 (&response_v5[14 + 17 * 4], 11);
    set_le32 (&response_v5[14 + 18 * 4], 10);
    set_le32 (&response_v5[14 + 19 * 4], 1);
    set_le32 (&response_v5[14 + 20 * 4], 11);
    set_le32 (&response_v5[14 + 21 * 4], 2);
    message = mm_sit_message_parse (response_v5, sizeof (response_v5), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_signal (message, &signal, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (signal.version, ==, 5);
    g_assert_cmphex (signal.rat_mask, ==, 1 << 2);
    g_assert_true (signal.has_lte);
    g_assert_false (signal.has_gsm);
    g_assert_cmpint (signal.lte_signal_strength, ==, 27);
    g_assert_cmpint (signal.lte_rsrp, ==, 83);
    g_assert_cmpint (signal.lte_rsrq, ==, 11);
    g_assert_cmpint (signal.lte_rssnr, ==, 10);
    g_assert_cmpint (signal.lte_cqi_table_index, ==, 1);
    g_assert_cmpint (signal.lte_cqi, ==, 11);
    g_assert_cmpint (signal.lte_timing_advance, ==, 2);

    INIT_SIGNAL_FRAME (indication_v5, MM_SIT_MESSAGE_TYPE_INDICATION,
                       MM_SIT_MESSAGE_SIGNAL_STRENGTH_CHANGED);
    memcpy (&indication_v5[8], &response_v5[12], sizeof (indication_v5) - 8);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (indication_v5, sizeof (indication_v5), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_signal (message, &signal, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (signal.version, ==, 5);
    g_assert_cmpint (signal.lte_rsrp, ==, 83);

    INIT_SIGNAL_FRAME (response_v4, MM_SIT_MESSAGE_TYPE_RESPONSE,
                       MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH);
    response_v4[12] = 1 << 2;
    set_le32 (&response_v4[14 + 16 * 4], 95);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (response_v4, sizeof (response_v4), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_signal (message, &signal, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (signal.version, ==, 4);
    g_assert_cmpint (signal.lte_rsrp, ==, 95);

    INIT_SIGNAL_FRAME (response_v2, MM_SIT_MESSAGE_TYPE_RESPONSE,
                       MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH);
    response_v2[12] = 1 << 2;
    set_le32 (&response_v2[14 + 16 * 4], 101);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (response_v2, sizeof (response_v2), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_signal (message, &signal, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (signal.version, ==, 2);
    g_assert_cmpint (signal.lte_rsrp, ==, 101);
    g_assert_cmpint (signal.lte_cqi_table_index, ==, G_MAXINT32);

    INIT_SIGNAL_FRAME (response_v1, MM_SIT_MESSAGE_TYPE_RESPONSE,
                       MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH);
    response_v1[12] = 1 << 3;
    set_le32 (&response_v1[14 + 8 * 4], 105);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (response_v1, sizeof (response_v1), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_signal (message, &signal, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (signal.version, ==, 1);
    g_assert_cmpint (signal.lte_rsrp, ==, 105);

    INIT_SIGNAL_FRAME (unsupported, MM_SIT_MESSAGE_TYPE_RESPONSE,
                       MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (unsupported, sizeof (unsupported), &error);
    g_assert_no_error (error);
    g_assert_false (mm_sit_message_parse_signal (message, &signal, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

#undef INIT_SIGNAL_FRAME
}

static void
test_parse_cell_info_lte_v5 (void)
{
    guint8                   raw[332] = { 1, 0, 0x0c, 0x07, 0x4c, 0x01, 1, 0 };
    guint8                  *record = &raw[16];
    guint8                  *lte = &record[3];
    g_autoptr(MMSitMessage)  message = NULL;
    g_autoptr(GArray)        cells = NULL;
    g_autoptr(GError)        error = NULL;
    MMSitCellInfoLte        *cell;

    /* Offset 8 is the transport status word (zero on success), per a live
     * cellmapper capture; the record count follows at offset 12. */
    set_le32 (&raw[8], 0);
    set_le32 (&raw[12], 1);
    record[0] = 2;
    record[1] = 1;
    record[2] = 1;
    memcpy (lte, "26203#", 6);
    set_le32 (&lte[6], 0x019de53f);
    set_le32 (&lte[10], 191);
    set_le32 (&lte[14], 51510);
    set_le32 (&lte[18], 3350);
    set_le32 (&lte[22], 20000);
    set_le32 (&lte[0x119], 1);
    set_le32 (&lte[0x11d], 41);
    set_le32 (&lte[0x121], 100);
    set_le32 (&lte[0x125], 9);
    set_le32 (&lte[0x129], 20);
    set_le32 (&lte[0x131], 255);
    set_le32 (&lte[0x135], 2);

    message = mm_sit_message_parse (raw, sizeof (raw), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_cell_info_list (message, &cells, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (cells->len, ==, 1);
    cell = &g_array_index (cells, MMSitCellInfoLte, 0);
    g_assert_true (cell->registered);
    g_assert_cmpstr (cell->operator_id, ==, "26203");
    g_assert_cmphex (cell->ci, ==, 0x019de53f);
    g_assert_cmpuint (cell->physical_ci, ==, 191);
    g_assert_cmpuint (cell->tac, ==, 51510);
    g_assert_cmpuint (cell->earfcn, ==, 3350);
    g_assert_cmpuint (cell->bandwidth, ==, 20000);
    g_assert_cmpint (cell->rsrp, ==, 100);
    g_assert_cmpint (cell->rsrq, ==, 9);
    g_assert_cmpint (cell->rssnr, ==, 20);
    g_assert_cmpint (cell->timing_advance, ==, 2);
}

static void
test_parse_data_call (void)
{
    guint8 response[304] = { 1, 0, 0, 6, 0x30, 0x01, 1, 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitDataCall           call;

    response[14] = 1;
    response[15] = 1;
    response[16] = 1;
    response[17] = 10;
    response[18] = 20;
    response[19] = 30;
    response[20] = 40;
    response[38] = 1;
    response[39] = 1;
    response[40] = 1;
    response[41] = 1;
    response[58] = 8;
    response[59] = 8;
    response[60] = 8;
    response[61] = 8;
    response[139] = 0xdc;
    response[140] = 0x05;

    message = mm_sit_message_parse (response, sizeof (response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_data_call (message, &call, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (call.cid, ==, 1);
    g_assert_cmpuint (call.active, ==, 1);
    g_assert_true (call.has_ipv4);
    g_assert_cmpstr (call.ipv4, ==, "10.20.30.40");
    g_assert_cmpstr (call.dns1, ==, "1.1.1.1");
    g_assert_cmpstr (call.dns2, ==, "8.8.8.8");
    g_assert_cmpuint (call.mtu, ==, 1500);
}

static void
test_parse_data_call_list (void)
{
    /* DATA_CALL_LIST_CHANGED (0x0604) with two PDNs: a default + a second call,
     * items back-to-back at the 132-byte base stride. Synthetic private IPs. */
    guint8 ind[273] = { 2, 0, 0x04, 0x06, 0x11, 0x01, 0, 0 }; /* len = 273 */
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    g_autoptr(GArray)       calls = NULL;
    const MMSitDataCall    *c0, *c1;

    ind[8] = 2; /* count */
    /* item 0 @9: cid 2, 10.0.0.2, mtu 1500 */
    ind[11] = 2; ind[12] = 1; ind[13] = 1;
    ind[14] = 10; ind[15] = 0; ind[16] = 0; ind[17] = 2;
    ind[136] = 0xdc; ind[137] = 0x05;
    /* item 1 @141: cid 3, 10.0.0.3, mtu 1428 */
    ind[143] = 3; ind[144] = 1; ind[145] = 1;
    ind[146] = 10; ind[147] = 0; ind[148] = 0; ind[149] = 3;
    ind[268] = 0x94; ind[269] = 0x05;

    message = mm_sit_message_parse (ind, sizeof (ind), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_data_call_list (message, &calls, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (calls->len, ==, 2);

    c0 = &g_array_index (calls, MMSitDataCall, 0);
    g_assert_cmpuint (c0->cid, ==, 2);
    g_assert_cmpstr (c0->ipv4, ==, "10.0.0.2");
    g_assert_cmpuint (c0->mtu, ==, 1500);

    c1 = &g_array_index (calls, MMSitDataCall, 1);
    g_assert_cmpuint (c1->cid, ==, 3);
    g_assert_cmpstr (c1->ipv4, ==, "10.0.0.3");
    g_assert_cmpuint (c1->mtu, ==, 1428);
}

static void
test_parse_carrier_sim_info (void)
{
    guint8 raw[207] = { 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitCarrierSimInfo     info;

    raw[0] = MM_SIT_MESSAGE_TYPE_INDICATION;
    raw[2] = MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO & 0xff;
    raw[3] = MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO >> 8;
    raw[4] = sizeof (raw) & 0xff;
    raw[5] = sizeof (raw) >> 8;
    raw[8] = (sizeof (raw) - MM_SIT_HEADER_SIZE) & 0xff;
    memcpy (&raw[12], "8901234567890123456", 19);
    memcpy (&raw[33], "262071234567890", 15);

    message = mm_sit_message_parse (raw, sizeof (raw), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_carrier_sim_info (message, &info, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (info.iccid, ==, "8901234567890123456");
    g_assert_cmpstr (info.imsi, ==, "262071234567890");

    raw[8]--;
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (raw, sizeof (raw), &error);
    g_assert_no_error (error);
    g_assert_false (mm_sit_message_parse_carrier_sim_info (message, &info, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
database_exec (sqlite3     *database,
               const gchar *sql)
{
    gchar *message = NULL;

    g_assert_cmpint (sqlite3_exec (database, sql, NULL, NULL, &message), ==, SQLITE_OK);
    g_assert_null (message);
}

static void
create_manifest (const gchar *directory,
                 const gchar *hash)
{
    g_autofree gchar *path = g_build_filename (directory, "manifests", hash, NULL);

    g_assert_true (g_file_set_contents (path, "", 0, NULL));
}

static void
remove_manifest (const gchar *directory,
                 const gchar *hash)
{
    g_autofree gchar *path = g_build_filename (directory, "manifests", hash, NULL);

    g_assert_cmpint (g_remove (path), ==, 0);
}

static void
test_carrier_config_resolver (void)
{
    static const gchar hash_global[] = "0000000000000000000000000000000000000000";
    static const gchar hash_base[] = "1111111111111111111111111111111111111111";
    static const gchar hash_imsi[] = "2222222222222222222222222222222222222222";
    static const gchar hash_gid[] = "3333333333333333333333333333333333333333";
    static const gchar hash_both_gids[] = "6666666666666666666666666666666666666666";
    static const gchar hash_france[] = "7777777777777777777777777777777777777777";
    static const gchar hash_france_iin[] = "8888888888888888888888888888888888888888";
    static const gchar hash_iin[] = "4444444444444444444444444444444444444444";
    static const gchar hash_rule[] = "5555555555555555555555555555555555555555";
    g_autofree gchar *directory = NULL;
    g_autofree gchar *manifest_directory = NULL;
    g_autofree gchar *database_path = NULL;
    sqlite3          *database = NULL;
    MMSitCarrierIdentity identity = { 0 };
    g_autoptr(MMSitCarrierConfig) config = NULL;
    g_autoptr(GError) error = NULL;

    directory = g_dir_make_tmp ("mm-sit-carrier-XXXXXX", &error);
    g_assert_no_error (error);
    g_assert_nonnull (directory);
    manifest_directory = g_build_filename (directory, "manifests", NULL);
    g_assert_cmpint (g_mkdir (manifest_directory, 0700), ==, 0);
    database_path = g_build_filename (directory, "cfg.db", NULL);
    g_assert_cmpint (sqlite3_open (database_path, &database), ==, SQLITE_OK);
    database_exec (database,
                   "CREATE TABLE carrier_info (carrier_id INTEGER NOT NULL,mccmnc TEXT NOT NULL,"
                   "imsi_prefix_xpattern TEXT,spn TEXT,plmn_name TEXT,gid1 TEXT,gid2 TEXT,"
                   "preferred_apn TEXT,iccid_prefix TEXT,privelege_access_rule TEXT);"
                   "CREATE TABLE confmap (carrier_id TEXT NOT NULL UNIQUE,parent_id TEXT,confman TEXT NOT NULL);"
                   "CREATE TABLE confnames (carrier_id INTEGER NOT NULL UNIQUE,name TEXT NOT NULL);"
                   "CREATE TABLE iin (carrier_id TEXT,iccid_prefix TEXT);"
                   "CREATE TABLE regional_fallback (country_code TEXT,carrier_id TEXT);"
                   "INSERT INTO carrier_info VALUES (1,'26207','%','%',NULL,'%','%',NULL,NULL,NULL);"
                   "INSERT INTO carrier_info VALUES (2,'26207','26207123%','%',NULL,'%','%',NULL,NULL,NULL);"
                   "INSERT INTO carrier_info VALUES (3,'26207','%','Special',NULL,'AB','%',NULL,NULL,NULL);"
                   "INSERT INTO carrier_info VALUES (4,'26207','%','Wrong SPN',NULL,'AB','CD',NULL,NULL,NULL);"
                   "INSERT INTO carrier_info VALUES (5,'26207','%','%',NULL,'%','%',NULL,NULL,'RULE');"
                   "INSERT INTO carrier_info VALUES (6,'20801','%','%',NULL,'%','%',NULL,NULL,NULL);"
                   "INSERT INTO carrier_info VALUES (7,'20801','%','%',NULL,'%','%',NULL,'894936',NULL);"
                   "INSERT INTO confmap VALUES ('0',NULL,'0000000000000000000000000000000000000000');"
                   "INSERT INTO confmap VALUES ('1',NULL,'1111111111111111111111111111111111111111');"
                   "INSERT INTO confmap VALUES ('2',NULL,'2222222222222222222222222222222222222222');"
                   "INSERT INTO confmap VALUES ('3',NULL,'3333333333333333333333333333333333333333');"
                   "INSERT INTO confmap VALUES ('4',NULL,'6666666666666666666666666666666666666666');"
                   "INSERT INTO confmap VALUES ('6',NULL,'7777777777777777777777777777777777777777');"
                   "INSERT INTO confmap VALUES ('7',NULL,'8888888888888888888888888888888888888888');"
                   "INSERT INTO confmap VALUES ('9',NULL,'4444444444444444444444444444444444444444');"
                   "INSERT INTO confmap VALUES ('5',NULL,'5555555555555555555555555555555555555555');"
                   "INSERT INTO confnames VALUES (0,'wildcard');"
                   "INSERT INTO confnames VALUES (1,'base');"
                   "INSERT INTO confnames VALUES (2,'imsi');"
                   "INSERT INTO confnames VALUES (3,'gid');"
                   "INSERT INTO confnames VALUES (4,'both-gids');"
                   "INSERT INTO confnames VALUES (6,'france');"
                   "INSERT INTO confnames VALUES (7,'france-iin');"
                   "INSERT INTO confnames VALUES (9,'iin');"
                   "INSERT INTO confnames VALUES (5,'rule');"
                   "INSERT INTO iin VALUES ('9','89999%');"
                   "INSERT INTO regional_fallback VALUES ('0','0');");
    g_assert_cmpint (sqlite3_close (database), ==, SQLITE_OK);
    database = NULL;

    create_manifest (directory, hash_global);
    create_manifest (directory, hash_base);
    create_manifest (directory, hash_imsi);
    create_manifest (directory, hash_gid);
    create_manifest (directory, hash_both_gids);
    create_manifest (directory, hash_france);
    create_manifest (directory, hash_france_iin);
    create_manifest (directory, hash_iin);
    create_manifest (directory, hash_rule);

    identity.imsi = "262071234567890";
    identity.iccid = "8999912345678901234";
    identity.mccmnc = "26207";
    identity.spn = "Special";
    identity.gid1 = "AB";
    identity.gid2 = "CD";
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 4);
    g_assert_cmpstr (config->carrier_name, ==, "both-gids");
    g_assert_cmpstr (config->manifest_sha1, ==, hash_both_gids);

    g_clear_pointer (&config, mm_sit_carrier_config_free);
    identity.spn = NULL;
    identity.gid1 = NULL;
    identity.gid2 = NULL;
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 2);
    g_assert_cmpstr (config->manifest_sha1, ==, hash_imsi);

    g_clear_pointer (&config, mm_sit_carrier_config_free);
    identity.imsi = "208010000000000";
    identity.iccid = "8949361234567890123";
    identity.mccmnc = NULL;
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 7);
    g_assert_cmpstr (config->manifest_sha1, ==, hash_france_iin);

    g_clear_pointer (&config, mm_sit_carrier_config_free);
    identity.iccid = "8999912345678901234";
    identity.mccmnc = "20801";
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 6);
    g_assert_cmpstr (config->manifest_sha1, ==, hash_france);

    g_clear_pointer (&config, mm_sit_carrier_config_free);
    identity.imsi = "999991234567890";
    identity.mccmnc = "99999";
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 9);
    g_assert_cmpstr (config->manifest_sha1, ==, hash_iin);

    g_clear_pointer (&config, mm_sit_carrier_config_free);
    identity.iccid = "12345";
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 0);
    g_assert_cmpstr (config->manifest_sha1, ==, hash_global);

    g_clear_pointer (&config, mm_sit_carrier_config_free);
    identity.imsi = "262071234567890";
    identity.mccmnc = "26207";
    identity.privilege_access_rule = "rule";
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 5);
    g_assert_cmpstr (config->manifest_sha1, ==, hash_rule);

    g_assert_cmpint (g_remove (config->manifest_path), ==, 0);
    g_clear_pointer (&config, mm_sit_carrier_config_free);
    g_assert_false (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error (&error);
    g_assert_cmpint (g_remove (database_path), ==, 0);
    remove_manifest (directory, hash_global);
    remove_manifest (directory, hash_base);
    remove_manifest (directory, hash_imsi);
    remove_manifest (directory, hash_gid);
    remove_manifest (directory, hash_both_gids);
    remove_manifest (directory, hash_france);
    remove_manifest (directory, hash_france_iin);
    remove_manifest (directory, hash_iin);
    g_assert_cmpint (g_rmdir (manifest_directory), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_carrier_config_reference_database (void)
{
    const gchar          *database_path;
    MMSitCarrierIdentity  identity = { 0 };
    g_autoptr(MMSitCarrierConfig) config = NULL;
    g_autoptr(GError)     error = NULL;

    database_path = g_getenv ("MM_SIT_TEST_CARRIER_DATABASE");
    if (!database_path) {
        g_test_skip ("MM_SIT_TEST_CARRIER_DATABASE is not set");
        return;
    }

    /* Synthetic subscriber digits with the same captured O2-DE MCC/MNC. */
    identity.imsi = "262072000000000";
    identity.iccid = "8900000000000000000";
    identity.mccmnc = "26207";
    g_assert_true (mm_sit_carrier_config_resolve (database_path, &identity, &config, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (config->carrier_id, ==, 1454);
    g_assert_cmpstr (config->carrier_name, ==, "de_o2");
    g_assert_cmpstr (config->manifest_sha1, ==, "c5a7aca02e9a6b8d20ee742c87a941b4cac55676");
}

typedef struct {
    gint       peer_fd;
    GMainLoop *loop;
    guint      indication_count;
    gboolean   command_completed;
    gboolean   timed_out;
} PortTestContext;

static gboolean
write_all (gint          fd,
           const guint8 *buffer,
           gsize         len)
{
    while (len) {
        gssize written;

        written = write (fd, buffer, len);
        if (written <= 0)
            return FALSE;
        buffer += written;
        len -= written;
    }
    return TRUE;
}

static gpointer
port_test_peer (PortTestContext *ctx)
{
    guint8 request[64];
    guint8 reply[24] = {
        /* A radio-state indication before the solicited response. */
        2, 0, 2, 8, 12, 0, 0, 0, 2, 0, 0, 0,
        /* Response header/body completed below from the request. */
        1, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0,
    };
    gssize request_len;

    request_len = read (ctx->peer_fd, request, sizeof (request));
    g_assert_cmpint (request_len, >=, MM_SIT_HEADER_SIZE);
    reply[14] = request[2];
    reply[15] = request[3];
    reply[18] = request[6];
    reply[19] = request[7];
    g_assert_true (write_all (ctx->peer_fd, reply, sizeof (reply)));
    return NULL;
}

static void
port_test_indication (MMPortSerialSit    *port,
                      const MMSitMessage *message,
                      PortTestContext    *ctx)
{
    MMSitRadioState state;

    g_assert_cmphex (message->id, ==, MM_SIT_MESSAGE_RADIO_STATE_CHANGED);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, NULL));
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_ON);
    ctx->indication_count++;
}

static void
port_test_command_ready (MMPortSerialSit *port,
                         GAsyncResult    *res,
                         PortTestContext *ctx)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;

    response = mm_port_serial_sit_command_finish (port, res, &error);
    g_assert_no_error (error);
    g_assert_nonnull (response);
    g_assert_cmphex (response->id, ==, MM_SIT_MESSAGE_GET_SIM_STATUS);
    g_assert_cmphex (response->token, ==, 1);
    g_assert_cmpuint (response->status, ==, 0);
    ctx->command_completed = TRUE;
    g_main_loop_quit (ctx->loop);
}

static gboolean
port_test_timeout (PortTestContext *ctx)
{
    ctx->timed_out = TRUE;
    g_main_loop_quit (ctx->loop);
    return G_SOURCE_REMOVE;
}

static void
test_port_command_and_indication (void)
{
    PortTestContext            ctx = { 0 };
    gint                       sockets[2];
    guint                      handler_id;
    guint                      timeout_id;
    g_autoptr(GThread)         peer = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(GByteArray)      request = NULL;
    g_autoptr(GError)          error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);

    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    g_assert_true (mm_port_serial_open (MM_PORT_SERIAL (port), &error));
    g_assert_no_error (error);
    handler_id = mm_port_serial_sit_add_unsolicited_handler (
        port,
        (MMPortSerialSitUnsolicitedFn) port_test_indication,
        &ctx,
        NULL);

    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_STATUS,
                                        mm_port_serial_sit_next_token (port));
    peer = g_thread_new ("sit-test-peer", (GThreadFunc) port_test_peer, &ctx);
    mm_port_serial_sit_command (port,
                                request,
                                2,
                                NULL,
                                (GAsyncReadyCallback) port_test_command_ready,
                                &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) port_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);

    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    g_thread_join (g_steal_pointer (&peer));
    mm_port_serial_sit_remove_unsolicited_handler (port, handler_id);
    mm_port_serial_close (MM_PORT_SERIAL (port));
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);

    g_assert_false (ctx.timed_out);
    g_assert_true (ctx.command_completed);
    g_assert_cmpuint (ctx.indication_count, ==, 1);
}

typedef struct {
    gint          peer_fd;
    GMainLoop    *loop;
    MMSitDataCall call;
    GError       *error;
    gboolean      completed;
    gboolean      timed_out;
} ClientTestContext;

static gboolean
read_all (gint    fd,
          guint8 *buffer,
          gsize   len)
{
    while (len) {
        gssize n = read (fd, buffer, len);

        if (n <= 0)
            return FALSE;
        buffer += n;
        len -= n;
    }
    return TRUE;
}

static gpointer
client_test_peer (ClientTestContext *ctx)
{
    static const guint16 expected_ids[] = {
        MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM,
        MM_SIT_MESSAGE_SET_DATA_PROFILE,
        MM_SIT_MESSAGE_SET_INITIAL_ATTACH_APN,
        0x0900,
        0x0106,
        0x0107,
        MM_SIT_MESSAGE_GET_DATA_REGISTRATION,
        MM_SIT_MESSAGE_SETUP_DATA_CALL,
        MM_SIT_MESSAGE_SET_FAST_DORMANCY_INFO,
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (expected_ids); i++) {
        guint8  header[MM_SIT_HEADER_SIZE];
        guint8  response[304] = { 0 };
        guint16 request_len;
        guint16 response_len = 12;

        g_assert_true (read_all (ctx->peer_fd, header, sizeof (header)));
        request_len = header[4] | (((guint16) header[5]) << 8);
        g_assert_cmpuint (header[2] | (((guint16) header[3]) << 8), ==, expected_ids[i]);
        g_assert_cmpuint (request_len, >=, MM_SIT_HEADER_SIZE);
        if (request_len > MM_SIT_HEADER_SIZE) {
            g_autofree guint8 *body = g_malloc (request_len - MM_SIT_HEADER_SIZE);

            g_assert_true (read_all (ctx->peer_fd, body, request_len - MM_SIT_HEADER_SIZE));
        }

        if (expected_ids[i] == MM_SIT_MESSAGE_GET_DATA_REGISTRATION) {
            response_len = 86;
            response[12] = 1;
            response[15] = 14;
        } else if (expected_ids[i] == MM_SIT_MESSAGE_SETUP_DATA_CALL) {
            response_len = sizeof (response);
            response[14] = 1;
            response[15] = 1;
            response[16] = 1;
            response[17] = 10;
            response[18] = 20;
            response[19] = 30;
            response[20] = 40;
            response[38] = 1;
            response[39] = 1;
            response[40] = 1;
            response[41] = 1;
            response[58] = 8;
            response[59] = 8;
            response[60] = 8;
            response[61] = 8;
            response[139] = 0xdc;
            response[140] = 0x05;
        }

        response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
        response[2] = header[2];
        response[3] = header[3];
        response[4] = response_len & 0xff;
        response[5] = response_len >> 8;
        response[6] = header[6];
        response[7] = header[7];
        g_assert_true (write_all (ctx->peer_fd, response, response_len));
    }
    return NULL;
}

static void
client_test_connect_ready (MMSitClient      *client,
                           GAsyncResult     *res,
                           ClientTestContext *ctx)
{
    ctx->completed = mm_sit_client_connect_data_finish (client, res, &ctx->call, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
client_test_timeout (ClientTestContext *ctx)
{
    ctx->timed_out = TRUE;
    g_main_loop_quit (ctx->loop);
    return G_SOURCE_REMOVE;
}

static void
test_client_connect_sequence (void)
{
    ClientTestContext          ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    g_autoptr(GThread)         peer = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient)     client = NULL;
    g_autoptr(GError)          error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit-client",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    g_assert_no_error (error);

    peer = g_thread_new ("sit-client-test-peer", (GThreadFunc) client_test_peer, &ctx);
    mm_sit_client_connect_data (client, "internet", NULL,
                                (GAsyncReadyCallback) client_test_connect_ready, &ctx);
    timeout_id = g_timeout_add_seconds (10, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);

    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    g_thread_join (g_steal_pointer (&peer));
    mm_sit_client_close (client);
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);

    g_assert_false (ctx.timed_out);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.completed);
    g_assert_cmpuint (ctx.call.cid, ==, 1);
    g_assert_cmpstr (ctx.call.ipv4, ==, "10.20.30.40");
    g_assert_cmpstr (ctx.call.dns1, ==, "1.1.1.1");
    g_assert_cmpuint (ctx.call.mtu, ==, 1500);
}

static gpointer
client_radio_test_peer (ClientTestContext *ctx)
{
    /* The client always sends the power request first (a parked CP claims
     * "on" without ever searching), then polls the resulting state. */
    static const guint16 expected_ids[] = {
        MM_SIT_MESSAGE_SET_RADIO_POWER,
        MM_SIT_MESSAGE_GET_RADIO_POWER,
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (expected_ids); i++) {
        guint8  header[MM_SIT_HEADER_SIZE];
        guint8  response[16] = { 0 };
        guint16 request_len;
        guint16 response_len;

        g_assert_true (read_all (ctx->peer_fd, header, sizeof (header)));
        request_len = header[4] | (((guint16) header[5]) << 8);
        g_assert_cmpuint (header[2] | (((guint16) header[3]) << 8), ==, expected_ids[i]);
        if (request_len > MM_SIT_HEADER_SIZE) {
            g_autofree guint8 *body = g_malloc (request_len - MM_SIT_HEADER_SIZE);

            g_assert_true (read_all (ctx->peer_fd, body, request_len - MM_SIT_HEADER_SIZE));
        }

        response_len = (expected_ids[i] == MM_SIT_MESSAGE_GET_RADIO_POWER) ? 16 : 12;
        response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
        response[2] = header[2];
        response[3] = header[3];
        response[4] = response_len;
        response[6] = header[6];
        response[7] = header[7];
        if (response_len == 16)
            response[12] = MM_SIT_RADIO_STATE_ON;
        else
            response[10] = MM_SIT_RADIO_STATE_ON;
        g_assert_true (write_all (ctx->peer_fd, response, response_len));
    }
    return NULL;
}

static void
client_radio_test_ready (MMSitClient       *client,
                         GAsyncResult      *res,
                         ClientTestContext *ctx)
{
    ctx->completed = mm_sit_client_set_radio_state_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static void
test_client_radio_transition (void)
{
    ClientTestContext          ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    g_autoptr(GThread)         peer = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient)     client = NULL;
    g_autoptr(GError)          error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit-radio",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    g_assert_no_error (error);

    peer = g_thread_new ("sit-radio-test-peer", (GThreadFunc) client_radio_test_peer, &ctx);
    g_assert_false (mm_sit_client_radio_state_trusted (client));
    mm_sit_client_set_radio_state (client, MM_SIT_RADIO_STATE_ON, NULL,
                                   (GAsyncReadyCallback) client_radio_test_ready, &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);

    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    g_thread_join (g_steal_pointer (&peer));

    g_assert_false (ctx.timed_out);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.completed);
    g_assert_true (mm_sit_client_radio_state_trusted (client));

    mm_sit_client_close (client);
    g_assert_false (mm_sit_client_radio_state_trusted (client));
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);
}

static gpointer
client_boot_config_test_peer (ClientTestContext *ctx)
{
    static const guint16 expected_ids[] = {
        MM_SIT_MESSAGE_SET_AP_SYSTEM_TIME,
        MM_SIT_MESSAGE_DEVICE_INFO,
        MM_SIT_MESSAGE_OEM_CONFIG,
        MM_SIT_MESSAGE_SET_MODEM_CONFIG,
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (expected_ids); i++) {
        guint8  header[MM_SIT_HEADER_SIZE];
        guint8  response[12] = { 0 };
        guint16 request_len;

        g_assert_true (read_all (ctx->peer_fd, header, sizeof (header)));
        request_len = header[4] | (((guint16) header[5]) << 8);
        g_assert_cmpuint (header[2] | (((guint16) header[3]) << 8), ==, expected_ids[i]);
        if (request_len > MM_SIT_HEADER_SIZE) {
            g_autofree guint8 *body = g_malloc (request_len - MM_SIT_HEADER_SIZE);

            g_assert_true (read_all (ctx->peer_fd, body, request_len - MM_SIT_HEADER_SIZE));
        }

        response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
        response[2] = header[2];
        response[3] = header[3];
        response[4] = sizeof (response);
        response[6] = header[6];
        response[7] = header[7];
        g_assert_true (write_all (ctx->peer_fd, response, sizeof (response)));
    }
    return NULL;
}

static void
client_boot_config_test_ready (MMSitClient       *client,
                               GAsyncResult      *res,
                               ClientTestContext *ctx)
{
    ctx->completed = mm_sit_client_send_boot_config_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static void
test_client_boot_config (void)
{
    ClientTestContext          ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    g_autoptr(GThread)         peer = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient)     client = NULL;
    g_autoptr(GError)          error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit-boot-config",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    g_assert_no_error (error);

    peer = g_thread_new ("sit-boot-config-test-peer",
                         (GThreadFunc) client_boot_config_test_peer, &ctx);
    mm_sit_client_send_boot_config (client, NULL,
                                    (GAsyncReadyCallback) client_boot_config_test_ready, &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);
    g_thread_join (g_steal_pointer (&peer));

    g_assert_false (ctx.timed_out);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.completed);

    /* A second run must complete without any further CP traffic: the batch
     * is sent once per CP boot (the peer thread is already gone, so any
     * write here would time out). */
    ctx.completed = FALSE;
    mm_sit_client_send_boot_config (client, NULL,
                                    (GAsyncReadyCallback) client_boot_config_test_ready, &ctx);
    g_main_loop_run (ctx.loop);

    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    mm_sit_client_close (client);
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);

    g_assert_false (ctx.timed_out);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.completed);
}

int
main (int argc,
      char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/MM/sit/parse/header", test_parse_header);
    g_test_add_func ("/MM/sit/parse/invalid", test_parse_invalid);
    g_test_add_func ("/MM/sit/message-id/names", test_message_id_names);
    g_test_add_func ("/MM/sit/build/radio", test_build_radio);
    g_test_add_func ("/MM/sit/build/slot-mapping", test_build_slot_mapping);
    g_test_add_func ("/MM/sit/build/data", test_build_data_requests);
    g_test_add_func ("/MM/sit/build/config", test_build_config_requests);
    g_test_add_func ("/MM/sit/build/boot-config", test_build_boot_config_requests);
    g_test_add_func ("/MM/sit/decode/radio", test_parse_radio);
    g_test_add_func ("/MM/sit/decode/sim-registration", test_parse_status_and_registration);
    g_test_add_func ("/MM/sit/decode/sim-slot-status", test_parse_sim_slot_status);
    g_test_add_func ("/MM/sit/decode/signal", test_parse_signal);
    g_test_add_func ("/MM/sit/decode/cell-info-lte-v5", test_parse_cell_info_lte_v5);
    g_test_add_func ("/MM/sit/decode/data-call", test_parse_data_call);
    g_test_add_func ("/MM/sit/decode/data-call-list", test_parse_data_call_list);
    g_test_add_func ("/MM/sit/decode/carrier-sim-info", test_parse_carrier_sim_info);
    g_test_add_func ("/MM/sit/carrier-config/resolve", test_carrier_config_resolver);
    g_test_add_func ("/MM/sit/carrier-config/reference-database", test_carrier_config_reference_database);
    g_test_add_func ("/MM/sit/port/command-and-indication", test_port_command_and_indication);
    g_test_add_func ("/MM/sit/client/connect-sequence", test_client_connect_sequence);
    g_test_add_func ("/MM/sit/client/radio-transition", test_client_radio_transition);
    g_test_add_func ("/MM/sit/client/boot-config", test_client_boot_config);

    return g_test_run ();
}
