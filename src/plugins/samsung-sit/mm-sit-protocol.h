/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_PROTOCOL_H
#define MM_SIT_PROTOCOL_H

#include <gio/gio.h>

#define MM_SIT_HEADER_SIZE          8
#define MM_SIT_STATUS_SIZE          4
/* Offset of a response's status byte, past the 10-byte header. */
#define MM_SIT_STATUS_OFFSET        10
#define MM_SIT_MAX_APN_LENGTH       100
#define MM_SIT_SHA1_LENGTH          20
#define MM_SIT_MAX_SIM_SLOTS        4
#define MM_SIT_MAX_SIM_PORTS        4
#define MM_SIT_MAX_ATR_LENGTH       33
#define MM_SIT_MAX_ICCID_LENGTH     10
#define MM_SIT_MAX_EID_LENGTH       16
#define MM_SIT_MAX_SIM_APPS         8
#define MM_SIT_MAX_AID_LENGTH       16
#define MM_SIT_MAX_SIM_IO_DATA      512
#define MM_SIT_MAX_SIM_IO_PATH      6
#define MM_SIT_MAX_SMS_PDU          244
#define MM_SIT_MAX_SMSC_LEN         12
#define MM_SIT_MAX_IMEI_LEN         15
/* INET_ADDRSTRLEN / INET6_ADDRSTRLEN, spelled out so this header does not have
 * to pull <arpa/inet.h> into every user (it declares connect(), which collides
 * with the bearer's own static one). */
#define MM_SIT_IPV4_STRLEN          16
#define MM_SIT_IPV6_STRLEN          46

typedef enum {
    MM_SIT_MESSAGE_TYPE_REQUEST    = 0,
    MM_SIT_MESSAGE_TYPE_RESPONSE   = 1,
    MM_SIT_MESSAGE_TYPE_INDICATION = 2,
} MMSitMessageType;

typedef enum {
    /* CS voice call group (native SIT, independent of the AIMS/IMS group) */
    MM_SIT_MESSAGE_GET_CALL_LIST              = 0x0000,
    MM_SIT_MESSAGE_DIAL                       = 0x0001,
    MM_SIT_MESSAGE_ANSWER                     = 0x0004,
    MM_SIT_MESSAGE_HANGUP                     = 0x0008,
    /* Empty poke: the CP never pushes call state, it just tells the AP to
     * re-read GET_CALL_LIST. */
    MM_SIT_MESSAGE_CALL_STATE_CHANGED         = 0x0009,
    /* SMS. Which group carries a message depends on whether IMS is up: the
     * AIMS one while registered, the classic one otherwise. */
    MM_SIT_MESSAGE_SEND_SMS                   = 0x0100,
    MM_SIT_MESSAGE_SEND_SMS_EXPECT_MORE       = 0x0101,
    MM_SIT_MESSAGE_SMS_ACK                    = 0x0102,
    MM_SIT_MESSAGE_GET_SMSC_ADDRESS           = 0x0108,
    MM_SIT_MESSAGE_NEW_SMS                    = 0x010c,
    MM_SIT_MESSAGE_NEW_SMS_STATUS_REPORT      = 0x010d,
    MM_SIT_MESSAGE_IMS_SEND_SMS               = 0x0d1b,
    MM_SIT_MESSAGE_IMS_SMS_ACK                = 0x0d1d,
    MM_SIT_MESSAGE_IMS_NEW_SMS                = 0x0d1e,
    MM_SIT_MESSAGE_GET_SIM_STATUS             = 0x0200,
    MM_SIT_MESSAGE_SIM_IO                     = 0x0208,
    MM_SIT_MESSAGE_GET_FACILITY_LOCK          = 0x0209,
    MM_SIT_MESSAGE_SIM_STATUS_CHANGED         = 0x0210,
    MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS        = 0x024d,
    MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED    = 0x024e,
    MM_SIT_MESSAGE_SET_LOGICAL_SLOT_MAPPING   = 0x0250,
    MM_SIT_MESSAGE_GET_IMEI                    = 0x0401,
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
    MM_SIT_MESSAGE_QUERY_NETWORK_SELECTION_MODE = 0x0703,
    MM_SIT_MESSAGE_SET_NETWORK_SELECTION_AUTO   = 0x0704,
    MM_SIT_MESSAGE_SET_NETWORK_SELECTION_MANUAL = 0x0705,
    MM_SIT_MESSAGE_GET_CELL_INFO_LIST          = 0x070c,
    MM_SIT_MESSAGE_SERVING_NETWORK             = 0x070e,
    MM_SIT_MESSAGE_NETWORK_SCAN                = 0x0734,
    MM_SIT_MESSAGE_NET_SCAN_RESULT             = 0x0736,
    MM_SIT_MESSAGE_REGISTRATION_FAILED         = 0x074a,
    MM_SIT_MESSAGE_SET_ALLOWED_NETWORK_TYPE_BITMAP = 0x074f,
    MM_SIT_MESSAGE_GET_ALLOWED_NETWORK_TYPE_BITMAP = 0x0750,
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
    MM_SIT_MESSAGE_GET_NITZ_TIME               = 0x0955,
    MM_SIT_MESSAGE_CARRIER_CONFIG_MANIFEST     = 0x4600,
    MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO     = 0x4602,
    MM_SIT_MESSAGE_OEM_CONFIG                  = 0x4605,
} MMSitMessageId;

typedef enum {
    MM_SIT_RADIO_STATE_UNKNOWN = 0,
    MM_SIT_RADIO_STATE_OFF     = 1,
    MM_SIT_RADIO_STATE_ON      = 2,
} MMSitRadioState;

/* Address families to ask the network for in a data context. The CP's own
 * numbering, so a data call's granted addr_type uses the same values. */
/* Why a data call is being torn down. PDP_RESET is the RIL's own
 * DEACT_REASON_PDP_RESET, which a context the CP refuses to drop normally does
 * respond to. */
typedef enum {
    MM_SIT_DEACT_REASON_NORMAL    = 0,
    MM_SIT_DEACT_REASON_PDP_RESET = 2,
} MMSitDeactReason;

typedef enum {
    MM_SIT_PDP_TYPE_IPV4   = 1,
    MM_SIT_PDP_TYPE_IPV6   = 2,
    MM_SIT_PDP_TYPE_IPV4V6 = 3,
} MMSitPdpType;

/* How the network wants an APN's credentials presented, in the CP's own
 * numbering. It knows only these: MSCHAP, MSCHAPv2 and EAP have no
 * representation on this modem. */
typedef enum {
    MM_SIT_AUTH_TYPE_NONE        = 0,
    MM_SIT_AUTH_TYPE_PAP         = 1,
    MM_SIT_AUTH_TYPE_CHAP        = 2,
    MM_SIT_AUTH_TYPE_PAP_OR_CHAP = 3,
} MMSitAuthType;

/* What a context is for, in the CP's own bitmask. The values are Android's
 * ApnTypes: every mask in the captured corpus decomposes into these with
 * nothing left over. SUPL and HIPRI have no ModemManager equivalent -- the
 * vendor gets them from its APN database, which we do not have. */
typedef enum {
    MM_SIT_APN_TYPE_DEFAULT   = 0x001,
    MM_SIT_APN_TYPE_MMS       = 0x002,
    MM_SIT_APN_TYPE_SUPL      = 0x004,
    MM_SIT_APN_TYPE_DUN       = 0x008,
    MM_SIT_APN_TYPE_HIPRI     = 0x010,
    MM_SIT_APN_TYPE_FOTA      = 0x020,
    MM_SIT_APN_TYPE_IMS       = 0x040,
    MM_SIT_APN_TYPE_IA        = 0x100,
    MM_SIT_APN_TYPE_EMERGENCY = 0x200,
    MM_SIT_APN_TYPE_XCAP      = 0x800,
} MMSitApnType;

/* Credentials for an APN. Pass NULL wherever one of these is accepted to send
 * none, which is the ordinary case; user and password may each be NULL. */
typedef struct {
    MMSitAuthType  auth;
    const gchar   *user;
    const gchar   *password;
} MMSitApnAuth;

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

/* Values of the SIM status' PIN-state byte (the RIL PinState enum). */
typedef enum {
    MM_SIT_PIN_STATE_UNKNOWN              = 0,
    MM_SIT_PIN_STATE_ENABLED_NOT_VERIFIED = 1,
    MM_SIT_PIN_STATE_ENABLED_VERIFIED     = 2,
    MM_SIT_PIN_STATE_DISABLED             = 3,
    MM_SIT_PIN_STATE_ENABLED_BLOCKED      = 4,
    MM_SIT_PIN_STATE_ENABLED_PERM_BLOCKED = 5,
} MMSitPinState;

/* Facility codes the CP takes in a facility-lock query. Only these two appear in
 * the corpus; 0x03 is the SIM PIN ("SC"), and 0x10 was queried alongside it but
 * always answered unlocked, so which facility it is has not been pinned down. */
#define MM_SIT_FACILITY_SIM_PIN 0x03

/* Values of an application's state byte (the RIL AppState enum). */
typedef enum {
    MM_SIT_APP_STATE_UNKNOWN            = 0,
    MM_SIT_APP_STATE_DETECTED           = 1,
    MM_SIT_APP_STATE_PIN                = 2,
    MM_SIT_APP_STATE_PUK                = 3,
    MM_SIT_APP_STATE_SUBSCRIPTION_PERSO = 4,
    MM_SIT_APP_STATE_READY              = 5,
} MMSitAppState;

/* One application on the card. The PIN/PUK retry counters are here, which is
 * how they can be read without spending an attempt. */
typedef struct {
    guint8 app_type;
    guint8 aid_len;
    guint8 aid[MM_SIT_MAX_AID_LENGTH];
    guint8 pin1_state;
    guint8 pin2_state;
    guint8 pin1_retries;
    guint8 puk1_retries;
    guint8 pin2_retries;
    guint8 puk2_retries;
    guint8 app_state;
} MMSitSimApp;

typedef struct {
    guint8      card_state;
    guint8      universal_pin_state;
    gboolean    present;
    guint8      app_count;
    MMSitSimApp apps[MM_SIT_MAX_SIM_APPS];
    guint8      atr_len;
    guint8      atr[MM_SIT_MAX_ATR_LENGTH];
    guint8      iccid_len;
    guint8      iccid[MM_SIT_MAX_ICCID_LENGTH];
    guint8      eid_len;
    guint8      eid[MM_SIT_MAX_EID_LENGTH];
} MMSitSimStatus;

/* APDU instruction bytes a SIM_IO request takes (TS 27.007 +CRSM). */
typedef enum {
    MM_SIT_SIM_IO_READ_BINARY   = 0xb0,
    MM_SIT_SIM_IO_READ_RECORD   = 0xb2,
    MM_SIT_SIM_IO_GET_RESPONSE  = 0xc0,
    MM_SIT_SIM_IO_UPDATE_BINARY = 0xd6,
    MM_SIT_SIM_IO_UPDATE_RECORD = 0xdc,
    MM_SIT_SIM_IO_STATUS        = 0xf2,
} MMSitSimIoCommand;

/* One elementary-file access, the fields of +CRSM. 'path' is the chain of file
 * ids from the MF down to the file's parent and may be empty. */
typedef struct {
    guint8   command;
    guint16  file_id;
    guint16  path[MM_SIT_MAX_SIM_IO_PATH];
    guint    path_len;
    guint8   p1;
    guint8   p2;
    guint8   p3;
} MMSitSimIoRequest;

/* The card's answer. 'success' only reflects the status word: a refusal comes
 * back here, not as a GError, because the caller may accept it. */
typedef struct {
    guint8   sw1;
    guint8   sw2;
    gboolean success;
    guint16  data_len;
    guint8   data[MM_SIT_MAX_SIM_IO_DATA];
} MMSitSimIoResponse;

/* A message the network delivered. 'over_ims' only decides which ack answers
 * it; the PDU is the TS 23.040 one, service centre included. */
typedef struct {
    guint8   format;
    gboolean over_ims;
    /* A delivery report rather than a message; same shape, same size. */
    gboolean status_report;
    guint16  pdu_len;
    guint8   pdu[MM_SIT_MAX_SMS_PDU];
} MMSitSmsDeliver;

/* How an elementary file is organised. */
typedef enum {
    MM_SIT_EF_TRANSPARENT  = 0,
    MM_SIT_EF_LINEAR_FIXED = 1,
    MM_SIT_EF_CYCLIC       = 2,
} MMSitEfStructure;

/* What a GET_RESPONSE says about a file; 'record_len'/'record_count' are zero
 * for a transparent one. */
typedef struct {
    MMSitEfStructure structure;
    gboolean         has_file_id;
    guint16          file_id;
    guint16          size;
    guint8           record_len;
    guint8           record_count;
} MMSitEfInfo;

/* The registration responses' radio-technology numbering (distinct from the
 * scan's, see MM_SIT_SCAN_RAT_LTE). */
#define MM_SIT_REGISTRATION_RAT_LTE 14

typedef struct {
    guint8   state;
    guint8   radio_technology;
    gboolean registered;
} MMSitRegistration;

/* Reported for a cause the CP says it does not have. */
#define MM_SIT_REJECT_CAUSE_NONE 0xffff
/* 3GPP TS 24.301 EMM cause 10: the network has no context for this UE and
 * expects a fresh attach (TS 24.301 5.5.3.2.5). The one cause worth retrying. */
#define MM_SIT_REJECT_CAUSE_IMPLICITLY_DETACHED 10

typedef struct {
    gchar    plmn[7];
    gboolean circuit_switched;
    gboolean packet_switched;
    guint16  cause;
    guint16  additional_cause;
    /* The network is asking for a new attach rather than refusing outright. */
    gboolean retryable;
} MMSitRegistrationFailure;

typedef struct {
    gchar    plmn[7];
    gchar    operator_short[33];
    gchar    operator_long[65];
} MMSitOperator;

/* The last NITZ the CP was handed. The CP keeps no clock of its own: the
 * timestamp is UTC as of 'age_ms' milliseconds ago, so the network's current
 * time is the two added together. 'timezone_quarter_hours' is the network's
 * offset from UTC and 'daylight_saving_hours' how much of it is DST. */
typedef struct {
    gboolean valid;
    guint16  year;
    guint8   month;      /* 1-12 */
    guint8   day;
    guint8   hour;       /* UTC */
    guint8   minute;
    guint8   second;
    gint8    timezone_quarter_hours;
    guint8   daylight_saving_hours;
    guint8   day_of_week; /* 1 = Monday */
    gchar    plmn[7];
    guint64  age_ms;
} MMSitNitzTime;

/* One PLMN of a NET_SCAN_RESULT (0x0736) batch. 'rat' is the scan's own radio
 * technology numbering, not the one the registration responses use; only
 * MM_SIT_SCAN_RAT_LTE is seen in the capture corpus. */
#define MM_SIT_SCAN_RAT_LTE 2

typedef struct {
    gchar    plmn[7];
    gboolean serving;
    guint8   rat;
} MMSitScannedNetwork;

/* The radio generations the CP may use, as the allowed-network-type bitmap
 * selects them. */
typedef struct {
    gboolean gsm;
    gboolean umts;
    gboolean lte;
    gboolean nr;
} MMSitNetworkGenerations;

/* Bits of the signal report's RAT mask. This is the CP's only statement of which
 * radio it is using; under EN-DC the LTE and NR bits are set together. */
#define MM_SIT_SIGNAL_RAT_GSM   (1 << 0)
#define MM_SIT_SIGNAL_RAT_WCDMA (1 << 1)
#define MM_SIT_SIGNAL_RAT_LTE   (1 << 2)
#define MM_SIT_SIGNAL_RAT_NR5G  (1 << 6)

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

/* One NR cell record. Under EN-DC these arrive alongside the LTE records, so an
 * empty NR list means the CP reports no NR cell in reach. */
typedef struct {
    gboolean registered;
    guint8   connection_status;
    gchar    operator_id[7];
    guint64  nci;
    guint32  physical_ci;
    guint32  tac;
    guint32  nrarfcn;
    gint32   ss_rsrp;
    gint32   ss_rsrq;
    gint32   ss_sinr;
    gint32   csi_rsrp;
    gint32   csi_rsrq;
    gint32   csi_sinr;
} MMSitCellInfoNr;

typedef struct {
    gboolean registered;
    guint8   connection_status;
    gchar    operator_id[7];
    guint32  ci;
    guint32  physical_ci;
    guint32  tac;
    guint32  earfcn;
    guint32  bandwidth;
    gint32   rsrp;
    gint32   rsrq;
    gint32   rssnr;
    gint32   timing_advance;
} MMSitCellInfoLte;

/* A data call as the CP reports it. Every address slot on the wire has room for
 * both families and the CP fills whichever the network granted, so a dual-stack
 * call arrives as one call carrying both -- not as two. An address string is
 * empty when that family is absent. */
typedef struct {
    guint16  cause;
    guint8   cid;
    guint8   active;
    gboolean has_ipv4;
    gchar    ipv4[MM_SIT_IPV4_STRLEN];
    gchar    dns1[MM_SIT_IPV4_STRLEN];
    gchar    dns2[MM_SIT_IPV4_STRLEN];
    gboolean has_ipv6;
    gchar    ipv6[MM_SIT_IPV6_STRLEN];
    gchar    dns1_v6[MM_SIT_IPV6_STRLEN];
    gchar    dns2_v6[MM_SIT_IPV6_STRLEN];
    guint16  mtu;
} MMSitDataCall;

/* One entry of a GET_CALL_LIST response. 'state' is the raw RIL_CallState
 * (0 ACTIVE, 1 HOLDING, 2 DIALING, 3 ALERTING, 4 INCOMING, 5 WAITING); the
 * codec passes through values it does not model rather than flattening them. */
typedef struct {
    guint32  state;
    guint32  index;
    gboolean incoming;
    gchar    number[84];
} MMSitCall;

typedef struct {
    gchar iccid[21];
    gchar imsi[16];
} MMSitCarrierSimInfo;

typedef struct {
    guint8 iccid_len;
    guint8 iccid[MM_SIT_MAX_ICCID_LENGTH];
    guint8 logical_slot_id;
    guint8 state;
} MMSitSimPort;

typedef struct {
    guint8       card_state;
    guint8       atr_len;
    guint8       atr[MM_SIT_MAX_ATR_LENGTH];
    guint8       eid_len;
    guint8       eid[MM_SIT_MAX_EID_LENGTH];
    guint8       port_count;
    MMSitSimPort ports[MM_SIT_MAX_SIM_PORTS];
} MMSitSimSlot;

typedef struct {
    gboolean     legacy;
    guint8       slot_count;
    MMSitSimSlot slots[MM_SIT_MAX_SIM_SLOTS];
} MMSitSimSlotStatus;

typedef struct {
    guint8 physical_slot_id;
    guint8 port_id;
} MMSitLogicalSlotMapping;

/* Decode three packed PLMN bytes (EF_PLMNwAcT, EF_FPLMN, ...) into "MCCMNC";
 * NULL for an unused or malformed entry. */
gchar       *mm_sit_plmn_from_packed                  (const guint8          *entry);

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
/* ModemManager's notion of a connection, in the CP's terms. */
MMSitPdpType mm_sit_pdp_type_from_ip_family           (MMBearerIpFamily       family);
gboolean     mm_sit_apn_auth_from_properties          (MMBearerProperties    *properties,
                                                       MMSitApnAuth          *out,
                                                       GError               **error);
guint32      mm_sit_apn_types_from_properties         (MMBearerProperties    *properties);

GByteArray *mm_sit_request_new_set_data_profile       (guint16                token,
                                                       const gchar           *apn,
                                                       MMSitPdpType           pdp,
                                                       guint32                apn_types,
                                                       const MMSitApnAuth    *auth,
                                                       GError               **error);
GByteArray *mm_sit_request_new_set_initial_attach_apn (guint16                token,
                                                       const gchar           *apn,
                                                       MMSitPdpType           pdp,
                                                       const MMSitApnAuth    *auth,
                                                       GError               **error);
GByteArray *mm_sit_request_new_setup_data_call        (guint16                token,
                                                       const gchar           *apn,
                                                       MMSitPdpType           pdp,
                                                       guint32                apn_types,
                                                       const MMSitApnAuth    *auth,
                                                       GError               **error);
GByteArray *mm_sit_request_new_deactivate_data_call   (guint16                token,
                                                       guint8                 cid,
                                                       MMSitDeactReason       reason);
GByteArray *mm_sit_request_new_get_data_call_list     (guint16                token);
GByteArray *mm_sit_request_new_dial                   (guint16                token,
                                                       const gchar           *number,
                                                       GError               **error);
GByteArray *mm_sit_request_new_answer                 (guint16                token);
GByteArray *mm_sit_request_new_hangup                 (guint16                token,
                                                       guint32                call_id);
GByteArray *mm_sit_request_new_get_call_list          (guint16                token);
GByteArray *mm_sit_request_new_set_fast_dormancy_info (guint16                token);
GByteArray *mm_sit_request_new_set_indication_filter  (guint16                token,
                                                       guint32                filter);
GByteArray *mm_sit_request_new_set_screen_state       (guint16                token,
                                                       gboolean               screen_on);
GByteArray *mm_sit_request_new_set_ap_system_time     (guint16                token,
                                                       GDateTime             *time,
                                                       GError               **error);
GByteArray *mm_sit_request_new_oem_config             (guint16                token);
GByteArray *mm_sit_request_new_set_modem_config       (guint16                token);
GByteArray *mm_sit_request_new_device_info            (guint16                token,
                                                       const gchar           *model,
                                                       const gchar           *build,
                                                       GError               **error);
GByteArray *mm_sit_request_new_carrier_manifest       (guint16                token,
                                                       const gchar           *directory,
                                                       const guint8           sha1[MM_SIT_SHA1_LENGTH],
                                                       GError               **error);
GByteArray *mm_sit_request_new_set_logical_slot_mapping
                                                      (guint16                token,
                                                       const MMSitLogicalSlotMapping *mappings,
                                                       guint                  mapping_count,
                                                       GError               **error);
GByteArray *mm_sit_request_new_set_network_selection_auto
                                                      (guint16                token);
GByteArray *mm_sit_request_new_set_network_selection_manual
                                                      (guint16                token,
                                                       const gchar           *plmn,
                                                       guint8                 rat,
                                                       GError               **error);
GByteArray *mm_sit_request_new_start_network_scan      (guint16                token);
GByteArray *mm_sit_request_new_stop_network_scan       (guint16                token);
GByteArray *mm_sit_request_new_get_facility_lock       (guint16                token,
                                                       guint8                 facility);
GByteArray *mm_sit_request_new_get_allowed_network_types
                                                      (guint16                token);
GByteArray *mm_sit_request_new_set_allowed_network_types
                                                      (guint16                token,
                                                       const MMSitNetworkGenerations *generations,
                                                       GError               **error);

gboolean mm_sit_message_parse_radio_state             (const MMSitMessage    *message,
                                                       MMSitRadioState       *out_state,
                                                       GError               **error);
/* 'smsc' is the TS 23.040 service-centre field as mm_sit_message_parse_smsc_address
 * returns it, and may be NULL to let the CP use the card's own. Every part but
 * the last of a concatenated message sets 'expect_more'. */
GByteArray *mm_sit_request_new_send_sms               (guint16                token,
                                                       const guint8          *smsc,
                                                       gsize                  smsc_len,
                                                       const guint8          *tpdu,
                                                       gsize                  tpdu_len,
                                                       gboolean               expect_more,
                                                       gboolean               over_ims,
                                                       GError               **error);
GByteArray *mm_sit_request_new_sms_ack                (guint16                token,
                                                       gboolean               over_ims);
GByteArray *mm_sit_request_new_get_smsc_address       (guint16                token);
gboolean mm_sit_message_parse_new_sms                 (const MMSitMessage    *message,
                                                       MMSitSmsDeliver       *out_deliver,
                                                       GError               **error);
gboolean mm_sit_message_parse_send_sms_result         (const MMSitMessage    *message,
                                                       guint32               *out_reference,
                                                       GError               **error);
/* Fills a caller buffer of MM_SIT_MAX_SMSC_LEN bytes. */
gboolean mm_sit_message_parse_smsc_address            (const MMSitMessage    *message,
                                                       guint8                *out_smsc,
                                                       gsize                 *out_len,
                                                       GError               **error);
/* '*out_imei' is set to NULL, with no error, when the CP holds no IMEI --
 * an unprovisioned board, which is a state to report rather than a failure. */
gboolean mm_sit_message_parse_imei                    (const MMSitMessage    *message,
                                                       gchar                **out_imei,
                                                       GError               **error);
gboolean mm_sit_message_parse_sim_status              (const MMSitMessage    *message,
                                                       MMSitSimStatus        *out_status,
                                                       GError               **error);
/* One elementary-file access. 'aid' selects the application and may be NULL:
 * the CP resolves the file without it, but rild always sends it. */
GByteArray *mm_sit_request_new_sim_io                 (guint16                token,
                                                       const MMSitSimIoRequest *request,
                                                       const guint8          *aid,
                                                       gsize                  aid_len,
                                                       GError               **error);
/* A card refusal is not an error here: it comes back as success == FALSE with
 * the card's status word, because callers accept more than plain 0x9000. */
gboolean mm_sit_message_parse_sim_io                  (const MMSitMessage    *message,
                                                       MMSitSimIoResponse    *out_response,
                                                       GError               **error);
/* Decode the File Control Parameters a GET_RESPONSE returned. Takes the SIM_IO
 * response's data, not a SIT frame. */
gboolean mm_sit_file_control_parameters_parse         (const guint8          *data,
                                                       gsize                  data_len,
                                                       MMSitEfInfo           *out_info,
                                                       GError               **error);
gboolean mm_sit_message_parse_registration            (const MMSitMessage    *message,
                                                       MMSitRegistration     *out_registration,
                                                       GError               **error);
gboolean mm_sit_message_parse_registration_failed     (const MMSitMessage       *message,
                                                       MMSitRegistrationFailure *out_failure,
                                                       GError                  **error);
gboolean mm_sit_message_parse_operator                (const MMSitMessage    *message,
                                                       MMSitOperator         *out_operator,
                                                       GError               **error);
gboolean mm_sit_message_parse_signal                  (const MMSitMessage    *message,
                                                       MMSitSignal           *out_signal,
                                                       GError               **error);
/* A CP that has not been handed a NITZ yet is not a failure: it parses fine
 * with out_time->valid FALSE. */
gboolean mm_sit_message_parse_nitz_time               (const MMSitMessage    *message,
                                                       MMSitNitzTime         *out_time,
                                                       GError               **error);
/* The network's *current* local time as an ISO-8601 string: the cached
 * timestamp rolled forward by its own age and into the network's timezone.
 * Fails with MM_CORE_ERROR_RETRY if the CP holds no time. */
gchar   *mm_sit_nitz_time_to_iso8601                  (const MMSitNitzTime   *time,
                                                       GError               **error);
gboolean mm_sit_message_parse_cell_info_list          (const MMSitMessage    *message,
                                                       GArray               **out_cells,
                                                       GError               **error);
/* The NR companion: fills *out_cells with a GArray of MMSitCellInfoNr (may be
 * empty, never NULL on success). */
gboolean mm_sit_message_parse_nr_cell_info_list       (const MMSitMessage    *message,
                                                       GArray               **out_cells,
                                                       GError               **error);
/* Returns only the first (default) call. TODO: remove once every caller uses
 * mm_sit_message_parse_data_call_list -- it drops the other PDNs of a
 * DATA_CALL_LIST_CHANGED. */
gboolean mm_sit_message_parse_data_call               (const MMSitMessage    *message,
                                                       MMSitDataCall         *out_call,
                                                       GError               **error);
/* Parses every PDN of a data-call message into a GArray of MMSitDataCall. */
gboolean mm_sit_message_parse_data_call_list          (const MMSitMessage    *message,
                                                       GArray               **out_calls,
                                                       GError               **error);

/* Validate a SET_NETWORK_SELECTION_MANUAL ack. There is nothing to read out of
 * it: a refusal is the CP's status byte, so it comes back as an error. */
gboolean mm_sit_message_parse_network_selection_manual_ack
                                                      (const MMSitMessage    *message,
                                                       GError               **error);
/* Fills *out_networks with a GArray of MMSitScannedNetwork (may be empty, never
 * NULL on success); the caller owns it. Each NET_SCAN_RESULT batch repeats every
 * network found so far, so callers should replace rather than accumulate. */
gboolean mm_sit_message_parse_net_scan_result         (const MMSitMessage    *message,
                                                       GArray               **out_networks,
                                                       GError               **error);
gboolean mm_sit_message_parse_facility_lock            (const MMSitMessage    *message,
                                                       gboolean              *out_locked,
                                                       GError               **error);
gboolean mm_sit_message_parse_allowed_network_types    (const MMSitMessage    *message,
                                                       MMSitNetworkGenerations *out_generations,
                                                       GError               **error);

/* Fills *out_calls with a GArray of MMSitCall (may be empty, never NULL on
 * success); the caller owns it. */
gboolean mm_sit_message_parse_call_list               (const MMSitMessage    *message,
                                                       GArray               **out_calls,
                                                       GError               **error);
gboolean mm_sit_message_parse_carrier_sim_info        (const MMSitMessage    *message,
                                                       MMSitCarrierSimInfo   *out_info,
                                                       GError               **error);
gboolean mm_sit_message_parse_sim_slot_status         (const MMSitMessage    *message,
                                                       MMSitSimSlotStatus    *out_status,
                                                       GError               **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitMessage, mm_sit_message_free)

#endif /* MM_SIT_PROTOCOL_H */
