/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <glib.h>

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
test_build_data_requests (void)
{
    g_autoptr(GByteArray) profile = NULL;
    g_autoptr(GByteArray) attach = NULL;
    g_autoptr(GByteArray) setup = NULL;
    g_autoptr(GByteArray) deactivate = NULL;
    g_autoptr(GByteArray) list = NULL;
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
    g_assert_cmpuint (list->len, ==, 16);
    g_assert_cmpmem (&list->data[12], 4, "\x0a\x0a\x0a\x0a", 4);
}

static void
test_build_config_requests (void)
{
    static const guint8 capabilities[6] = { 0x7e, 0x06, 0x08, 0x0f, 0x26, 0x0c };
    static const guint8 sha1[20] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
        0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
    };
    g_autoptr(GByteArray) caps = NULL;
    g_autoptr(GByteArray) info = NULL;
    g_autoptr(GByteArray) manifest = NULL;
    g_autoptr(GError)     error = NULL;

    caps = mm_sit_request_new_device_capabilities (1, capabilities);
    g_assert_cmpuint (caps->len, ==, 18);
    g_assert_cmpmem (&caps->data[12], 6, capabilities, 6);

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
}

static void
test_parse_radio (void)
{
    static const guint8 get_response[] = {
        1, 0, 1, 8, 16, 0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0,
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
    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_OFF);
}

static void
test_parse_status_and_registration (void)
{
    guint8 sim_response[18] = { 1, 0, 0, 2, 18, 0, 1, 0 };
    guint8 reg_response[32] = { 1, 0, 2, 7, 32, 0, 2, 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitSimStatus          sim;
    MMSitRegistration       registration;

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

    memcpy (&reg_response[12], "26203#", 6);
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (reg_response, sizeof (reg_response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_registration (message, &registration, &error));
    g_assert_no_error (error);
    g_assert_true (registration.registered);
    g_assert_cmpstr (registration.plmn, ==, "26203");
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

int
main (int argc,
      char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/MM/sit/parse/header", test_parse_header);
    g_test_add_func ("/MM/sit/parse/invalid", test_parse_invalid);
    g_test_add_func ("/MM/sit/build/radio", test_build_radio);
    g_test_add_func ("/MM/sit/build/data", test_build_data_requests);
    g_test_add_func ("/MM/sit/build/config", test_build_config_requests);
    g_test_add_func ("/MM/sit/decode/radio", test_parse_radio);
    g_test_add_func ("/MM/sit/decode/sim-registration", test_parse_status_and_registration);
    g_test_add_func ("/MM/sit/decode/data-call", test_parse_data_call);

    return g_test_run ();
}
