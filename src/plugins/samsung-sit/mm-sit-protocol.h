/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_PROTOCOL_H
#define MM_SIT_PROTOCOL_H

#include <gio/gio.h>

#define MM_SIT_HEADER_SIZE          8
#define MM_SIT_STATUS_SIZE          4
#define MM_SIT_MAX_APN_LENGTH       100
#define MM_SIT_SHA1_LENGTH          20

typedef enum {
    MM_SIT_MESSAGE_TYPE_REQUEST    = 0,
    MM_SIT_MESSAGE_TYPE_RESPONSE   = 1,
    MM_SIT_MESSAGE_TYPE_INDICATION = 2,
} MMSitMessageType;

typedef enum {
    MM_SIT_MESSAGE_GET_SIM_STATUS             = 0x0200,
    MM_SIT_MESSAGE_SIM_IO                     = 0x0208,
    MM_SIT_MESSAGE_SIM_STATUS_CHANGED         = 0x0210,
    MM_SIT_MESSAGE_SETUP_DATA_CALL             = 0x0600,
    MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL        = 0x0601,
    MM_SIT_MESSAGE_SET_INITIAL_ATTACH_APN      = 0x0603,
    MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED      = 0x0604,
    MM_SIT_MESSAGE_GET_DATA_CALL_LIST          = 0x0602,
    MM_SIT_MESSAGE_SET_FAST_DORMANCY_INFO      = 0x0605,
    MM_SIT_MESSAGE_SET_DATA_PROFILE            = 0x0613,
    MM_SIT_MESSAGE_GET_VOICE_REGISTRATION      = 0x0700,
    MM_SIT_MESSAGE_GET_DATA_REGISTRATION       = 0x0701,
    MM_SIT_MESSAGE_GET_OPERATOR                = 0x0702,
    MM_SIT_MESSAGE_SERVING_NETWORK             = 0x070e,
    MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM    = 0x0740,
    MM_SIT_MESSAGE_SET_RADIO_POWER             = 0x0800,
    MM_SIT_MESSAGE_GET_RADIO_POWER             = 0x0801,
    MM_SIT_MESSAGE_RADIO_STATE_CHANGED         = 0x0802,
    MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH          = 0x0900,
    MM_SIT_MESSAGE_SET_SCREEN_STATE             = 0x0902,
    MM_SIT_MESSAGE_SIGNAL_STRENGTH_CHANGED      = 0x0906,
    MM_SIT_MESSAGE_DEVICE_INFO                 = 0x0922,
    MM_SIT_MESSAGE_SET_INDICATION_FILTER       = 0x0928,
    MM_SIT_MESSAGE_SET_MODEM_CONFIG            = 0x093f,
    MM_SIT_MESSAGE_SET_AP_SYSTEM_TIME          = 0x0949,
    MM_SIT_MESSAGE_CARRIER_CONFIG_MANIFEST     = 0x4600,
    MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO     = 0x4602,
    MM_SIT_MESSAGE_OEM_CONFIG                  = 0x4605,
} MMSitMessageId;

typedef enum {
    MM_SIT_RADIO_STATE_UNKNOWN = 0,
    MM_SIT_RADIO_STATE_OFF     = 1,
    MM_SIT_RADIO_STATE_ON      = 2,
} MMSitRadioState;

typedef struct {
    MMSitMessageType type;
    guint8           reserved;
    guint16          id;
    guint16          length;
    guint16          token;
    gboolean         has_status;
    guint16          status;
    GBytes          *raw;
} MMSitMessage;

typedef struct {
    guint8   card_state;
    guint8   universal_pin_state;
    guint8   gsm_umts_app_index;
    guint8   cdma_app_index;
    guint8   ims_app_index;
    guint8   app_count;
    gboolean present;
} MMSitSimStatus;

typedef struct {
    guint8   state;
    guint8   radio_technology;
    gboolean registered;
} MMSitRegistration;

typedef struct {
    gchar    plmn[7];
    gchar    operator_short[33];
    gchar    operator_long[65];
} MMSitOperator;

typedef struct {
    guint8   version;
    guint16  rat_mask;

    gboolean has_gsm;
    gint32   gsm_signal_strength;
    gint32   gsm_bit_error_rate;
    gint32   gsm_timing_advance;

    gboolean has_wcdma;
    gint32   wcdma_signal_strength;
    gint32   wcdma_bit_error_rate;
    gint32   wcdma_rscp;
    gint32   wcdma_ecno;

    gboolean has_lte;
    gint32   lte_signal_strength;
    gint32   lte_rsrp;
    gint32   lte_rsrq;
    gint32   lte_rssnr;
    gint32   lte_cqi_table_index;
    gint32   lte_cqi;
    gint32   lte_timing_advance;

    gboolean has_nr5g;
    gint32   nr5g_ss_rsrp;
    gint32   nr5g_ss_rsrq;
    gint32   nr5g_ss_sinr;
    gint32   nr5g_csi_rsrp;
    gint32   nr5g_csi_rsrq;
    gint32   nr5g_csi_sinr;
    gint32   nr5g_timing_advance;
} MMSitSignal;

typedef struct {
    guint16  cause;
    guint8   cid;
    guint8   active;
    gboolean has_ipv4;
    gchar    ipv4[16];
    gchar    dns1[16];
    gchar    dns2[16];
    guint16  mtu;
} MMSitDataCall;

typedef struct {
    gchar iccid[21];
    gchar imsi[16];
} MMSitCarrierSimInfo;

gboolean     mm_sit_frame_peek_length                 (const guint8          *data,
                                                       gsize                  data_len,
                                                       guint16               *out_length,
                                                       GError               **error);
MMSitMessage *mm_sit_message_parse                    (const guint8          *data,
                                                       gsize                  data_len,
                                                       GError               **error);
void          mm_sit_message_free                     (MMSitMessage          *self);
const guint8 *mm_sit_message_get_data                 (const MMSitMessage    *self,
                                                       gsize                 *out_len);
const guint8 *mm_sit_message_get_payload              (const MMSitMessage    *self,
                                                       gsize                 *out_len);
const gchar  *mm_sit_message_id_get_string            (guint16                id);

GByteArray *mm_sit_request_new                        (guint16                id,
                                                       guint16                token,
                                                       const guint8          *body,
                                                       gsize                  body_len,
                                                       GError               **error);
GByteArray *mm_sit_request_new_query                  (guint16                id,
                                                       guint16                token);
GByteArray *mm_sit_request_new_set_radio_power        (guint16                token,
                                                       MMSitRadioState        state);
GByteArray *mm_sit_request_new_set_preferred_modem    (guint16                token,
                                                       guint8                 modem_index);
GByteArray *mm_sit_request_new_set_data_profile       (guint16                token,
                                                       const gchar           *apn,
                                                       GError               **error);
GByteArray *mm_sit_request_new_set_initial_attach_apn (guint16                token,
                                                       const gchar           *apn,
                                                       GError               **error);
GByteArray *mm_sit_request_new_setup_data_call        (guint16                token,
                                                       const gchar           *apn,
                                                       GError               **error);
GByteArray *mm_sit_request_new_deactivate_data_call   (guint16                token,
                                                       guint8                 cid,
                                                       guint8                 reason);
GByteArray *mm_sit_request_new_get_data_call_list     (guint16                token);
GByteArray *mm_sit_request_new_set_fast_dormancy_info (guint16                token);
GByteArray *mm_sit_request_new_set_indication_filter  (guint16                token,
                                                       guint32                filter);
GByteArray *mm_sit_request_new_set_screen_state       (guint16                token,
                                                       gboolean               screen_on);
GByteArray *mm_sit_request_new_device_info            (guint16                token,
                                                       const gchar           *model,
                                                       const gchar           *build,
                                                       GError               **error);
GByteArray *mm_sit_request_new_carrier_manifest       (guint16                token,
                                                       const gchar           *directory,
                                                       const guint8           sha1[MM_SIT_SHA1_LENGTH],
                                                       GError               **error);

gboolean mm_sit_message_parse_radio_state             (const MMSitMessage    *message,
                                                       MMSitRadioState       *out_state,
                                                       GError               **error);
gboolean mm_sit_message_parse_sim_status              (const MMSitMessage    *message,
                                                       MMSitSimStatus        *out_status,
                                                       GError               **error);
gboolean mm_sit_message_parse_registration            (const MMSitMessage    *message,
                                                       MMSitRegistration     *out_registration,
                                                       GError               **error);
gboolean mm_sit_message_parse_operator                (const MMSitMessage    *message,
                                                       MMSitOperator         *out_operator,
                                                       GError               **error);
gboolean mm_sit_message_parse_signal                  (const MMSitMessage    *message,
                                                       MMSitSignal           *out_signal,
                                                       GError               **error);
gboolean mm_sit_message_parse_data_call               (const MMSitMessage    *message,
                                                       MMSitDataCall         *out_call,
                                                       GError               **error);
gboolean mm_sit_message_parse_carrier_sim_info        (const MMSitMessage    *message,
                                                       MMSitCarrierSimInfo   *out_info,
                                                       GError               **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitMessage, mm_sit_message_free)

#endif /* MM_SIT_PROTOCOL_H */
