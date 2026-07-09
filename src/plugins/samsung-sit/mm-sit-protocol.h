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
    MM_SIT_MESSAGE_GET_DATA_CALL_LIST          = 0x0605,
    MM_SIT_MESSAGE_SET_DATA_PROFILE            = 0x0613,
    MM_SIT_MESSAGE_GET_VOICE_REGISTRATION      = 0x0700,
    MM_SIT_MESSAGE_GET_NETWORK_REGISTRATION    = 0x0701,
    MM_SIT_MESSAGE_GET_DATA_REGISTRATION       = 0x0702,
    MM_SIT_MESSAGE_SERVING_NETWORK             = 0x070e,
    MM_SIT_MESSAGE_SET_PREFERRED_DATA_MODEM    = 0x0740,
    MM_SIT_MESSAGE_SET_RADIO_POWER             = 0x0800,
    MM_SIT_MESSAGE_GET_RADIO_POWER             = 0x0801,
    MM_SIT_MESSAGE_RADIO_STATE_CHANGED         = 0x0802,
    MM_SIT_MESSAGE_DEVICE_INFO                 = 0x0922,
    MM_SIT_MESSAGE_SET_INDICATION_FILTER       = 0x0928,
    MM_SIT_MESSAGE_SET_MODEM_CONFIG            = 0x093f,
    MM_SIT_MESSAGE_DEVICE_CAPABILITIES         = 0x0949,
    MM_SIT_MESSAGE_OEM_CONFIG                  = 0x4605,
    MM_SIT_MESSAGE_CARRIER_CONFIG_MANIFEST     = 0x4600,
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
    gboolean registered;
    gchar    plmn[7];
} MMSitRegistration;

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
GByteArray *mm_sit_request_new_set_indication_filter  (guint16                token,
                                                       guint32                filter);
GByteArray *mm_sit_request_new_device_capabilities    (guint16                token,
                                                       const guint8           capabilities[6]);
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
gboolean mm_sit_message_parse_data_call               (const MMSitMessage    *message,
                                                       MMSitDataCall         *out_call,
                                                       GError               **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitMessage, mm_sit_message_free)

#endif /* MM_SIT_PROTOCOL_H */
