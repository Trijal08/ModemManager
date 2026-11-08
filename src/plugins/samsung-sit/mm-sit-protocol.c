/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

/*
 * Thin adapter over the samsung-sit codec (Rust, libsamsung_sit_ffi). The wire
 * format lives in that crate; this file only bridges its GLib-free C ABI
 * (byte-buffer out-params, status codes, #[repr(C)] structs) to the
 * GByteArray/GError/GDateTime shapes the rest of the plugin expects. The
 * MMSitMessage container and the message-id table stay here because they are
 * pure GLib glue with no codec equivalent.
 */

#include <arpa/inet.h>
#include <string.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-common-helpers.h"
#include "mm-sit-protocol.h"

#include <samsung_sit_ffi.h>

static guint16
read_le16 (const guint8 *data)
{
    return ((guint16) data[0]) | (((guint16) data[1]) << 8);
}

static guint32
read_le32 (const guint8 *data)
{
    return ((guint32) data[0]) | (((guint32) data[1]) << 8) |
           (((guint32) data[2]) << 16) | (((guint32) data[3]) << 24);
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

    if (self->type == MM_SIT_MESSAGE_TYPE_RESPONSE && data_len > MM_SIT_STATUS_OFFSET) {
        /*
         * The status is the single byte at @10, right after the 10-byte header
         * (the token is 32 bits, so @8/@9 are its high half and always zero --
         * reading a status there is why a rejected request used to look like a
         * successful one).  The RIL maps this byte to an Android RIL_Errno;
         * 2 is GENERIC_FAILURE.
         */
        self->has_status = TRUE;
        self->status = data[MM_SIT_STATUS_OFFSET];
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
    /* The table lives in the codec, not here: SIT and AIMS ids share one
     * number space, sit-proxy and mm-sit-tool need the same names, and the
     * three separate tables that used to exist disagreed on 22 ids while two
     * of them were unreachable from the tools whose traces most needed them. */
    const gchar *name = samsung_sit_message_name (id);

    return name ?: "UNKNOWN";
}


/* Decode the three packed PLMN bytes used by EF_PLMNwAcT, EF_FPLMN and friends,
 * TS 24.008 10.5.1.13: the MCC digits are the low then high nibble of the first
 * byte and the low nibble of the second, the MNC's *third* digit is the high
 * nibble of the second byte -- 0xf when the MNC has only two -- and its first
 * two are the low then high nibble of the third. So the digits do not come out
 * in reading order and a plain BCD conversion mixes the MNC up. Returns NULL for
 * an unused entry (all ones) or any non-digit nibble. */
gchar *
mm_sit_plmn_from_packed (const guint8 *entry)
{
    guint8 digits[6];
    gchar  plmn[7];
    guint  i;
    guint  len;

    g_return_val_if_fail (entry != NULL, NULL);

    digits[0] = entry[0] & 0x0f;
    digits[1] = entry[0] >> 4;
    digits[2] = entry[1] & 0x0f;
    digits[3] = entry[2] & 0x0f;
    digits[4] = entry[2] >> 4;
    digits[5] = entry[1] >> 4;

    len = (digits[5] == 0x0f) ? 5 : 6;
    for (i = 0; i < len; i++) {
        if (digits[i] > 9)
            return NULL;
        plmn[i] = '0' + digits[i];
    }
    plmn[len] = '\0';
    return g_strdup (plmn);
}

/*****************************************************************************/
/* FFI bridging helpers                                                      */

static GIOErrorEnum
ffi_status_to_gio (SamsungSitStatus status)
{
    /* Map codec status codes to the GError codes callers expect; cast away the
     * enum type so -Wswitch-enum does not demand every value. */
    switch ((int) status) {
    case SAMSUNG_SIT_STATUS_APN_REQUIRED:
    case SAMSUNG_SIT_STATUS_ARG_TOO_LONG:
    case SAMSUNG_SIT_STATUS_ARG_OUT_OF_RANGE:
    case SAMSUNG_SIT_STATUS_UNEXPECTED_MESSAGE:
    case SAMSUNG_SIT_STATUS_NOT_A_RESPONSE:
    case SAMSUNG_SIT_STATUS_NOT_AN_INDICATION:
    case SAMSUNG_SIT_STATUS_NULL_POINTER:
    case SAMSUNG_SIT_STATUS_UTF8:
        return G_IO_ERROR_INVALID_ARGUMENT;
    case SAMSUNG_SIT_STATUS_CP_STATUS:
        return G_IO_ERROR_FAILED;
    case SAMSUNG_SIT_STATUS_UNSUPPORTED:
        return G_IO_ERROR_NOT_SUPPORTED;
    default:
        /* Truncated, TooManySlots, FieldTooLong, Malformed, Binrw. */
        return G_IO_ERROR_INVALID_DATA;
    }
}

static void
set_ffi_error (GError          **error,
               SamsungSitStatus  status)
{
    const char *message = samsung_sit_last_error_message ();

    g_set_error (error, G_IO_ERROR, ffi_status_to_gio (status), "%s",
                 message ? message : samsung_sit_status_name (status));
}

/* Wrap a codec-built frame in a GByteArray, or set @error and return NULL. */
static GByteArray *
finish_build (SamsungSitStatus  status,
              guint8           *buffer,
              gsize             len,
              GError          **error)
{
    GByteArray *request;

    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return NULL;
    }

    request = g_byte_array_sized_new (len);
    g_byte_array_append (request, buffer, len);
    samsung_sit_buf_free (buffer, len);
    return request;
}

static void
format_ipv4 (gchar        destination[MM_SIT_IPV4_STRLEN],
             const guint8 address[4])
{
    g_snprintf (destination, MM_SIT_IPV4_STRLEN, "%u.%u.%u.%u",
                address[0], address[1], address[2], address[3]);
}

static void
format_ipv6 (gchar        destination[MM_SIT_IPV6_STRLEN],
             const guint8 address[16])
{
    if (!inet_ntop (AF_INET6, address, destination, MM_SIT_IPV6_STRLEN))
        destination[0] = '\0';
}

G_STATIC_ASSERT (MM_SIT_MAX_PCSCF == SAMSUNG_SIT_PCSCF_MAX);

/* Reshape one codec data call into the plugin's. Both families come from the
 * same record: the CP fills the v4 slots, the v6 slots, or both. */
static void
fill_data_call (MMSitDataCall            *out,
                const SamsungSitDataCall *dc)
{
    guint i;

    memset (out, 0, sizeof (*out));
    out->cause  = dc->cause;
    out->cid    = dc->cid;
    out->active = dc->active;
    out->mtu    = (guint16) dc->mtu;

    if (dc->address.has_v4) {
        out->has_ipv4 = TRUE;
        format_ipv4 (out->ipv4, dc->address.v4);
    }
    if (dc->dns[0].has_v4)
        format_ipv4 (out->dns1, dc->dns[0].v4);
    if (dc->dns[1].has_v4)
        format_ipv4 (out->dns2, dc->dns[1].v4);

    if (dc->address.has_v6) {
        out->has_ipv6 = TRUE;
        format_ipv6 (out->ipv6, dc->address.v6);
    }
    if (dc->dns[0].has_v6)
        format_ipv6 (out->dns1_v6, dc->dns[0].v6);
    if (dc->dns[1].has_v6)
        format_ipv6 (out->dns2_v6, dc->dns[1].v6);

    out->pcscf_v4_count = MIN (dc->pcscf_v4_count, MM_SIT_MAX_PCSCF);
    for (i = 0; i < out->pcscf_v4_count; i++)
        format_ipv4 (out->pcscf_v4[i], dc->pcscf_v4[i]);
    out->pcscf_v6_count = MIN (dc->pcscf_v6_count, MM_SIT_MAX_PCSCF);
    for (i = 0; i < out->pcscf_v6_count; i++)
        format_ipv6 (out->pcscf_v6[i], dc->pcscf_v6[i]);
}


/*****************************************************************************/
/* Request builders                                                          */

GByteArray *
mm_sit_request_new (guint16        id,
                    guint16        token,
                    const guint8  *body,
                    gsize          body_len,
                    GError       **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_request (id, token, body, body_len, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_query (guint16 id,
                          guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_query (id, token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_radio_power (guint16         token,
                                    MMSitRadioState state)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_radio_power (token, (SamsungSitRadioState) state,
                                                &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_preferred_modem (guint16 token,
                                        guint8  modem_index)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_preferred_modem (token, modem_index, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_logical_slot_mapping (guint16                        token,
                                             const MMSitLogicalSlotMapping *mappings,
                                             guint                          mapping_count,
                                             GError                       **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    /* MMSitLogicalSlotMapping and SamsungSitLogicalSlotMapping share a layout. */
    status = samsung_sit_build_set_logical_slot_mapping (
        token, (const SamsungSitLogicalSlotMapping *) mappings, mapping_count,
        &buffer, &len);
    return finish_build (status, buffer, len, error);
}

/* Which families to ask the network for. A connection that states no
 * preference gets a dual-stack request -- the CP narrows it to whatever the
 * network grants, so asking for both costs nothing on a v4-only APN. */
MMSitPdpType
mm_sit_pdp_type_from_ip_family (MMBearerIpFamily family)
{
    if (family == MM_BEARER_IP_FAMILY_IPV4)
        return MM_SIT_PDP_TYPE_IPV4;
    if (family == MM_BEARER_IP_FAMILY_IPV6)
        return MM_SIT_PDP_TYPE_IPV6;
    return MM_SIT_PDP_TYPE_IPV4V6;
}

/* Credentials out of a connection's properties, in the CP's own terms.
 *
 * The CP takes a single choice where ModemManager offers a set, so the set has
 * to be narrowed: PAP and CHAP together become "let the network pick", and
 * either alone becomes itself. MSCHAP, MSCHAPv2 and EAP have no representation
 * here at all -- asking for only those is refused rather than quietly
 * downgraded, because sending PAP to a caller who explicitly refused it would
 * put the password on the wire in a form they rejected.
 *
 * An unstated preference is the common case: NetworkManager leaves the mask
 * unknown unless the user touches it. Take that as "whatever the network
 * wants" when there are credentials to send, and as no authentication at all
 * when there are not. */
gboolean
mm_sit_apn_auth_from_properties (MMBearerProperties *properties,
                                                MMSitApnAuth       *out,
                                                GError            **error)
{
    MMBearerAllowedAuth allowed;
    gboolean            pap;
    gboolean            chap;

    out->user = mm_bearer_properties_get_user (properties);
    out->password = mm_bearer_properties_get_password (properties);
    allowed = mm_bearer_properties_get_allowed_auth (properties);

    /* Credentials decide before the mask does. NetworkManager sends a mask
     * allowing everything it knows unless the user refused something, so an
     * ordinary connection with no username arrives asking for PAP or CHAP with
     * nothing to present -- and the vendor sends 0 for exactly that case.
     * mm-bearer-mbim.c guards it the same way, and in the same order. */
    if (!out->user && !out->password) {
        out->auth = MM_SIT_AUTH_TYPE_NONE;
        return TRUE;
    }

    if (allowed == MM_BEARER_ALLOWED_AUTH_UNKNOWN) {
        out->auth = MM_SIT_AUTH_TYPE_PAP_OR_CHAP;
        return TRUE;
    }

    if (allowed == MM_BEARER_ALLOWED_AUTH_NONE) {
        out->auth = MM_SIT_AUTH_TYPE_NONE;
        return TRUE;
    }

    pap = !!(allowed & MM_BEARER_ALLOWED_AUTH_PAP);
    chap = !!(allowed & MM_BEARER_ALLOWED_AUTH_CHAP);
    if (pap && chap)
        out->auth = MM_SIT_AUTH_TYPE_PAP_OR_CHAP;
    else if (pap)
        out->auth = MM_SIT_AUTH_TYPE_PAP;
    else if (chap)
        out->auth = MM_SIT_AUTH_TYPE_CHAP;
    else {
        g_autofree gchar *str = NULL;

        str = mm_bearer_allowed_auth_build_string_from_mask (allowed);
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                     "This modem supports only PAP and CHAP, not '%s'", str);
        return FALSE;
    }
    return TRUE;
}

/* What a context is for, in the CP's terms.
 *
 * ModemManager models this as a set and so does the CP, so this is a straight
 * bit-for-bit translation of the types both understand. SUPL and HIPRI have no
 * MM equivalent and nothing maps onto them: the vendor reads those off its APN
 * database entry, which is exactly the thing we do not have.
 *
 * Nothing populates the property today -- NetworkManager never sets it -- so in
 * practice this returns the default. That follows what ModemManager already
 * does where a modem insists on a type it was not told: mm-bearer-mbim.c takes
 * the profile's type and falls back to MM_BEARER_APN_TYPE_DEFAULT, and
 * mm_base_bearer_get_apn_type says the same in its own words, "when none
 * explicitly requested, apn type always defaults to internet". */
guint32
mm_sit_apn_types_from_properties (MMBearerProperties *properties)
{
    static const struct {
        MMBearerApnType mm;
        MMSitApnType    sit;
    } map[] = {
        { MM_BEARER_APN_TYPE_DEFAULT,    MM_SIT_APN_TYPE_DEFAULT   },
        { MM_BEARER_APN_TYPE_INITIAL,    MM_SIT_APN_TYPE_IA        },
        { MM_BEARER_APN_TYPE_IMS,        MM_SIT_APN_TYPE_IMS       },
        { MM_BEARER_APN_TYPE_MMS,        MM_SIT_APN_TYPE_MMS       },
        { MM_BEARER_APN_TYPE_MANAGEMENT, MM_SIT_APN_TYPE_FOTA      },
        { MM_BEARER_APN_TYPE_EMERGENCY,  MM_SIT_APN_TYPE_EMERGENCY },
        { MM_BEARER_APN_TYPE_XCAP,       MM_SIT_APN_TYPE_XCAP      },
        { MM_BEARER_APN_TYPE_TETHERING,  MM_SIT_APN_TYPE_DUN       },
    };
    MMBearerApnType requested;
    guint32         types = 0;
    guint           i;

    requested = mm_bearer_properties_get_apn_type (properties);
    if (requested == MM_BEARER_APN_TYPE_NONE)
        return MM_SIT_APN_TYPE_DEFAULT;

    for (i = 0; i < G_N_ELEMENTS (map); i++) {
        if (requested & map[i].mm)
            types |= map[i].sit;
    }

    /* Every type MM knows that the CP does not -- voice, private, purchase and
     * the rest -- drops out here. A context that is only those is still a
     * context, so fall back rather than telling the CP it is for nothing. */
    return types ? types : MM_SIT_APN_TYPE_DEFAULT;
}

/* Borrow an MMSitApnAuth as the FFI struct, using caller-provided storage so
 * this stays allocation-free. NULL stays NULL: the codec reads that as "no
 * authentication", which is the ordinary case. */
static const SamsungSitApnAuth *
ffi_auth (const MMSitApnAuth *auth,
          SamsungSitApnAuth  *storage)
{
    if (!auth)
        return NULL;

    storage->auth_type = (guint8) auth->auth;
    storage->user = auth->user;
    storage->password = auth->password;
    return storage;
}

GByteArray *
mm_sit_request_new_set_data_profile (guint16      token,
                                     const gchar *apn,
                                     MMSitPdpType pdp,
                                     guint32      apn_types,
                                     const MMSitApnAuth *auth,
                                     GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitApnAuth ffi;
    SamsungSitStatus status;

    status = samsung_sit_build_set_data_profile (token, apn, (SamsungSitPdpType) pdp, apn_types, ffi_auth (auth, &ffi), &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_set_initial_attach_apn (guint16      token,
                                           const gchar *apn,
                                           MMSitPdpType pdp,
                                           const MMSitApnAuth *auth,
                                           GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitApnAuth ffi;
    SamsungSitStatus status;

    status = samsung_sit_build_set_initial_attach_apn (token, apn, (SamsungSitPdpType) pdp, ffi_auth (auth, &ffi), &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_set_initial_attach_apn_for_stack (guint16      token,
                                                   guint8       stack,
                                           const gchar *apn,
                                           MMSitPdpType pdp,
                                           const MMSitApnAuth *auth,
                                           GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitApnAuth ffi;
    SamsungSitStatus status;

    status = samsung_sit_build_set_initial_attach_apn_for_stack (token, stack, apn, (SamsungSitPdpType) pdp, ffi_auth (auth, &ffi), &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_setup_data_call (guint16      token,
                                    guint8       cid,
                                    const gchar *apn,
                                    MMSitPdpType pdp,
                                    guint32      apn_types,
                                    const MMSitApnAuth *auth,
                                    GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitApnAuth ffi;
    SamsungSitStatus status;

    status = samsung_sit_build_setup_data_call (token, cid, apn, (SamsungSitPdpType) pdp, apn_types, ffi_auth (auth, &ffi), &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_deactivate_data_call (guint16          token,
                                         guint8           cid,
                                         MMSitDeactReason reason)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_deactivate_data_call (token, cid, (SamsungSitDeactReason) reason,
                                                    &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_get_data_call_list (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_data_call_list (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_dial (guint16       token,
                         const gchar  *number,
                         GError      **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_val_if_fail (number != NULL, NULL);

    status = samsung_sit_build_dial (token, number, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_answer (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_answer (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_dtmf (guint16       token,
                         const gchar  *digits,
                         GError      **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_val_if_fail (digits != NULL, NULL);

    status = samsung_sit_build_dtmf (token, digits, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_hangup (guint16 token,
                           guint32 call_id)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_hangup (token, call_id, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_get_call_list (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_call_list (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_fast_dormancy_info (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_fast_dormancy_info (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_indication_filter (guint16 token,
                                          guint32 filter)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_indication_filter (token, filter, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_screen_state (guint16  token,
                                     gboolean screen_on)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_screen_state (token, screen_on ? 1 : 0, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_ap_system_time (guint16    token,
                                       GDateTime *time,
                                       GError   **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    if (!time) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "A time is required");
        return NULL;
    }

    status = samsung_sit_build_set_ap_system_time (
        token,
        (guint16) g_date_time_get_year (time),
        g_date_time_get_month (time),
        g_date_time_get_day_of_month (time),
        g_date_time_get_hour (time),
        g_date_time_get_minute (time),
        g_date_time_get_second (time),
        &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_oem_config (guint16       token,
                               const gchar  *software_version,
                               GError      **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_oem_config (token, software_version, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_set_modem_config (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_modem_config (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_modem_config_count (guint16 token, guint8 count)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_modem_config_count (token, count, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_device_info (guint16      token,
                                const gchar *model,
                                const gchar *build,
                                const gchar *codename,
                                GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_device_info (token, model, build, codename, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_carrier_manifest (guint16      token,
                                     const gchar *directory,
                                     const guint8 sha1[MM_SIT_SHA1_LENGTH],
                                     GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_carrier_manifest (token, directory, sha1, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

/*****************************************************************************/
/* Response/indication parsers                                               */

gboolean
mm_sit_message_parse_radio_state (const MMSitMessage *message,
                                  MMSitRadioState    *out_state,
                                  GError            **error)
{
    const guint8        *data;
    gsize                len;
    SamsungSitRadioState state;
    SamsungSitStatus     status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_state != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_radio_state (data, len, &state);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    *out_state = (MMSitRadioState) state;
    return TRUE;
}

gboolean
mm_sit_message_parse_sim_status (const MMSitMessage *message,
                                 MMSitSimStatus     *out_status,
                                 GError            **error)
{
    const guint8       *data;
    gsize               len;
    SamsungSitSimStatus s;
    SamsungSitStatus    status;
    guint               i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_sim_status (data, len, &s);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_status, 0, sizeof (*out_status));
    out_status->card_state          = s.card_state;
    out_status->universal_pin_state = s.universal_pin_state;
    out_status->present             = s.present ? TRUE : FALSE;
    out_status->app_count           = MIN (s.app_count, MM_SIT_MAX_SIM_APPS);
    for (i = 0; i < out_status->app_count; i++) {
        out_status->apps[i].app_type     = s.apps[i].app_type;
        out_status->apps[i].aid_len      = MIN (s.apps[i].aid_len, MM_SIT_MAX_AID_LENGTH);
        memcpy (out_status->apps[i].aid, s.apps[i].aid, out_status->apps[i].aid_len);
        out_status->apps[i].pin1_state   = s.apps[i].pin1_state;
        out_status->apps[i].pin2_state   = s.apps[i].pin2_state;
        out_status->apps[i].pin1_retries = s.apps[i].pin1_retries;
        out_status->apps[i].puk1_retries = s.apps[i].puk1_retries;
        out_status->apps[i].pin2_retries = s.apps[i].pin2_retries;
        out_status->apps[i].puk2_retries = s.apps[i].puk2_retries;
        out_status->apps[i].app_state    = s.apps[i].app_state;
    }
    out_status->atr_len   = MIN (s.atr_len, MM_SIT_MAX_ATR_LENGTH);
    memcpy (out_status->atr, s.atr, out_status->atr_len);
    out_status->iccid_len = MIN (s.iccid_len, MM_SIT_MAX_ICCID_LENGTH);
    memcpy (out_status->iccid, s.iccid, out_status->iccid_len);
    out_status->eid_len   = MIN (s.eid_len, MM_SIT_MAX_EID_LENGTH);
    memcpy (out_status->eid, s.eid, out_status->eid_len);
    return TRUE;
}

GByteArray *
mm_sit_request_new_sim_io (guint16                  token,
                           const MMSitSimIoRequest *request,
                           const guint8            *aid,
                           gsize                    aid_len,
                           GError                 **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_val_if_fail (request != NULL, NULL);

    status = samsung_sit_build_sim_io (token,
                                       request->command,
                                       request->file_id,
                                       request->path_len ? request->path : NULL,
                                       request->path_len,
                                       request->p1,
                                       request->p2,
                                       request->p3,
                                       NULL, 0,
                                       aid_len ? aid : NULL,
                                       aid_len,
                                       &buffer,
                                       &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_send_sms (guint16       token,
                             const guint8 *smsc,
                             gsize         smsc_len,
                             const guint8 *tpdu,
                             gsize         tpdu_len,
                             gboolean      expect_more,
                             gboolean      over_ims,
                             GError      **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_send_sms (token,
                                         smsc_len ? smsc : NULL, smsc_len,
                                         tpdu, tpdu_len,
                                         expect_more ? 1 : 0,
                                         over_ims ? 1 : 0,
                                         &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_sms_ack (guint16  token,
                            gboolean over_ims)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_sms_ack (token, over_ims ? 1 : 0, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_get_smsc_address (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_smsc_address (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

gboolean
mm_sit_message_parse_new_sms (const MMSitMessage *message,
                              MMSitSmsDeliver    *out_deliver,
                              GError            **error)
{
    const guint8         *data;
    gsize                 len;
    SamsungSitSmsDeliver  d;
    SamsungSitStatus      status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_deliver != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_new_sms (data, len, &d);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_deliver, 0, sizeof (*out_deliver));
    out_deliver->format        = d.format;
    out_deliver->over_ims      = d.over_ims ? TRUE : FALSE;
    out_deliver->status_report = d.status_report ? TRUE : FALSE;
    out_deliver->pdu_len  = MIN (d.pdu_len, MM_SIT_MAX_SMS_PDU);
    memcpy (out_deliver->pdu, d.pdu, out_deliver->pdu_len);
    return TRUE;
}

gboolean
mm_sit_message_parse_send_sms_result (const MMSitMessage *message,
                                      guint32            *out_reference,
                                      GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitStatus status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_reference != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_send_sms_result (data, len, out_reference);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_smsc_address (const MMSitMessage *message,
                                   guint8             *out_smsc,
                                   gsize              *out_len,
                                   GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitStatus status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_smsc != NULL && out_len != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_smsc_address (data, len, out_smsc, out_len);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_radio_capability (const MMSitMessage      *message,
                                       MMSitNetworkGenerations *out_generations,
                                       GError                 **error)
{
    const guint8                   *data;
    gsize                           len;
    SamsungSitNetworkGenerations    generations;
    SamsungSitStatus                status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_generations != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_radio_capability (data, len, &generations);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    out_generations->gsm  = generations.gsm;
    out_generations->umts = generations.umts;
    out_generations->lte  = generations.lte;
    out_generations->nr   = generations.nr;
    return TRUE;
}

GByteArray *
mm_sit_request_new_get_base_band_version (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_base_band_version (token, SAMSUNG_SIT_VERSION_ALL_FIELDS,
                                                      &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

gboolean
mm_sit_message_parse_base_band_version (const MMSitMessage    *message,
                                        MMSitBaseBandVersion  *out_version,
                                        GError               **error)
{
    const guint8                 *data;
    gsize                         len;
    SamsungSitBaseBandVersion     version;
    SamsungSitStatus              status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_version != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_base_band_version (data, len, &version);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    G_STATIC_ASSERT (sizeof (version.sw_ver) == MM_SIT_MAX_VERSION_FIELD);
    memcpy (out_version->sw_ver,      version.sw_ver,      MM_SIT_MAX_VERSION_FIELD);
    memcpy (out_version->hw_ver,      version.hw_ver,      MM_SIT_MAX_VERSION_FIELD);
    memcpy (out_version->rf_cal_date, version.rf_cal_date, MM_SIT_MAX_VERSION_FIELD);
    memcpy (out_version->prod_code,   version.prod_code,   MM_SIT_MAX_VERSION_FIELD);
    memcpy (out_version->model_id,    version.model_id,    MM_SIT_MAX_VERSION_FIELD);
    memcpy (out_version->cp_chipset,  version.cp_chipset,  MM_SIT_MAX_VERSION_FIELD);
    return TRUE;
}

gboolean
mm_sit_message_parse_band_mode (const MMSitMessage  *message,
                                MMSitBandMode       *out_bands,
                                GError             **error)
{
    const guint8               *data;
    gsize                       len;
    SamsungSitManualBandMode    m;
    SamsungSitStatus            status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_bands != NULL, FALSE);

    G_STATIC_ASSERT (sizeof (m.supported_lte) == sizeof (out_bands->supported_lte));
    G_STATIC_ASSERT (sizeof (m.payload) == sizeof (out_bands->payload));

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_manual_band_mode (data, len, &m);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_bands->manual_mode = !!m.manual_mode;
    out_bands->supported_lte_len = m.supported_lte_len;
    memcpy (out_bands->supported_lte, m.supported_lte, sizeof (m.supported_lte));
    out_bands->supported_wcdma_len = m.supported_wcdma_len;
    memcpy (out_bands->supported_wcdma, m.supported_wcdma, sizeof (m.supported_wcdma));
    out_bands->current_lte_len = m.current_lte_len;
    memcpy (out_bands->current_lte, m.current_lte, sizeof (m.current_lte));
    out_bands->current_wcdma_len = m.current_wcdma_len;
    memcpy (out_bands->current_wcdma, m.current_wcdma, sizeof (m.current_wcdma));
    memcpy (out_bands->payload, m.payload, sizeof (m.payload));
    out_bands->payload_len = m.payload_len;
    return TRUE;
}

GByteArray *
mm_sit_request_new_set_band_mode (guint16              token,
                                  const MMSitBandMode *mode,
                                  const guint16       *lte,
                                  gsize                n_lte,
                                  const guint16       *wcdma,
                                  gsize                n_wcdma,
                                  GError             **error)
{
    guint8                   *buffer = NULL;
    gsize                     len = 0;
    SamsungSitManualBandMode  m = { 0 };
    SamsungSitStatus          status;

    g_return_val_if_fail (mode != NULL, NULL);

    /* The codec rebuilds what it needs from the payload; only that and the
     * flag cross the boundary. */
    m.manual_mode = mode->manual_mode ? 1 : 0;
    memcpy (m.payload, mode->payload, sizeof (m.payload));
    m.payload_len = mode->payload_len;

    status = samsung_sit_build_set_manual_band_mode (token, &m, lte, n_lte,
                                                    wcdma, n_wcdma, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

gboolean
mm_sit_message_parse_struct_versions (const MMSitMessage   *message,
                                      MMSitStructVersions  *out_versions,
                                      GError              **error)
{
    const guint8              *data;
    gsize                      len;
    SamsungSitStructVersions   versions;
    SamsungSitStatus           status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_versions != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_struct_versions (data, len, &versions);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_versions->cell_info                 = versions.cell_info;
    out_versions->cell_identity             = versions.cell_identity;
    out_versions->signal_strength           = versions.signal_strength;
    out_versions->cell_info_supported       = !!versions.cell_info_supported;
    out_versions->cell_identity_supported   = !!versions.cell_identity_supported;
    out_versions->signal_strength_supported = !!versions.signal_strength_supported;
    return TRUE;
}

gboolean
mm_sit_message_parse_network_selection_mode (const MMSitMessage        *message,
                                             MMSitNetworkSelectionMode *out_mode,
                                             GError                   **error)
{
    const guint8                    *data;
    gsize                            len;
    SamsungSitNetworkSelectionMode   mode;
    SamsungSitStatus                 status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_mode != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_network_selection_mode (data, len, &mode);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    *out_mode = (MMSitNetworkSelectionMode) mode;
    return TRUE;
}

gboolean
mm_sit_message_parse_imei (const MMSitMessage  *message,
                           gchar              **out_imei,
                           GError             **error)
{
    const guint8    *data;
    gsize            len;
    gsize            imei_len = 0;
    guint8           imei[MM_SIT_MAX_IMEI_LEN];
    SamsungSitStatus status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_imei != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_imei (data, len, imei, &imei_len);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    *out_imei = imei_len ? g_strndup ((const gchar *) imei, imei_len) : NULL;
    return TRUE;
}

gboolean
mm_sit_message_parse_software_version (const MMSitMessage  *message,
                                       gchar              **out_version,
                                       GError             **error)
{
    const guint8    *data;
    gsize            len;
    gsize            version_len = 0;
    guint8           version[MM_SIT_SOFTWARE_VERSION_LEN];
    SamsungSitStatus status;

    G_STATIC_ASSERT (sizeof (version) == SAMSUNG_SIT_SOFTWARE_VERSION_LEN);

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_version != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_software_version (data, len, version, &version_len);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    *out_version = version_len ? g_strndup ((const gchar *) version, version_len) : NULL;
    return TRUE;
}

gboolean
mm_sit_file_control_parameters_parse (const guint8 *data,
                                      gsize         data_len,
                                      MMSitEfInfo  *out_info,
                                      GError      **error)
{
    SamsungSitFileControlParameters p;
    SamsungSitStatus                status;

    g_return_val_if_fail (out_info != NULL, FALSE);

    status = samsung_sit_parse_file_control_parameters (data, data_len, &p);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_info->structure    = (MMSitEfStructure) p.structure;
    out_info->has_file_id  = p.has_file_id ? TRUE : FALSE;
    out_info->file_id      = p.file_id;
    out_info->size         = p.size;
    out_info->record_len   = p.record_len;
    out_info->record_count = p.record_count;
    return TRUE;
}

gboolean
mm_sit_message_parse_sim_io (const MMSitMessage *message,
                             MMSitSimIoResponse *out_response,
                             GError            **error)
{
    const guint8           *data;
    gsize                   len;
    SamsungSitSimIoResponse r;
    SamsungSitStatus        status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_response != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_sim_io (data, len, &r);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_response, 0, sizeof (*out_response));
    out_response->sw1      = r.sw1;
    out_response->sw2      = r.sw2;
    out_response->success  = r.success ? TRUE : FALSE;
    out_response->data_len = MIN (r.data_len, MM_SIT_MAX_SIM_IO_DATA);
    memcpy (out_response->data, r.data, out_response->data_len);
    return TRUE;
}

gboolean
mm_sit_message_parse_sim_slot_status (const MMSitMessage *message,
                                      MMSitSimSlotStatus *out_status,
                                      GError            **error)
{
    const guint8           *data;
    gsize                   len;
    SamsungSitSimSlotStatus s;
    SamsungSitStatus        status;
    guint                   i, j;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_sim_slot_status (data, len, &s);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_status, 0, sizeof (*out_status));
    out_status->legacy     = s.legacy ? TRUE : FALSE;
    out_status->slot_count = s.slot_count;

    /* The slot/port records are all-u8 and defined identically on both sides. */
    for (i = 0; i < MM_SIT_MAX_SIM_SLOTS; i++) {
        out_status->slots[i].card_state = s.slots[i].card_state;
        out_status->slots[i].atr_len    = s.slots[i].atr_len;
        memcpy (out_status->slots[i].atr, s.slots[i].atr, MM_SIT_MAX_ATR_LENGTH);
        out_status->slots[i].eid_len    = s.slots[i].eid_len;
        memcpy (out_status->slots[i].eid, s.slots[i].eid, MM_SIT_MAX_EID_LENGTH);
        out_status->slots[i].port_count = s.slots[i].port_count;
        for (j = 0; j < MM_SIT_MAX_SIM_PORTS; j++) {
            out_status->slots[i].ports[j].iccid_len = s.slots[i].ports[j].iccid_len;
            memcpy (out_status->slots[i].ports[j].iccid, s.slots[i].ports[j].iccid,
                    MM_SIT_MAX_ICCID_LENGTH);
            out_status->slots[i].ports[j].logical_slot_id = s.slots[i].ports[j].logical_slot_id;
            out_status->slots[i].ports[j].state           = s.slots[i].ports[j].state;
        }
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_carrier_config_status (const MMSitMessage *message,
                                            gboolean           *out_applied,
                                            GError            **error)
{
    const guint8 *payload;
    gsize         len;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_applied != NULL, FALSE);

    /*
     * Eight bytes: a verdict word, then one the CP leaves zero in every
     * captured indication. Parsed here rather than in the codec because there
     * is nothing to model -- one word read one way -- and the codec's parsers
     * all exist to flatten structures that have a layout worth naming.
     */
    payload = mm_sit_message_get_payload (message, &len);
    if (len < 4) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "SIT carrier-config status is %" G_GSIZE_FORMAT " bytes, need 4", len);
        return FALSE;
    }

    *out_applied = (read_le32 (payload) != 0);
    return TRUE;
}

gboolean
mm_sit_message_parse_carrier_sim_info (const MMSitMessage  *message,
                                       MMSitCarrierSimInfo *out_info,
                                       GError             **error)
{
    const guint8            *data;
    gsize                    len;
    SamsungSitCarrierSimInfo info;
    SamsungSitStatus         status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_info != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_carrier_sim_info (data, len, &info);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memcpy (out_info->iccid, info.iccid, sizeof (out_info->iccid));
    memcpy (out_info->imsi, info.imsi, sizeof (out_info->imsi));
    return TRUE;
}

/*
 * Frame offsets of the 5G NSA bytes in a registration response; see
 * mm_sit_message_parse_registration(). These are the vendor's payload offsets
 * plus twelve, because mm_sit_message_get_data() hands out the whole frame --
 * the eight-byte header and the reserved word included -- while the vendor's
 * adapter indexes from the payload proper.
 */
#define REG_ENDC_AVAILABLE  51
#define REG_DCNR_RESTRICTED 52
#define REG_NR_AVAILABLE    53
#define REG_NR_MIN_LEN      54

gboolean
mm_sit_message_parse_registration (const MMSitMessage *message,
                                   MMSitRegistration  *out_registration,
                                   GError            **error)
{
    const guint8          *data;
    gsize                  len;
    SamsungSitRegistration reg;
    SamsungSitStatus       status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_registration != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_registration (data, len, &reg);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_registration->state            = reg.state;
    out_registration->radio_technology = reg.radio_technology;
    out_registration->registered       = reg.registered ? TRUE : FALSE;

    /*
     * The codec does not model the three 5G NSA bytes, so read them here.
     * They come from ProtocolNetDataRegStateAdapter in the vendor's own
     * vendor.radio.protocol.sit.stream.so -- IsEndcAvailable(),
     * IsDcNrRestricted() and IsNrAvailable() each compare one byte against 1.
     * Two sanity checks pin the offsets down, because the adapter counts from
     * the payload while 'data' here is the whole frame: GetImsVops() reads the
     * frame's byte 45, which is the VoPS this function already reports, and
     * getChannelNumber() reads a 32-bit field twelve past its own base, which
     * lands on the serving cell's EARFCN (900 on a B2 carrier, 3200 on B7).
     */
    out_registration->endc_available  = FALSE;
    out_registration->dcnr_restricted = FALSE;
    out_registration->nr_available    = FALSE;
    if (data && len >= REG_NR_MIN_LEN) {
        out_registration->endc_available  = (data[REG_ENDC_AVAILABLE] == 1);
        out_registration->dcnr_restricted = (data[REG_DCNR_RESTRICTED] == 1);
        out_registration->nr_available    = (data[REG_NR_AVAILABLE] == 1);
    }

    switch (reg.vops_support) {
    case SAMSUNG_SIT_VOPS_SUPPORT_SUPPORTED:
        out_registration->vops_support = MM_SIT_VOPS_SUPPORT_SUPPORTED;
        break;
    case SAMSUNG_SIT_VOPS_SUPPORT_UNSUPPORTED:
        out_registration->vops_support = MM_SIT_VOPS_SUPPORT_UNSUPPORTED;
        break;
    default:
        out_registration->vops_support = MM_SIT_VOPS_SUPPORT_UNKNOWN;
        break;
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_registration_failed (const MMSitMessage       *message,
                                          MMSitRegistrationFailure *out_failure,
                                          GError                  **error)
{
    const guint8                     *data;
    gsize                             len;
    SamsungSitRegistrationFailure     failure;
    SamsungSitStatus                  status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_failure != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_registration_failed (data, len, &failure);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    g_strlcpy (out_failure->plmn, failure.plmn, sizeof (out_failure->plmn));
    out_failure->circuit_switched = failure.circuit_switched ? TRUE : FALSE;
    out_failure->packet_switched  = failure.packet_switched ? TRUE : FALSE;
    out_failure->cause            = failure.cause;
    out_failure->additional_cause = failure.additional_cause;
    out_failure->retryable        = failure.retryable ? TRUE : FALSE;
    return TRUE;
}

gboolean
mm_sit_message_parse_nitz_time (const MMSitMessage *message,
                                MMSitNitzTime      *out_time,
                                GError            **error)
{
    const guint8      *data;
    gsize              len;
    SamsungSitNitzTime t;
    SamsungSitStatus   status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_time != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_nitz_time (data, len, &t);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_time, 0, sizeof (*out_time));
    out_time->valid = t.valid ? TRUE : FALSE;
    if (!out_time->valid)
        return TRUE;

    out_time->year                   = t.year;
    out_time->month                  = t.month;
    out_time->day                    = t.day;
    out_time->hour                   = t.hour;
    out_time->minute                 = t.minute;
    out_time->second                 = t.second;
    out_time->timezone_quarter_hours = t.timezone_quarter_hours;
    out_time->daylight_saving_hours  = t.daylight_saving_hours;
    out_time->day_of_week            = t.day_of_week;
    out_time->age_ms                 = t.age_ms;
    memcpy (out_time->plmn, t.plmn, sizeof (out_time->plmn));
    return TRUE;
}

gchar *
mm_sit_nitz_time_to_iso8601 (const MMSitNitzTime *time,
                             GError             **error)
{
    g_autoptr(GDateTime) utc = NULL;
    g_autoptr(GDateTime) local = NULL;
    gint                 offset_minutes;

    g_return_val_if_fail (time != NULL, NULL);

    if (!time->valid) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                             "the modem has not received a network time yet");
        return NULL;
    }

    utc = g_date_time_new_utc (time->year, time->month, time->day,
                               time->hour, time->minute, time->second);
    if (!utc) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "invalid network time: %04u-%02u-%02u %02u:%02u:%02u",
                     time->year, time->month, time->day,
                     time->hour, time->minute, time->second);
        return NULL;
    }

    /* Roll the cached timestamp forward by its own age and on into the
     * network's timezone, so what comes out is the current local time there --
     * which is the form the Time interface publishes (same as +CCLK's). */
    offset_minutes = time->timezone_quarter_hours * 15;
    local = g_date_time_add_seconds (utc,
                                     (gdouble) time->age_ms / 1000.0 + offset_minutes * 60);
    if (!local) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "network time %" G_GUINT64_FORMAT " ms old is out of range",
                     time->age_ms);
        return NULL;
    }

    return mm_new_iso8601_time (g_date_time_get_year (local),
                                g_date_time_get_month (local),
                                g_date_time_get_day_of_month (local),
                                g_date_time_get_hour (local),
                                g_date_time_get_minute (local),
                                g_date_time_get_second (local),
                                TRUE,
                                offset_minutes,
                                error);
}

gboolean
mm_sit_message_parse_operator (const MMSitMessage *message,
                               MMSitOperator      *out_operator,
                               GError            **error)
{
    const guint8      *data;
    gsize              len;
    SamsungSitOperator op;
    SamsungSitStatus   status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_operator != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_operator (data, len, &op);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memcpy (out_operator->plmn, op.plmn, sizeof (out_operator->plmn));
    memcpy (out_operator->operator_short, op.operator_short, sizeof (out_operator->operator_short));
    memcpy (out_operator->operator_long, op.operator_long, sizeof (out_operator->operator_long));
    return TRUE;
}

gboolean
mm_sit_message_parse_signal (const MMSitMessage *message,
                             MMSitSignal        *out_signal,
                             GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitSignal s;
    SamsungSitStatus status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_signal != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_signal (data, len, &s);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    memset (out_signal, 0, sizeof (*out_signal));
    out_signal->version  = s.version;
    out_signal->rat_mask = s.rat_mask;

    out_signal->has_gsm             = s.has_gsm ? TRUE : FALSE;
    out_signal->gsm_signal_strength = s.gsm_signal_strength;
    out_signal->gsm_bit_error_rate  = s.gsm_bit_error_rate;
    out_signal->gsm_timing_advance  = s.gsm_timing_advance;

    out_signal->has_wcdma             = s.has_wcdma ? TRUE : FALSE;
    out_signal->wcdma_signal_strength = s.wcdma_signal_strength;
    out_signal->wcdma_bit_error_rate  = s.wcdma_bit_error_rate;
    out_signal->wcdma_rscp            = s.wcdma_rscp;
    out_signal->wcdma_ecno            = s.wcdma_ecno;

    out_signal->has_lte             = s.has_lte ? TRUE : FALSE;
    out_signal->lte_signal_strength = s.lte_signal_strength;
    out_signal->lte_rsrp            = s.lte_rsrp;
    out_signal->lte_rsrq            = s.lte_rsrq;
    out_signal->lte_rssnr           = s.lte_rssnr;
    out_signal->lte_cqi_table_index = s.lte_cqi_table_index;
    out_signal->lte_cqi             = s.lte_cqi;
    out_signal->lte_timing_advance  = s.lte_timing_advance;

    out_signal->has_nr5g            = s.has_nr5g ? TRUE : FALSE;
    out_signal->nr5g_ss_rsrp        = s.nr5g_ss_rsrp;
    out_signal->nr5g_ss_rsrq        = s.nr5g_ss_rsrq;
    out_signal->nr5g_ss_sinr        = s.nr5g_ss_sinr;
    out_signal->nr5g_csi_rsrp       = s.nr5g_csi_rsrp;
    out_signal->nr5g_csi_rsrq       = s.nr5g_csi_rsrq;
    out_signal->nr5g_csi_sinr       = s.nr5g_csi_sinr;
    out_signal->nr5g_timing_advance = s.nr5g_timing_advance;
    return TRUE;
}

gboolean
mm_sit_message_parse_cell_info_list (const MMSitMessage *message,
                                     GArray            **out_cells,
                                     GError            **error)
{
    const guint8          *data;
    gsize                  len;
    SamsungSitCellInfoLte *cells = NULL;
    gsize                  count = 0;
    SamsungSitStatus       status;
    GArray                *array;
    gsize                  i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_cells != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_cell_info_list (data, len, &cells, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitCellInfoLte), count);
    for (i = 0; i < count; i++) {
        MMSitCellInfoLte cell;

        memset (&cell, 0, sizeof (cell));
        cell.registered        = cells[i].registered ? TRUE : FALSE;
        cell.connection_status = cells[i].connection_status;
        memcpy (cell.operator_id, cells[i].operator_id, sizeof (cell.operator_id));
        cell.ci             = cells[i].ci;
        cell.physical_ci    = cells[i].physical_ci;
        cell.tac            = cells[i].tac;
        cell.earfcn         = cells[i].earfcn;
        cell.bandwidth      = cells[i].bandwidth;
        cell.rsrp           = cells[i].rsrp;
        cell.rsrq           = cells[i].rsrq;
        cell.rssnr          = cells[i].rssnr;
        cell.timing_advance = cells[i].timing_advance;
        g_array_append_val (array, cell);
    }
    samsung_sit_cell_info_list_free (cells, count);

    *out_cells = array;
    return TRUE;
}

gboolean
mm_sit_message_parse_data_call (const MMSitMessage *message,
                                MMSitDataCall      *out_call,
                                GError            **error)
{
    const guint8      *data;
    gsize              len;
    SamsungSitDataCall dc;
    SamsungSitStatus   status;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_call != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_data_call (data, len, &dc);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    fill_data_call (out_call, &dc);
    return TRUE;
}

gboolean
mm_sit_message_parse_data_call_list (const MMSitMessage *message,
                                     GArray            **out_calls,
                                     GError            **error)
{
    const guint8       *data;
    gsize               len;
    SamsungSitDataCall *calls = NULL;
    gsize               count = 0;
    SamsungSitStatus    status;
    GArray             *array;
    gsize               i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_calls != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_data_call_list (data, len, &calls, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitDataCall), count);
    for (i = 0; i < count; i++) {
        MMSitDataCall call;

        fill_data_call (&call, &calls[i]);
        g_array_append_val (array, call);
    }
    samsung_sit_data_call_list_free (calls, count);

    *out_calls = array;
    return TRUE;
}

gboolean
mm_sit_message_parse_call_list (const MMSitMessage *message,
                                GArray            **out_calls,
                                GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitCall  *calls = NULL;
    gsize            count = 0;
    SamsungSitStatus status;
    GArray          *array;
    gsize            i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_calls != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_call_list (data, len, &calls, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitCall), count);
    for (i = 0; i < count; i++) {
        MMSitCall call;

        memset (&call, 0, sizeof (call));
        call.state    = calls[i].state;
        call.index    = calls[i].index;
        call.incoming = (calls[i].incoming != 0);
        g_strlcpy (call.number, calls[i].number, sizeof (call.number));
        g_array_append_val (array, call);
    }
    samsung_sit_call_list_free (calls, count);

    *out_calls = array;
    return TRUE;
}

GByteArray *
mm_sit_request_new_set_network_selection_auto (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_set_network_selection_auto (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_network_selection_manual (guint16      token,
                                                 const gchar *plmn,
                                                 guint8       rat,
                                                 GError     **error)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    g_return_val_if_fail (plmn != NULL, NULL);

    status = samsung_sit_build_set_network_selection_manual (token, plmn, rat, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

GByteArray *
mm_sit_request_new_start_network_scan (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_start_network_scan (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_stop_network_scan (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_stop_network_scan (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

gboolean
mm_sit_message_parse_network_selection_manual_ack (const MMSitMessage *message,
                                                   GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitStatus status;

    g_return_val_if_fail (message != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_network_selection_manual_ack (data, len);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    return TRUE;
}

gboolean
mm_sit_message_parse_net_scan_result (const MMSitMessage *message,
                                      GArray            **out_networks,
                                      GError            **error)
{
    const guint8              *data;
    gsize                      len;
    SamsungSitScannedNetwork  *networks = NULL;
    gsize                      count = 0;
    SamsungSitStatus           status;
    GArray                    *array;
    gsize                      i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_networks != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_net_scan_result (data, len, &networks, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitScannedNetwork), count);
    for (i = 0; i < count; i++) {
        MMSitScannedNetwork network;

        memset (&network, 0, sizeof (network));
        g_strlcpy (network.plmn, networks[i].plmn, sizeof (network.plmn));
        network.serving = (networks[i].serving != 0);
        network.rat     = networks[i].rat;
        g_array_append_val (array, network);
    }
    samsung_sit_scanned_network_list_free (networks, count);

    *out_networks = array;
    return TRUE;
}

GByteArray *
mm_sit_request_new_get_allowed_network_types (guint16 token)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_allowed_network_type_bitmap (token, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

GByteArray *
mm_sit_request_new_set_allowed_network_types (guint16                        token,
                                              const MMSitNetworkGenerations *generations,
                                              GError                       **error)
{
    guint8                        *buffer = NULL;
    gsize                          len = 0;
    SamsungSitStatus               status;
    SamsungSitNetworkGenerations   wire;

    g_return_val_if_fail (generations != NULL, NULL);

    /* MMSitNetworkGenerations uses gboolean fields, the wire struct u8 ones. */
    wire.gsm  = generations->gsm ? 1 : 0;
    wire.umts = generations->umts ? 1 : 0;
    wire.lte  = generations->lte ? 1 : 0;
    wire.nr   = generations->nr ? 1 : 0;

    status = samsung_sit_build_set_allowed_network_type_bitmap (token, wire, &buffer, &len);
    return finish_build (status, buffer, len, error);
}

gboolean
mm_sit_message_parse_allowed_network_types (const MMSitMessage      *message,
                                            MMSitNetworkGenerations *out_generations,
                                            GError                 **error)
{
    const guint8                 *data;
    gsize                         len;
    SamsungSitStatus              status;
    SamsungSitNetworkGenerations  wire;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_generations != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_allowed_network_type_bitmap (data, len, &wire);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    out_generations->gsm  = (wire.gsm != 0);
    out_generations->umts = (wire.umts != 0);
    out_generations->lte  = (wire.lte != 0);
    out_generations->nr   = (wire.nr != 0);
    return TRUE;
}

gboolean
mm_sit_message_parse_nr_cell_info_list (const MMSitMessage *message,
                                        GArray            **out_cells,
                                        GError            **error)
{
    const guint8             *data;
    gsize                     len;
    SamsungSitCellInfoNr     *cells = NULL;
    gsize                     count = 0;
    SamsungSitStatus          status;
    GArray                   *array;
    gsize                     i;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_cells != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_nr_cell_info_list (data, len, &cells, &count);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }

    array = g_array_sized_new (FALSE, FALSE, sizeof (MMSitCellInfoNr), count);
    for (i = 0; i < count; i++) {
        MMSitCellInfoNr cell;

        memset (&cell, 0, sizeof (cell));
        cell.registered        = (cells[i].registered != 0);
        cell.connection_status = cells[i].connection_status;
        g_strlcpy (cell.operator_id, cells[i].operator_id, sizeof (cell.operator_id));
        cell.nci         = cells[i].nci;
        cell.physical_ci = cells[i].physical_ci;
        cell.tac         = cells[i].tac;
        cell.nrarfcn     = cells[i].nrarfcn;
        cell.ss_rsrp     = cells[i].ss_rsrp;
        cell.ss_rsrq     = cells[i].ss_rsrq;
        cell.ss_sinr     = cells[i].ss_sinr;
        cell.csi_rsrp    = cells[i].csi_rsrp;
        cell.csi_rsrq    = cells[i].csi_rsrq;
        cell.csi_sinr    = cells[i].csi_sinr;
        g_array_append_val (array, cell);
    }
    samsung_sit_nr_cell_info_list_free (cells, count);

    *out_cells = array;
    return TRUE;
}

GByteArray *
mm_sit_request_new_get_facility_lock (guint16 token,
                                      guint8  facility)
{
    guint8          *buffer = NULL;
    gsize            len = 0;
    SamsungSitStatus status;

    status = samsung_sit_build_get_facility_lock (token, facility, &buffer, &len);
    return finish_build (status, buffer, len, NULL);
}

gboolean
mm_sit_message_parse_facility_lock (const MMSitMessage *message,
                                    gboolean           *out_locked,
                                    GError            **error)
{
    const guint8    *data;
    gsize            len;
    SamsungSitStatus status;
    guint8           locked = 0;

    g_return_val_if_fail (message != NULL, FALSE);
    g_return_val_if_fail (out_locked != NULL, FALSE);

    data = mm_sit_message_get_data (message, &len);
    status = samsung_sit_parse_facility_lock (data, len, &locked);
    if (status != SAMSUNG_SIT_STATUS_OK) {
        set_ffi_error (error, status);
        return FALSE;
    }
    *out_locked = (locked != 0);
    return TRUE;
}
