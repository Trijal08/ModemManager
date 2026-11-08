/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mm-log.h"
#include "mm-port-serial-sit.h"
#include "mm-sit-carrier-config.h"
#include "mm-sit-client.h"
#include "mm-sit-cp.h"
#include "mm-sit-ims-config.h"
#include "mm-sit-ims.h"
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
    /* A captured 12-byte SET_RADIO_POWER reply: header, then the token's high
     * half at @8/@9 and the status byte at @10.  This particular one is the
     * refusal the CP sends when the radio is already in the requested state --
     * status 2, GENERIC_FAILURE -- which used to read as success because the
     * status was taken from @8. */
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
    g_assert_cmpuint (message->status, ==, 2);
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
    /* Where the two tables that used to hold this disagreed, the codec's own
     * name wins -- it is the one the code uses, so a log line greps to
     * something. The alias the other table carried is in the protocol notes. */
    g_assert_cmpstr (mm_sit_message_id_get_string (MM_SIT_MESSAGE_GET_DATA_REGISTRATION), ==,
                     "GET_DATA_REGISTRATION");
    g_assert_cmpstr (mm_sit_message_id_get_string (MM_SIT_MESSAGE_SET_AP_SYSTEM_TIME), ==,
                     "SET_AP_SYSTEM_TIME");
    g_assert_cmpstr (mm_sit_message_id_get_string (0x4605), ==, "OEM_CONFIG");
    g_assert_cmpstr (mm_sit_message_id_get_string (0xffff), ==, "UNKNOWN");
    /* AIMS ids share the number space and used to log as UNKNOWN, because the
     * table that knew them was in a crate this plugin never asked. */
    g_assert_cmpstr (mm_sit_message_id_get_string (0x0d07), ==, "AIMS_HIDDEN_MENU");
    /* And an indication no consumer had needed to name until it turned up
     * twenty-six times in one boot. */
    g_assert_cmpstr (mm_sit_message_id_get_string (0x0742), ==,
                     "SIT_IND_PHYSICAL_CHANNEL_CONFIG");
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

/* The CP takes a single auth choice where ModemManager offers a set, so the set
 * has to be narrowed -- and refused rather than downgraded when it names only
 * methods this modem cannot do. */
static void
test_apn_auth_from_properties (void)
{
    static const struct {
        MMBearerAllowedAuth  allowed;
        const gchar         *user;
        MMSitAuthType        expected;
        gboolean             supported;
    } cases[] = {
        /* Nothing stated and nothing to send: no authentication. */
        { MM_BEARER_ALLOWED_AUTH_UNKNOWN,  NULL,    MM_SIT_AUTH_TYPE_NONE,        TRUE },
        /* Credentials decide before the mask does: NetworkManager's default
         * mask allows PAP and CHAP, but with no username there is nothing to
         * present, and the vendor sends 0 for exactly this case. */
        { MM_BEARER_ALLOWED_AUTH_PAP | MM_BEARER_ALLOWED_AUTH_CHAP |
          MM_BEARER_ALLOWED_AUTH_MSCHAP | MM_BEARER_ALLOWED_AUTH_MSCHAPV2 |
          MM_BEARER_ALLOWED_AUTH_EAP,      NULL,    MM_SIT_AUTH_TYPE_NONE,        TRUE },
        /* ...and a method the CP cannot do is not an error when there are no
         * credentials to protect. */
        { MM_BEARER_ALLOWED_AUTH_EAP,      NULL,    MM_SIT_AUTH_TYPE_NONE,        TRUE },
        /* Nothing stated but credentials present: let the network choose. */
        { MM_BEARER_ALLOWED_AUTH_UNKNOWN,  "alice", MM_SIT_AUTH_TYPE_PAP_OR_CHAP, TRUE },
        { MM_BEARER_ALLOWED_AUTH_NONE,     "alice", MM_SIT_AUTH_TYPE_NONE,        TRUE },
        { MM_BEARER_ALLOWED_AUTH_PAP,      "alice", MM_SIT_AUTH_TYPE_PAP,         TRUE },
        { MM_BEARER_ALLOWED_AUTH_CHAP,     "alice", MM_SIT_AUTH_TYPE_CHAP,        TRUE },
        { MM_BEARER_ALLOWED_AUTH_PAP |
          MM_BEARER_ALLOWED_AUTH_CHAP,     "alice", MM_SIT_AUTH_TYPE_PAP_OR_CHAP, TRUE },
        /* NetworkManager's default mask allows everything it knows; the two the
         * CP understands are in there, so it still resolves. */
        { MM_BEARER_ALLOWED_AUTH_PAP | MM_BEARER_ALLOWED_AUTH_CHAP |
          MM_BEARER_ALLOWED_AUTH_MSCHAP | MM_BEARER_ALLOWED_AUTH_MSCHAPV2 |
          MM_BEARER_ALLOWED_AUTH_EAP,      "alice", MM_SIT_AUTH_TYPE_PAP_OR_CHAP, TRUE },
        /* Only methods the CP cannot do: refuse, do not silently send PAP. */
        { MM_BEARER_ALLOWED_AUTH_EAP,      "alice", 0,                            FALSE },
        { MM_BEARER_ALLOWED_AUTH_MSCHAPV2, "alice", 0,                            FALSE },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autoptr(MMBearerProperties) props = NULL;
        g_autoptr(GError)             error = NULL;
        MMSitApnAuth                  auth = { 0 };
        gboolean                      ok;

        props = mm_bearer_properties_new ();
        mm_bearer_properties_set_apn (props, "internet");
        mm_bearer_properties_set_allowed_auth (props, cases[i].allowed);
        if (cases[i].user) {
            mm_bearer_properties_set_user (props, cases[i].user);
            mm_bearer_properties_set_password (props, "s3cret");
        }

        ok = mm_sit_apn_auth_from_properties (props, &auth, &error);
        if (!cases[i].supported) {
            g_assert_false (ok);
            g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED);
            continue;
        }
        g_assert_no_error (error);
        g_assert_true (ok);
        g_assert_cmpuint (auth.auth, ==, cases[i].expected);
        if (cases[i].user)
            g_assert_cmpstr (auth.user, ==, cases[i].user);
    }
}

/* The requested families reach the wire; so do credentials, at the offsets each
 * request uses -- SET_DATA_PROFILE leads with the type, the other two follow
 * the strings with it. */
static void
test_apn_auth_on_the_wire (void)
{
    g_autoptr(GByteArray) profile = NULL;
    g_autoptr(GByteArray) attach = NULL;
    g_autoptr(GByteArray) setup = NULL;
    g_autoptr(GError)     error = NULL;
    const MMSitApnAuth    auth = {
        .auth = MM_SIT_AUTH_TYPE_CHAP,
        .user = "alice",
        .password = "s3cret",
    };

    profile = mm_sit_request_new_set_data_profile (1, "internet", MM_SIT_PDP_TYPE_IPV4V6, MM_SIT_APN_TYPE_DEFAULT, 0x14CE08, &auth, &error);
    attach = mm_sit_request_new_set_initial_attach_apn (2, "internet", MM_SIT_PDP_TYPE_IPV4V6, &auth, &error);
    setup = mm_sit_request_new_setup_data_call (3, MM_SIT_DATA_CALL_CID_DEFAULT, "internet", MM_SIT_PDP_TYPE_IPV4V6, MM_SIT_APN_TYPE_DEFAULT, &auth, &error);
    g_assert_no_error (error);

    /* @0xee is the radio-technology mask the CP matches URSP rules against;
     * zero declares a profile valid on no RAT and the IMS PDN is refused with
     * MATCH_ALL_RULE_NOT_ALLOWED. Telus's IMS entry reaches the wire as
     * 0x14CE08, which is what this modem's own Android sends. */
    g_assert_cmpuint (profile->data[0xee] | (profile->data[0xef] << 8) |
                      (profile->data[0xf0] << 16) | (profile->data[0xf1] << 24),
                      ==, 0x14CE08);

    g_assert_cmpuint (profile->data[0x77], ==, MM_SIT_AUTH_TYPE_CHAP);
    g_assert_cmpmem (&profile->data[0x78], 5, "alice", 5);
    g_assert_cmpmem (&profile->data[0xaa], 6, "s3cret", 6);

    g_assert_cmpuint (attach->data[0xd9], ==, MM_SIT_AUTH_TYPE_CHAP);
    g_assert_cmpmem (&attach->data[0x75], 5, "alice", 5);
    g_assert_cmpmem (&attach->data[0xa7], 6, "s3cret", 6);

    g_assert_cmpuint (setup->data[0xd9], ==, MM_SIT_AUTH_TYPE_CHAP);
    g_assert_cmpmem (&setup->data[0x75], 5, "alice", 5);
    g_assert_cmpmem (&setup->data[0xa7], 6, "s3cret", 6);
    /* The APN stops short of the username rather than running over it. */
    g_assert_cmpuint (setup->data[0x74], ==, 0);
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

    profile = mm_sit_request_new_set_data_profile (1, "internet", MM_SIT_PDP_TYPE_IPV4, MM_SIT_APN_TYPE_DEFAULT, 0x14CE08, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (profile->len, ==, 246);
    g_assert_cmpmem (&profile->data[16], 8, "internet", 8);
    g_assert_cmphex (profile->data[12], ==, 0xff);
    g_assert_cmpuint (profile->data[0x75], ==, 1);
    g_assert_cmpuint (profile->data[0xe9], ==, 1);

    attach = mm_sit_request_new_set_initial_attach_apn (2, "internet", MM_SIT_PDP_TYPE_IPV4, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (attach->len, ==, 251);
    g_assert_cmpmem (&attach->data[16], 8, "internet", 8);
    g_assert_cmpuint (attach->data[0xda], ==, 1);
    g_assert_cmpuint (attach->data[0xf8], ==, 1);

    setup = mm_sit_request_new_setup_data_call (3, MM_SIT_DATA_CALL_CID_DEFAULT, "internet", MM_SIT_PDP_TYPE_IPV4, MM_SIT_APN_TYPE_DEFAULT, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (setup->len, ==, 983);
    g_assert_cmpmem (&setup->data[16], 8, "internet", 8);
    /* @0xf1 is a u32 and the caller's to choose: it describes the APN, so only
     * whoever knows what the context is for can fill it in. */
    g_assert_cmpuint (GUINT32_FROM_LE (*(guint32 *) &setup->data[0xf1]), ==, MM_SIT_APN_TYPE_DEFAULT);
    g_assert_cmphex (setup->data[0xfc], ==, 0xff);
    g_assert_cmphex (setup->data[0x101], ==, 0xff);

    /* The requested families reach every one of the three, at the offset each
     * uses: a dual-stack request is 3 everywhere. */
    g_clear_pointer (&profile, g_byte_array_unref);
    g_clear_pointer (&attach, g_byte_array_unref);
    g_clear_pointer (&setup, g_byte_array_unref);
    profile = mm_sit_request_new_set_data_profile (1, "internet", MM_SIT_PDP_TYPE_IPV4V6, MM_SIT_APN_TYPE_DEFAULT, 0x14CE08, NULL, &error);
    attach = mm_sit_request_new_set_initial_attach_apn (2, "internet", MM_SIT_PDP_TYPE_IPV4V6, NULL, &error);
    setup = mm_sit_request_new_setup_data_call (3, MM_SIT_DATA_CALL_CID_DEFAULT, "internet", MM_SIT_PDP_TYPE_IPV6, MM_SIT_APN_TYPE_DEFAULT, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (profile->data[0x75], ==, 3);
    g_assert_cmpuint (profile->data[0x76], ==, 3);
    g_assert_cmpuint (attach->data[0xda], ==, 3);
    /* @0xf8 is the attach's roaming protocol, and it is IPv4 whatever the home
     * protocol is: this modem's own Android attaches pdp=3 rpdp=1 on Telus and
     * pdp=1 rpdp=1 on Fizz. */
    g_assert_cmpuint (attach->data[0xf8], ==, 1);
    g_assert_cmpuint (setup->data[0xda], ==, 2);

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
    g_autoptr(GByteArray) indication_filter = NULL;
    g_autoptr(GByteArray) screen_state = NULL;
    g_autoptr(GError)     error = NULL;

    info = mm_sit_request_new_device_info (2, "Pixel 9a", "TEST.BUILD", "tegu", "17", &error);
    g_assert_no_error (error);
    g_assert_cmpuint (info->len, ==, 140);
    g_assert_cmpmem (&info->data[12], 8, "Pixel 9a", 8);
    /* Field 4 sits at body+100 == frame+108. */
    g_assert_cmpmem (&info->data[108], 2, "17", 2);
    g_assert_cmpmem (&info->data[44], 10, "TEST.BUILD", 10);

    manifest = mm_sit_request_new_carrier_manifest (3, "/vendor/firmware/carrierconfig", sha1, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (manifest->len, ==, 532);
    g_assert_cmpuint (manifest->data[12], ==, 30);
    g_assert_cmpuint (manifest->data[0x110], ==, 20);
    g_assert_cmpmem (&manifest->data[0x114], 20, sha1, 20);

    indication_filter = mm_sit_request_new_set_indication_filter (
        4, MM_SIT_INDICATION_FILTER_REGISTRATION_FAILURE);
    g_assert_cmpuint (indication_filter->len, ==, 16);
    g_assert_cmpuint (indication_filter->data[2], ==, 0x28);
    g_assert_cmpuint (indication_filter->data[3], ==, 0x09);
    g_assert_cmpmem (&indication_filter->data[12], 4, "\x20\0\0\0", 4);

    screen_state = mm_sit_request_new_set_screen_state (5, FALSE);
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

    /* July: the CP numbers months from zero, so the frame carries 0x06. */
    when = g_date_time_new_local (2026, 7, 8, 15, 38, 12.0);
    request = mm_sit_request_new_set_ap_system_time (0x42, when, &error);
    g_assert_no_error (error);
    assert_bytes (request, expected_time, sizeof (expected_time));

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_oem_config (0x43, "33", &error);
    g_assert_no_error (error);
    assert_bytes (request, expected_oem, sizeof (expected_oem));

    /* The version is the CP's, so anything that is not two digits of it is a
     * caller bug rather than something to send. */
    g_clear_pointer (&request, g_byte_array_unref);
    g_assert_null (mm_sit_request_new_oem_config (0x43, "3", &error));
    g_assert_nonnull (error);
    g_clear_error (&error);

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_set_modem_config (0x44);
    assert_bytes (request, expected_modem, sizeof (expected_modem));
}

static void
test_parse_nitz_time (void)
{
    /* A live GET_NITZ_TIME reply with the PLMN replaced by the 001/01 test
     * network: 2026-07-30 16:55:50 UTC, +02:00 of which 1 h is DST, Thursday,
     * received 91171 ms ago. */
    static const guint8 response[] = {
        1, 0, 0x55, 0x09, 37, 0, 1, 0,
        0, 0, 0, 0,
        1, 26, 7, 30, 16, 55, 50, 8, 1, 4, 1,
        '0', '0', '1', '0', '1', '#',
        0x23, 0x64, 0x01, 0, 0, 0, 0, 0,
    };
    static const guint8 no_time[] = {
        1, 0, 0x55, 0x09, 37, 0, 1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    g_autofree gchar       *iso8601 = NULL;
    MMSitNitzTime           time;

    message = mm_sit_message_parse (response, sizeof (response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_nitz_time (message, &time, &error));
    g_assert_no_error (error);
    g_assert_true (time.valid);
    g_assert_cmpuint (time.year, ==, 2026);
    g_assert_cmpuint (time.month, ==, 7);
    g_assert_cmpuint (time.day, ==, 30);
    g_assert_cmpuint (time.hour, ==, 16);
    g_assert_cmpuint (time.minute, ==, 55);
    g_assert_cmpuint (time.second, ==, 50);
    g_assert_cmpint (time.timezone_quarter_hours, ==, 8);
    g_assert_cmpuint (time.daylight_saving_hours, ==, 1);
    g_assert_cmpuint (time.day_of_week, ==, 4);
    g_assert_cmpstr (time.plmn, ==, "00101");
    g_assert_cmpuint (time.age_ms, ==, 91171);

    /* 16:55:50 UTC + 91.171 s = 16:57:21 UTC, published as +02:00 local.
     * GLib renders a whole-hour offset without its minutes, as every other
     * ModemManager plugin's network time does. */
    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (iso8601, ==, "2026-07-30T18:57:21+02");

    /* An age that crosses midnight has to carry the date with it, and a
     * negative offset has to move the clock back rather than forward. */
    time.age_ms = 25 * 60 * 60 * 1000;
    time.timezone_quarter_hours = -32; /* UTC-08:00 */
    g_clear_pointer (&iso8601, g_free);
    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (iso8601, ==, "2026-07-31T09:55:50-08");

    /* A half-hour zone keeps its minutes. */
    time.age_ms = 0;
    time.timezone_quarter_hours = 22; /* UTC+05:30 */
    g_clear_pointer (&iso8601, g_free);
    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (iso8601, ==, "2026-07-30T22:25:50+05:30");

    /* No NITZ yet: parses fine, but there is nothing to publish. */
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (no_time, sizeof (no_time), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_nitz_time (message, &time, &error));
    g_assert_no_error (error);
    g_assert_false (time.valid);

    g_clear_pointer (&iso8601, g_free);
    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    g_assert_null (iso8601);
    g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_RETRY);
}

static void
test_parse_nitz_time_indication (void)
{
    /* The same NITZ as the CP pushes it (0x0905): no status word, so 33 bytes
     * with the payload four earlier, and an age of zero because it has just
     * arrived. Modelled on the live indications. */
    static const guint8 indication[] = {
        2, 0, 0x05, 0x09, 33, 0, 0, 0,
        1, 26, 7, 30, 16, 55, 50, 8, 1, 4, 1,
        '0', '0', '1', '0', '1', '#',
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    g_autofree gchar       *iso8601 = NULL;
    MMSitNitzTime           time;

    g_assert_cmpstr (mm_sit_message_id_get_string (0x0905), ==, "NITZ_TIME_RECEIVED");

    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_nitz_time (message, &time, &error));
    g_assert_no_error (error);
    g_assert_true (time.valid);
    g_assert_cmpuint (time.year, ==, 2026);
    g_assert_cmpuint (time.month, ==, 7);
    g_assert_cmpuint (time.day, ==, 30);
    g_assert_cmpuint (time.hour, ==, 16);
    g_assert_cmpuint (time.minute, ==, 55);
    g_assert_cmpuint (time.second, ==, 50);
    g_assert_cmpint (time.timezone_quarter_hours, ==, 8);
    g_assert_cmpuint (time.daylight_saving_hours, ==, 1);
    g_assert_cmpuint (time.day_of_week, ==, 4);
    g_assert_cmpstr (time.plmn, ==, "00101");
    /* A pushed NITZ is the arrival, so it is never stale. */
    g_assert_cmpuint (time.age_ms, ==, 0);

    iso8601 = mm_sit_nitz_time_to_iso8601 (&time, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (iso8601, ==, "2026-07-30T18:55:50+02");
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
    /* The two messages do not share a value space. An indication's 1 is off
     * (ProtocolRadioStateAdapter's table maps 0 and 1 alike), while a GET's 1
     * is UNAVAILABLE and a GET reports off as 0. */
    static const guint8 indication_off[] = {
        2, 0, 2, 8, 12, 0, 0, 0, 1, 0, 0, 0,
    };
    static const guint8 get_response_off[] = {
        1, 0, 1, 8, 16, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    static const guint8 get_response_unavailable[] = {
        1, 0, 1, 8, 16, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,
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
    message = mm_sit_message_parse (indication_off, sizeof (indication_off), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_OFF);

    /* The frame the radio-off poll waits for: read as anything but OFF, the
     * poll runs to its timeout and reports "did not reach state 1". */
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (get_response_off, sizeof (get_response_off), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_OFF);

    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (get_response_unavailable,
                                    sizeof (get_response_unavailable), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_radio_state (message, &state, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (state, ==, MM_SIT_RADIO_STATE_UNKNOWN);
}

static void
test_parse_registration_failed (void)
{
    g_autoptr(MMSitMessage)  message = NULL;
    g_autoptr(GError)        error = NULL;
    MMSitRegistrationFailure failure;
    /* The burst this hardware produced on a boot that would not attach: the
     * home network refusing a combined attach with cause 10. */
    guint8                   indication[299] = { 2, 0, 0x4a, 0x07, 299 & 0xff, 299 >> 8, 0, 0 };

    memcpy (&indication[8], "26203#", 6);
    indication[0x0e] = 1;  /* CS+PS */
    indication[0x0f] = MM_SIT_REJECT_CAUSE_IMPLICITLY_DETACHED;
    indication[0x10] = 0;

    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_registration_failed (message, &failure, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (failure.plmn, ==, "26203");
    g_assert_true (failure.circuit_switched);
    g_assert_true (failure.packet_switched);
    g_assert_cmpuint (failure.cause, ==, MM_SIT_REJECT_CAUSE_IMPLICITLY_DETACHED);
    g_assert_true (failure.retryable);

    /* A permanent refusal must not be retried: cause 11 is what puts the PLMN
     * on the card's forbidden list. */
    indication[0x0f] = 11;
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_registration_failed (message, &failure, &error));
    g_assert_false (failure.retryable);

    /* And an absent cause reads as absent, not as 255. */
    indication[0x0f] = 0xff;
    indication[0x10] = 0xff;
    g_clear_pointer (&message, mm_sit_message_free);
    message = mm_sit_message_parse (indication, sizeof (indication), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_registration_failed (message, &failure, &error));
    g_assert_cmpuint (failure.cause, ==, MM_SIT_REJECT_CAUSE_NONE);
    g_assert_cmpuint (failure.additional_cause, ==, MM_SIT_REJECT_CAUSE_NONE);
    g_assert_false (failure.retryable);
}

static void
test_parse_status_and_registration (void)
{
    /* One application record plus the card block: 17 + 63 * 2. */
    guint8 sim_response[143] = { 1, 0, 0, 2, 143, 0, 1, 0 };
    guint8 reg_response[86] = { 1, 0, 1, 7, 86, 0, 2, 0 };
    guint8 operator_response[119] = { 1, 0, 2, 7, 119, 0, 3, 0 };
    guint8 operator_indication[131] = { 2, 0, 0x0e, 7, 131, 0, 0, 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitSimStatus          sim;
    MMSitRegistration       registration;
    MMSitOperator           operator_info;

    sim_response[12] = 1;                            /* card present */
    sim_response[13] = MM_SIT_PIN_STATE_DISABLED;
    sim_response[14] = 1;                            /* one application */
    sim_response[17] = 2;                            /* USIM */
    sim_response[18] = 4;                            /* AID length */
    sim_response[19] = 0xa0;
    sim_response[72] = MM_SIT_PIN_STATE_DISABLED;    /* PIN1 state */
    sim_response[74] = 3;                            /* PIN1 retries */
    sim_response[75] = 10;                           /* PUK1 retries */
    sim_response[76] = 3;                            /* PIN2 retries */
    sim_response[77] = 10;                           /* PUK2 retries */
    sim_response[79] = MM_SIT_APP_STATE_READY;
    sim_response[114] = 3;                           /* ICCID length */
    sim_response[115] = 0x98;
    message = mm_sit_message_parse (sim_response, sizeof (sim_response), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_sim_status (message, &sim, &error));
    g_assert_no_error (error);
    g_assert_true (sim.present);
    g_assert_cmpuint (sim.app_count, ==, 1);
    g_assert_cmpuint (sim.apps[0].aid_len, ==, 4);
    g_assert_cmpuint (sim.apps[0].aid[0], ==, 0xa0);
    g_assert_cmpuint (sim.apps[0].pin1_retries, ==, 3);
    g_assert_cmpuint (sim.apps[0].puk1_retries, ==, 10);
    g_assert_cmpuint (sim.apps[0].app_state, ==, MM_SIT_APP_STATE_READY);
    g_assert_cmpuint (sim.iccid_len, ==, 3);

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
    g_assert_cmpint (registration.vops_support, ==, MM_SIT_VOPS_SUPPORT_UNSUPPORTED);
    {
        static const struct {
            guint8 wire;
            MMSitVopsSupport expected;
        } cases[] = {
            { 1, MM_SIT_VOPS_SUPPORT_SUPPORTED },
            { 0xff, MM_SIT_VOPS_SUPPORT_UNKNOWN },
        };
        guint i;

        reg_response[15] = 20; /* SIT's second LTE value, not Android NR */
        for (i = 0; i < G_N_ELEMENTS (cases); i++) {
            reg_response[0x2d] = cases[i].wire;
            g_clear_pointer (&message, mm_sit_message_free);
            message = mm_sit_message_parse (reg_response, sizeof (reg_response), &error);
            g_assert_no_error (error);
            g_assert_true (mm_sit_message_parse_registration (message, &registration, &error));
            g_assert_no_error (error);
            g_assert_cmpint (registration.vops_support, ==, cases[i].expected);
        }
    }

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
test_build_call_requests (void)
{
    /* Byte-pinned against a live MO call capture (docs/subsystems/modem/reference/ims.md): DIAL is
     * 104 B with the number in a fixed 82-byte slot, ANSWER is header-only,
     * HANGUP carries {u32 call_id, u32 = 1}. */
    g_autoptr(GByteArray) dial = NULL;
    g_autoptr(GByteArray) answer = NULL;
    g_autoptr(GByteArray) hangup = NULL;
    g_autoptr(GError)     error = NULL;

    dial = mm_sit_request_new_dial (0x1234, "+15550101", &error);
    g_assert_no_error (error);
    g_assert_nonnull (dial);
    g_assert_cmpuint (dial->len, ==, 104);
    g_assert_cmpuint (dial->data[0], ==, 0);      /* request */
    g_assert_cmpuint (dial->data[2], ==, 0x01);   /* DIAL */
    g_assert_cmpuint (dial->data[3], ==, 0x00);
    g_assert_cmpuint (dial->data[0x0c], ==, 0x01); /* voice call type */
    g_assert_cmpuint (dial->data[0x0e], ==, 9);    /* number length */
    g_assert_cmpmem (dial->data + 0x0f, 9, "+15550101", 9);
    g_assert_cmpuint (dial->data[0x61], ==, 0x10); /* '+' -> international TON */

    answer = mm_sit_request_new_answer (0x1235);
    g_assert_nonnull (answer);
    g_assert_cmpuint (answer->len, ==, 12);
    g_assert_cmpuint (answer->data[2], ==, 0x04);

    hangup = mm_sit_request_new_hangup (0x1236, 7);
    g_assert_nonnull (hangup);
    g_assert_cmpuint (hangup->len, ==, 20);
    g_assert_cmpuint (hangup->data[2], ==, 0x08);
    g_assert_cmpuint (hangup->data[12], ==, 7);
}

static void
test_parse_call_list (void)
{
    /* GET_CALL_LIST response: u32 count then fixed-stride records. One MO
     * call in ALERTING with a 4-digit number. */
    guint8 resp[12 + 4 + 0x20] = { 1, 0, 0x00, 0x00, sizeof (resp), 0, 1, 0 };
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;
    g_autoptr(GArray)       calls = NULL;
    const MMSitCall        *c;
    const guint             rec = 16;

    resp[12] = 1;                 /* count */
    resp[rec + 0x00] = 3;         /* state: ALERTING */
    resp[rec + 0x04] = 2;         /* index */
    resp[rec + 0x0a] = 0;         /* MO */
    resp[rec + 0x0e] = 4;         /* numLen */
    memcpy (resp + rec + 0x0f, "0155", 4);

    message = mm_sit_message_parse (resp, sizeof (resp), &error);
    g_assert_no_error (error);
    g_assert_true (mm_sit_message_parse_call_list (message, &calls, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (calls->len, ==, 1);

    c = &g_array_index (calls, MMSitCall, 0);
    g_assert_cmpuint (c->state, ==, 3);
    g_assert_cmpuint (c->index, ==, 2);
    g_assert_false (c->incoming);
    g_assert_cmpstr (c->number, ==, "0155");
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

/* Peer that answers with a stale response first -- the wreckage of a command
 * that already timed out -- and only then with the real one. Reproduces the
 * race a busy CP produces: MMPortSerial has moved on, and the late frame lands
 * in front of a caller it has nothing to do with. */
static gpointer
port_test_stale_peer (PortTestContext *ctx)
{
    guint8 request[64];
    /* A response to some earlier command: same shape, a token nobody awaits. */
    guint8 stale[] = {
        1, 0, 0x00, 0x02, 20, 0, 0xff, 0xee, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    guint8 reply[] = {
        1, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    gssize request_len;

    request_len = read (ctx->peer_fd, request, sizeof (request));
    g_assert_cmpint (request_len, >=, MM_SIT_HEADER_SIZE);
    g_assert_true (write_all (ctx->peer_fd, stale, sizeof (stale)));
    reply[2] = request[2];
    reply[3] = request[3];
    reply[6] = request[6];
    reply[7] = request[7];
    g_assert_true (write_all (ctx->peer_fd, reply, sizeof (reply)));
    return NULL;
}

static void
port_test_stale_ready (MMPortSerialSit *port,
                       GAsyncResult    *res,
                       PortTestContext *ctx)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;

    response = mm_port_serial_sit_command_finish (port, res, &error);
    /* The stale frame must not be mistaken for this command's answer. */
    g_assert_no_error (error);
    g_assert_nonnull (response);
    g_assert_cmphex (response->id, ==, MM_SIT_MESSAGE_GET_SIM_STATUS);
    g_assert_cmphex (response->token, ==, 1);
    ctx->command_completed = TRUE;
    g_main_loop_quit (ctx->loop);
}

static void
test_port_drops_a_stale_response (void)
{
    PortTestContext            ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    g_autoptr(GThread)         peer = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(GByteArray)      request = NULL;
    g_autoptr(GError)          error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);

    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit-stale",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    g_assert_true (mm_port_serial_open (MM_PORT_SERIAL (port), &error));
    g_assert_no_error (error);

    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_STATUS,
                                        mm_port_serial_sit_next_token (port));
    peer = g_thread_new ("sit-test-stale-peer", (GThreadFunc) port_test_stale_peer, &ctx);
    mm_port_serial_sit_command (port, request, 2, NULL,
                                (GAsyncReadyCallback) port_test_stale_ready, &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) port_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);

    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    g_thread_join (g_steal_pointer (&peer));
    mm_port_serial_close (MM_PORT_SERIAL (port));
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);

    g_assert_false (ctx.timed_out);
    g_assert_true (ctx.command_completed);
}

/* Peer for the queued-commands case: answer each request in the order it
 * arrives, promptly and correctly. Nothing is stale here -- the point is that
 * two commands are outstanding at once. */
static gpointer
port_test_queue_peer (PortTestContext *ctx)
{
    guint i;

    for (i = 0; i < 2; i++) {
        guint8 request[64];
        guint8 reply[] = {
            1, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        gssize request_len;

        request_len = read (ctx->peer_fd, request, sizeof (request));
        g_assert_cmpint (request_len, >=, MM_SIT_HEADER_SIZE);
        reply[2] = request[2];
        reply[3] = request[3];
        reply[6] = request[6];
        reply[7] = request[7];
        g_assert_true (write_all (ctx->peer_fd, reply, sizeof (reply)));
    }
    return NULL;
}

static void
port_test_queue_ready (MMPortSerialSit *port,
                       GAsyncResult    *res,
                       PortTestContext *ctx)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;

    response = mm_port_serial_sit_command_finish (port, res, &error);
    /* Each command must get its own answer. Tracking a single in-flight token
     * instead of one per queued command discards the running command's reply as
     * soon as a second command is submitted behind it, and both time out. */
    g_assert_no_error (error);
    g_assert_nonnull (response);
    ctx->indication_count++;
    if (ctx->indication_count == 2) {
        ctx->command_completed = TRUE;
        g_main_loop_quit (ctx->loop);
    }
}

/* Two commands submitted back to back, before the first has been written.
 * MMPortSerial queues them and runs them in order, so both tokens are
 * outstanding at once. */
static void
test_port_answers_queued_commands (void)
{
    PortTestContext            ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    g_autoptr(GThread)         peer = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(GByteArray)      first = NULL;
    g_autoptr(GByteArray)      second = NULL;
    g_autoptr(GError)          error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);

    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit-queue",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    g_assert_true (mm_port_serial_open (MM_PORT_SERIAL (port), &error));
    g_assert_no_error (error);

    first = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_STATUS,
                                      mm_port_serial_sit_next_token (port));
    second = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_RADIO_POWER,
                                       mm_port_serial_sit_next_token (port));
    peer = g_thread_new ("sit-test-queue-peer", (GThreadFunc) port_test_queue_peer, &ctx);
    mm_port_serial_sit_command (port, first, 2, NULL,
                                (GAsyncReadyCallback) port_test_queue_ready, &ctx);
    mm_port_serial_sit_command (port, second, 2, NULL,
                                (GAsyncReadyCallback) port_test_queue_ready, &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) port_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);

    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    g_thread_join (g_steal_pointer (&peer));
    mm_port_serial_close (MM_PORT_SERIAL (port));
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);

    g_assert_false (ctx.timed_out);
    g_assert_true (ctx.command_completed);
    g_assert_cmpuint (ctx.indication_count, ==, 2);
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

/* One connect attempt per round; the peer serves as many as the test drives. */
#define CLIENT_TEST_MAX_ROUNDS 3

typedef struct {
    gint          peer_fd;
    gint          control_fd;
    gboolean      dsds;
    gboolean      ipv6_only;
    gboolean      empty_sims;
    gboolean      same_sim;
    guint         add_mapping; /* 1: explicit fallback, 2: CP maps asynchronously */
    guint         callbacks;
    GMainLoop    *loop;
    MMSitDataCall call;
    GError       *error;
    gboolean      completed;
    gboolean      timed_out;
    guint         rounds;
    /* The context id the client asked for in each round's SETUP_DATA_CALL. */
    guint8        requested_cid[CLIENT_TEST_MAX_ROUNDS];
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

static void reply_request (gint fd, guint16 expected, guint8 *response, gsize response_len);

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
    guint round;
    guint i;

    for (round = 0; round < ctx->rounds; round++)
    for (i = 0; i < G_N_ELEMENTS (expected_ids); i++) {
        guint8  header[MM_SIT_HEADER_SIZE];
        guint8  response[304] = { 0 };
        guint16 request_len;
        guint16 response_len = 12;

        gint fd = ctx->dsds && expected_ids[i] == MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM
            ? ctx->control_fd : ctx->peer_fd;

        if (ctx->dsds && expected_ids[i] == MM_SIT_MESSAGE_SET_INITIAL_ATTACH_APN)
            continue;
        g_assert_true (read_all (fd, header, sizeof (header)));
        request_len = header[4] | (((guint16) header[5]) << 8);
        g_assert_cmpuint (header[2] | (((guint16) header[3]) << 8), ==, expected_ids[i]);
        g_assert_cmpuint (request_len, >=, MM_SIT_HEADER_SIZE);
        if (request_len > MM_SIT_HEADER_SIZE) {
            g_autofree guint8 *body = g_malloc (request_len - MM_SIT_HEADER_SIZE);

            g_assert_true (read_all (fd, body, request_len - MM_SIT_HEADER_SIZE));
            /* The requested PDP type has to reach the wire: @0xda of the frame,
             * so 0xda - 8 of the body. */
            if (ctx->dsds && expected_ids[i] == MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM)
                g_assert_cmpuint (body[4], ==, 1);
            if (expected_ids[i] == MM_SIT_MESSAGE_SETUP_DATA_CALL) {
                g_assert_cmpuint (body[0xda - MM_SIT_HEADER_SIZE], ==, MM_SIT_PDP_TYPE_IPV4V6);
                /* @0x0c is the context id the host picked. */
                ctx->requested_cid[round] = body[0x0c - MM_SIT_HEADER_SIZE];
            }
        }

        if (expected_ids[i] == MM_SIT_MESSAGE_GET_DATA_REGISTRATION) {
            response_len = 86;
            response[12] = 1;
            response[15] = 14;
        } else if (expected_ids[i] == MM_SIT_MESSAGE_SETUP_DATA_CALL) {
            response_len = sizeof (response);
            /* Echo the cid back, as the CP does -- the host picks it and the
             * response carries whichever one was actually used. */
            response[14] = ctx->requested_cid[round];
            response[15] = 1;
            /* A dual-stack grant: addr_type 3, with both families filled in
             * from the same record. IPv6 uses the documentation prefix. */
            response[16] = 3;
            response[17] = 10;
            response[18] = 20;
            response[19] = 30;
            response[20] = 40;
            response[21] = 0x20;
            response[22] = 0x01;
            response[23] = 0x0d;
            response[24] = 0xb8;
            response[36] = 1;
            response[38] = 1;
            response[39] = 1;
            response[40] = 1;
            response[41] = 1;
            response[42] = 0x20;
            response[43] = 0x01;
            response[44] = 0x0d;
            response[45] = 0xb8;
            response[57] = 0x53;
            response[58] = 8;
            response[59] = 8;
            response[60] = 8;
            response[61] = 8;
            response[139] = 0xdc;
            response[140] = 0x05;
        }

        if (ctx->ipv6_only && expected_ids[i] == MM_SIT_MESSAGE_SETUP_DATA_CALL) {
            response[16] = 2;
            memset (&response[17], 0, 4);
        }
        response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
        response[2] = header[2];
        response[3] = header[3];
        response[4] = response_len & 0xff;
        response[5] = response_len >> 8;
        response[6] = header[6];
        response[7] = header[7];
        g_assert_true (write_all (fd, response, response_len));
    }
    if (ctx->dsds) {
        guint8 ack[12] = { 0 };

        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL, ack, sizeof (ack));
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
client_sim_changed (MMSitClient *client, const MMSitMessage *message, ClientTestContext *ctx)
{
    MMSitCarrierSimInfo info;

    ctx->rounds++;
    if (ctx->rounds == 2) {
        g_assert_cmpuint (message->id, ==, MM_SIT_MESSAGE_SIM_STATUS_CHANGED);
        g_assert_false (mm_sit_client_get_carrier_sim_info (client, &info));
    } else {
        g_assert_cmpuint (message->id, ==, MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO);
        g_assert_true (mm_sit_client_get_carrier_sim_info (client, &info));
        g_assert_cmpstr (info.iccid, ==,
                         ctx->rounds == 1 ? "8901234567890123456" : "8901234567890123457");
    }
    if (ctx->rounds == 3)
        g_main_loop_quit (ctx->loop);
}

static void
test_client_sim_cache_refresh (void)
{
    ClientTestContext ctx = { 0 };
    gint sockets[2];
    guint timeout_id;
    MMSitCarrierSimInfo info;
    guint8 carrier[207] = { MM_SIT_MESSAGE_TYPE_INDICATION, 0, 0x02, 0x46, 207 };
    guint8 changed[12] = { MM_SIT_MESSAGE_TYPE_INDICATION, 0, 0x10, 0x02, 12 };
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient) client = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
        MM_PORT_DEVICE, "sim-refresh-sit1", MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
        MM_PORT_TYPE, MM_PORT_TYPE_SIT, MM_PORT_SERIAL_FD, sockets[0], NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    g_assert_no_error (error);
    ctx.loop = g_main_loop_new (NULL, FALSE);
    g_signal_connect (client, "indication", G_CALLBACK (client_sim_changed), &ctx);

    /* The old profile must be unavailable as soon as the change is delivered,
     * even if the replacement carrier information has not arrived yet. */
    carrier[8] = sizeof (carrier) - MM_SIT_HEADER_SIZE;
    memcpy (&carrier[12], "8901234567890123456", 19);
    memcpy (&carrier[33], "262071234567890", 15);
    g_assert_true (write_all (sockets[1], carrier, sizeof (carrier)));
    g_assert_true (write_all (sockets[1], changed, sizeof (changed)));
    carrier[30] = '7';
    g_assert_true (write_all (sockets[1], carrier, sizeof (carrier)));
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_false (ctx.timed_out);
    g_assert_cmpuint (ctx.rounds, ==, 3);
    g_source_remove (timeout_id);
    g_assert_true (mm_sit_client_get_carrier_sim_info (client, &info));
    mm_sit_client_invalidate_sim (client);
    g_assert_false (mm_sit_client_get_carrier_sim_info (client, &info));
    mm_sit_client_close (client);
    close (sockets[1]);
    g_main_loop_unref (ctx.loop);
}

static void
test_client_connect_sequence (void)
{
    /* Three attempts on one client, because the context id is what keeps
     * concurrent PDNs apart and nothing else in ModemManager allocates it: a
     * second APN must not land on the first one's context, and the same APN
     * asked for twice must not burn a third. */
    static const gchar * const apns[] = { "internet", "ims", "internet" };
    static const guint8        want[] = { 2, 3, 2 };
    ClientTestContext          ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    guint                      round;
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

    ctx.rounds = 3;
    peer = g_thread_new ("sit-client-test-peer", (GThreadFunc) client_test_peer, &ctx);

    for (round = 0; round < ctx.rounds; round++) {
        ctx.completed = FALSE;
        g_clear_error (&ctx.error);
        mm_sit_client_connect_data (client, apns[round], MM_SIT_PDP_TYPE_IPV4V6,
                                    MM_SIT_APN_TYPE_DEFAULT, NULL, NULL,
                                    (GAsyncReadyCallback) client_test_connect_ready, &ctx);
        timeout_id = g_timeout_add_seconds (10, (GSourceFunc) client_test_timeout, &ctx);
        g_main_loop_run (ctx.loop);
        if (!ctx.timed_out)
            g_source_remove (timeout_id);

        g_assert_false (ctx.timed_out);
        g_assert_no_error (ctx.error);
        g_assert_true (ctx.completed);
        /* Asked for on the wire, and echoed back into the decoded call. */
        g_assert_cmpuint (ctx.requested_cid[round], ==, want[round]);
        g_assert_cmpuint (ctx.call.cid, ==, want[round]);
    }

    g_thread_join (g_steal_pointer (&peer));
    mm_sit_client_close (client);
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);

    /* The last round's grant, decoded in full. */
    g_assert_true (ctx.call.has_ipv4);
    g_assert_cmpstr (ctx.call.ipv4, ==, "10.20.30.40");
    g_assert_cmpstr (ctx.call.dns1, ==, "1.1.1.1");
    g_assert_true (ctx.call.has_ipv6);
    g_assert_cmpstr (ctx.call.ipv6, ==, "2001:db8::1");
    g_assert_cmpstr (ctx.call.dns1_v6, ==, "2001:db8::53");
    g_assert_cmpuint (ctx.call.mtu, ==, 1500);
}

static void
test_dsds_data_ownership (void)
{
    g_autoptr(MMSitClient) first = mm_sit_client_new ("test-sit0");
    g_autoptr(MMSitClient) second = mm_sit_client_new ("test-sit1");
    g_autoptr(MMSitCp) cp = mm_sit_cp_new (first, second);
    g_autoptr(GArray) calls = g_array_new (FALSE, FALSE, sizeof (MMSitDataCall));
    g_autoptr(GError) error = NULL;

    g_assert_true (mm_sit_cp_claim_data (cp, 0, 2, &error));
    g_assert_false (mm_sit_cp_claim_data (cp, 0, 2, &error));
    g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS);
    g_clear_error (&error);
    /* Pending setup alone excludes the other stack, even before an address. */
    mm_sit_cp_update_data_calls (cp, 0, calls);
    g_assert_false (mm_sit_cp_claim_data (cp, 1, 17, &error));
    g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS);
    g_clear_error (&error);
    mm_sit_cp_commit_data (cp, 2);
    g_assert_true (mm_sit_cp_claim_data (cp, 0, 3, &error));
    mm_sit_cp_release_data (cp, 2);
    g_assert_false (mm_sit_cp_claim_data (cp, 1, 17, &error));
    g_clear_error (&error);
    mm_sit_cp_release_data (cp, 3);
    g_assert_true (mm_sit_cp_claim_data (cp, 1, 17, &error));
    mm_sit_cp_commit_data (cp, 17);
    /* A confirmed network deactivation frees the preference, too. */
    mm_sit_cp_update_data_calls (cp, 1, calls);
    g_assert_true (mm_sit_cp_claim_data (cp, 0, 2, &error));
    g_assert_no_error (error);
}

static void
client_test_disconnect_ready (MMSitClient *client, GAsyncResult *res, ClientTestContext *ctx)
{
    ctx->completed = mm_sit_client_disconnect_data_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static void
test_client_dsds_secondary (void)
{
    ClientTestContext ctx = { .dsds = TRUE, .ipv6_only = TRUE, .rounds = 1 };
    gint primary[2], secondary[2];
    guint timeout_id;
    g_autoptr(MMPortSerialSit) port0 = NULL;
    g_autoptr(MMPortSerialSit) port1 = NULL;
    g_autoptr(MMSitClient) first = NULL;
    g_autoptr(MMSitClient) second = NULL;
    g_autoptr(MMSitCp) cp = NULL;
    g_autoptr(GThread) peer = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, primary), ==, 0);
    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, secondary), ==, 0);
    port0 = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
        MM_PORT_DEVICE, "test-dsds-sit0", MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
        MM_PORT_TYPE, MM_PORT_TYPE_SIT, MM_PORT_SERIAL_FD, primary[0], NULL));
    port1 = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
        MM_PORT_DEVICE, "test-dsds-sit1", MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
        MM_PORT_TYPE, MM_PORT_TYPE_SIT, MM_PORT_SERIAL_FD, secondary[0], NULL));
    first = mm_sit_client_new_from_port (port0);
    second = mm_sit_client_new_from_port (port1);
    g_assert_true (mm_sit_client_open (first, &error));
    g_assert_true (mm_sit_client_open (second, &error));
    cp = mm_sit_cp_new (first, second);
    ctx.peer_fd = secondary[1];
    ctx.control_fd = primary[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    peer = g_thread_new ("sit-dsds-peer", (GThreadFunc) client_test_peer, &ctx);
    mm_sit_client_connect_data (second, "wbdata", MM_SIT_PDP_TYPE_IPV4V6,
                                MM_SIT_APN_TYPE_DEFAULT, NULL, NULL,
                                (GAsyncReadyCallback) client_test_connect_ready, &ctx);
    g_assert_false (mm_sit_cp_claim_data (cp, 0, 2, &error));
    g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS);
    g_clear_error (&error);
    timeout_id = g_timeout_add_seconds (10, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);
    if (!ctx.timed_out)
        g_source_remove (timeout_id);
    g_assert_false (ctx.timed_out);
    g_assert_no_error (ctx.error);
    g_assert_true (ctx.completed);
    g_assert_cmpuint (ctx.requested_cid[0], ==, 17);
    g_assert_cmpuint (ctx.call.cid, ==, 17);
    g_assert_false (ctx.call.has_ipv4);
    g_assert_true (ctx.call.has_ipv6);
    g_assert_false (mm_sit_cp_claim_data (cp, 0, 2, &error));
    g_clear_error (&error);
    ctx.completed = FALSE;
    mm_sit_client_disconnect_data (second, 17, NULL,
                                   (GAsyncReadyCallback) client_test_disconnect_ready, &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_false (ctx.timed_out);
    g_source_remove (timeout_id);
    g_assert_true (ctx.completed);
    g_assert_no_error (ctx.error);
    g_assert_true (mm_sit_cp_claim_data (cp, 0, 2, &error));
    g_assert_no_error (error);
    g_thread_join (g_steal_pointer (&peer));
    mm_sit_client_close (first);
    mm_sit_client_close (second);
    close (primary[1]);
    close (secondary[1]);
    g_main_loop_unref (ctx.loop);
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
            /* The requested PDP type has to reach the wire: @0xda of the frame,
             * so 0xda - 8 of the body. */
            if (expected_ids[i] == MM_SIT_MESSAGE_SETUP_DATA_CALL)
                g_assert_cmpuint (body[0xda - MM_SIT_HEADER_SIZE], ==, MM_SIT_PDP_TYPE_IPV4V6);
        }

        /* Captured shapes: a successful SET reply is 13 bytes carrying nothing
         * but a zero status, and the state comes from the GET reply's u32 at
         * @12.  (A 12-byte SET reply with 2 at @10 is not "state on" but the
         * CP's GENERIC_FAILURE for a radio already in that state.) */
        response_len = (expected_ids[i] == MM_SIT_MESSAGE_GET_RADIO_POWER) ? 16 : 13;
        response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
        response[2] = header[2];
        response[3] = header[3];
        response[4] = response_len;
        response[6] = header[6];
        response[7] = header[7];
        if (response_len == 16)
            response[12] = MM_SIT_RADIO_STATE_ON;
        g_assert_true (write_all (ctx->peer_fd, response, response_len));
    }
    return NULL;
}

/* Same exchange, but the CP refuses the SET (12-byte reply, status 2 at @10)
 * and still reports the target state -- a fresh, parked CP. */
static gpointer
client_radio_refused_test_peer (ClientTestContext *ctx)
{
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
            response[10] = 2;
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
client_sim_status_ready (MMSitClient *client, GAsyncResult *res, ClientTestContext *ctx)
{
    MMSitSimStatus status;

    ctx->completed = mm_sit_client_get_sim_status_finish (client, res, &status, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gpointer
client_sim_status_peer (ClientTestContext *ctx)
{
    guint i;

    for (i = 0; i < 4; i++) {
        /* No application records, followed by one 63-byte card block. */
        guint8 status[80] = { 0 };

        status[12] = i != 2;
        status[51] = i == 2 ? 0 : 2;
        status[52] = 0x98;
        status[53] = i == 3 && !ctx->same_sim ? 0x11 : 0x10;
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_SIM_STATUS, status, sizeof (status));
    }
    return NULL;
}

static void
test_client_radio_transition (gconstpointer data)
{
    ClientTestContext          ctx = { .same_sim = GPOINTER_TO_UINT (data) };
    gint                       sockets[2];
    guint                      timeout_id;
    guint                      i;
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

    peer = g_thread_new ("sit-sim-status-peer", (GThreadFunc) client_sim_status_peer, &ctx);
    /* Same identity (including a transient absent card) preserves radio
     * trust. Only a confirmed new ICCID requires subscription setup again. */
    for (i = 0; i < 4; i++) {
        ctx.completed = FALSE;
        mm_sit_client_get_sim_status (client, NULL,
                                      (GAsyncReadyCallback) client_sim_status_ready, &ctx);
        timeout_id = g_timeout_add_seconds (5, (GSourceFunc) client_test_timeout, &ctx);
        g_main_loop_run (ctx.loop);
        g_assert_false (ctx.timed_out);
        g_source_remove (timeout_id);
        g_assert_no_error (ctx.error);
        g_assert_true (ctx.completed);
        g_assert_cmpint (mm_sit_client_radio_state_trusted (client), ==, i != 3);
        if (ctx.same_sim && i == 2) {
            /* A confirmed empty subscription invalidates setup even when
             * the next enabled profile has the exact same ICCID. */
            mm_sit_client_invalidate_sim (client);
            g_assert_false (mm_sit_client_radio_state_trusted (client));
        }
    }
    g_thread_join (g_steal_pointer (&peer));

    mm_sit_client_close (client);
    g_assert_false (mm_sit_client_radio_state_trusted (client));
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);
}

/* A refused SET must not make the polled state trustworthy: the core has to be
 * able to see through the parked CP's "on" and run the power-up chain again. */
static void
test_client_radio_transition_refused (void)
{
    ClientTestContext          ctx = { 0 };
    gint                       sockets[2];
    guint                      timeout_id;
    g_autoptr(GError)          error = NULL;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient)     client = NULL;
    GThread                   *peer;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
                                             MM_PORT_DEVICE, "test-sit-radio-refused",
                                             MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
                                             MM_PORT_TYPE, MM_PORT_TYPE_SIT,
                                             MM_PORT_SERIAL_FD, sockets[0],
                                             NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    g_assert_no_error (error);

    peer = g_thread_new ("sit-radio-refused-test-peer",
                         (GThreadFunc) client_radio_refused_test_peer, &ctx);
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
    g_assert_false (mm_sit_client_radio_state_trusted (client));

    mm_sit_client_close (client);
    close (ctx.peer_fd);
    g_main_loop_unref (ctx.loop);
}

static gpointer
client_boot_config_test_peer (ClientTestContext *ctx)
{
    static const guint16 expected_ids[] = {
        MM_SIT_MESSAGE_SET_AP_SYSTEM_TIME,
        MM_SIT_MESSAGE_DEVICE_INFO,
        MM_SIT_MESSAGE_GET_IMEISV,
        MM_SIT_MESSAGE_OEM_CONFIG,
        MM_SIT_MESSAGE_SET_MODEM_CONFIG,
        /*
         * The elementary-file pass. One GET_RESPONSE each, every one refused
         * with 6a82 as a card that lacks the file would, so read_ef() gives up
         * after a single exchange and the count here is the file count. On a
         * card that has them, every file but the two operator-name tables stops
         * after one data pass -- reading all records of all of them overshot
         * stock by 85 exchanges.
         */
#define BOOT_EF_FILES 29
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        MM_SIT_MESSAGE_SIM_IO, MM_SIT_MESSAGE_SIM_IO,
        /*
         * The UICC access-control probe that closes the boot batch: three
         * opens by AID that the card refuses, one with no AID that yields a
         * channel, the three EF_DIR reads on it, and the close. Asserting the
         * exact order is the point -- the channel has to come from the open
         * response and the close has to happen.
         */
        MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL,
        MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL,
        MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL,
        MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL,
        MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL,
        MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL,
        MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL,
        MM_SIT_MESSAGE_CLOSE_LOGICAL_CHANNEL,
    };
    guint opens = 0, apdus = 0;
    /* A GET_IMEISV payload of the captured shape -- a length byte and 17
     * digits -- with a test identity, so the software version the batch then
     * sends can be checked against its tail. */
    static const guint8 imeisv_payload[] = {
        17, '4', '9', '0', '1', '5', '4', '2', '0', '3',
        '2', '3', '7', '5', '1', '8', '3', '3',
    };
    /* The live SET_DEVICE_INFO reply: the CP's struct-version list, NUL
     * terminated. Every version here is one the codec parses. */
    static const guint8 versions_payload[] =
        "CellInfo=5,CellIdentity=5,SignalStrength=5";
    guint i;

    for (i = 0; i < G_N_ELEMENTS (expected_ids); i++) {
        guint8  header[MM_SIT_HEADER_SIZE];
        guint8  response[12 + sizeof (versions_payload)] = { 0 };
        gsize   response_len = 12;
        guint16 request_len;
        guint32 body_channel = 0;

        G_STATIC_ASSERT (sizeof (versions_payload) > sizeof (imeisv_payload));

        g_assert_true (read_all (ctx->peer_fd, header, sizeof (header)));
        request_len = header[4] | (((guint16) header[5]) << 8);
        g_assert_cmpuint (header[2] | (((guint16) header[3]) << 8), ==, expected_ids[i]);
        if (request_len > MM_SIT_HEADER_SIZE) {
            g_autofree guint8 *body = g_malloc (request_len - MM_SIT_HEADER_SIZE);

            g_assert_true (read_all (ctx->peer_fd, body, request_len - MM_SIT_HEADER_SIZE));
            /* The requested PDP type has to reach the wire: @0xda of the frame,
             * so 0xda - 8 of the body. */
            if (expected_ids[i] == MM_SIT_MESSAGE_SETUP_DATA_CALL)
                g_assert_cmpuint (body[0xda - MM_SIT_HEADER_SIZE], ==, MM_SIT_PDP_TYPE_IPV4V6);
            if (expected_ids[i] == MM_SIT_MESSAGE_SET_MODEM_CONFIG)
                g_assert_cmpuint (body[4], ==, ctx->dsds ? 1 : 0);
            if (expected_ids[i] == MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL ||
                expected_ids[i] == MM_SIT_MESSAGE_CLOSE_LOGICAL_CHANNEL)
                memcpy (&body_channel, body, 4);
            /* And the software version has to be the one the CP just gave,
             * not a constant: @12 of the frame, ASCII. */
            if (expected_ids[i] == MM_SIT_MESSAGE_OEM_CONFIG) {
                g_assert_cmpuint (request_len, ==, 14);
                g_assert_cmpuint (body[12 - MM_SIT_HEADER_SIZE], ==, '3');
                g_assert_cmpuint (body[13 - MM_SIT_HEADER_SIZE], ==, '3');
            }
        }

        if (expected_ids[i] == MM_SIT_MESSAGE_SIM_IO) {
            /* sw1/sw2 at frame+12/13: 6a82, "file not found". */
            response_len = 16;
            response[12] = 0x6a;
            response[13] = 0x82;
        } else if (expected_ids[i] == MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL) {
            /* Channel at frame+12, its SW at +16. The first three opens name
             * an applet this card does not have (6a82); the fourth names none
             * and gets channel 1. */
            response_len = 18;
            /* SW as the capture has it: `6a 82` for an absent applet and
             * `90 00` for success, i.e. 0x826a and 0x0090 read little-endian. */
            if (++opens < 4) {
                memset (&response[12], 0xff, 4);
                response[16] = 0x6a;
                response[17] = 0x82;
            } else {
                response[12] = 1;
                response[16] = 0x90;
                response[17] = 0x00;
            }
        } else if (expected_ids[i] == MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL) {
            /* Every APDU has to carry the channel the open handed back. */
            g_assert_cmpuint (body_channel, ==, 1);
            apdus++;
        } else if (expected_ids[i] == MM_SIT_MESSAGE_CLOSE_LOGICAL_CHANNEL) {
            g_assert_cmpuint (apdus, ==, 3);
            g_assert_cmpuint (body_channel, ==, 1);
        } else if (expected_ids[i] == MM_SIT_MESSAGE_GET_IMEISV) {
            memcpy (&response[12], imeisv_payload, sizeof (imeisv_payload));
            response_len = 12 + sizeof (imeisv_payload);
        } else if (expected_ids[i] == MM_SIT_MESSAGE_DEVICE_INFO) {
            memcpy (&response[12], versions_payload, sizeof (versions_payload));
            response_len = 12 + sizeof (versions_payload);
        }
        response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
        response[2] = header[2];
        response[3] = header[3];
        response[4] = response_len;
        response[6] = header[6];
        response[7] = header[7];
        g_assert_true (write_all (ctx->peer_fd, response, response_len));
    }
    return NULL;
}

static void
reply_request (gint fd, guint16 expected, guint8 *response, gsize response_len)
{
    guint8 header[8];
    guint16 request_len;
    g_autofree guint8 *body = NULL;

    g_assert_true (read_all (fd, header, sizeof (header)));
    g_assert_cmpuint (header[2] | ((guint16) header[3] << 8), ==, expected);
    request_len = header[4] | ((guint16) header[5] << 8);
    g_assert_cmpuint (request_len, >=, 8);
    body = g_malloc (request_len);
    g_assert_true (read_all (fd, body, request_len - 8));
    if (expected == MM_SIT_MESSAGE_SET_RADIO_POWER)
        g_assert_cmpuint (body[4], ==, MM_SIT_RADIO_STATE_OFF);
    if (expected == MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL)
        g_assert_cmpuint (body[4], ==, 17);
    if (expected == MM_SIT_MESSAGE_SET_LOGICAL_SLOT_MAPPING) {
        static const guint8 mapping[] = { 0, 0, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0 };

        g_assert_cmpmem (body, request_len - 8, mapping, sizeof (mapping));
    }
    response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
    set_le16 (&response[2], expected);
    set_le16 (&response[4], response_len);
    response[6] = header[6];
    response[7] = header[7];
    g_assert_true (write_all (fd, response, response_len));
}

static gpointer
cp_prepare_peer (ClientTestContext *ctx)
{
    guint8 ack[12] = { 0 };
    guint8 radio[16] = { 0 };
    guint8 slots[433] = { 0 };
    guint i;

    client_boot_config_test_peer (ctx);
    radio[12] = 0; /* GET reports off/on as 0/1; SET uses 1/2. */
    slots[12] = 2;
    for (i = 0; i < 2; i++) {
        guint8 *slot = &slots[13 + 105 * i];

        slot[0] = 1;
        slot[52] = 1;
        slot[53] = 10;
        memset (&slot[54], 0x98, 10);
        slot[64] = 1 - i; /* physical 0 -> stack 1, physical 1 -> stack 0 */
        slot[65] = 1;
        if (ctx->empty_sims) {
            /* Present eUICC with no enabled profile; absent physical card. */
            slot[0] = i == 0;
            slot[35] = i == 0 ? 16 : 0;
            slot[53] = 0;
            memset (&slot[54], 0, 10);
        }
    }
    if (ctx->add_mapping) {
        slots[13 + 64] = 0xff;
        slots[13 + 65] = 0;
    }
    /*
     * The inventory is the first thing after the boot config now. The radios
     * used to be taken down here unconditionally -- stock never sends
     * SET_RADIO_POWER OFF at all, so that cost moved to the one branch that
     * needs it, writing a slot mapping. A boot whose stacks are already mapped
     * sends one GET_SIM_SLOT_STATUS and is done.
     */
    reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS, slots, sizeof (slots));
    if (ctx->add_mapping == 1) {
        /* Five more inventories before the mapping branch opens, then both
         * radios down -- stack 1 first, then stack 0 -- each followed by the
         * re-query that reaches the branch again. */
        for (i = 0; i < 5; i++)
            reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS, slots, sizeof (slots));
        reply_request (ctx->control_fd, MM_SIT_MESSAGE_SET_RADIO_POWER, ack, sizeof (ack));
        reply_request (ctx->control_fd, MM_SIT_MESSAGE_GET_RADIO_POWER, radio, sizeof (radio));
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS, slots, sizeof (slots));
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_SET_RADIO_POWER, ack, sizeof (ack));
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_RADIO_POWER, radio, sizeof (radio));
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS, slots, sizeof (slots));
        ack[10] = 7; /* CP may reject a mapping that nevertheless appears. */
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_SET_LOGICAL_SLOT_MAPPING, ack, sizeof (ack));
        ack[10] = 0;
    }
    if (ctx->add_mapping) {
        slots[13 + 64] = 1;
        slots[13 + 65] = 1;
        reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS, slots, sizeof (slots));
    }
    return NULL;
}

static void
cp_prepare_ready (MMSitCp *cp, GAsyncResult *res, ClientTestContext *ctx)
{
    g_assert_true (mm_sit_cp_prepare_finish (cp, res, &ctx->error));
    g_assert_no_error (ctx->error);
    if (++ctx->callbacks == 2)
        g_main_loop_quit (ctx->loop);
}

static void
test_cp_prepare_coalesced (gconstpointer data)
{
    ClientTestContext ctx = { .dsds = TRUE, .add_mapping = GPOINTER_TO_UINT (data) % 3,
                              .empty_sims = GPOINTER_TO_UINT (data) >= 3 };
    gint sockets[2][2];
    guint timeout_id, i;
    g_autoptr(GPtrArray) clients = g_ptr_array_new_with_free_func (g_object_unref);
    g_autoptr(MMSitCp) cp = NULL;
    g_autoptr(GThread) peer = NULL;
    g_autoptr(GError) error = NULL;

    for (i = 0; i < 2; i++) {
        g_autofree gchar *name = g_strdup_printf ("prepare-sit%u", i);
        g_autoptr(MMPortSerialSit) port = NULL;
        MMSitClient *client;

        g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets[i]), ==, 0);
        port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
            MM_PORT_DEVICE, name, MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
            MM_PORT_TYPE, MM_PORT_TYPE_SIT, MM_PORT_SERIAL_FD, sockets[i][0], NULL));
        client = mm_sit_client_new_from_port (port);
        g_assert_true (mm_sit_client_open (client, &error));
        g_ptr_array_add (clients, client);
    }
    cp = mm_sit_cp_new (g_ptr_array_index (clients, 0), g_ptr_array_index (clients, 1));
    ctx.peer_fd = sockets[0][1];
    ctx.control_fd = sockets[1][1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    peer = g_thread_new ("cp-prepare", (GThreadFunc) cp_prepare_peer, &ctx);
    mm_sit_cp_prepare (cp, (GAsyncReadyCallback) cp_prepare_ready, &ctx);
    mm_sit_cp_prepare (cp, (GAsyncReadyCallback) cp_prepare_ready, &ctx);
    timeout_id = g_timeout_add_seconds (10, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_false (ctx.timed_out);
    g_source_remove (timeout_id);
    g_assert_cmpuint (ctx.callbacks, ==, 2);
    g_thread_join (g_steal_pointer (&peer));
    for (i = 0; i < 2; i++) {
        mm_sit_client_close (g_ptr_array_index (clients, i));
        close (sockets[i][1]);
    }
    g_main_loop_unref (ctx.loop);
}

static gpointer
registration_camped_peer (ClientTestContext *ctx)
{
    guint8 registration[86] = { 0 };
    guint8 operator[119] = { 0 };

    registration[15] = 14;
    memcpy (&operator[12], "26202#", 6);
    reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_DATA_REGISTRATION, registration, sizeof (registration));
    reply_request (ctx->peer_fd, MM_SIT_MESSAGE_GET_OPERATOR, operator, sizeof (operator));
    return NULL;
}

static void
registration_camped_ready (MMSitClient *client, GAsyncResult *res, ClientTestContext *ctx)
{
    MMSitRegistration registration;

    g_assert_true (mm_sit_client_get_registration_finish (client, res, &registration, &ctx->error));
    g_assert_no_error (ctx->error);
    g_assert_false (registration.registered);
    g_assert_cmpuint (registration.state, ==, 0);
    g_main_loop_quit (ctx->loop);
}

static void
test_registration_camped (void)
{
    ClientTestContext ctx = { 0 };
    gint sockets[2];
    guint timeout_id;
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient) client = NULL;
    g_autoptr(GThread) peer = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
        MM_PORT_DEVICE, "camped-sit0", MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
        MM_PORT_TYPE, MM_PORT_TYPE_SIT, MM_PORT_SERIAL_FD, sockets[0], NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    ctx.peer_fd = sockets[1];
    ctx.loop = g_main_loop_new (NULL, FALSE);
    peer = g_thread_new ("camped-registration", (GThreadFunc) registration_camped_peer, &ctx);
    mm_sit_client_get_registration (client, NULL, (GAsyncReadyCallback) registration_camped_ready, &ctx);
    timeout_id = g_timeout_add_seconds (5, (GSourceFunc) client_test_timeout, &ctx);
    g_main_loop_run (ctx.loop);
    g_assert_false (ctx.timed_out);
    g_source_remove (timeout_id);
    g_thread_join (g_steal_pointer (&peer));
    mm_sit_client_close (client);
    close (sockets[1]);
    g_main_loop_unref (ctx.loop);
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

/*****************************************************************************/
/* IMS configuration */

static const SamsungAimsConfigEntry *
find_ims_entry (const SamsungAimsConfigEntry *entries,
                gsize                         n,
                guint16                       tag,
                guint                        *out_count)
{
    const SamsungAimsConfigEntry *found = NULL;
    guint                         count = 0;
    gsize                         i;

    for (i = 0; i < n; i++) {
        if (entries[i].tag == tag) {
            found = &entries[i];
            count++;
        }
    }
    if (out_count)
        *out_count = count;
    return found;
}

static const MMSitImsDeviceIdentity test_device = {
    "Google",           /* manufacturer */
    "Pixel9a",          /* model */
    "Android",          /* os_name */
    "17",               /* os_version */
    "CP2A.260705.006",  /* software_version */
};

/*
 * A stand-in for the generated ims.db, small enough to read but the same shape:
 * a `default` carrier every unknown SIM falls back to, one real carrier
 * selected by MCCMNC, and one row of each stored type. Built here rather than
 * checked in as a fixture so that what each assertion below depends on is
 * visible next to the assertion.
 */
static gchar *
build_test_ims_db (void)
{
    static const gchar *const sql[] = {
        "CREATE TABLE carrier (id INTEGER PRIMARY KEY, canonical_name TEXT NOT NULL UNIQUE,"
        " version INTEGER)",
        "CREATE TABLE carrier_id (carrier INTEGER NOT NULL, mccmnc TEXT, spn TEXT, imsi TEXT,"
        " gid1 TEXT, gid2 TEXT, iccid TEXT)",
        "CREATE TABLE setting (carrier INTEGER NOT NULL, tag INTEGER NOT NULL, kind TEXT NOT NULL,"
        " int_value INTEGER, text_value TEXT, blob_value BLOB, key TEXT, PRIMARY KEY (carrier, tag))",
        "INSERT INTO carrier (id, canonical_name) VALUES (1, 'default'), (2, 'o2_de'), (3, 'mvno_de')",
        /* Two records on one MCCMNC, told apart only by GID1 -- the shape that
         * makes the match rule matter rather than a formality. */
        "INSERT INTO carrier_id (carrier, mccmnc, gid1) VALUES (2, '26203', NULL), (3, '26203', 'AB')",
        "INSERT INTO setting (carrier, tag, kind, int_value) VALUES (3, 1028, 'int', 1400)",
        /* The defaults, including a User-Agent template with no suffix. */
        "INSERT INTO setting (carrier, tag, kind, int_value, text_value) VALUES"
        " (1,   10, 'int', 5060, NULL),"
        " (1,  528, 'int', 128000, NULL),"
        " (1, 1028, 'int', 1500, NULL),"
        " (1, 1029, 'int', 1500, NULL),"
        " (1,  519, 'str', NULL, '#MANUFACTURE# #MODEL# Android #AV# #BUILD#')",
        /* o2, which overrides some of them and adds a certification suffix. */
        "INSERT INTO setting (carrier, tag, kind, int_value, text_value) VALUES"
        " (2,   10, 'int', 5060, NULL),"
        " (2,  528, 'int', 32000, NULL),"
        " (2, 1028, 'int', 1300, NULL),"
        " (2, 1032, 'str', NULL, 'teleconferenceuri-factory@telefonica.de'),"
        " (2,  519, 'str', NULL, '#MANUFACTURE# #MODEL# Android #AV# #BUILD# EU01454')",
        "INSERT INTO setting (carrier, tag, kind, blob_value) VALUES"
        " (2, 1060, 'bytes', x'68' || x'01' || x'ff' || x'00')",
        NULL,
    };
    g_autofree gchar *dir = NULL;
    gchar            *path;
    sqlite3          *db = NULL;
    guint             i;

    dir = g_dir_make_tmp ("mm-sit-ims-XXXXXX", NULL);
    g_assert_nonnull (dir);
    path = g_build_filename (dir, "ims.db", NULL);
    g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
    for (i = 0; sql[i]; i++)
        g_assert_cmpint (sqlite3_exec (db, sql[i], NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close (db);
    return path;
}

typedef struct {
    guint setups;
    guint stops;
    guint pdn_infos;
    guint deactivations;
    guint succeed_on;
} ImsRetryPeer;

/* Exercise the real IMS/client state machines over the SIT socket. The test
 * target uses a one-second retry delay; production keeps thirty seconds. */
static gboolean
ims_retry_peer_ready (gint fd, GIOCondition condition, ImsRetryPeer *peer)
{
    guint8 header[8];
    guint8 response[304] = { 0 };
    guint16 id, length, response_len = 12;
    g_autofree guint8 *body = NULL;

    g_assert_cmpuint (condition & (G_IO_HUP | G_IO_ERR), ==, 0);
    g_assert_true (read_all (fd, header, sizeof (header)));
    id = header[2] | ((guint16) header[3] << 8);
    length = header[4] | ((guint16) header[5] << 8);
    g_assert_cmpuint (length, >=, sizeof (header));
    body = g_malloc (length);
    g_assert_true (read_all (fd, body, length - sizeof (header)));

    switch (id) {
    case MM_SIT_MESSAGE_SETUP_DATA_CALL:
        g_assert_cmpuint (length, >, 218);
        g_assert_cmpmem (&body[8], 4, "ims", 4);
        peer->setups++;
        response_len = sizeof (response);
        response[14] = body[4]; /* requested CID */
        if (!peer->succeed_on || peer->setups < peer->succeed_on)
            set_le16 (&response[12], 33);
        else {
            response[15] = 1;
            response[16] = 1; /* IPv4 grant with a P-CSCF */
            response[17] = 10;
            response[20] = 2;
            response[79] = 192;
            response[81] = 2;
            response[82] = 1;
        }
        break;
    case MM_SIT_MESSAGE_SET_DATA_PROFILE:
        /* The IMS PDN registers its profile before asking for the context.
         * A plain success here; the caller carries on either way. */
        break;
    case 0x0d06: /* AIMS_DEREGISTRATION */
    case 0x0d39: /* AIMS_STACK_START_REQ */
    case 0x0d07: /* AIMS_HIDDEN_MENU */
        break;
    /*
     * The boot-parity PRE_PDN preamble, replayed from STEP_PREAMBLE, and the
     * indication filter and display state re-asserted from
     * STEP_REASSERT_FILTER once the context is up. Both are advisory in the
     * plugin and this test is about the retry ladder, so a plain success is
     * all they need -- but they do reach the peer, and without these the
     * default arm trips.
     */
    case 0x0d0b:
    case 0x0d3b:
    case 0x0d3c:
    case 0x091a:
    case MM_SIT_MESSAGE_QUERY_NETWORK_SELECTION_MODE:
    case MM_SIT_MESSAGE_GET_VOICE_REGISTRATION:
    case MM_SIT_MESSAGE_SET_INDICATION_FILTER:
    case MM_SIT_MESSAGE_SET_SCREEN_STATE:
        break;
    case 0x0d3a: /* AIMS_STACK_STOP_REQ */
        peer->stops++;
        break;
    case 0x0d08: /* AIMS_ADD_PDN_INFO */
        peer->pdn_infos++;
        break;
    case MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL:
        peer->deactivations++;
        break;
    default:
        g_assert_not_reached ();
    }
    response[0] = MM_SIT_MESSAGE_TYPE_RESPONSE;
    set_le16 (&response[2], id);
    set_le16 (&response[4], response_len);
    response[6] = header[6];
    response[7] = header[7];
    g_assert_true (write_all (fd, response, response_len));
    return G_SOURCE_CONTINUE;
}

typedef struct {
    GMainLoop *loop;
    guint *counter;
    guint target;
} ImsRetryWait;

static gboolean
ims_retry_wait_poll (ImsRetryWait *wait)
{
    if (*wait->counter < wait->target)
        return G_SOURCE_CONTINUE;
    g_main_loop_quit (wait->loop);
    return G_SOURCE_REMOVE;
}

static gboolean
ims_retry_wait_done (GMainLoop *loop)
{
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static void
ims_retry_wait (guint *counter, guint target)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
    ImsRetryWait wait = { loop, counter, target };
    guint poll_id, timeout_id;

    poll_id = g_timeout_add (10, (GSourceFunc) ims_retry_wait_poll, &wait);
    timeout_id = g_timeout_add_seconds (8, (GSourceFunc) ims_retry_wait_done, loop);
    g_main_loop_run (loop);
    if (*counter < target) {
        g_source_remove (poll_id);
        g_assert_cmpuint (*counter, >=, target);
    }
    g_source_remove (timeout_id);
}

static void
ims_wait_retry_interval (void)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);

    g_timeout_add (2200, (GSourceFunc) ims_retry_wait_done, loop);
    g_main_loop_run (loop);
}

static void
ims_retry_check_quiet (MMSitIms *ims)
{
    guint i;

    /* Normal registration/RAT/operator updates must not defeat either the
     * exhausted budget or successful provisioning. Observe beyond a retry. */
    for (i = 0; i < 3; i++) {
        mm_sit_ims_update_access_tech (ims, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
        mm_sit_ims_set_registered (ims, TRUE);
        mm_sit_ims_set_operator_code (ims, i % 2 ? "00102" : "00101");
    }
    ims_wait_retry_interval ();
}

static void
ims_test_vops_updates (MMSitIms *ims, ImsRetryPeer *peer)
{
    MMSitRegistration registration = {
        .registered = TRUE,
        .vops_support = MM_SIT_VOPS_SUPPORT_UNSUPPORTED,
    };

    /* Install capability before the first registered/RAT notification. */
    mm_sit_ims_update_registration (ims, &registration);
    ims_retry_check_quiet (ims);
    g_assert_cmpuint (peer->setups, ==, 0);
    g_assert_cmpuint (peer->stops, ==, 0);

    /* A capability change alone can unblock IMS on an already registered SIM. */
    registration.vops_support = MM_SIT_VOPS_SUPPORT_SUPPORTED;
    mm_sit_ims_update_registration (ims, &registration);
    ims_retry_wait (&peer->pdn_infos, 1);
    g_assert_cmpuint (peer->setups, ==, 1);

    registration.vops_support = MM_SIT_VOPS_SUPPORT_UNSUPPORTED;
    mm_sit_ims_update_registration (ims, &registration);
    ims_retry_wait (&peer->deactivations, 1);
    ims_retry_check_quiet (ims);
    g_assert_cmpuint (peer->setups, ==, 1);
    g_assert_cmpuint (peer->stops, ==, 1);

    /* Clearing the old refusal on registration loss must not briefly start
     * IMS using the previous registered state. */
    registration.registered = FALSE;
    registration.vops_support = MM_SIT_VOPS_SUPPORT_UNKNOWN;
    mm_sit_ims_update_registration (ims, &registration);
    ims_wait_retry_interval ();
    g_assert_cmpuint (peer->setups, ==, 1);

    /* Older frames/other RATs provide no capability: retain the fallback. */
    registration.registered = TRUE;
    mm_sit_ims_update_registration (ims, &registration);
    ims_retry_wait (&peer->pdn_infos, 2);
    g_assert_cmpuint (peer->setups, ==, 2);
    mm_sit_ims_stop (ims);
    ims_retry_wait (&peer->deactivations, 2);
}

static void
test_ims_provisioning_retries (gconstpointer data)
{
    gboolean recover = GPOINTER_TO_UINT (data) == 1;
    gboolean vops = GPOINTER_TO_UINT (data) == 2;
    ImsRetryPeer peer = { .succeed_on = vops ? 1 : (recover ? 3 : 0) };
    g_autofree gchar *database = build_test_ims_db ();
    g_autofree gchar *directory = g_path_get_dirname (database);
    g_autoptr(MMPortSerialSit) port = NULL;
    g_autoptr(MMSitClient) client = NULL;
    g_autoptr(MMSitIms) ims = NULL;
    g_autoptr(GError) error = NULL;
    gint sockets[2];
    guint peer_id;

    g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    port = MM_PORT_SERIAL_SIT (g_object_new (MM_TYPE_PORT_SERIAL_SIT,
        MM_PORT_DEVICE, "ims-retry-sit0", MM_PORT_SUBSYS, MM_PORT_SUBSYS_WWAN,
        MM_PORT_TYPE, MM_PORT_TYPE_SIT, MM_PORT_SERIAL_FD, sockets[0], NULL));
    client = mm_sit_client_new_from_port (port);
    g_assert_true (mm_sit_client_open (client, &error));
    g_assert_no_error (error);
    peer_id = g_unix_fd_add (sockets[1], G_IO_IN | G_IO_HUP | G_IO_ERR,
                             (GUnixFDSourceFunc) ims_retry_peer_ready, &peer);
    ims = mm_sit_ims_new (client, 0, database);
    mm_sit_ims_set_device_identity (ims, &test_device);
    if (vops) {
        ims_test_vops_updates (ims, &peer);
        goto out;
    }
    mm_sit_ims_update_access_tech (ims, MM_MODEM_ACCESS_TECHNOLOGY_LTE);
    mm_sit_ims_set_registered (ims, TRUE);
    ims_retry_wait (recover ? &peer.pdn_infos : &peer.stops, 1);
    ims_retry_check_quiet (ims);
    g_assert_cmpuint (peer.setups, ==, 3);
    g_assert_cmpuint (peer.pdn_infos, ==, recover ? 1 : 0);
    g_assert_cmpuint (peer.stops, ==, recover ? 0 : 1);

    if (!recover) {
        /* A new registration cycle gets the full budget again. */
        mm_sit_ims_set_registered (ims, FALSE);
        mm_sit_ims_set_registered (ims, TRUE);
        ims_retry_wait (&peer.stops, 3);
        ims_retry_check_quiet (ims);
        g_assert_cmpuint (peer.setups, ==, 6);
    }
    mm_sit_ims_stop (ims);
    ims_retry_wait (&peer.stops, recover ? 1 : 4);
    if (recover)
        ims_retry_wait (&peer.deactivations, 1);
out:
    g_clear_object (&ims);
    g_source_remove (peer_id);
    mm_sit_client_close (client);
    close (sockets[1]);
    g_unlink (database);
    g_rmdir (directory);
}

/* An unknown carrier is the defaults, not a failure: a CP with sane timers and
 * a valid User-Agent is worth trying. */
static void
test_ims_config_defaults (void)
{
    g_autoptr(MMSitImsConfig)     config = NULL;
    const SamsungAimsConfigEntry *entries;
    const SamsungAimsConfigEntry *e;
    gsize                         n = 0;

    g_autofree gchar *path = build_test_ims_db ();

    config = mm_sit_ims_config_resolve (path, &test_device, &(MMSitCarrierIdentity){ .mccmnc = "99999" }, NULL);
    g_assert_nonnull (config);
    g_assert_cmpstr (mm_sit_ims_config_get_carrier (config), ==, "default");

    entries = mm_sit_ims_config_peek_entries (config, &n);

    e = find_ims_entry (entries, n, 1028, NULL); /* IPv4 SIP MTU */
    g_assert_nonnull (e);
    g_assert_cmpuint (e->int_value, ==, 1500);
    e = find_ims_entry (entries, n, 10, NULL);   /* SIP server port */
    g_assert_nonnull (e);
    g_assert_cmpuint (e->int_value, ==, 5060);

    /*
     * The seven tags that are in no database because the vendor does not read
     * them from carrier configuration either. They have to be sent whatever the
     * database says, so a carrier that never mentions them is the case worth
     * asserting.
     */
    {
        static const struct {
            guint16 tag;
            guint32 value;
        } constants[] = {
            { 523, 30 }, { 527, 30 }, { 529, 30 }, { 531, 30 }, { 533, 30 },
            { 1044, 1 }, { 1045, 0 },
        };
        guint i;

        for (i = 0; i < G_N_ELEMENTS (constants); i++) {
            e = find_ims_entry (entries, n, constants[i].tag, NULL);
            g_assert_nonnull (e);
            g_assert_cmpuint (e->int_value, ==, constants[i].value);
        }
    }
}

/* A database that is not there is a packaging fault, and reporting it as one
 * beats registering with whatever another carrier's timers happen to be. */
static void
test_ims_config_missing_database (void)
{
    g_autoptr(MMSitImsConfig) config = NULL;
    g_autoptr(GError)         error = NULL;

    config = mm_sit_ims_config_resolve ("/nonexistent/ims.db", &test_device,
                                        &(MMSitCarrierIdentity){ .mccmnc = "26203" }, &error);
    g_assert_null (config);
    g_assert_nonnull (error);
}

/*
 * The override has to replace the default, not sit after it. Two entries for
 * one tag would still work -- the CP takes the last -- and that is exactly why
 * it is worth asserting: it would work by accident.
 */
static void
test_ims_config_carrier_overrides_replace (void)
{
    g_autoptr(MMSitImsConfig)     config = NULL;
    const SamsungAimsConfigEntry *entries;
    const SamsungAimsConfigEntry *e;
    gsize                         n = 0;
    guint                         count = 0;

    g_autofree gchar *path = build_test_ims_db ();

    config = mm_sit_ims_config_resolve (path, &test_device, &(MMSitCarrierIdentity){ .mccmnc = "26203" }, NULL);
    g_assert_cmpstr (mm_sit_ims_config_get_carrier (config), ==, "o2_de");
    entries = mm_sit_ims_config_peek_entries (config, &n);

    e = find_ims_entry (entries, n, 1028, &count);
    g_assert_cmpuint (count, ==, 1);
    g_assert_cmpuint (e->int_value, ==, 1300);

    /* Timer F: 32 s here where AOSP says 128 s. */
    e = find_ims_entry (entries, n, 528, &count);
    g_assert_cmpuint (count, ==, 1);
    g_assert_cmpuint (e->int_value, ==, 32000);

    /* A string override, and a bytes one. */
    e = find_ims_entry (entries, n, 1032, &count);
    g_assert_cmpuint (count, ==, 1);
    g_assert_cmpint (e->value_type, ==, SAMSUNG_AIMS_VALUE_TYPE_STR);
    g_assert_cmpstr (e->str_value, ==, "teleconferenceuri-factory@telefonica.de");

    e = find_ims_entry (entries, n, 1060, &count); /* AMR-WB description */
    g_assert_cmpuint (count, ==, 1);
    g_assert_cmpint (e->value_type, ==, SAMSUNG_AIMS_VALUE_TYPE_BYTES);
    g_assert_cmpuint (e->bytes_len, ==, 4);
    g_assert_cmpuint (e->bytes_value[0], ==, 0x68);

    /* A default the carrier does not override stays. */
    e = find_ims_entry (entries, n, 10, &count);
    g_assert_cmpuint (count, ==, 1);
    g_assert_cmpuint (e->int_value, ==, 5060);
}

/*
 * Several carriers share an MCCMNC and are told apart only by SPN, GID or an
 * IMSI prefix. Every field a record specifies has to agree with the SIM, and
 * the most specific surviving record wins -- so a record asking for nothing
 * beyond the MCCMNC is the fallback for it, not a competitor.
 */
static void
test_ims_config_identity_discriminators (void)
{
    g_autofree gchar         *path = build_test_ims_db ();
    g_autoptr(MMSitImsConfig) generic = NULL;
    g_autoptr(MMSitImsConfig) specific = NULL;

    generic = mm_sit_ims_config_resolve (
        path, &test_device, &(MMSitCarrierIdentity){ .mccmnc = "26203" }, NULL);
    g_assert_cmpstr (mm_sit_ims_config_get_carrier (generic), ==, "o2_de");

    specific = mm_sit_ims_config_resolve (
        path, &test_device,
        &(MMSitCarrierIdentity){ .mccmnc = "26203", .gid1 = "ABCD" }, NULL);
    g_assert_cmpstr (mm_sit_ims_config_get_carrier (specific), ==, "mvno_de");
}

/*
 * The User-Agent is the one value the CP cannot invent and the one a network is
 * most likely to reject us over, so both the assembled string and the parts it
 * came from have to be present.
 */
static void
test_ims_config_user_agent (void)
{
    g_autoptr(MMSitImsConfig)     config = NULL;
    g_autoptr(MMSitImsConfig)     plain = NULL;
    const SamsungAimsConfigEntry *entries;
    gsize                         n = 0;

    g_autofree gchar *path = build_test_ims_db ();

    config = mm_sit_ims_config_resolve (path, &test_device, &(MMSitCarrierIdentity){ .mccmnc = "26203" }, NULL);
    g_assert_cmpstr (mm_sit_ims_config_get_user_agent (config), ==,
                     "Google Pixel9a Android 17 CP2A.260705.006 EU01454");

    entries = mm_sit_ims_config_peek_entries (config, &n);
    {
        static const struct {
            guint16      tag;
            const gchar *value;
        } expected[] = {
            { 519, "Google Pixel9a Android 17 CP2A.260705.006 EU01454" },
            { 772, "Google" },
            { 773, "Pixel9a" },
            { 776, "17" },
        };
        guint i;

        for (i = 0; i < G_N_ELEMENTS (expected); i++) {
            const SamsungAimsConfigEntry *e;

            e = find_ims_entry (entries, n, expected[i].tag, NULL);
            g_assert_nonnull (e);
            g_assert_cmpstr (e->str_value, ==, expected[i].value);
        }
    }

    /* Without a carrier there is no certification suffix to append, and no
     * trailing space where one would have gone. */
    plain = mm_sit_ims_config_resolve (path, &test_device, NULL, NULL);
    g_assert_cmpstr (mm_sit_ims_config_get_user_agent (plain), ==,
                     "Google Pixel9a Android 17 CP2A.260705.006");
}

int
main (int argc,
      char **argv)
{
    g_test_init (&argc, &argv, NULL);

    /*
     * Without this, mm_obj_warn() and mm_obj_err() jump to a NULL
     * log_backend: it is a static function pointer that only mm_log_setup()
     * ever assigns, and _mm_log() filters debug messages out before reaching
     * it but not warnings. Any code under test that warns therefore crashes
     * the binary rather than printing, which is how a warning in the SIT
     * timeout handler became a SIGSEGV in dsds/secondary-connect -- the one
     * test that lets a command time out.
     */
    {
        g_autoptr(GError) error = NULL;

        if (!mm_log_setup ("ERR", NULL, FALSE, FALSE, FALSE, FALSE, &error))
            g_error ("cannot set up logging: %s", error->message);
    }

    g_test_add_func ("/MM/sit/parse/header", test_parse_header);
    g_test_add_func ("/MM/sit/parse/invalid", test_parse_invalid);
    g_test_add_func ("/MM/sit/message-id/names", test_message_id_names);
    g_test_add_func ("/MM/sit/build/radio", test_build_radio);
    g_test_add_func ("/MM/sit/build/slot-mapping", test_build_slot_mapping);
    g_test_add_func ("/MM/sit/build/data", test_build_data_requests);
    g_test_add_func ("/MM/sit/build/apn-auth", test_apn_auth_on_the_wire);
    g_test_add_func ("/MM/sit/apn-auth-from-properties", test_apn_auth_from_properties);
    g_test_add_func ("/MM/sit/build/config", test_build_config_requests);
    g_test_add_func ("/MM/sit/build/boot-config", test_build_boot_config_requests);
    g_test_add_func ("/MM/sit/decode/radio", test_parse_radio);
    g_test_add_func ("/MM/sit/decode/sim-registration", test_parse_status_and_registration);
    g_test_add_func ("/MM/sit/decode/sim-slot-status", test_parse_sim_slot_status);
    g_test_add_func ("/MM/sit/decode/signal", test_parse_signal);
    g_test_add_func ("/MM/sit/decode/nitz-time", test_parse_nitz_time);
    g_test_add_func ("/MM/sit/decode/nitz-time-indication", test_parse_nitz_time_indication);
    g_test_add_func ("/MM/sit/decode/cell-info-lte-v5", test_parse_cell_info_lte_v5);
    g_test_add_func ("/MM/sit/decode/data-call", test_parse_data_call);
    g_test_add_func ("/MM/sit/decode/data-call-list", test_parse_data_call_list);
    g_test_add_func ("/MM/sit/build/call-requests", test_build_call_requests);
    g_test_add_func ("/MM/sit/decode/call-list", test_parse_call_list);
    g_test_add_func ("/MM/sit/decode/carrier-sim-info", test_parse_carrier_sim_info);
    g_test_add_func ("/MM/sit/carrier-config/resolve", test_carrier_config_resolver);
    g_test_add_func ("/MM/sit/carrier-config/reference-database", test_carrier_config_reference_database);
    g_test_add_func ("/MM/sit/port/command-and-indication", test_port_command_and_indication);
    g_test_add_func ("/MM/sit/port/stale-response", test_port_drops_a_stale_response);
    g_test_add_func ("/MM/sit/port/queued-commands", test_port_answers_queued_commands);
    g_test_add_func ("/MM/sit/client/connect-sequence", test_client_connect_sequence);
    g_test_add_func ("/MM/sit/client/sim-cache-refresh", test_client_sim_cache_refresh);
    g_test_add_func ("/MM/sit/ims/config-defaults", test_ims_config_defaults);
    g_test_add_func ("/MM/sit/ims/config-missing-database", test_ims_config_missing_database);
    g_test_add_func ("/MM/sit/ims/config-identity", test_ims_config_identity_discriminators);
    g_test_add_func ("/MM/sit/ims/config-carrier-overrides", test_ims_config_carrier_overrides_replace);
    g_test_add_func ("/MM/sit/ims/config-user-agent", test_ims_config_user_agent);
    g_test_add_data_func ("/MM/sit/ims/provisioning-retry-limit", NULL, test_ims_provisioning_retries);
    g_test_add_data_func ("/MM/sit/ims/provisioning-retry-recovery", GUINT_TO_POINTER (1), test_ims_provisioning_retries);
    g_test_add_data_func ("/MM/sit/ims/vops-gating", GUINT_TO_POINTER (2), test_ims_provisioning_retries);
    g_test_add_data_func ("/MM/sit/client/radio-transition", NULL, test_client_radio_transition);
    g_test_add_data_func ("/MM/sit/client/same-profile-after-empty", GUINT_TO_POINTER (1), test_client_radio_transition);
    g_test_add_func ("/MM/sit/client/radio-transition-refused", test_client_radio_transition_refused);
    g_test_add_func ("/MM/sit/client/boot-config", test_client_boot_config);
    g_test_add_func ("/MM/sit/dsds/data-ownership", test_dsds_data_ownership);
    g_test_add_data_func ("/MM/sit/dsds/prepare-coalesced", NULL, test_cp_prepare_coalesced);
    g_test_add_data_func ("/MM/sit/dsds/prepare-add-mapping", GUINT_TO_POINTER (1), test_cp_prepare_coalesced);
    g_test_add_data_func ("/MM/sit/dsds/prepare-automatic-mapping", GUINT_TO_POINTER (2), test_cp_prepare_coalesced);
    g_test_add_data_func ("/MM/sit/dsds/prepare-empty", GUINT_TO_POINTER (3), test_cp_prepare_coalesced);
    g_test_add_data_func ("/MM/sit/dsds/prepare-empty-add-mapping", GUINT_TO_POINTER (4), test_cp_prepare_coalesced);
    g_test_add_data_func ("/MM/sit/dsds/prepare-empty-automatic-mapping", GUINT_TO_POINTER (5), test_cp_prepare_coalesced);
    g_test_add_func ("/MM/sit/client/camped-is-unregistered", test_registration_camped);
    g_test_add_func ("/MM/sit/dsds/secondary-connect", test_client_dsds_secondary);
    g_test_add_func ("/MM/sit/decode/registration-failed", test_parse_registration_failed);

    return g_test_run ();
}
