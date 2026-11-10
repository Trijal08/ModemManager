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
/* A GSM cell broadcast page is 88 bytes; the UMTS and LTE forms carry up to
 * this, which is the widest the CP's signed length field can mean. */
#define MM_SIT_MAX_CBS_PAGE         1252
/* Most broadcast ranges the CP accepts, and so the most it can report. */
#define MM_SIT_MAX_BROADCAST_RANGES 50
#define MM_SIT_MAX_SMSC_LEN         12
#define MM_SIT_MAX_IMEI_LEN         15
#define MM_SIT_SOFTWARE_VERSION_LEN 2
#define MM_SIT_MAX_VERSION_FIELD    33
#define MM_SIT_MAX_DEVICE_INFO_FIELD 32
/* INET_ADDRSTRLEN / INET6_ADDRSTRLEN, spelled out so this header does not have
 * to pull <arpa/inet.h> into every user (it declares connect(), which collides
 * with the bearer's own static one). */
#define MM_SIT_IPV4_STRLEN          16
#define MM_SIT_IPV6_STRLEN          46

/*
 * The mode an ON_USSD indication reports, as the u32 at frame+8 -- the slot a
 * response uses for its status. Values are RIL's: logcat printed
 * "UNSOL_ON_USSD 4" for the capture where the network rejected the code, and
 * the dialler turned that into onUssdFinishedError.
 */
typedef enum {
    MM_SIT_USSD_MODE_NOTIFY        = 0,
    MM_SIT_USSD_MODE_REQUEST       = 1,
    MM_SIT_USSD_MODE_NW_RELEASE    = 2,
    MM_SIT_USSD_MODE_LOCAL_CLIENT  = 3,
    MM_SIT_USSD_MODE_NOT_SUPPORTED = 4,
    MM_SIT_USSD_MODE_NW_TIMEOUT    = 5,
} MMSitUssdMode;

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
    /* AIMS -- a different protocol sharing this id space; see samsung-aims. */
    MM_SIT_MESSAGE_AIMS_CALL_RING             = 0x0d01,
    MM_SIT_MESSAGE_AIMS_CALL_STATUS           = 0x0d04,
    MM_SIT_MESSAGE_AIMS_REGISTRATION          = 0x0d05,
    /* SMS. Which group carries a message depends on whether IMS is up: the
     * AIMS one while registered, the classic one otherwise. */
    MM_SIT_MESSAGE_SEND_SMS                   = 0x0100,
    MM_SIT_MESSAGE_SEND_SMS_EXPECT_MORE       = 0x0101,
    MM_SIT_MESSAGE_SMS_ACK                    = 0x0102,
    MM_SIT_MESSAGE_GET_SMSC_ADDRESS           = 0x0108,
    MM_SIT_MESSAGE_GET_BROADCAST_SMS_CONFIG   = 0x0105,
    MM_SIT_MESSAGE_SET_BROADCAST_SMS_CONFIG   = 0x0106,
    MM_SIT_MESSAGE_SMS_BROADCAST_ACTIVATION   = 0x0107,
    MM_SIT_MESSAGE_NEW_SMS                    = 0x010c,
    MM_SIT_MESSAGE_NEW_SMS_STATUS_REPORT      = 0x010d,
    MM_SIT_MESSAGE_NEW_BROADCAST_SMS          = 0x0110,
    MM_SIT_MESSAGE_IMS_SEND_SMS               = 0x0d1b,
    MM_SIT_MESSAGE_IMS_SMS_ACK                = 0x0d1d,
    MM_SIT_MESSAGE_IMS_NEW_SMS                = 0x0d1e,
    MM_SIT_MESSAGE_GET_SIM_STATUS             = 0x0200,
    MM_SIT_MESSAGE_SIM_IO                     = 0x0208,
    /*
     * The UICC access-control triple. Stock opens a logical channel by AID to
     * look for the GSMA access-control applets, falls back to reading EF_DIR,
     * and closes the channel again; see mm_sit_client_probe_uicc_access_rules().
     */
    MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL        = 0x0247,
    MM_SIT_MESSAGE_CLOSE_LOGICAL_CHANNEL       = 0x020e,
    MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL       = 0x020f,
    MM_SIT_MESSAGE_GET_FACILITY_LOCK          = 0x0209,
    MM_SIT_MESSAGE_SIM_STATUS_CHANGED         = 0x0210,
    MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS        = 0x024d,
    MM_SIT_MESSAGE_SIM_SLOT_STATUS_CHANGED    = 0x024e,
    MM_SIT_MESSAGE_SET_LOGICAL_SLOT_MAPPING   = 0x0250,
    MM_SIT_MESSAGE_GET_IMEI                    = 0x0401,
    MM_SIT_MESSAGE_GET_IMEISV                  = 0x0402,
    /* The CP pushing a NITZ as it arrives: the GET_NITZ_TIME payload with no
     * status word, so 33 bytes rather than 37. */
    MM_SIT_MESSAGE_NITZ_TIME_RECEIVED          = 0x0905,
    MM_SIT_MESSAGE_GET_MANUAL_BAND_MODE        = 0x0730,
    MM_SIT_MESSAGE_SET_MANUAL_BAND_MODE        = 0x0731,
    MM_SIT_MESSAGE_GET_BASE_BAND_VERSION       = 0x0901,
    /*
     * Supplementary services. Ids read off ProtocolCallBuilder in
     * vendor.radio.protocol.sit.stream.so, each of which loads its own as an
     * inline immediate: BuildSendUssd and BuildSendEncodedUssd both use
     * 0x0505, BuildCancelUssd 0x0506. ON_USSD is the matching indication,
     * captured 2026-11-08.
     */
    MM_SIT_MESSAGE_SEND_USSD                   = 0x0505,
    MM_SIT_MESSAGE_CANCEL_USSD                 = 0x0506,
    MM_SIT_MESSAGE_ON_USSD                     = 0x0510,
    MM_SIT_MESSAGE_SETUP_DATA_CALL             = 0x0600,
    MM_SIT_MESSAGE_DEACTIVATE_DATA_CALL        = 0x0601,
    MM_SIT_MESSAGE_SET_INITIAL_ATTACH_APN      = 0x0603,
    MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED      = 0x0604,
    MM_SIT_MESSAGE_GET_DATA_CALL_LIST          = 0x0602,
    MM_SIT_MESSAGE_SET_FAST_DORMANCY_INFO      = 0x0605,
    MM_SIT_MESSAGE_SET_MOBILE_DATA_STATE       = 0x0625,
    MM_SIT_MESSAGE_SET_DATA_PROFILE            = 0x0613,
    MM_SIT_MESSAGE_GET_VOICE_REGISTRATION      = 0x0700,
    MM_SIT_MESSAGE_GET_DATA_REGISTRATION       = 0x0701,
    MM_SIT_MESSAGE_GET_OPERATOR                = 0x0702,
    MM_SIT_MESSAGE_QUERY_NETWORK_SELECTION_MODE = 0x0703,
    MM_SIT_MESSAGE_SET_NETWORK_SELECTION_AUTO   = 0x0704,
    MM_SIT_MESSAGE_SET_NETWORK_SELECTION_MANUAL = 0x0705,
    MM_SIT_MESSAGE_GET_RADIO_CAPABILITY        = 0x0718,
    MM_SIT_MESSAGE_GET_CELL_INFO_LIST          = 0x070c,
    MM_SIT_MESSAGE_SERVING_NETWORK             = 0x070e,
    MM_SIT_MESSAGE_NETWORK_SCAN                = 0x0734,
    MM_SIT_MESSAGE_NET_SCAN_RESULT             = 0x0736,
    MM_SIT_MESSAGE_PHYSICAL_CHANNEL_CONFIG     = 0x0742,
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
    MM_SIT_MESSAGE_CARRIER_CONFIG_STATUS       = 0x4601,
    MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO     = 0x4602,
    MM_SIT_MESSAGE_OEM_CONFIG                  = 0x4605,
} MMSitMessageId;

typedef enum {
    MM_SIT_RADIO_STATE_UNKNOWN = 0,
    MM_SIT_RADIO_STATE_OFF     = 1,
    MM_SIT_RADIO_STATE_ON      = 2,
} MMSitRadioState;

/* Android's radio indication-filter bits, passed unchanged on the SIT wire. */
typedef enum {
    MM_SIT_INDICATION_FILTER_SIGNAL_STRENGTH      = 1 << 0,
    MM_SIT_INDICATION_FILTER_FULL_NETWORK_STATE   = 1 << 1,
    MM_SIT_INDICATION_FILTER_DATA_CALL_DORMANCY   = 1 << 2,
    MM_SIT_INDICATION_FILTER_LINK_CAPACITY        = 1 << 3,
    MM_SIT_INDICATION_FILTER_PHYSICAL_CHANNEL     = 1 << 4,
    MM_SIT_INDICATION_FILTER_REGISTRATION_FAILURE = 1 << 5,
    MM_SIT_INDICATION_FILTER_BARRING_INFO         = 1 << 6,
    /*
     * Stock sends 0xff, not 0x7f -- measured on identical firmware, every
     * SET_INDICATION_FILTER in the capture is `ff 00 00 00` on both stacks.
     * Six named bits plus bit 7, whose meaning the vendor headers do not give
     * a name; enabling it is what stock does, and an indication we do not
     * understand is ignored rather than harmful.
     */
    MM_SIT_INDICATION_FILTER_UNKNOWN_BIT7         = 1 << 7,
    MM_SIT_INDICATION_FILTER_ALL                   = 0xff,
} MMSitIndicationFilter;

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

/* One inclusive range of cell broadcast message identifiers, and whether the
 * CP should accept it. 4352-4356 are ETWS, 4370 onwards CMAS. */
typedef struct {
    guint16  from_service_id;
    guint16  to_service_id;
    guint8   from_code_scheme;
    guint8   to_code_scheme;
    gboolean selected;
} MMSitBroadcastRange;

/* A cell broadcast page as it arrived: a TS 23.041 CBS message, untouched. */
typedef struct {
    guint16 len;
    guint8  page[MM_SIT_MAX_CBS_PAGE];
} MMSitCbsPage;

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
/* Standalone 5G. An NSA carrier stays registered as LTE, which is what IMS
 * cares about anyway: the anchor is the LTE leg either way. */
#define MM_SIT_REGISTRATION_RAT_NR5G 20

typedef enum {
    MM_SIT_VOPS_SUPPORT_UNKNOWN,
    MM_SIT_VOPS_SUPPORT_UNSUPPORTED,
    MM_SIT_VOPS_SUPPORT_SUPPORTED,
} MMSitVopsSupport;

typedef struct {
    guint8   state;
    guint8   radio_technology;
    gboolean registered;
    MMSitVopsSupport vops_support;
    /*
     * What the serving cell offers for 5G NSA. 'endc_available' is the network
     * advertising EN-DC -- the UE may add a secondary NR carrier here -- and
     * 'dcnr_restricted' the network withdrawing it for this subscriber.
     * 'nr_available' is the CP's own summary of the two. None of them says an
     * NR carrier is actually up; that only shows in the signal report's RAT
     * mask, and only once there is data to carry.
     */
    gboolean endc_available;
    gboolean dcnr_restricted;
    gboolean nr_available;
    /*
     * The serving cell's identity, zero when the response does not carry one
     * (not registered, or a response too short to hold the field).
     */
    guint32  tac;
    guint32  cell_id;
} MMSitRegistration;

/* Reported for a cause the CP says it does not have. */
#define MM_SIT_REJECT_CAUSE_NONE 0xffff
/* 3GPP TS 24.301 EMM cause 10: the network has no context for this UE and
 * expects a fresh attach (TS 24.301 5.5.3.2.5). The one cause worth retrying. */
#define MM_SIT_REJECT_CAUSE_IMPLICITLY_DETACHED 10

/*
 * The cause SETUP_DATA_CALL comes back with when the CP will not create the
 * PDN at all: 0x8CD, which Android's DataFailCause calls
 * MATCH_ALL_RULE_NOT_ALLOWED -- a UE-side URSP decision rather than a network
 * rejection. This CP answers it for the IMS DNN specifically.
 */
#define MM_SIT_REJECT_CAUSE_MATCH_ALL_RULE_NOT_ALLOWED 2253

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

/* P-CSCF slots a data-call record carries, per family. Kept in step with the
 * codec's own by a static assert in mm-sit-protocol.c rather than by including
 * the FFI header here, which nearly every file would then pull in. */
#define MM_SIT_MAX_PCSCF 3

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
    /* SIP proxies the network sent in the PCO. Empty on an ordinary APN; an ims
     * context comes back with the set the CP must register through, and it does
     * not read them back for itself -- the AP hands them over in ADD_PDN_INFO. */
    guint8   pcscf_v4_count;
    gchar    pcscf_v4[MM_SIT_MAX_PCSCF][MM_SIT_IPV4_STRLEN];
    guint8   pcscf_v6_count;
    gchar    pcscf_v6[MM_SIT_MAX_PCSCF][MM_SIT_IPV6_STRLEN];
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
                                                       guint32                bearers,
                                                       const MMSitApnAuth    *auth,
                                                       GError               **error);
/* As above, with the carrier's roaming protocol named separately. Stock sends
 * it from the carrier database rather than copying the home protocol. */
GByteArray *mm_sit_request_new_set_data_profile_roaming (guint16          token,
                                                       const gchar         *apn,
                                                       MMSitPdpType         pdp,
                                                       MMSitPdpType         roaming_pdp,
                                                       guint32              apn_types,
                                                       guint32              bearers,
                                                       const MMSitApnAuth  *auth,
                                                       GError             **error);
GByteArray *mm_sit_request_new_set_initial_attach_apn (guint16                token,
                                                       const gchar           *apn,
                                                       MMSitPdpType           pdp,
                                                       const MMSitApnAuth    *auth,
                                                       GError               **error);
/* The context id a data call occupies. The CP echoes it back, and it is what
 * keeps two concurrent PDNs apart -- so once there is more than one bearer it
 * has to come from the profile manager rather than a constant. Until then every
 * call takes the same slot, which is what the plugin did implicitly before the
 * cid was a parameter at all. */
#define MM_SIT_DATA_CALL_CID_DEFAULT 1
GByteArray *mm_sit_request_new_setup_data_call        (guint16                token,
                                                       guint8                 cid,
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
GByteArray *mm_sit_request_new_dtmf                   (guint16                token,
                                                       const gchar           *digits,
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
GByteArray *mm_sit_request_new_oem_config             (guint16                token,
                                                       const gchar           *software_version,
                                                       GError               **error);
GByteArray *mm_sit_request_new_set_modem_config       (guint16                token);
GByteArray *mm_sit_request_new_set_modem_config_count (guint16 token, guint8 count);
/*
 * What the attach asks the network for at @0xdb. Stock asks for nothing: its CP
 * grants the IMS PDN on request, so the addresses arrive with that PDN. Ask for
 * both only where the attach is what creates the IMS context, because a later
 * SETUP_DATA_CALL for the same DNN returns the parameters the context was
 * created with -- too late to start asking.
 */
#define MM_SIT_PCSCF_REQUEST_NONE 0
#define MM_SIT_PCSCF_REQUEST_BOTH 3

GByteArray *mm_sit_request_new_set_initial_attach_apn_for_stack
                                                      (guint16 token, guint8 stack,
                                                       const gchar *apn, MMSitPdpType pdp,
                                                       guint8 pcscf,
                                                       const MMSitApnAuth *auth, GError **error);
GByteArray *mm_sit_request_new_device_info            (guint16                token,
                                                       const gchar           *model,
                                                       const gchar           *build,
                                                       const gchar           *codename,
                                                      const gchar           *sw_version,
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
GByteArray *mm_sit_request_new_get_facility_lock     (guint16                token,
                                                      guint8                 facility,
                                                      const guint8          *aid,
                                                      gsize                  aid_len);
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
GByteArray *mm_sit_request_new_get_broadcast_sms_config (guint16              token);
/* Pass ranges == NULL to ask for what that country accepts by default: real
 * alerts on, tests and exercises off. */
/* mcc is the country whose identifier assignment applies, 0 when unknown: a
 * category's channel moves between countries, so the same ranges do not mean
 * the same channels everywhere. */
GByteArray *mm_sit_request_new_set_broadcast_sms_config (guint16              token,
                                                       guint16                mcc,
                                                       const MMSitBroadcastRange *ranges,
                                                       guint                  n_ranges);
GByteArray *mm_sit_request_new_sms_broadcast_activation (guint16              token,
                                                       gboolean               activate);
gboolean mm_sit_message_parse_new_sms                 (const MMSitMessage    *message,
                                                       MMSitSmsDeliver       *out_deliver,
                                                       GError               **error);
gboolean mm_sit_message_parse_new_broadcast_sms       (const MMSitMessage    *message,
                                                       MMSitCbsPage          *out_page,
                                                       GError               **error);
/* out_ranges must have room for MM_SIT_MAX_BROADCAST_RANGES entries. A CP with
 * no config yet is not an error: it reports zero ranges. */
gboolean mm_sit_message_parse_broadcast_sms_config    (const MMSitMessage    *message,
                                                       MMSitBroadcastRange   *out_ranges,
                                                       guint                 *out_n_ranges,
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
/* How the CP is choosing a network. It reports Automatic out of reset, so an
 * explicit auto-select is usually asking for what is already happening. */
typedef enum {
    MM_SIT_NETWORK_SELECTION_AUTOMATIC = 0,
    MM_SIT_NETWORK_SELECTION_MANUAL    = 1,
} MMSitNetworkSelectionMode;

/* The CP's version block. Every field is NUL-terminated and empty when the CP
 * does not report it; 'sw_ver' is the firmware build, the same string the AT
 * service answers +CGMR with, and 'cp_chipset' is the baseband part. */
typedef struct {
    gchar sw_ver[MM_SIT_MAX_VERSION_FIELD];
    gchar hw_ver[MM_SIT_MAX_VERSION_FIELD];
    gchar rf_cal_date[MM_SIT_MAX_VERSION_FIELD];
    gchar prod_code[MM_SIT_MAX_VERSION_FIELD];
    gchar model_id[MM_SIT_MAX_VERSION_FIELD];
    gchar cp_chipset[MM_SIT_MAX_VERSION_FIELD];
} MMSitBaseBandVersion;

/* Per-RAT band lists from GET_MANUAL_BAND_MODE (0x0730). Band numbers are 3GPP.
 * 'payload' is the response's own bytes, kept because a SET request is built
 * *from* a read: the supported block is the CP's report of the part, not ours
 * to invent. 'current' is the selection when manual_mode is set and the
 * supported set otherwise. */
#define MM_SIT_MAX_BANDS        80
#define MM_SIT_MAX_BAND_PAYLOAD 87
typedef struct {
    gboolean manual_mode;
    guint8   supported_lte_len;
    guint16  supported_lte[MM_SIT_MAX_BANDS];
    guint8   supported_wcdma_len;
    guint16  supported_wcdma[MM_SIT_MAX_BANDS];
    guint8   current_lte_len;
    guint16  current_lte[MM_SIT_MAX_BANDS];
    guint8   current_wcdma_len;
    guint16  current_wcdma[MM_SIT_MAX_BANDS];
    guint8   payload[MM_SIT_MAX_BAND_PAYLOAD];
    gsize    payload_len;
} MMSitBandMode;

gboolean mm_sit_message_parse_band_mode              (const MMSitMessage    *message,
                                                      MMSitBandMode         *out_bands,
                                                      GError               **error);
/* Restrict the modem to exactly these bands, or lift any restriction when both
 * lists are empty. Everything not named is cleared: this is "use exactly
 * these", not a per-RAT overlay, which is the contract MM's set_current_bands
 * has. 'mode' must come from a preceding parse; see MMSitBandMode. */
GByteArray *mm_sit_request_new_set_band_mode         (guint16                token,
                                                      const MMSitBandMode   *mode,
                                                      const guint16         *lte,
                                                      gsize                  n_lte,
                                                      const guint16         *wcdma,
                                                      gsize                  n_wcdma,
                                                      GError               **error);

/* The struct versions the CP announces in its SET_DEVICE_INFO reply, with the
 * codec's verdict on each. A version of 0 means the CP did not name that
 * layout, which counts as supported: it has made no claim to check. */
typedef struct {
    guint8   cell_info;
    guint8   cell_identity;
    guint8   signal_strength;
    gboolean cell_info_supported;
    gboolean cell_identity_supported;
    gboolean signal_strength_supported;
} MMSitStructVersions;

gboolean mm_sit_message_parse_struct_versions         (const MMSitMessage    *message,
                                                       MMSitStructVersions   *out_versions,
                                                       GError               **error);

/* The selector is mandatory: a bare query leaves it off and the CP answers a
 * partly filled block. */
GByteArray *mm_sit_request_new_get_base_band_version  (guint16                token);
/* Which generations the modem *has*, as opposed to which it is allowed to use.
 * Unlike the allowed bitmap this does not move when the modes are restricted. */
gboolean mm_sit_message_parse_radio_capability        (const MMSitMessage      *message,
                                                       MMSitNetworkGenerations *out_generations,
                                                       GError                 **error);
gboolean mm_sit_message_parse_base_band_version       (const MMSitMessage    *message,
                                                       MMSitBaseBandVersion  *out_version,
                                                       GError               **error);
gboolean mm_sit_message_parse_network_selection_mode  (const MMSitMessage        *message,
                                                       MMSitNetworkSelectionMode *out_mode,
                                                       GError                   **error);
gboolean mm_sit_message_parse_imei                    (const MMSitMessage    *message,
                                                       gchar                **out_imei,
                                                       GError               **error);
/* The CP firmware's two software-version digits, off the tail of a GET_IMEISV
 * reply. NULL out on a CP whose identity is unprovisioned. */
gboolean mm_sit_message_parse_software_version        (const MMSitMessage    *message,
                                                       gchar                **out_version,
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
/*
 * The CP's verdict on the carrier configuration it was pointed at, pushed
 * after every CARRIER_CONFIG_MANIFEST. @applied is the first word: non-zero
 * once the CP has fetched and applied the manifest, zero while it has not.
 *
 * Reading it matters because nothing else reports this. The manifest request
 * itself is answered "status 0" -- the request was well formed -- and the CP
 * then says separately whether it did anything with it.
 *
 * Zero is not a failure: it means the CP already holds that manifest and had
 * nothing to fetch, which is what it reports on every boot after the first.
 * Handed a hash it does not have, it opens <dir>/manifests/<hash> over RFS
 * within a second and then every confseq the manifest names.
 */
gboolean mm_sit_message_parse_carrier_config_status (const MMSitMessage *message,
                                                     gboolean           *out_applied,
                                                     GError            **error);

gboolean mm_sit_message_parse_carrier_sim_info        (const MMSitMessage    *message,
                                                       MMSitCarrierSimInfo   *out_info,
                                                       GError               **error);
gboolean mm_sit_message_parse_sim_slot_status         (const MMSitMessage    *message,
                                                       MMSitSimSlotStatus    *out_status,
                                                       GError               **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitMessage, mm_sit_message_free)

#endif /* MM_SIT_PROTOCOL_H */
