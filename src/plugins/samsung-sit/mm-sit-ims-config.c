/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <string.h>

#include "mm-sit-ims-config.h"

/*
 * Tag numbers are HiddenMenuBuilder's own, recovered from ShannonIms.apk; the
 * decode is in research/sit-protocol.md. Only the ones a registration and a
 * voice call depend on are here -- the RCS, video and Wi-Fi-calling families
 * exist and are deliberately absent.
 */
enum {
    TAG_SIP_SERVER_PORT_NUMBER            = 10,
    TAG_REGISTRATION_EXPIRY_TIMER_SEC     = 11,
    TAG_IPSEC_ENCRYPTION_ALGORITHM        = 15,
    TAG_IPSEC_AUTHENTICATION_ALGORITHM    = 16,
    TAG_REGISTRATION_RETRY_BASE_TIMER_MS  = 20,
    TAG_REGISTRATION_RETRY_MAX_TIMER_MS   = 21,
    TAG_AUDIO_CODEC_CAPABILITY            = 258,
    TAG_SESSION_EXPIRES_TIMER_SEC         = 517,
    TAG_USER_AGENT                        = 519,
    TAG_SIP_TIMER_TA_MS                   = 523,
    TAG_SIP_TIMER_TB_MS                   = 524,
    TAG_SIP_TIMER_TC_MS                   = 525,
    TAG_SIP_TIMER_TD_MS                   = 526,
    TAG_SIP_TIMER_TE_MS                   = 527,
    TAG_SIP_TIMER_TF_MS                   = 528,
    TAG_SIP_TIMER_TG_MS                   = 529,
    TAG_SIP_TIMER_TH_MS                   = 530,
    TAG_SIP_TIMER_TI_MS                   = 531,
    TAG_SIP_TIMER_TJ_MS                   = 532,
    TAG_SIP_TIMER_TK_MS                   = 533,
    TAG_SIP_TIMER_T1_MS                   = 534,
    TAG_SIP_TIMER_T2_MS                   = 535,
    TAG_SIP_TIMER_T4_MS                   = 536,
    TAG_SESSION_REFRESHER_TYPE            = 539,
    TAG_UA_MANUFACTURER                   = 772,
    TAG_UA_MODEL                          = 773,
    TAG_UA_SOFTWARE_VERSION               = 774,
    TAG_UA_PRODUCT_LINE                   = 775,
    TAG_UA_OS_VERSION                     = 776,
    TAG_PHONE_CONTEXT_URI                 = 1025,
    TAG_SIP_URI_TYPE                      = 1026,
    TAG_MIN_SESSION_EXPIRES_TIMER_SEC     = 1027,
    TAG_IPV4_MTU_SIZE                     = 1028,
    TAG_IPV6_MTU_SIZE                     = 1029,
    TAG_PRACK_ENABLED_FOR_18X             = 1030,
    TAG_SIP_OVER_IPSEC_ENABLED            = 1031,
    TAG_CONFERENCE_FACTORY_URI            = 1032,
    TAG_CONFERENCE_SUBSCRIBE_TYPE         = 1033,
    TAG_FLASH_SIP_TIMER_TQOS_MS           = 1034,
    TAG_AUDIO_RTP_INACTIVITY_TIMER_MS     = 1035,
    TAG_RINGBACK_TIMER_MS                 = 1036,
    TAG_RINGING_TIMER_MS                  = 1037,
    TAG_XCAP_AS_SERVER_FQDN               = 1038,
    TAG_XCAP_AS_SERVER_PORT               = 1039,
    TAG_XCAP_AS_TRANSPORT_TYPE            = 1040,
    TAG_XCAP_BSF_SERVER_FQDN              = 1041,
    TAG_XCAP_BSF_SERVER_PORT              = 1042,
    TAG_XCAP_BSF_TRANSPORT_TYPE           = 1043,
    TAG_GEOLOCATION_VOWIFI_EMERGENCY      = 1044,
    TAG_GEOLOCATION_VOLTE_EMERGENCY       = 1045,
    TAG_GRUU_ENABLED                      = 1046,
    TAG_REG_SUBSCRIBE_EXPIRY_TIMER_SEC    = 1048,
    TAG_SESSION_PRIVACY_TYPE              = 1050,
    TAG_VOICE_QOS_PRECONDITION_ENABLED    = 1051,
    TAG_MEDIA_ALLOWED_ON_DEFAULT_BEARER   = 1052,
    TAG_SRVCC_SUPPORT_ENABLED             = 1055,
    TAG_AUDIO_INACTIVITY_HANDLING         = 1056,
    TAG_AUDIO_RTCP_INACTIVITY_TIMER_MS    = 1057,
    TAG_TIMER_MO_REQUEST_TIMEOUT_MS       = 1058,
    TAG_OIP_SOURCE_FROM_HEADER            = 1059,
    TAG_AMR_WB_PAYLOAD_DESCRIPTION        = 1060,
    TAG_AMR_NB_PAYLOAD_DESCRIPTION        = 1061,
    TAG_EVS_PAYLOAD_DESCRIPTION           = 1062,
    TAG_TEXT_CODEC_CAPABILITY             = 1064,
    TAG_TEXT_AS_BANDWIDTH                 = 1065,
    TAG_TEXT_RR_BANDWIDTH                 = 1066,
    TAG_TEXT_RS_BANDWIDTH                 = 1067,
    TAG_NETWORK_INITIATED_USSD_SUPPORTED  = 1073,
    TAG_DTMF_PAYLOAD_DESCRIPTION          = 1074,
    TAG_AUDIO_AS_BANDWIDTH                = 1075,
    TAG_AUDIO_RS_BANDWIDTH                = 1076,
    TAG_AUDIO_RR_BANDWIDTH                = 1077,
    TAG_EPSFB_SILENT_RETRY_ERROR          = 1080,
    TAG_VOLTE_VOWIFI_SILENT_RETRY         = 1081,
    TAG_VOLTE_VOWIFI_RETRY_ERROR_CODE     = 1082,
    TAG_SIPMSG_MTU_CHECK                  = 1083,
    TAG_RTT_SUPPORTED                     = 1084,
};

struct _MMSitImsConfig {
    GArray      *entries;   /* SamsungAimsConfigEntry, pointers into `strings` */
    GPtrArray   *strings;   /* owned gchar* and GBytes the entries reference */
    const gchar *carrier;
    gchar       *user_agent;
};

typedef struct {
    guint16 tag;
    guint32 value;
} IntSetting;

typedef struct {
    guint16      tag;
    const gchar *value;
} StrSetting;

typedef struct {
    guint16       tag;
    const guint8 *value;
    gsize         len;
} BytesSetting;

/*
 * AOSP defaults, decoded whole from a captured HIDDEN_MENU frame rather than
 * transcribed from documentation -- so these are values the CP has been seen to
 * accept, not values that ought to work.
 */
static const IntSetting default_ints[] = {
    { TAG_SESSION_EXPIRES_TIMER_SEC,        1800 },
    { TAG_SIP_URI_TYPE,                     1 },
    { TAG_MIN_SESSION_EXPIRES_TIMER_SEC,    90 },
    { TAG_IPV4_MTU_SIZE,                    1500 },
    { TAG_IPV6_MTU_SIZE,                    1500 },
    { TAG_PRACK_ENABLED_FOR_18X,            0 },
    { TAG_SIP_OVER_IPSEC_ENABLED,           1 },
    { TAG_CONFERENCE_SUBSCRIBE_TYPE,        1 },
    { TAG_FLASH_SIP_TIMER_TQOS_MS,          8000 },
    { TAG_AUDIO_RTP_INACTIVITY_TIMER_MS,    20000 },
    { TAG_RINGBACK_TIMER_MS,                90000 },
    { TAG_RINGING_TIMER_MS,                 90000 },
    { TAG_XCAP_AS_SERVER_PORT,              80 },
    { TAG_XCAP_AS_TRANSPORT_TYPE,           0 },
    { TAG_XCAP_BSF_SERVER_PORT,             80 },
    { TAG_XCAP_BSF_TRANSPORT_TYPE,          0 },
    { TAG_SIP_SERVER_PORT_NUMBER,           5060 },
    { TAG_SIP_TIMER_T1_MS,                  2000 },
    { TAG_SIP_TIMER_T2_MS,                  16000 },
    { TAG_SIP_TIMER_T4_MS,                  17000 },
    { TAG_SIP_TIMER_TA_MS,                  30 },
    { TAG_SIP_TIMER_TB_MS,                  128000 },
    { TAG_SIP_TIMER_TC_MS,                  210000 },
    { TAG_SIP_TIMER_TD_MS,                  130000 },
    { TAG_SIP_TIMER_TE_MS,                  30 },
    { TAG_SIP_TIMER_TF_MS,                  128000 },
    { TAG_SIP_TIMER_TG_MS,                  30 },
    { TAG_SIP_TIMER_TH_MS,                  128000 },
    { TAG_SIP_TIMER_TI_MS,                  30 },
    { TAG_SIP_TIMER_TJ_MS,                  128000 },
    { TAG_SIP_TIMER_TK_MS,                  30 },
    { TAG_GRUU_ENABLED,                     0 },
    { TAG_IPSEC_AUTHENTICATION_ALGORITHM,   6 },
    { TAG_IPSEC_ENCRYPTION_ALGORITHM,       7 },
    { TAG_REGISTRATION_EXPIRY_TIMER_SEC,    600000 },
    { TAG_REGISTRATION_RETRY_BASE_TIMER_MS, 30000 },
    { TAG_REGISTRATION_RETRY_MAX_TIMER_MS,  1800000 },
    { TAG_REG_SUBSCRIBE_EXPIRY_TIMER_SEC,   600000 },
    { TAG_SESSION_REFRESHER_TYPE,           1 },
    { TAG_SESSION_PRIVACY_TYPE,             0 },
    { TAG_VOICE_QOS_PRECONDITION_ENABLED,   15 },
    { TAG_MEDIA_ALLOWED_ON_DEFAULT_BEARER,  0 },
    { TAG_SRVCC_SUPPORT_ENABLED,            15 },
    { TAG_AUDIO_INACTIVITY_HANDLING,        21 },
    { TAG_AUDIO_RTCP_INACTIVITY_TIMER_MS,   20000 },
    { TAG_TIMER_MO_REQUEST_TIMEOUT_MS,      5000 },
    { TAG_OIP_SOURCE_FROM_HEADER,           0 },
    { TAG_AUDIO_CODEC_CAPABILITY,           5 },
    { TAG_AUDIO_AS_BANDWIDTH,               41 },
    { TAG_AUDIO_RR_BANDWIDTH,               2000 },
    { TAG_AUDIO_RS_BANDWIDTH,               600 },
    { TAG_TEXT_AS_BANDWIDTH,                4 },
    { TAG_TEXT_RR_BANDWIDTH,                51 },
    { TAG_TEXT_RS_BANDWIDTH,                153 },
    { TAG_NETWORK_INITIATED_USSD_SUPPORTED, 1 },
    { TAG_EPSFB_SILENT_RETRY_ERROR,         0 },
    { TAG_VOLTE_VOWIFI_SILENT_RETRY,        0 },
    { TAG_VOLTE_VOWIFI_RETRY_ERROR_CODE,    0 },
    { TAG_SIPMSG_MTU_CHECK,                 1 },
    { TAG_RTT_SUPPORTED,                    1 },
    { TAG_GEOLOCATION_VOWIFI_EMERGENCY,     1 },
    { TAG_GEOLOCATION_VOLTE_EMERGENCY,      0 },
};

/* An empty string is a real value here, not an omission: the vendor emits the
 * tag with a bare "##" terminator, and the CP distinguishes that from silence. */
static const StrSetting default_strings[] = {
    { TAG_PHONE_CONTEXT_URI,   "" },
    { TAG_CONFERENCE_FACTORY_URI, "" },
    { TAG_XCAP_AS_SERVER_FQDN, "" },
    { TAG_XCAP_BSF_SERVER_FQDN, "" },
};

/* Codec descriptions: payload type, then the vendor's own attribute bytes. */
static const guint8 default_amr_wb[] = { 0x64, 0x01, 0xff, 0x00, 0x69, 0x02, 0xff, 0x00 };
static const guint8 default_amr_nb[] = { 0x62, 0x01, 0xff, 0x00, 0x67, 0x02, 0xff, 0x00 };
static const guint8 default_evs[]    = { 0x7f, 0x06, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
                                         0x0f, 0x0f, 0x0f, 0x01, 0x01, 0xff, 0xff, 0xff };
static const guint8 default_text[]   = { 0x6f, 0x70 };
static const guint8 default_dtmf[]   = { 0x60, 0x61 };

static const BytesSetting default_bytes[] = {
    { TAG_AMR_WB_PAYLOAD_DESCRIPTION, default_amr_wb, sizeof (default_amr_wb) },
    { TAG_AMR_NB_PAYLOAD_DESCRIPTION, default_amr_nb, sizeof (default_amr_nb) },
    { TAG_EVS_PAYLOAD_DESCRIPTION,    default_evs,    sizeof (default_evs) },
    { TAG_TEXT_CODEC_CAPABILITY,      default_text,   sizeof (default_text) },
    { TAG_DTMF_PAYLOAD_DESCRIPTION,   default_dtmf,   sizeof (default_dtmf) },
};

/*
 * o2 Germany. Every value here is a real override in the captured frame, not a
 * restatement of a default -- notably the 1300-byte SIP MTU and the 32 s
 * timers F and J, which are what this network wants instead of AOSP's.
 */
static const guint8 o2_amr_wb[] = { 0x68, 0x01, 0xff, 0x00 };
static const guint8 o2_amr_nb[] = { 0x66, 0x01, 0xff, 0x00 };
static const guint8 o2_evs[]    = { 0x7e, 0x06, 0x01, 0x00, 0x07, 0x00, 0x02, 0x00,
                                    0x0f, 0x01, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff };

static const IntSetting o2_ints[] = {
    { TAG_MIN_SESSION_EXPIRES_TIMER_SEC,    600 },
    { TAG_IPV4_MTU_SIZE,                    1300 },
    { TAG_IPV6_MTU_SIZE,                    1300 },
    { TAG_PRACK_ENABLED_FOR_18X,            1 },
    { TAG_AUDIO_RTP_INACTIVITY_TIMER_MS,    10000 },
    { TAG_RINGBACK_TIMER_MS,                180000 },
    { TAG_RINGING_TIMER_MS,                 180000 },
    { TAG_SIP_TIMER_TF_MS,                  32000 },
    { TAG_SIP_TIMER_TJ_MS,                  32000 },
    { TAG_SESSION_REFRESHER_TYPE,           255 },
    { TAG_OIP_SOURCE_FROM_HEADER,           1 },
    { TAG_NETWORK_INITIATED_USSD_SUPPORTED, 0 },
};

static const StrSetting o2_strings[] = {
    { TAG_CONFERENCE_FACTORY_URI, "teleconferenceuri-factory@telefonica.de" },
};

static const BytesSetting o2_bytes[] = {
    { TAG_AMR_WB_PAYLOAD_DESCRIPTION, o2_amr_wb, sizeof (o2_amr_wb) },
    { TAG_AMR_NB_PAYLOAD_DESCRIPTION, o2_amr_nb, sizeof (o2_amr_nb) },
    { TAG_EVS_PAYLOAD_DESCRIPTION,    o2_evs,    sizeof (o2_evs) },
};

typedef struct {
    const gchar        *name;
    /* MCCMNCs this record covers, NULL-terminated. */
    const gchar *const *mccmncs;
    /* The certification suffix the carrier expects in the User-Agent. */
    const gchar        *ua_suffix;
    const IntSetting   *ints;
    gsize               n_ints;
    const StrSetting   *strings;
    gsize               n_strings;
    const BytesSetting *bytes;
    gsize               n_bytes;
} CarrierOverride;

static const gchar *const o2_de_mccmncs[] = { "26203", "26207", "26208", "26211", NULL };

static const CarrierOverride carriers[] = {
    {
        .name = "o2_de",
        .mccmncs = o2_de_mccmncs,
        .ua_suffix = "EU01454",
        .ints = o2_ints, .n_ints = G_N_ELEMENTS (o2_ints),
        .strings = o2_strings, .n_strings = G_N_ELEMENTS (o2_strings),
        .bytes = o2_bytes, .n_bytes = G_N_ELEMENTS (o2_bytes),
    },
};

/* Replace an already-emitted tag, or append. Overrides arrive after the
 * defaults and must not leave two entries for one tag -- the CP would take the
 * second, which happens to be right, but only by accident. */
static void
set_entry (MMSitImsConfig             *self,
           const SamsungAimsConfigEntry *entry)
{
    guint i;

    for (i = 0; i < self->entries->len; i++) {
        SamsungAimsConfigEntry *existing;

        existing = &g_array_index (self->entries, SamsungAimsConfigEntry, i);
        if (existing->tag == entry->tag) {
            *existing = *entry;
            return;
        }
    }
    g_array_append_vals (self->entries, entry, 1);
}

static void
set_int (MMSitImsConfig *self,
         guint16         tag,
         guint32         value)
{
    SamsungAimsConfigEntry e = { 0 };

    e.tag = tag;
    e.value_type = SAMSUNG_AIMS_VALUE_TYPE_INT;
    e.int_value = value;
    set_entry (self, &e);
}

static void
set_string (MMSitImsConfig *self,
            guint16         tag,
            const gchar    *value)
{
    SamsungAimsConfigEntry e = { 0 };
    gchar                 *owned;

    owned = g_strdup (value ? value : "");
    g_ptr_array_add (self->strings, owned);
    e.tag = tag;
    e.value_type = SAMSUNG_AIMS_VALUE_TYPE_STR;
    e.str_value = owned;
    set_entry (self, &e);
}

static void
set_bytes (MMSitImsConfig *self,
           guint16         tag,
           const guint8   *value,
           gsize           len)
{
    SamsungAimsConfigEntry e = { 0 };

    e.tag = tag;
    e.value_type = SAMSUNG_AIMS_VALUE_TYPE_BYTES;
    /* Static tables, so no copy is needed and none is made; the entry borrows
     * for as long as the process lives. */
    e.bytes_value = value;
    e.bytes_len = len;
    set_entry (self, &e);
}

static const CarrierOverride *
find_carrier (const gchar *mccmnc)
{
    guint i;

    if (!mccmnc || !mccmnc[0])
        return NULL;

    for (i = 0; i < G_N_ELEMENTS (carriers); i++) {
        const gchar *const *m;

        for (m = carriers[i].mccmncs; *m; m++) {
            if (g_str_equal (*m, mccmnc))
                return &carriers[i];
        }
    }
    return NULL;
}

MMSitImsConfig *
mm_sit_ims_config_resolve (const MMSitImsDeviceIdentity *device,
                           const gchar                  *mccmnc)
{
    MMSitImsConfig        *self;
    const CarrierOverride *carrier;
    guint                  i;

    g_return_val_if_fail (device != NULL, NULL);

    self = g_new0 (MMSitImsConfig, 1);
    self->entries = g_array_new (FALSE, TRUE, sizeof (SamsungAimsConfigEntry));
    self->strings = g_ptr_array_new_with_free_func (g_free);
    self->carrier = "default";

    for (i = 0; i < G_N_ELEMENTS (default_ints); i++)
        set_int (self, default_ints[i].tag, default_ints[i].value);
    for (i = 0; i < G_N_ELEMENTS (default_strings); i++)
        set_string (self, default_strings[i].tag, default_strings[i].value);
    for (i = 0; i < G_N_ELEMENTS (default_bytes); i++)
        set_bytes (self, default_bytes[i].tag, default_bytes[i].value, default_bytes[i].len);

    carrier = find_carrier (mccmnc);
    if (carrier) {
        self->carrier = carrier->name;
        for (i = 0; i < carrier->n_ints; i++)
            set_int (self, carrier->ints[i].tag, carrier->ints[i].value);
        for (i = 0; i < carrier->n_strings; i++)
            set_string (self, carrier->strings[i].tag, carrier->strings[i].value);
        for (i = 0; i < carrier->n_bytes; i++)
            set_bytes (self, carrier->bytes[i].tag, carrier->bytes[i].value, carrier->bytes[i].len);
    }

    /*
     * The device identity, and the User-Agent assembled from it. The CP wants
     * both: the parts, and the finished string it puts in REGISTER -- it does
     * not assemble one for itself, which is why an unprovisioned CP cannot
     * register at all.
     */
    set_string (self, TAG_UA_MANUFACTURER, device->manufacturer);
    set_string (self, TAG_UA_MODEL, device->model);
    set_string (self, TAG_UA_SOFTWARE_VERSION, device->software_version);
    set_string (self, TAG_UA_PRODUCT_LINE, "SH000000");
    set_string (self, TAG_UA_OS_VERSION, device->os_version);

    /*
     * "<manufacturer> <model> <os> <version> <build> [suffix]", the shape the
     * vendor template expands to. The device tree names the model with its
     * vendor already in it ("Google Pixel 9a") while the template wants them
     * separately, so a repeated vendor is dropped rather than emitted twice.
     */
    {
        const gchar *model = device->model ?: "";
        const gchar *vendor = device->manufacturer ?: "";
        gsize        vendor_len = strlen (vendor);

        if (vendor_len && g_ascii_strncasecmp (model, vendor, vendor_len) == 0 &&
            model[vendor_len] == ' ')
            model += vendor_len + 1;

        self->user_agent = g_strdup_printf ("%s %s %s %s %s%s%s",
                                            vendor,
                                            model,
                                            device->os_name ?: "",
                                            device->os_version ?: "",
                                            device->software_version ?: "",
                                            (carrier && carrier->ua_suffix) ? " " : "",
                                            (carrier && carrier->ua_suffix) ? carrier->ua_suffix : "");
    }
    set_string (self, TAG_USER_AGENT, self->user_agent);

    return self;
}

void
mm_sit_ims_config_free (MMSitImsConfig *self)
{
    if (!self)
        return;
    g_array_unref (self->entries);
    g_ptr_array_unref (self->strings);
    g_free (self->user_agent);
    g_free (self);
}

const SamsungAimsConfigEntry *
mm_sit_ims_config_peek_entries (const MMSitImsConfig *self,
                                gsize                *out_n_entries)
{
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (out_n_entries != NULL, NULL);

    *out_n_entries = self->entries->len;
    return (const SamsungAimsConfigEntry *) self->entries->data;
}

const gchar *
mm_sit_ims_config_get_carrier (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->carrier;
}

const gchar *
mm_sit_ims_config_get_user_agent (const MMSitImsConfig *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->user_agent;
}
