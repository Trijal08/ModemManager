/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <errno.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mm-log.h"
#include "mm-sit-client.h"

static gchar    *opt_device;
static gboolean  opt_verbose;
static gboolean  opt_dump_payload;
static gboolean  opt_hold;
static gchar    *opt_configure;
static gchar    *opt_model;
static gchar    *opt_build_id;
static gchar    *opt_codename;
static gchar    *opt_carrier_directory;
static gchar    *opt_carrier_sha1;
static gboolean  opt_no_proxy;
static gint      opt_indication_filter = 0x7f;

static GOptionEntry entries[] = {
    { "device", 'd', 0, G_OPTION_ARG_FILENAME, &opt_device,
      "SIT device (default: first /dev/wwan*sit*, then /dev/umts_ipc0)", "PATH" },
    { "no-proxy", 0, 0, G_OPTION_ARG_NONE, &opt_no_proxy,
      "Open the device node directly instead of going through sit-proxy "
      "(requires stopping ModemManager first)", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose,
      "Enable transport debug logging (payloads stay redacted)", NULL },
    { "dump-payload", 0, 0, G_OPTION_ARG_NONE, &opt_dump_payload,
      "Print full payloads as hex (may expose subscriber data)", NULL },
    { "hold", 'H', 0, G_OPTION_ARG_NONE, &opt_hold,
      "Keep the control channel open and monitor after connect", NULL },
    { "configure", 0, 0, G_OPTION_ARG_STRING, &opt_configure,
      "Configure this raw-IP interface after connect", "INTERFACE" },
    { "model", 0, 0, G_OPTION_ARG_STRING, &opt_model,
      "Device model for the initialize command", "MODEL" },
    { "codename", 0, 0, G_OPTION_ARG_STRING, &opt_codename,
      "Board codename for the initialize command", "NAME" },
    { "build-id", 0, 0, G_OPTION_ARG_STRING, &opt_build_id,
      "Build ID for the initialize command", "BUILD" },
    { "carrier-directory", 0, 0, G_OPTION_ARG_FILENAME, &opt_carrier_directory,
      "Carrier-config directory for initialize", "PATH" },
    { "carrier-sha1", 0, 0, G_OPTION_ARG_STRING, &opt_carrier_sha1,
      "20-byte carrier manifest SHA1 for initialize", "HEX" },
    { "indication-filter", 0, 0, G_OPTION_ARG_INT, &opt_indication_filter,
      "Unsolicited indication mask for initialize (default: 127)", "MASK" },
    { NULL }
};

typedef struct {
    GMainLoop         *loop;
    GError            *error;
    gboolean           boolean_result;
    MMSitRadioState    radio_state;
    MMSitSimStatus     sim_status;
    MMSitSimSlotStatus sim_slot_status;
    MMSitRegistration  registration;
    MMSitOperator      operator_info;
    MMSitDataCall      data_call;
    MMSitMessage      *message;
} WaitContext;

static gint
compare_strings (gconstpointer a,
                 gconstpointer b)
{
    return g_strcmp0 (*(gchar * const *) a, *(gchar * const *) b);
}

static gchar *
find_default_device (void)
{
    g_autoptr(GDir) dir = NULL;
    g_autoptr(GPtrArray) candidates = NULL;
    const gchar *name;

    dir = g_dir_open ("/dev", 0, NULL);
    if (dir) {
        candidates = g_ptr_array_new_with_free_func (g_free);
        while ((name = g_dir_read_name (dir))) {
            if (g_str_has_prefix (name, "wwan") && strstr (name, "sit"))
                g_ptr_array_add (candidates, g_strdup (name));
        }
        if (candidates->len) {
            g_ptr_array_sort (candidates, compare_strings);
            return g_build_filename ("/dev", g_ptr_array_index (candidates, 0), NULL);
        }
    }

    if (g_file_test ("/dev/umts_ipc0", G_FILE_TEST_EXISTS))
        return g_strdup ("/dev/umts_ipc0");
    return NULL;
}

static GByteArray *
parse_hex (const gchar  *text,
           GError      **error)
{
    g_autoptr(GByteArray) bytes = NULL;
    gint high_nibble = -1;
    const gchar *p;

    bytes = g_byte_array_new ();
    for (p = text; p && *p; p++) {
        gint value;

        if (g_ascii_isspace (*p) || *p == ':' || *p == '-')
            continue;
        value = g_ascii_xdigit_value (*p);
        if (value < 0) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "Invalid hexadecimal character '%c'", *p);
            return NULL;
        }
        if (high_nibble < 0)
            high_nibble = value;
        else {
            guint8 byte = (high_nibble << 4) | value;

            g_byte_array_append (bytes, &byte, 1);
            high_nibble = -1;
        }
    }
    if (high_nibble >= 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Hexadecimal input contains an incomplete byte");
        return NULL;
    }
    return g_steal_pointer (&bytes);
}

static gboolean
parse_uint64 (const gchar  *text,
              guint64       maximum,
              guint64      *out_value,
              GError      **error)
{
    gchar  *end = NULL;
    guint64 value;

    errno = 0;
    value = g_ascii_strtoull (text, &end, 0);
    if (errno || !end || end == text || *end || value > maximum) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Invalid numeric value '%s'", text);
        return FALSE;
    }
    *out_value = value;
    return TRUE;
}

static void
command_ready (MMSitClient *client,
               GAsyncResult *res,
               WaitContext *ctx)
{
    ctx->message = mm_sit_client_command_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static MMSitMessage *
command_sync (MMSitClient *client,
              GByteArray  *request,
              guint32      timeout,
              GError     **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_command (client, request, timeout, NULL,
                           (GAsyncReadyCallback) command_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.message) {
        g_propagate_error (error, ctx.error);
        return NULL;
    }
    return ctx.message;
}

static gboolean
response_success (const MMSitMessage *message,
                  GError            **error)
{
    if (!message->has_status || message->status != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "%s returned status %u",
                     mm_sit_message_id_get_string (message->id),
                     message->has_status ? message->status : G_MAXUINT16);
        return FALSE;
    }
    return TRUE;
}

static gboolean
run_request (MMSitClient *client,
             GByteArray  *request,
             guint32      timeout,
             GError     **error)
{
    g_autoptr(MMSitMessage) response = NULL;

    response = command_sync (client, request, timeout, error);
    return response && response_success (response, error);
}

static void
get_radio_ready (MMSitClient *client,
                 GAsyncResult *res,
                 WaitContext *ctx)
{
    ctx->radio_state = mm_sit_client_get_radio_state_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
get_radio_sync (MMSitClient     *client,
                MMSitRadioState *out_state,
                GError         **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_get_radio_state (client, NULL,
                                   (GAsyncReadyCallback) get_radio_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (ctx.error) {
        g_propagate_error (error, ctx.error);
        return FALSE;
    }
    *out_state = ctx.radio_state;
    return TRUE;
}

static void
set_radio_ready (MMSitClient *client,
                 GAsyncResult *res,
                 WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_set_radio_state_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
set_radio_sync (MMSitClient     *client,
                MMSitRadioState  state,
                GError         **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_set_radio_state (client, state, NULL,
                                   (GAsyncReadyCallback) set_radio_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result)
        g_propagate_error (error, ctx.error);
    return ctx.boolean_result;
}

static void
get_sim_ready (MMSitClient *client,
               GAsyncResult *res,
               WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_get_sim_status_finish (client, res,
                                                               &ctx->sim_status,
                                                               &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
get_sim_sync (MMSitClient    *client,
              MMSitSimStatus *out_status,
              GError        **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_get_sim_status (client, NULL,
                                  (GAsyncReadyCallback) get_sim_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result) {
        g_propagate_error (error, ctx.error);
        return FALSE;
    }
    *out_status = ctx.sim_status;
    return TRUE;
}

static void
get_sim_slot_status_ready (MMSitClient *client,
                           GAsyncResult *res,
                           WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_get_sim_slot_status_finish (client, res,
                                                                    &ctx->sim_slot_status,
                                                                    &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
get_sim_slot_status_sync (MMSitClient        *client,
                          MMSitSimSlotStatus *out_status,
                          GError            **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_get_sim_slot_status (client, NULL,
                                       (GAsyncReadyCallback) get_sim_slot_status_ready,
                                       &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result) {
        g_propagate_error (error, ctx.error);
        return FALSE;
    }
    *out_status = ctx.sim_slot_status;
    return TRUE;
}

static void
set_sim_slot_mapping_ready (MMSitClient *client,
                            GAsyncResult *res,
                            WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_set_logical_slot_mapping_finish (client, res,
                                                                          &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
set_sim_slot_mapping_sync (MMSitClient                  *client,
                           const MMSitLogicalSlotMapping *mappings,
                           guint                         mapping_count,
                           GError                      **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_set_logical_slot_mapping (client, mappings, mapping_count, NULL,
                                            (GAsyncReadyCallback) set_sim_slot_mapping_ready,
                                            &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result)
        g_propagate_error (error, ctx.error);
    return ctx.boolean_result;
}

static void
get_registration_ready (MMSitClient *client,
                        GAsyncResult *res,
                        WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_get_registration_finish (client, res,
                                                                  &ctx->registration,
                                                                  &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
get_registration_sync (MMSitClient       *client,
                       MMSitRegistration *out_registration,
                       GError           **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_get_registration (client, NULL,
                                    (GAsyncReadyCallback) get_registration_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result) {
        g_propagate_error (error, ctx.error);
        return FALSE;
    }
    *out_registration = ctx.registration;
    return TRUE;
}

static void
get_operator_ready (MMSitClient *client,
                    GAsyncResult *res,
                    WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_get_operator_finish (client, res,
                                                              &ctx->operator_info,
                                                              &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
get_operator_sync (MMSitClient   *client,
                   MMSitOperator *out_operator,
                   GError       **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_get_operator (client, NULL,
                                (GAsyncReadyCallback) get_operator_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result) {
        g_propagate_error (error, ctx.error);
        return FALSE;
    }
    *out_operator = ctx.operator_info;
    return TRUE;
}

static void
connect_ready (MMSitClient *client,
               GAsyncResult *res,
               WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_connect_data_finish (client, res,
                                                              &ctx->data_call,
                                                              &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
connect_sync (MMSitClient   *client,
              const gchar   *apn,
              MMSitDataCall *out_call,
              GError       **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_connect_data (client, apn, MM_SIT_PDP_TYPE_IPV4V6, MM_SIT_APN_TYPE_DEFAULT, NULL, NULL,
                                (GAsyncReadyCallback) connect_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result) {
        g_propagate_error (error, ctx.error);
        return FALSE;
    }
    *out_call = ctx.data_call;
    return TRUE;
}

static void
disconnect_ready (MMSitClient *client,
                  GAsyncResult *res,
                  WaitContext *ctx)
{
    ctx->boolean_result = mm_sit_client_disconnect_data_finish (client, res, &ctx->error);
    g_main_loop_quit (ctx->loop);
}

static gboolean
disconnect_sync (MMSitClient *client,
                 guint8       cid,
                 GError     **error)
{
    WaitContext ctx = { 0 };

    ctx.loop = g_main_loop_new (NULL, FALSE);
    mm_sit_client_disconnect_data (client, cid, NULL,
                                   (GAsyncReadyCallback) disconnect_ready, &ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);
    if (!ctx.boolean_result)
        g_propagate_error (error, ctx.error);
    return ctx.boolean_result;
}

static void
print_registration (const MMSitRegistration *registration)
{
    g_print ("packet registration: %s (state=%u rat=%u)\n",
             registration->registered ? "registered" : "not registered",
             registration->state,
             registration->radio_technology);
}

static void
print_operator (const MMSitOperator *operator_info)
{
    g_print ("operator: PLMN=%s short=\"%s\" long=\"%s\"\n",
             operator_info->plmn[0] ? operator_info->plmn : "unknown",
             operator_info->operator_short,
             operator_info->operator_long);
}

static void
print_sim_slot_status (const MMSitSimSlotStatus *status)
{
    guint i;

    g_print ("physical SIM slots: %u (%s layout)\n",
             status->slot_count, status->legacy ? "legacy" : "port-aware");
    for (i = 0; i < status->slot_count; i++) {
        const MMSitSimSlot *slot = &status->slots[i];
        guint               j;

        g_print ("  slot %u: card-state=%u ATR=%u bytes EID=%s ports=%u\n",
                 i + 1, slot->card_state, slot->atr_len,
                 slot->eid_len ? "present" : "none", slot->port_count);
        for (j = 0; j < slot->port_count; j++) {
            const MMSitSimPort *port = &slot->ports[j];

            g_print ("    port %u: ICCID=%s logical-stack=",
                     j, port->iccid_len ? "present" : "none");
            if (port->logical_slot_id == 0xff)
                g_print ("unmapped");
            else
                g_print ("%u", port->logical_slot_id);
            g_print (" state=%u\n", port->state);
        }
    }
}

static void
print_payload (const MMSitMessage *message)
{
    const guint8 *payload;
    gsize len;
    gsize offset;

    payload = mm_sit_message_get_payload (message, &len);
    g_print ("  payload: %" G_GSIZE_FORMAT " bytes (may contain sensitive data)\n", len);
    for (offset = 0; offset < len; offset += 16) {
        gsize i;

        g_print ("    %04" G_GSIZE_FORMAT ":", offset);
        for (i = offset; i < MIN (offset + 16, len); i++)
            g_print (" %02x", payload[i]);
        g_print ("\n");
    }
}

static void
print_message (const MMSitMessage *message)
{
    g_print ("%s type=%u id=0x%04x token=0x%04x length=%u",
             mm_sit_message_id_get_string (message->id),
             message->type, message->id, message->token, message->length);
    if (message->has_status && message->id != MM_SIT_MESSAGE_GET_CELL_INFO_LIST)
        g_print (" status=%u", message->status);
    g_print ("\n");

    if (opt_dump_payload)
        print_payload (message);

    if (message->id == MM_SIT_MESSAGE_RADIO_STATE_CHANGED ||
        message->id == MM_SIT_MESSAGE_GET_RADIO_POWER ||
        message->id == MM_SIT_MESSAGE_SET_RADIO_POWER) {
        MMSitRadioState state;

        if (mm_sit_message_parse_radio_state (message, &state, NULL))
            g_print ("  radio-state: %s\n",
                     state == MM_SIT_RADIO_STATE_ON ? "on" :
                     state == MM_SIT_RADIO_STATE_OFF ? "off" : "unknown");
    } else if (message->id == MM_SIT_MESSAGE_GET_DATA_REGISTRATION) {
        MMSitRegistration registration;

        if (mm_sit_message_parse_registration (message, &registration, NULL))
            print_registration (&registration);
    } else if (message->id == MM_SIT_MESSAGE_GET_OPERATOR ||
               message->id == MM_SIT_MESSAGE_SERVING_NETWORK) {
        MMSitOperator operator_info;

        if (mm_sit_message_parse_operator (message, &operator_info, NULL))
            print_operator (&operator_info);
    } else if (message->id == MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS ||
               message->id == MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED) {
        MMSitSimSlotStatus status;

        if (mm_sit_message_parse_sim_slot_status (message, &status, NULL))
            print_sim_slot_status (&status);
    } else if (message->id == MM_SIT_MESSAGE_GET_CELL_INFO_LIST) {
        g_autoptr(GArray) cells = NULL;

        if (mm_sit_message_parse_cell_info_list (message, &cells, NULL)) {
            guint i;

            g_print ("  LTE cells: %u\n", cells->len);
            for (i = 0; i < cells->len; i++) {
                const MMSitCellInfoLte *cell = &g_array_index (cells, MMSitCellInfoLte, i);

                g_print ("  [%u]", i);
                if (cell->operator_id[0])
                    g_print (" PLMN=%s", cell->operator_id);
                if (cell->ci != G_MAXINT32)
                    g_print (" ECI=%u (0x%08x) eNodeB=%u sector=%u",
                             cell->ci, cell->ci, cell->ci >> 8, cell->ci & 0xff);
                if (cell->tac != G_MAXINT32)
                    g_print (" TAC=%u", cell->tac);
                if (cell->physical_ci != G_MAXINT32)
                    g_print (" PCI=%u", cell->physical_ci);
                if (cell->earfcn != G_MAXINT32)
                    g_print (" EARFCN=%u", cell->earfcn);
                if (cell->bandwidth != G_MAXINT32)
                    g_print (" bandwidth=%u kHz", cell->bandwidth);
                if (cell->rsrp >= 44 && cell->rsrp <= 140)
                    g_print (" RSRP=-%d dBm", cell->rsrp);
                if (cell->rsrq >= 3 && cell->rsrq <= 34)
                    g_print (" RSRQ=-%d dB", cell->rsrq);
                if (cell->rssnr >= -200 && cell->rssnr <= 300)
                    g_print (" SNR=%.1f dB", cell->rssnr / 10.0);
                if (cell->registered &&
                    cell->timing_advance >= 0 && cell->timing_advance < G_MAXINT32)
                    g_print (" TA=%d", cell->timing_advance);
                if (cell->registered)
                    g_print (" serving");
                g_print ("\n");
            }
        }
    } else if (message->id == MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED ||
               message->id == MM_SIT_MESSAGE_SETUP_DATA_CALL) {
        MMSitDataCall call;

        if (mm_sit_message_parse_data_call (message, &call, NULL))
            g_print ("  data-call: cid=%u active=%u cause=%u%s%s\n",
                     call.cid, call.active, call.cause,
                     call.has_ipv4 ? " IPv4=" : "",
                     call.ipv4);
    }
}

static void
indication_received (MMSitClient        *client,
                     const MMSitMessage *message,
                     gpointer            user_data)
{
    print_message (message);
}

static gboolean
stop_loop (GMainLoop *loop)
{
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

static void
monitor (MMSitClient *client)
{
    g_autoptr(GMainLoop) loop = NULL;
    guint sigint_id;
    guint sigterm_id;

    loop = g_main_loop_new (NULL, FALSE);
    sigint_id = g_unix_signal_add (SIGINT, (GSourceFunc) stop_loop, loop);
    sigterm_id = g_unix_signal_add (SIGTERM, (GSourceFunc) stop_loop, loop);
    g_print ("Monitoring redacted SIT events on %s; press Ctrl-C to stop\n",
             mm_sit_client_get_device (client));
    g_main_loop_run (loop);
    g_source_remove (sigint_id);
    g_source_remove (sigterm_id);
}

static gboolean
run_ip (const gchar *const *argv,
        GError            **error)
{
    g_autoptr(GSubprocess) process = NULL;

    process = g_subprocess_newv (argv,
                                 G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                 G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                 error);
    if (!process)
        return FALSE;
    if (!g_subprocess_wait_check (process, NULL, error))
        return FALSE;
    return TRUE;
}

static gboolean
configure_interface (const gchar         *interface,
                     const MMSitDataCall *call,
                     GError             **error)
{
    g_autofree gchar *address = NULL;
    g_autofree gchar *mtu = NULL;
    const gchar *flush[] = { "ip", "address", "flush", "dev", interface, NULL };
    const gchar *add[] = { "ip", "address", "add", NULL, "dev", interface, NULL };
    const gchar *link[] = { "ip", "link", "set", "dev", interface, "mtu", NULL, "up", NULL };
    const gchar *route[] = { "ip", "route", "replace", "default", "dev", interface, NULL };

    if (!call->has_ipv4) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "Cannot configure an interface without an IPv4 address");
        return FALSE;
    }

    address = g_strdup_printf ("%s/32", call->ipv4);
    mtu = g_strdup_printf ("%u", call->mtu >= 576 && call->mtu <= 2000 ? call->mtu : 1500);
    add[3] = address;
    link[7] = mtu;

    return run_ip (flush, error) && run_ip (add, error) &&
           run_ip (link, error) && run_ip (route, error);
}

static gboolean
run_initialize (MMSitClient *client,
                GError     **error)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GByteArray) sha1 = NULL;
    guint8 oem_body[6] = { 0, 0, 0, 0, 0x33, 0x33 };
    guint8 modem_config[5] = { 0 };

    if (opt_model || opt_build_id) {
        if (!opt_model || !opt_build_id) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "--model and --build-id must be provided together");
            return FALSE;
        }
        g_clear_pointer (&request, g_byte_array_unref);
        request = mm_sit_request_new_device_info (1, opt_model, opt_build_id,
                                                 opt_codename ?: "", error);
        if (!request || !run_request (client, request, 5, error))
            return FALSE;
    }

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new (MM_SIT_MESSAGE_OEM_CONFIG, 2,
                                  oem_body, sizeof (oem_body), error);
    if (!request || !run_request (client, request, 5, error))
        return FALSE;
    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new (MM_SIT_MESSAGE_SET_MODEM_CONFIG, 3,
                                  modem_config, sizeof (modem_config), error);
    if (!request || !run_request (client, request, 5, error))
        return FALSE;

    if (opt_carrier_directory || opt_carrier_sha1) {
        if (!opt_carrier_directory || !opt_carrier_sha1) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "--carrier-directory and --carrier-sha1 must be provided together");
            return FALSE;
        }
        sha1 = parse_hex (opt_carrier_sha1, error);
        if (!sha1 || sha1->len != MM_SIT_SHA1_LENGTH) {
            if (sha1)
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                     "Carrier SHA1 must contain exactly 20 bytes");
            return FALSE;
        }
        g_clear_pointer (&request, g_byte_array_unref);
        request = mm_sit_request_new_carrier_manifest (4,
                                                       opt_carrier_directory,
                                                       sha1->data,
                                                       error);
        if (!request || !run_request (client, request, 10, error))
            return FALSE;
    }

    g_clear_pointer (&request, g_byte_array_unref);
    request = mm_sit_request_new_set_indication_filter (5, (guint32) opt_indication_filter);
    return run_request (client, request, 5, error);
}

static gboolean
message_name_matches (const gchar *known_names,
                      const gchar *requested_name)
{
    g_auto(GStrv) aliases = NULL;
    const gchar *requested = requested_name;
    guint i;

    if (g_ascii_strncasecmp (requested, "SIT_", 4) == 0)
        requested += 4;
    if (g_ascii_strcasecmp (known_names, requested) == 0)
        return TRUE;

    aliases = g_strsplit (known_names, " / ", -1);
    for (i = 0; aliases[i]; i++) {
        if (g_ascii_strcasecmp (aliases[i], requested) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
parse_message_id (const gchar  *text,
                  guint16      *out_id,
                  GError      **error)
{
    guint64 numeric_id;
    guint matched_id = G_MAXUINT;
    guint id;

    if (g_ascii_isdigit (text[0])) {
        if (!parse_uint64 (text, G_MAXUINT16, &numeric_id, error))
            return FALSE;
        *out_id = (guint16) numeric_id;
        return TRUE;
    }

    for (id = 0; id <= G_MAXUINT16; id++) {
        const gchar *known_names = mm_sit_message_id_get_string ((guint16) id);

        if (g_str_equal (known_names, "UNKNOWN") ||
            !message_name_matches (known_names, text))
            continue;
        if (matched_id != G_MAXUINT && matched_id != id) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "Message name '%s' is ambiguous (0x%04x and 0x%04x); use a numeric ID",
                         text, matched_id, id);
            return FALSE;
        }
        matched_id = id;
    }

    if (matched_id == G_MAXUINT) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Unknown SIT message name '%s'; use 'ids' to list known names", text);
        return FALSE;
    }
    *out_id = (guint16) matched_id;
    return TRUE;
}

static void
print_message_ids (const gchar *filter)
{
    g_autofree gchar *upper_filter = filter ? g_ascii_strup (filter, -1) : NULL;
    guint id;

    for (id = 0; id <= G_MAXUINT16; id++) {
        const gchar *name = mm_sit_message_id_get_string ((guint16) id);

        if (g_str_equal (name, "UNKNOWN"))
            continue;
        if (upper_filter) {
            g_autofree gchar *upper_name = g_ascii_strup (name, -1);

            if (!strstr (upper_name, upper_filter))
                continue;
        }
        g_print ("0x%04x  %s\n", id, name);
    }
}

/* Also handed to GOptionContext so it shows up under --help, which used to
 * print the options and no commands at all. */
static const gchar *command_summary =
    "Commands:\n"
    "  status                        modem, SIM, registration and signal overview\n"
    "  registration                  CS and PS registration state\n"
    "  operator                      serving operator\n"
    "  slots                         SIM slot status\n"
    "  slot-map PHYSICAL_SLOT [PORT] map a logical stack onto a slot\n"
    "  radio on|off                  set radio power\n"
    "  connect APN                   set up a data call\n"
    "  disconnect [CID]              tear one down\n"
    "  initialize                    send the boot-config batch\n"
    "  monitor                       print unsolicited indications\n"
    "  query MESSAGE_ID              bare 12-byte query, no payload\n"
    "  payload MESSAGE_ID [HEX]      request carrying HEX as the payload at @12\n"
    "  raw MESSAGE_ID [HEX]          request with HEX written from @8 (expert)\n"
    "  ids [FILTER]                  list known message ids\n"
    "\n"
    "Frame offsets: a request is an 8-byte header, a 4-byte reserved word at @8,\n"
    "then the payload at @12.  Every layout in docs/subsystems/modem/reference/sit-protocol.md is quoted\n"
    "as a frame offset, so 'payload' is almost always the one you want.  'raw'\n"
    "writes from @8, so it puts your first four bytes in the reserved word and\n"
    "the CP answers a request for nothing -- a plausible wrong answer, not an\n"
    "error.  'payload 0x901 ff' and 'raw 0x901 00000000ff' are the same frame.\n";

static void
print_usage (const gchar *program)
{
    g_printerr ("Usage:\n  %s [OPTIONS] COMMAND\n\n%s", program, command_summary);
}

int
main (int argc,
      char **argv)
{
    g_autoptr(GOptionContext) option_context = NULL;
    g_autoptr(MMSitClient) client = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *device = NULL;
    const gchar *command;
    gboolean success = FALSE;

    option_context = g_option_context_new ("COMMAND - Samsung SIT diagnostic client");
    g_option_context_set_description (option_context, command_summary);
    g_option_context_add_main_entries (option_context, entries, NULL);
    if (!g_option_context_parse (option_context, &argc, &argv, &error)) {
        g_printerr ("Option error: %s\n", error->message);
        return EXIT_FAILURE;
    }
    if (argc < 2) {
        print_usage (argv[0]);
        return EXIT_FAILURE;
    }
    if (opt_indication_filter < 0) {
        g_printerr ("Option error: indication filter must be non-negative\n");
        return EXIT_FAILURE;
    }
    command = argv[1];
    if (g_str_equal (command, "ids")) {
        if (argc > 3) {
            g_printerr ("SIT command failed: ids accepts at most one filter\n");
            return EXIT_FAILURE;
        }
        print_message_ids (argc == 3 ? argv[2] : NULL);
        return EXIT_SUCCESS;
    }

    if (!mm_log_setup (opt_verbose ? "DEBUG" : "WARN", NULL, FALSE,
                       FALSE, FALSE, FALSE, &error)) {
        g_printerr ("Logging setup failed: %s\n", error->message);
        return EXIT_FAILURE;
    }

    device = opt_device ? g_strdup (opt_device) : find_default_device ();
    if (!device) {
        g_printerr ("No SIT device found; use --device=/dev/wwan0sit0\n");
        mm_log_shutdown ();
        return EXIT_FAILURE;
    }

    /* Prefer sit-proxy when it is there. The device node has a single useful
     * owner -- concurrent readers race one shared rxq and steal each other's
     * frames -- so without the proxy this tool needs ModemManager stopped,
     * which has always been the tax on every debugging session. */
    if (!opt_no_proxy) {
        g_autofree gchar *socket_path = mm_sit_proxy_socket_path (device);

        if (g_file_test (socket_path, G_FILE_TEST_EXISTS)) {
            if (opt_verbose)
                g_printerr ("Using sit-proxy at %s\n", socket_path);
            client = mm_sit_client_new_proxy (device);
        }
    }
    if (!client)
        client = mm_sit_client_new (device);

    if (!mm_sit_client_open (client, &error)) {
        g_printerr ("Cannot open %s: %s\n", device, error->message);
        mm_log_shutdown ();
        return EXIT_FAILURE;
    }
    if (g_str_equal (command, "monitor") || opt_hold)
        g_signal_connect (client, "indication", G_CALLBACK (indication_received), NULL);

    if (g_str_equal (command, "status")) {
        MMSitRadioState radio;
        MMSitSimStatus sim;
        MMSitRegistration registration;
        MMSitOperator operator_info;

        if (argc != 2) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "status takes no arguments");
            goto out;
        }
        if (!get_radio_sync (client, &radio, &error))
            goto out;
        g_print ("radio: %s\n", radio == MM_SIT_RADIO_STATE_ON ? "on" :
                                 radio == MM_SIT_RADIO_STATE_OFF ? "off" : "unknown");
        if (!get_sim_sync (client, &sim, &error))
            goto out;
        g_print ("SIM: %s (card-state=%u apps=%u)\n",
                 sim.present ? "present" : "not present", sim.card_state, sim.app_count);
        if (!get_registration_sync (client, &registration, &error))
            goto out;
        print_registration (&registration);
        if (!get_operator_sync (client, &operator_info, &error))
            goto out;
        print_operator (&operator_info);
        success = TRUE;
    } else if (g_str_equal (command, "registration")) {
        MMSitRegistration registration;

        if (argc != 2) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "registration takes no arguments");
            goto out;
        }
        if (!get_registration_sync (client, &registration, &error))
            goto out;
        print_registration (&registration);
        success = TRUE;
    } else if (g_str_equal (command, "operator")) {
        MMSitOperator operator_info;

        if (argc != 2) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "operator takes no arguments");
            goto out;
        }
        if (!get_operator_sync (client, &operator_info, &error))
            goto out;
        print_operator (&operator_info);
        success = TRUE;
    } else if (g_str_equal (command, "slots")) {
        MMSitSimSlotStatus status;

        if (argc != 2) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "slots takes no arguments");
            goto out;
        }
        if (!get_sim_slot_status_sync (client, &status, &error))
            goto out;
        print_sim_slot_status (&status);
        success = TRUE;
    } else if (g_str_equal (command, "slot-map")) {
        MMSitLogicalSlotMapping mapping;
        guint64                 physical_slot;
        guint64                 port = 0;

        if (argc < 3 || argc > 4) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "slot-map requires a one-based physical slot and optional zero-based port");
            goto out;
        }
        if (!parse_uint64 (argv[2], MM_SIT_MAX_SIM_SLOTS, &physical_slot, &error) ||
            (argc == 4 && !parse_uint64 (argv[3], MM_SIT_MAX_SIM_PORTS - 1, &port, &error)))
            goto out;
        if (!physical_slot) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "Physical slot numbers start at 1");
            goto out;
        }
        mapping.physical_slot_id = physical_slot - 1;
        mapping.port_id = port;
        success = set_sim_slot_mapping_sync (client, &mapping, 1, &error);
        if (success)
            g_print ("logical stack 0 mapped to physical slot %" G_GUINT64_FORMAT
                     " port %" G_GUINT64_FORMAT "\n", physical_slot, port);
    } else if (g_str_equal (command, "radio")) {
        MMSitRadioState state;

        if (argc != 3 || (!g_str_equal (argv[2], "on") && !g_str_equal (argv[2], "off"))) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "radio requires 'on' or 'off'");
            goto out;
        }
        state = g_str_equal (argv[2], "on") ? MM_SIT_RADIO_STATE_ON : MM_SIT_RADIO_STATE_OFF;
        success = set_radio_sync (client, state, &error);
        if (success)
            g_print ("radio request accepted: %s\n", argv[2]);
    } else if (g_str_equal (command, "connect")) {
        MMSitDataCall call;

        if (argc != 3) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "connect requires an APN");
            goto out;
        }
        if (!connect_sync (client, argv[2], &call, &error))
            goto out;
        g_print ("data call: cid=%u active=%u IPv4=%s DNS=%s,%s MTU=%u\n",
                 call.cid, call.active, call.ipv4, call.dns1, call.dns2, call.mtu);
        if (opt_configure && !configure_interface (opt_configure, &call, &error))
            goto out;
        if (opt_configure)
            g_print ("configured %s (configure DNS separately: %s, %s)\n",
                     opt_configure, call.dns1, call.dns2);
        success = TRUE;
        if (opt_hold)
            monitor (client);
    } else if (g_str_equal (command, "disconnect")) {
        guint64 cid = 1;

        if (argc > 3) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "disconnect accepts at most one CID");
            goto out;
        }
        if (argc == 3 && !parse_uint64 (argv[2], G_MAXUINT8, &cid, &error))
            goto out;
        if (cid == 0) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "CID must be between 1 and 255");
            goto out;
        }
        success = disconnect_sync (client, cid, &error);
        if (success)
            g_print ("data call %" G_GUINT64_FORMAT " deactivated\n", cid);
    } else if (g_str_equal (command, "initialize")) {
        if (argc != 2) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "initialize takes no arguments");
            goto out;
        }
        success = run_initialize (client, &error);
        if (success)
            g_print ("SIT initialization sequence completed\n");
    } else if (g_str_equal (command, "monitor")) {
        if (argc != 2) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "monitor takes no arguments");
            goto out;
        }
        monitor (client);
        success = TRUE;
    } else if (g_str_equal (command, "query")) {
        g_autoptr(GByteArray) request = NULL;
        g_autoptr(MMSitMessage) response = NULL;
        guint16 id;

        if (argc != 3) {
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "query requires MESSAGE_ID");
            goto out;
        }
        if (!parse_message_id (argv[2], &id, &error))
            goto out;
        request = mm_sit_request_new_query (id, 1);
        response = command_sync (client, request, 30, &error);
        if (!response)
            goto out;
        print_message (response);
        success = TRUE;
    } else if (g_str_equal (command, "raw") || g_str_equal (command, "payload")) {
        /* Both build the same frame; they differ only in where the caller's
         * hex lands.  'raw' writes it from @8, which is where frame_header()
         * puts a body and therefore *inside* the reserved word -- the trap that
         * made 0x0901 look like a dead end for a day.  'payload' prepends the
         * four zero bytes so the hex starts at @12, which is how every layout
         * in docs/subsystems/modem/reference/sit-protocol.md is written. */
        gboolean at_payload = g_str_equal (command, "payload");
        g_autoptr(GByteArray) body = NULL;
        g_autoptr(GByteArray) request = NULL;
        g_autoptr(MMSitMessage) response = NULL;
        guint16 id;

        if (argc < 3 || argc > 4) {
            g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "%s requires MESSAGE_ID and optional %s", command,
                         at_payload ? "PAYLOAD_HEX" : "BODY_HEX");
            goto out;
        }
        if (!parse_message_id (argv[2], &id, &error))
            goto out;
        if (argc == 4) {
            body = parse_hex (argv[3], &error);
            if (!body)
                goto out;
        } else
            body = g_byte_array_new ();
        if (at_payload) {
            static const guint8 reserved[4] = { 0, 0, 0, 0 };

            g_byte_array_prepend (body, reserved, sizeof (reserved));
        }
        request = mm_sit_request_new (id, 1, body->data, body->len, &error);
        if (!request)
            goto out;
        response = command_sync (client, request, 30, &error);
        if (!response)
            goto out;
        print_message (response);
        success = TRUE;
    } else {
        g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     "Unknown command '%s'", command);
    }

out:
    if (!success && error)
        g_printerr ("SIT command failed: %s\n", error->message);
    mm_sit_client_close (client);
    mm_log_shutdown ();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
