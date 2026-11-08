/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#include <string.h>
#include <sys/utsname.h>

#include "mm-log-object.h"
#include "mm-sit-client.h"
#include "mm-sit-boot-config.h"
#include "mm-sit-cp.h"

enum {
    SIGNAL_INDICATION,
    SIGNAL_DATA_PDN_REFUSED,
    SIGNAL_LAST,
};

static guint signals[SIGNAL_LAST];

/*
 * A registration query in flight, and everyone waiting on its answer.
 *
 * Stock never has more than two registration queries outstanding -- one
 * GET_VOICE_REGISTRATION and one GET_DATA_REGISTRATION -- in any of five
 * captures, while sending *more* of them than we do: 33-50 and 34-52 in the
 * first minute against our 23-27 and 30-31. We had no cap at all and peaked at
 * nine, because several callers poll independently and nothing serialises them
 * now that requests are pipelined.
 *
 * So the first caller of each kind issues the query and the rest share its
 * answer. These are reads of the current state, so one round trip is the right
 * answer for every caller that asked inside it, and the cap falls out at
 * exactly stock's two.
 */
typedef enum {
    REG_QUERY_DATA,
    REG_QUERY_VOICE,
    REG_QUERY_N,
} RegQueryKind;

typedef struct {
    GAsyncReadyCallback callback;
    gpointer            user_data;
} RegWaiter;

struct _MMSitClientPrivate {
    /* Callers waiting on the query of each kind that is in flight. */
    GPtrArray *reg_waiters[REG_QUERY_N];
    MMSitCp             *cp; /* weak; the CP owns its clients */
    guint                stack;
    gchar               *device;
    MMPortSerialSit     *port;
    /* Device node to retry on if the proxy socket will not open; NULL when we
     * were told to talk to the node directly in the first place. */
    gchar               *proxy_fallback_device;
    guint                unsolicited_handler_id;
    /*
     * The last attach APN handed to the CP, kept so it can be re-asserted.
     *
     * Stock re-sends SET_INITIAL_ATTACH_APN, unchanged, immediately before the
     * first SETUP_DATA_CALL it makes on a stack -- captured on both stacks of
     * the same boot: Telus attach at 2896 ms, re-sent at 9932 ms, context
     * asked for at 11238 ms; Fizz attach at 2820 ms, re-sent at 10358 ms,
     * context asked for 29 ms later. Both were granted. We sent the attach
     * once at radio-on and never again, and the stack whose context came
     * latest was refused with cause 168 twelve milliseconds in -- a local
     * refusal, before any air interface. The attach request is what populates
     * the CP's per-context tables, and they do not survive the registration
     * that happens in between.
     *
     * Re-asserted once per attach epoch, which is what stock does: the second
     * context on a stack follows the first without another attach. No radio
     * cycle goes with it -- stock's only SET_RADIO_POWER was at radio-on.
     */
    gchar               *attach_apn;
    MMSitPdpType         attach_pdp;
    guint8               attach_pcscf;
    MMSitAuthType        attach_auth_type;
    gchar               *attach_auth_user;
    gchar               *attach_auth_password;
    gboolean             attach_reasserted;
    gboolean             has_carrier_sim_info;
    MMSitCarrierSimInfo  carrier_sim_info;
    gboolean             boot_config_done;
    /*
     * The whole per-stack boot sequence, not just the config batch: the modem
     * sets this when its post-radio-on replay finishes. IMS provisioning waits
     * for it; see mm_sit_client_boot_sequence_done().
     */
    gboolean             boot_sequence_done;
    /* The struct versions the CP announced when answering 0x0922, and whether
     * it has answered yet. Every field reads "supported" until it has, so a CP
     * that never got the question is never blocked on its answer. */
    gboolean             struct_versions_known;
    MMSitStructVersions  struct_versions;
    /* What we tell the CP the AP is, in 0x0922. Resolved from the running
     * system, overridable per device. */
    gchar               *device_model;
    gchar               *device_build;
    gchar               *device_codename;
    /* 0x0922 field 4, and the value 0x4605 carries. Both are vendor constants
     * with no counterpart on a mainline system, so they come from the device
     * package (ID_MM_SIT_DEVICE_SW_VERSION / ID_MM_SIT_CP_SW_VERSION). Unset
     * keeps the old behaviour: an empty field 4, and 0x4605 echoing the SV
     * digits of GET_IMEISV. */
    gchar               *device_sw_version;
    gchar               *cp_sw_version;
    /* Resolved alongside the above, for the SIP User-Agent rather than for
     * SET_DEVICE_INFO -- same facts, second consumer. */
    gchar               *device_manufacturer;
    gchar               *device_os_name;
    gchar               *device_os_version;
    gboolean             radio_state_trusted;
    guint8               sim_iccid[MM_SIT_MAX_ICCID_LENGTH];
    gsize                sim_iccid_len;
    /* AID of the subscription application, cached from the last SIM status so
     * file reads can address it the way the downstream RIL does. */
    guint8               sim_aid[MM_SIT_MAX_AID_LENGTH];
    gsize                sim_aid_len;
    /*
     * Every application the card reports, not just the subscription one.
     * Stock queries the facility-lock pair once per application: four requests
     * on the stack whose SIM has an ISIM (AID ...871004...), two on the one
     * that does not. Caching only apps[0] made that impossible.
     */
    /* EF_DOMAIN from the ISIM, if the card has one. The operator chooses it,
     * so it is read rather than derived -- most use the 3GPP default form but
     * nothing requires them to. */
    gchar               *isim_domain;
    guint8               sim_aids[MM_SIT_MAX_SIM_APPS][MM_SIT_MAX_AID_LENGTH];
    gsize                sim_aid_lens[MM_SIT_MAX_SIM_APPS];
    guint                sim_aid_count;
    /* Service centre the CP reports, cached on first use: the downstream RIL
     * puts it in every send and this CP answers a send that lacks one with an
     * empty response. */
    guint8               smsc[MM_SIT_MAX_SMSC_LEN];
    gsize                smsc_len;
    /* Context ids handed out to data calls, cid -> APN. See "Context ids". */
    GHashTable          *cids;
};

static void log_object_iface_init (MMLogObjectInterface *iface);

G_DEFINE_TYPE_EXTENDED (MMSitClient, mm_sit_client, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_LOG_OBJECT, log_object_iface_init))

static gboolean
message_check_success (const MMSitMessage *message,
                       GError            **error)
{
    if (!message->has_status) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "SIT %s response has no status",
                     mm_sit_message_id_get_string (message->id));
        return FALSE;
    }
    if (message->status != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "SIT %s failed with status %u",
                     mm_sit_message_id_get_string (message->id),
                     message->status);
        return FALSE;
    }
    return TRUE;
}

static void
port_indication (MMPortSerialSit    *port,
                 const MMSitMessage *message,
                 MMSitClient        *self)
{
    if (message->id == MM_SIT_MESSAGE_SIM_STATUS_CHANGED) {
        /* DSDS clients are shared by the CP and can outlive the modem/SIM
         * objects replaced after a profile switch. Drop profile-specific
         * state before notifying their users. */
        self->priv->has_carrier_sim_info = FALSE;
        self->priv->sim_aid_len = 0;
        self->priv->sim_aid_count = 0;
        self->priv->smsc_len = 0;
    }
    if (message->id == MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO) {
        MMSitCarrierSimInfo info;

        if (mm_sit_message_parse_carrier_sim_info (message, &info, NULL)) {
            self->priv->carrier_sim_info = info;
            self->priv->has_carrier_sim_info = TRUE;
        }
    }
    if (self->priv->cp && message->id == MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED) {
        g_autoptr(GArray) calls = NULL;

        if (mm_sit_message_parse_data_call_list (message, &calls, NULL))
            mm_sit_cp_update_data_calls (self->priv->cp, self->priv->stack, calls);
    }
    g_signal_emit (self, signals[SIGNAL_INDICATION], 0, message);
}

void
mm_sit_client_set_stack (MMSitClient *self, MMSitCp *cp, guint stack)
{
    g_return_if_fail (stack < 2 && !self->priv->cp);
    self->priv->stack = stack;
    self->priv->cp = cp;
    g_object_add_weak_pointer (G_OBJECT (cp), (gpointer *) &self->priv->cp);
}

guint
mm_sit_client_get_stack (MMSitClient *self)
{
    return self->priv->stack;
}

static MMSitClient *
control_client (MMSitClient *self)
{
    return self->priv->cp ? mm_sit_cp_peek_client (self->priv->cp, 0) : self;
}

/*
 * How many commands have to time out in a row before this is reported as the
 * control plane being gone rather than one slow command.  The modem polls
 * signal quality and access technology every few seconds, so this is a handful
 * of seconds of silence.
 */
#define SIT_CONTROL_PLANE_ALARM 6

/*
 * A SIT command timing out is almost never about that command.  The CP's FMT
 * ring is a single 4 KiB slot, and a frame left in it undrained makes every
 * later frame unsendable, so one stuck frame silently takes down SMS, PDN
 * setup and everything else until something clears it.  That failure spent a
 * long time looking like unrelated SMS flakiness because the only trace of it
 * was a debug line per command, so say it plainly and point at where the
 * evidence is.
 */
static void
port_timed_out (MMPort      *port,
                guint        n_consecutive,
                MMSitClient *self)
{
    if (n_consecutive == SIT_CONTROL_PLANE_ALARM)
        mm_obj_err (self,
                    "SIT control plane unresponsive: %u commands in a row timed out; "
                    "the CP is not draining the FMT ring (look for \"txq full\" in dmesg)",
                    n_consecutive);
    else if (n_consecutive > 1)
        mm_obj_warn (self, "SIT command timed out (%u in a row)", n_consecutive);
    else
        mm_obj_warn (self, "SIT command timed out");
}

/* Take ownership of @port and watch it; the one place the signal is wired. */
static void
sit_client_take_port (MMSitClient     *self,
                      MMPortSerialSit *port)
{
    g_clear_object (&self->priv->port);
    self->priv->port = port;
    if (!port)
        return;
    /* Tied to both objects' lifetimes, so neither dispose has to undo it. */
    g_signal_connect_object (port, MM_PORT_SIGNAL_TIMED_OUT,
                             G_CALLBACK (port_timed_out), self, 0);
}

MMSitClient *
mm_sit_client_new (const gchar *device)
{
    MMSitClient *self;
    const gchar *name;

    g_return_val_if_fail (device != NULL, NULL);

    name = g_str_has_prefix (device, "/dev/") ? device + strlen ("/dev/") : device;
    self = g_object_new (MM_TYPE_SIT_CLIENT, NULL);
    self->priv->device = g_strdup (name);
    sit_client_take_port (self, mm_port_serial_sit_new (name));
    return self;
}

gchar *
mm_sit_proxy_socket_path (const gchar *device)
{
    const gchar *sit;
    const gchar *index = NULL;

    g_return_val_if_fail (device != NULL, NULL);

    /* sit-proxy names one socket per device node, by the node's own index:
     * wwan0sit0 -> /run/sit-proxy/0.  The port is the stack selector under
     * DSDS, so this mapping is the whole routing story -- there is no selector
     * inside the protocol. */
    sit = g_strrstr (device, "sit");
    if (sit) {
        const gchar *p = sit + strlen ("sit");

        if (g_ascii_isdigit (*p))
            index = p;
    }
    if (!index) {
        /* An unrecognised name (a downstream-style /dev/umts_ipc0, say) has no
         * index to read; assume the first stack, which is the one that carries
         * everything today. */
        index = "0";
    }
    return g_build_filename (MM_SIT_PROXY_SOCKET_DIRECTORY, index, NULL);
}

MMSitClient *
mm_sit_client_new_proxy (const gchar *device)
{
    MMSitClient      *self;
    const gchar      *name;
    g_autofree gchar *socket_path = NULL;

    g_return_val_if_fail (device != NULL, NULL);

    name = g_str_has_prefix (device, "/dev/") ? device + strlen ("/dev/") : device;
    socket_path = mm_sit_proxy_socket_path (name);

    self = g_object_new (MM_TYPE_SIT_CLIENT, NULL);
    /* Keep the device node as the identity -- it is what logs, DSDS and the
     * user think in, and it is what libqmi does when proxying too. Only the
     * transport underneath differs. */
    self->priv->device = g_strdup (name);
    self->priv->proxy_fallback_device = g_strdup (name);
    sit_client_take_port (self, mm_port_serial_sit_new_socket (socket_path));
    return self;
}

MMSitClient *
mm_sit_client_new_from_port (MMPortSerialSit *port)
{
    MMSitClient *self;

    g_return_val_if_fail (MM_IS_PORT_SERIAL_SIT (port), NULL);

    self = g_object_new (MM_TYPE_SIT_CLIENT, NULL);
    self->priv->device = g_strdup (mm_port_get_device (MM_PORT (port)));
    sit_client_take_port (self, g_object_ref (port));
    return self;
}

const gchar *
mm_sit_client_get_device (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return self->priv->device;
}

gboolean
mm_sit_client_is_proxied (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    /* Ask the port rather than remembering what was asked for: open() may have
     * fallen back to the device node, and a log that claims otherwise is worse
     * than no log at all. */
    return mm_port_get_subsys (MM_PORT (self->priv->port)) == MM_PORT_SUBSYS_UNIX;
}

gboolean
mm_sit_client_open (MMSitClient  *self,
                    GError      **error)
{
    g_autoptr(GError) inner = NULL;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    if (mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port)))
        return TRUE;

    if (!mm_port_serial_open (MM_PORT_SERIAL (self->priv->port), &inner)) {
        g_autofree gchar *node = NULL;

        /* Configured for the proxy but it did not answer. Rather than leave the
         * modem dead, say so loudly and take the device node -- the two
         * transports are interchangeable, so a proxy that is not running should
         * cost debuggability, not the network. */
        if (!self->priv->proxy_fallback_device) {
            g_propagate_error (error, g_steal_pointer (&inner));
            return FALSE;
        }

        mm_obj_warn (self->priv->port,
                     "couldn't reach sit-proxy (%s); falling back to %s directly, "
                     "so tools cannot share the port",
                     inner->message, self->priv->proxy_fallback_device);

        node = g_steal_pointer (&self->priv->proxy_fallback_device);
        sit_client_take_port (self, mm_port_serial_sit_new (node));
        if (!mm_port_serial_open (MM_PORT_SERIAL (self->priv->port), error))
            return FALSE;
    }

    self->priv->unsolicited_handler_id =
        mm_port_serial_sit_add_unsolicited_handler (self->priv->port,
                                                     (MMPortSerialSitUnsolicitedFn) port_indication,
                                                     self,
                                                     NULL);
    return TRUE;
}

void
mm_sit_client_invalidate_sim (MMSitClient *self)
{
    /* Call only for a confirmed subscription transition, not every global
     * SIM notification. Re-enabling even the same ICCID needs radio setup. */
    self->priv->has_carrier_sim_info = FALSE;
    self->priv->sim_iccid_len = 0;
    self->priv->sim_aid_len = 0;
    self->priv->sim_aid_count = 0;
    self->priv->smsc_len = 0;
    self->priv->radio_state_trusted = FALSE;
}

void
mm_sit_client_close (MMSitClient *self)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    mm_sit_client_invalidate_sim (self);
    memset (&self->priv->carrier_sim_info, 0, sizeof (self->priv->carrier_sim_info));
    self->priv->boot_config_done = FALSE;
    self->priv->boot_sequence_done = FALSE;
    self->priv->struct_versions_known = FALSE;
    if (!mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port)))
        return;
    if (self->priv->unsolicited_handler_id) {
        mm_port_serial_sit_remove_unsolicited_handler (self->priv->port,
                                                        self->priv->unsolicited_handler_id);
        self->priv->unsolicited_handler_id = 0;
    }
    mm_port_serial_close (MM_PORT_SERIAL (self->priv->port));
}

gboolean
mm_sit_client_is_open (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return mm_port_serial_is_open (MM_PORT_SERIAL (self->priv->port));
}

/*
 * Whether this stack's boot sequence has finished -- the config batch, the
 * card reads, and the post-radio-on replay.
 *
 * Stock lets about five seconds pass between the network becoming known and
 * asking for its IMS context, and it spends them finishing exactly this work:
 * measured on identical firmware, ch 0xf5 learns the network at 6491ms and
 * asks at 11238ms, ch 0xf6 at 5974ms and 11332ms. We asked 0.5-0.7s after the
 * network was known and the first request was refused 168 on essentially every
 * boot, where stock's first request is granted every time.
 *
 * So the condition is the work, not a delay: a faster or slower device asks
 * whenever its own boot sequence has finished, which is the portable form of
 * what stock does.
 */
gboolean
mm_sit_client_boot_sequence_done (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return self->priv->boot_sequence_done;
}

void
mm_sit_client_set_boot_sequence_done (MMSitClient *self)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));
    self->priv->boot_sequence_done = TRUE;
}

void
mm_sit_client_get_device_identity (MMSitClient  *self,
                                   const gchar **out_manufacturer,
                                   const gchar **out_model,
                                   const gchar **out_os_name,
                                   const gchar **out_os_version,
                                   const gchar **out_software_version)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    if (out_manufacturer)
        *out_manufacturer = self->priv->device_manufacturer;
    if (out_model)
        *out_model = self->priv->device_model;
    if (out_os_name)
        *out_os_name = self->priv->device_os_name;
    if (out_os_version)
        *out_os_version = self->priv->device_os_version;
    if (out_software_version)
        *out_software_version = self->priv->device_build;
}

/*
 * Whether the card carries an ISIM application.
 *
 * It decides one hidden-menu value: stock sends the IMS home domain only for
 * a card that has an ISIM. Of this pair, the Videotron card has one and is
 * sent ims.mnc500.mcc302.3gppnetwork.org; the Telus card has none and is sent
 * no domain at all.
 */
/* 3GPP TS 31.103: the ISIM's application identifier. */
static const guint8 isim_aid_prefix[] = { 0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x04 };

static const guint8 *
isim_aid (MMSitClient *self,
          gsize       *out_len)
{
    guint i;

    for (i = 0; i < self->priv->sim_aid_count; i++) {
        if (self->priv->sim_aid_lens[i] >= G_N_ELEMENTS (isim_aid_prefix) &&
            memcmp (self->priv->sim_aids[i], isim_aid_prefix,
                    G_N_ELEMENTS (isim_aid_prefix)) == 0) {
            *out_len = self->priv->sim_aid_lens[i];
            return self->priv->sim_aids[i];
        }
    }
    *out_len = 0;
    return NULL;
}

/*
 * EF_DOMAIN (TS 31.103 4.2.3): a single TLV, tag 0x80, holding the home
 * network domain name. Stock puts it in the hidden menu as tag 778.
 */
static void
isim_domain_ready (MMSitClient  *self,
                   GAsyncResult *res,
                   gpointer      unused)
{
    g_autoptr(GBytes) data = NULL;
    const guint8     *raw;
    gsize             len = 0;

    /* A card may have an ISIM and still refuse the file; that is not an
     * error, it only means no domain to send. */
    data = mm_sit_client_read_ef_finish (self, res, NULL, NULL);
    if (!data)
        return;
    raw = g_bytes_get_data (data, &len);
    if (len < 2 || raw[0] != 0x80 || raw[1] == 0 || (gsize) raw[1] + 2 > len)
        return;
    g_free (self->priv->isim_domain);
    self->priv->isim_domain = g_strndup ((const gchar *) raw + 2, raw[1]);
    mm_obj_dbg (self, "ISIM home domain '%s'", self->priv->isim_domain);
}

static void
read_isim_domain (MMSitClient  *self,
                  GCancellable *cancellable)
{
    const guint8 *aid;
    gsize         aid_len = 0;

    aid = isim_aid (self, &aid_len);
    if (!aid)
        return;
    mm_sit_client_read_ef_for_aid (self, 0x6f03, aid, aid_len, cancellable,
                                   (GAsyncReadyCallback) isim_domain_ready, NULL);
}

/* The home domain the ISIM names, or NULL. Owned by @self. */
const gchar *
mm_sit_client_get_isim_domain (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return self->priv->isim_domain;
}

gboolean
mm_sit_client_sim_has_isim (MMSitClient *self)
{
    gsize len = 0;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    return isim_aid (self, &len) != NULL;
}

gboolean
mm_sit_client_get_carrier_sim_info (MMSitClient         *self,
                                    MMSitCarrierSimInfo *out_info)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_info != NULL, FALSE);

    if (!self->priv->has_carrier_sim_info)
        return FALSE;
    *out_info = self->priv->carrier_sim_info;
    return TRUE;
}

/* The CP pushes CARRIER_CONFIG_SIM_INFO during boot; a client that opens
 * after that burst has an empty cache.  The record is re-queried: the CP
 * answers status 22 when it holds no record, in which case this errors and
 * the caller falls back as before. */
static void
query_carrier_sim_info_ready (MMSitClient  *self,
                              GAsyncResult *res,
                              GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitCarrierSimInfo     info;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response &&
        mm_sit_message_parse_carrier_sim_info (response, &info, NULL)) {
        self->priv->carrier_sim_info = info;
        self->priv->has_carrier_sim_info = TRUE;
        g_task_return_pointer (task,
                               g_memdup (&info, sizeof (info)),
                               g_free);
    } else
        g_task_return_error (task,
                             error ? g_steal_pointer (&error)
                                   : g_error_new (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                                  "SIT CARRIER_CONFIG_SIM_INFO response is not parseable"));
    g_object_unref (task);
}

void
mm_sit_client_query_carrier_sim_info (MMSitClient         *self,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    g_autoptr(GByteArray) request = NULL;

    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    request = mm_sit_request_new_query (MM_SIT_MESSAGE_CARRIER_CONFIG_SIM_INFO,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) query_carrier_sim_info_ready,
                           g_task_new (self, cancellable, callback, user_data));
}

MMSitCarrierSimInfo *
mm_sit_client_query_carrier_sim_info_finish (MMSitClient   *self,
                                             GAsyncResult  *res,
                                             GError       **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

gboolean
mm_sit_client_radio_state_trusted (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return self->priv->radio_state_trusted;
}

/* The distrust only exists to force the bring-up chain to run once. Once it
 * has, the CP's report is all there is, and keeping it distrusted would pin
 * the core's power state at "low" forever -- every later power-down then
 * becomes a no-op and the radio can never be cycled at all. */
void
mm_sit_client_assume_radio_state (MMSitClient *self)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));
    self->priv->radio_state_trusted = TRUE;
}

static void
port_command_ready (MMPortSerialSit *port,
                    GAsyncResult    *res,
                    GTask           *task)
{
    MMSitMessage *response;
    GError       *error = NULL;

    response = mm_port_serial_sit_command_finish (port, res, &error);
    if (!response)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, response, (GDestroyNotify) mm_sit_message_free);
    g_object_unref (task);
}

void
mm_sit_client_command (MMSitClient        *self,
                       GByteArray         *request,
                       guint32             timeout_seconds,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
    GTask *task;

    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    task = g_task_new (self, cancellable, callback, user_data);
    mm_port_serial_sit_command (self->priv->port,
                                request,
                                timeout_seconds,
                                cancellable,
                                (GAsyncReadyCallback) port_command_ready,
                                task);
}

/* Send a frame the CP does not answer; see the port layer for why. */
void
mm_sit_client_send_indication (MMSitClient *self,
                               GByteArray  *frame)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));
    mm_port_serial_sit_send_indication (self->priv->port, frame);
}

MMSitMessage *
mm_sit_client_command_finish (MMSitClient   *self,
                              GAsyncResult  *res,
                              GError       **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

guint16
mm_sit_client_next_token (MMSitClient *self)
{
    return mm_port_serial_sit_next_token (self->priv->port);
}

static void
get_radio_ready (MMSitClient *self,
                 GAsyncResult *res,
                 GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    MMSitRadioState         state;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && message_check_success (response, &error) &&
        mm_sit_message_parse_radio_state (response, &state, &error))
        g_task_return_int (task, state);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

void
mm_sit_client_get_radio_state (MMSitClient        *self,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_RADIO_POWER,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_radio_ready, task);
}

MMSitRadioState
mm_sit_client_get_radio_state_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), MM_SIT_RADIO_STATE_UNKNOWN);
    return g_task_propagate_int (G_TASK (res), error);
}

static void
checked_command_ready (MMSitClient *self,
                       GAsyncResult *res,
                       GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && message_check_success (response, &error))
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

static void
run_checked_command (MMSitClient        *self,
                     GByteArray         *request,
                     guint32             timeout_seconds,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);
    mm_sit_client_command (self, request, timeout_seconds, cancellable,
                           (GAsyncReadyCallback) checked_command_ready, task);
}

typedef struct {
    MMSitRadioState state;
    guint           poll_attempts;
    guint           retry_id;
    gboolean        request_accepted;
} SetRadioContext;

static void set_radio_poll (GTask *task);

static void
set_radio_context_free (SetRadioContext *ctx)
{
    if (ctx->retry_id)
        g_source_remove (ctx->retry_id);
    g_free (ctx);
}

static gboolean
set_radio_retry (GTask *task)
{
    SetRadioContext *ctx = g_task_get_task_data (task);

    ctx->retry_id = 0;
    set_radio_poll (task);
    return G_SOURCE_REMOVE;
}

static void
set_radio_poll_ready (MMSitClient *self,
                      GAsyncResult *res,
                      GTask *task)
{
    SetRadioContext *ctx = g_task_get_task_data (task);
    MMSitRadioState  state;
    GError          *error = NULL;

    state = mm_sit_client_get_radio_state_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (state == ctx->state) {
        /* Only a transition the CP accepted proves the state is real. A fresh
         * CP refuses the request and still reports the target state, and
         * trusting that hollow answer defeats the guard in load_power_state --
         * the core then never re-runs the power-up chain, so the CP attaches
         * with no APN and every SETUP_DATA_CALL fails with cause 133. */
        if (ctx->request_accepted)
            self->priv->radio_state_trusted = TRUE;
        if (state == MM_SIT_RADIO_STATE_OFF && self->priv->cp) {
            mm_sit_cp_release_stack_data (self->priv->cp, self->priv->stack);
            g_clear_pointer (&self->priv->cids, g_hash_table_unref);
        }
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    if (++ctx->poll_attempts >= 15) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                 "SIT radio did not reach state %u", ctx->state);
        g_object_unref (task);
        return;
    }
    ctx->retry_id = g_timeout_add_seconds (1, (GSourceFunc) set_radio_retry, task);
}

static void
set_radio_request_ready (MMSitClient *self,
                         GAsyncResult *res,
                         GTask *task)
{
    g_autoptr(GError) error = NULL;

    /*
     * The CP answers GENERIC_FAILURE when asked to put the radio into the state
     * it is already in, which is not a failure of anything we care about -- and
     * we send the request unconditionally on purpose (see below), so it is the
     * expected answer half the time.  The poll that follows is the authority on
     * the resulting state either way, so note the refusal and carry on; a state
     * that genuinely never arrives still fails there.
     */
    SetRadioContext *ctx = g_task_get_task_data (task);

    if (!g_task_propagate_boolean (G_TASK (res), &error))
        mm_obj_dbg (self->priv->port, "radio power request refused (%s); polling the state anyway",
                    error->message);
    else
        ctx->request_accepted = TRUE;

    set_radio_poll (task);
}

static void
set_radio_poll (GTask *task)
{
    MMSitClient *self = g_task_get_source_object (task);

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }
    mm_sit_client_get_radio_state (self, g_task_get_cancellable (task),
                                   (GAsyncReadyCallback) set_radio_poll_ready, task);
}

void
mm_sit_client_set_radio_state (MMSitClient        *self,
                               MMSitRadioState     state,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    SetRadioContext       *ctx;
    GTask                 *task;
    g_autoptr(GByteArray)  request = NULL;

    task = g_task_new (self, cancellable, callback, user_data);
    if (state != MM_SIT_RADIO_STATE_OFF && state != MM_SIT_RADIO_STATE_ON) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "Invalid SIT radio state %u", state);
        g_object_unref (task);
        return;
    }

    ctx = g_new0 (SetRadioContext, 1);
    ctx->state = state;
    g_task_set_task_data (task, ctx, (GDestroyNotify) set_radio_context_free);

    /* Always send the request, even when the CP already reports the target
     * state: a freshly booted CP claims the radio is on while it is parked
     * (never searching), and only an explicit SET_RADIO_POWER starts the
     * PLMN search (HW-verified; the downstream RIL sends it too). */
    request = mm_sit_request_new_set_radio_power (mm_sit_client_next_token (self), state);
    run_checked_command (self, request, 30, cancellable,
                         (GAsyncReadyCallback) set_radio_request_ready, task);
}

gboolean
mm_sit_client_set_radio_state_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
get_sim_ready (MMSitClient *self,
               GAsyncResult *res,
               GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitSimStatus         *status;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    status = g_new0 (MMSitSimStatus, 1);
    if (response && mm_sit_message_parse_sim_status (response, status, &error)) {
        if (status->iccid_len) {
            /* A profile refresh can leave GET_RADIO_POWER reporting on while
             * the new subscription is parked. Make the replacement modem run
             * carrier/attach-APN setup and an explicit radio-on again. Merely
             * receiving a global status indication must not reset the other
             * DSDS stack's radio state. */
            if (self->priv->sim_iccid_len &&
                (self->priv->sim_iccid_len != status->iccid_len ||
                 memcmp (self->priv->sim_iccid, status->iccid, status->iccid_len)))
                self->priv->radio_state_trusted = FALSE;
            self->priv->sim_iccid_len = status->iccid_len;
            memcpy (self->priv->sim_iccid, status->iccid, status->iccid_len);
        }
        if (status->app_count) {
            guint a;

            self->priv->sim_aid_len = MIN (status->apps[0].aid_len, MM_SIT_MAX_AID_LENGTH);
            memcpy (self->priv->sim_aid, status->apps[0].aid, self->priv->sim_aid_len);
            self->priv->sim_aid_count = MIN (status->app_count, MM_SIT_MAX_SIM_APPS);
            for (a = 0; a < self->priv->sim_aid_count; a++) {
                self->priv->sim_aid_lens[a] = MIN (status->apps[a].aid_len, MM_SIT_MAX_AID_LENGTH);
                memcpy (self->priv->sim_aids[a], status->apps[a].aid, self->priv->sim_aid_lens[a]);
            }
        } else {
            self->priv->sim_aid_len = 0;
            self->priv->sim_aid_count = 0;
        }
        g_task_return_pointer (task, status, g_free);
    } else {
        g_free (status);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_sim_status (MMSitClient        *self,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_STATUS,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_sim_ready, task);
}

gboolean
mm_sit_client_get_sim_status_finish (MMSitClient     *self,
                                     GAsyncResult    *res,
                                     MMSitSimStatus  *out_status,
                                     GError         **error)
{
    MMSitSimStatus *status;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    status = g_task_propagate_pointer (G_TASK (res), error);
    if (!status)
        return FALSE;
    *out_status = *status;
    g_free (status);
    return TRUE;
}

/*****************************************************************************/
/* SMS */

typedef struct {
    guint8   smsc[MM_SIT_MAX_SMSC_LEN];
    gsize    smsc_len;
    GBytes  *tpdu;
    gboolean expect_more;
    gboolean over_ims;
} SendSmsContext;

static void
send_sms_context_free (SendSmsContext *ctx)
{
    g_clear_pointer (&ctx->tpdu, g_bytes_unref);
    g_slice_free (SendSmsContext, ctx);
}

static void send_sms_step (GTask *task);

static void
send_sms_smsc_ready (MMSitClient  *self,
                     GAsyncResult *res,
                     GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    SendSmsContext         *ctx = g_task_get_task_data (task);
    guint8                  smsc[MM_SIT_MAX_SMSC_LEN];
    gsize                   len = 0;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_smsc_address (response, smsc, &len, &error) && len) {
        memcpy (self->priv->smsc, smsc, len);
        self->priv->smsc_len = len;
        memcpy (ctx->smsc, smsc, len);
        ctx->smsc_len = len;
    } else {
        /* Not fatal: send without one and let the CP decide. */
        mm_obj_dbg (self->priv->port, "couldn't read the service centre address: %s",
                    error ? error->message : "empty");
        self->priv->smsc_len = 0;
    }
    send_sms_step (task);
}

static void
send_sms_ready (MMSitClient  *self,
                GAsyncResult *res,
                GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    guint32                 reference = 0;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_send_sms_result (response, &reference, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_return_int (task, (gssize) reference);
    g_object_unref (task);
}

/* Send the part, first reading the service centre address if it is not known
 * and the caller supplied none. */
static void
send_sms_step (GTask *task)
{
    MMSitClient          *self = g_task_get_source_object (task);
    SendSmsContext       *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;
    gconstpointer         tpdu;
    gsize                 tpdu_len = 0;

    if (!ctx->smsc_len && !self->priv->smsc_len) {
        request = mm_sit_request_new_get_smsc_address (mm_sit_client_next_token (self));
        if (request) {
            mm_sit_client_command (self, request, 10, g_task_get_cancellable (task),
                                   (GAsyncReadyCallback) send_sms_smsc_ready, task);
            return;
        }
        /* Fall through and send without one. */
    }
    if (!ctx->smsc_len && self->priv->smsc_len) {
        memcpy (ctx->smsc, self->priv->smsc, self->priv->smsc_len);
        ctx->smsc_len = self->priv->smsc_len;
    }

    tpdu = g_bytes_get_data (ctx->tpdu, &tpdu_len);
    request = mm_sit_request_new_send_sms (mm_sit_client_next_token (self),
                                           ctx->smsc, ctx->smsc_len,
                                           tpdu, tpdu_len,
                                           ctx->expect_more, ctx->over_ims,
                                           &error);
    if (!request) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    /*
     * The network round trip is on the CP's side of this, so allow for it --
     * generously. Measured on the CS group: answers at 10.5, 13.7, 15.5, 17.2
     * and 29.5s, with the message already delivered well before the answer.
     * A timeout here can no longer be retried (SMS submission is not
     * idempotent), so it has to be long enough that reaching it really does
     * mean the CP never answered.
     */
    mm_sit_client_command (self, request, 180, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) send_sms_ready, task);
}

void
mm_sit_client_send_sms (MMSitClient        *self,
                        const guint8       *smsc,
                        gsize               smsc_len,
                        const guint8       *tpdu,
                        gsize               tpdu_len,
                        gboolean            expect_more,
                        gboolean            over_ims,
                        GCancellable       *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    SendSmsContext *ctx;
    GTask          *task;

    task = g_task_new (self, cancellable, callback, user_data);

    ctx = g_slice_new0 (SendSmsContext);
    ctx->smsc_len = MIN (smsc_len, MM_SIT_MAX_SMSC_LEN);
    if (ctx->smsc_len)
        memcpy (ctx->smsc, smsc, ctx->smsc_len);
    ctx->tpdu = g_bytes_new (tpdu, tpdu_len);
    ctx->expect_more = expect_more;
    ctx->over_ims = over_ims;
    g_task_set_task_data (task, ctx, (GDestroyNotify) send_sms_context_free);

    send_sms_step (task);
}

gboolean
mm_sit_client_send_sms_finish (MMSitClient  *self,
                               GAsyncResult *res,
                               guint32      *out_reference,
                               GError      **error)
{
    GError *inner = NULL;
    gssize  value;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);

    value = g_task_propagate_int (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return FALSE;
    }
    if (out_reference)
        *out_reference = (guint32) value;
    return TRUE;
}

static void
ack_sms_ready (MMSitClient  *self,
               GAsyncResult *res,
               GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

void
mm_sit_client_ack_sms (MMSitClient        *self,
                       gboolean            over_ims,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_sms_ack (mm_sit_client_next_token (self), over_ims);
    if (!request) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "couldn't build the SMS acknowledgement");
        g_object_unref (task);
        return;
    }
    mm_sit_client_command (self, request, 10, cancellable,
                           (GAsyncReadyCallback) ack_sms_ready, task);
}

gboolean
mm_sit_client_ack_sms_finish (MMSitClient  *self,
                              GAsyncResult *res,
                              GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*****************************************************************************/
/* Elementary-file reads
 *
 * One file read is two or more SIM_IO exchanges: GET_RESPONSE first, because
 * only the card knows whether the file is transparent or made of records and
 * how big it is, then READ_BINARY or READ_RECORD with those numbers. Callers
 * get the whole file (records concatenated) plus its geometry, and decode the
 * contents themselves -- that part is plain 3GPP, not SIT.
 */

/* P3 is one byte, so a transparent file longer than this is read in several
 * passes with the offset in P1/P2. */
#define SIM_IO_MAX_READ 255
/* Backstop on a file that claims an implausible number of records; a truncated
 * read is reported rather than passed off as the whole file. */
/*
 * Safety bound on record reads of one file, not a record count: the loop stops
 * when the SIM says there are no records left.
 *
 * It was 32, which is fewer records than real files carry. Stock reads 51
 * records of EF_OPL (0x6fc6) and 50 of EF_PNN (0x6fc5) on the secondary SIM
 * here -- identical in all three captures, because it is that SIM's record
 * count -- and we stopped at 32, which this loop treats as an error rather
 * than a short read. Counting records off the SIM instead of a constant is
 * also the only form that is right on somebody else's SIM.
 */
#define SIM_IO_MAX_PASSES 128
/* Absolute record addressing, the mode rild uses. */
#define SIM_IO_RECORD_MODE_ABSOLUTE 4

typedef struct {
    MMSitEfInfo  info;
    guint16      file_id;
    guint16      path[MM_SIT_MAX_SIM_IO_PATH];
    guint        path_len;
    GByteArray  *data;
    guint        passes;
    /* Transparent: bytes still to read. Records: the next record number. */
    guint        remaining;
    guint        next_record;
    /*
     * Stop after this many data passes, 0 meaning the whole file. The parity
     * pass uses 1: stock issues GET_RESPONSE plus a single read for most files
     * and reads every record only of the name tables, so reading everything
     * overshot it -- 31 records of EF_SMS (0x6f3c) and 25 of 0x6f3b where
     * stock reads none at all.
     */
    guint        max_data_passes;
    /*
     * The application the file lives under. Empty means the subscription
     * application, which is every file but the ISIM's: EF_DOMAIN and its
     * neighbours exist only under ADF_ISIM, and a read of them against the
     * USIM is refused.
     */
    guint8       aid[MM_SIT_MAX_AID_LENGTH];
    gsize        aid_len;
} ReadEfContext;

static void
read_ef_context_free (ReadEfContext *ctx)
{
    g_clear_pointer (&ctx->data, g_byte_array_unref);
    g_slice_free (ReadEfContext, ctx);
}

static void read_ef_step (GTask *task);

/* Turn a card refusal into an error naming the status word, which is the only
 * place a SIM_IO failure shows up: the transport status stays zero. */
static gboolean
sim_io_check (MMSitClient              *self,
              GAsyncResult             *res,
              guint16                   file_id,
              MMSitSimIoResponse       *out_response,
              GError                  **error)
{
    g_autoptr(MMSitMessage) response = NULL;

    response = mm_sit_client_command_finish (self, res, error);
    if (!response)
        return FALSE;
    if (!mm_sit_message_parse_sim_io (response, out_response, error))
        return FALSE;
    if (!out_response->success) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "the SIM refused a read of EF %04X (sw1 %02X sw2 %02X)",
                     file_id, out_response->sw1, out_response->sw2);
        return FALSE;
    }
    return TRUE;
}

static void
read_ef_data_ready (MMSitClient  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    ReadEfContext      *ctx = g_task_get_task_data (task);
    MMSitSimIoResponse  response;
    GError             *error = NULL;

    if (!sim_io_check (self, res, ctx->file_id, &response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_byte_array_append (ctx->data, response.data, response.data_len);
    if (ctx->info.structure == MM_SIT_EF_TRANSPARENT)
        ctx->remaining -= MIN (ctx->remaining, response.data_len);
    else {
        ctx->next_record++;
        ctx->remaining--;
    }
    read_ef_step (task);
}

static void
read_ef_info_ready (MMSitClient  *self,
                    GAsyncResult *res,
                    GTask        *task)
{
    ReadEfContext      *ctx = g_task_get_task_data (task);
    MMSitSimIoResponse  response;
    GError             *error = NULL;

    if (!sim_io_check (self, res, ctx->file_id, &response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!mm_sit_file_control_parameters_parse (response.data, response.data_len,
                                               &ctx->info, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (ctx->info.structure == MM_SIT_EF_TRANSPARENT)
        ctx->remaining = ctx->info.size;
    else {
        ctx->remaining = ctx->info.record_count;
        ctx->next_record = 1;
        if (!ctx->info.record_len) {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                     "EF %04X reports records of no length",
                                     ctx->file_id);
            g_object_unref (task);
            return;
        }
    }
    read_ef_step (task);
}

static void
read_ef_step (GTask *task)
{
    MMSitClient          *self = g_task_get_source_object (task);
    ReadEfContext        *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;
    MMSitSimIoRequest     sim_io = { 0 };
    GAsyncReadyCallback   ready;

    sim_io.file_id  = ctx->file_id;
    sim_io.path_len = ctx->path_len;
    memcpy (sim_io.path, ctx->path, sizeof (sim_io.path));

    if (!ctx->passes) {
        sim_io.command = MM_SIT_SIM_IO_GET_RESPONSE;
        sim_io.p3      = 15;
        ready = (GAsyncReadyCallback) read_ef_info_ready;
    } else if (ctx->max_data_passes && ctx->passes > ctx->max_data_passes) {
        /* Capped: hand back what came in rather than failing. */
        g_task_return_pointer (task,
                               g_byte_array_free_to_bytes (g_steal_pointer (&ctx->data)),
                               (GDestroyNotify) g_bytes_unref);
        g_object_unref (task);
        return;
    } else if (!ctx->remaining) {
        g_task_return_pointer (task,
                               g_byte_array_free_to_bytes (g_steal_pointer (&ctx->data)),
                               (GDestroyNotify) g_bytes_unref);
        g_object_unref (task);
        return;
    } else if (ctx->passes >= SIM_IO_MAX_PASSES) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "EF %04X needs more than %u reads; giving up with "
                                 "%u of %u bytes",
                                 ctx->file_id, SIM_IO_MAX_PASSES,
                                 ctx->data->len, ctx->info.size);
        g_object_unref (task);
        return;
    } else if (ctx->info.structure == MM_SIT_EF_TRANSPARENT) {
        guint offset = ctx->data->len;

        sim_io.command = MM_SIT_SIM_IO_READ_BINARY;
        sim_io.p1      = (offset >> 8) & 0xff;
        sim_io.p2      = offset & 0xff;
        sim_io.p3      = MIN (ctx->remaining, SIM_IO_MAX_READ);
        ready = (GAsyncReadyCallback) read_ef_data_ready;
    } else {
        sim_io.command = MM_SIT_SIM_IO_READ_RECORD;
        sim_io.p1      = ctx->next_record;
        sim_io.p2      = SIM_IO_RECORD_MODE_ABSOLUTE;
        sim_io.p3      = ctx->info.record_len;
        ready = (GAsyncReadyCallback) read_ef_data_ready;
    }

    ctx->passes++;
    request = mm_sit_request_new_sim_io (mm_sit_client_next_token (self), &sim_io,
                                         ctx->aid_len ? ctx->aid : self->priv->sim_aid,
                                         ctx->aid_len ? ctx->aid_len : self->priv->sim_aid_len,
                                         &error);
    if (!request) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    mm_sit_client_command (self, request, 10, g_task_get_cancellable (task), ready, task);
}

static void
read_ef_capped (MMSitClient        *self,
                guint16             file_id,
                const guint16      *path,
                guint               path_len,
                guint               max_data_passes,
                GCancellable       *cancellable,
                GAsyncReadyCallback callback,
                gpointer            user_data)
{
    ReadEfContext *ctx;
    GTask         *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (path_len > MM_SIT_MAX_SIM_IO_PATH) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "SIM file path is too deep");
        g_object_unref (task);
        return;
    }

    ctx = g_slice_new0 (ReadEfContext);
    ctx->file_id  = file_id;
    ctx->path_len = path_len;
    ctx->max_data_passes = max_data_passes;
    memcpy (ctx->path, path, path_len * sizeof (guint16));
    ctx->data = g_byte_array_new ();
    g_task_set_task_data (task, ctx, (GDestroyNotify) read_ef_context_free);

    read_ef_step (task);
}

/*
 * The same read against a named application.
 *
 * Only the ISIM needs it, and only because its files are not reachable from
 * the subscription application: a card with an ISIM answers EF_DOMAIN under
 * ADF_ISIM and refuses it everywhere else.
 */
void
mm_sit_client_read_ef_for_aid (MMSitClient        *self,
                               guint16             file_id,
                               const guint8       *aid,
                               gsize               aid_len,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    GTask         *task;
    ReadEfContext *ctx;

    task = g_task_new (self, cancellable, callback, user_data);
    ctx = g_slice_new0 (ReadEfContext);
    ctx->file_id = file_id;
    ctx->aid_len = MIN (aid_len, MM_SIT_MAX_AID_LENGTH);
    memcpy (ctx->aid, aid, ctx->aid_len);
    ctx->data = g_byte_array_new ();
    g_task_set_task_data (task, ctx, (GDestroyNotify) read_ef_context_free);
    read_ef_step (task);
}

void
mm_sit_client_read_ef (MMSitClient        *self,
                       guint16             file_id,
                       const guint16      *path,
                       guint               path_len,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
    read_ef_capped (self, file_id, path, path_len, 0, cancellable, callback, user_data);
}

GBytes *
mm_sit_client_read_ef_finish (MMSitClient  *self,
                              GAsyncResult *res,
                              MMSitEfInfo  *out_info,
                              GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);

    if (out_info) {
        ReadEfContext *ctx = g_task_get_task_data (G_TASK (res));

        *out_info = ctx->info;
    }
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
get_sim_slot_status_ready (MMSitClient *self,
                           GAsyncResult *res,
                           GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitSimSlotStatus     *status;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    status = g_new0 (MMSitSimSlotStatus, 1);
    if (response && mm_sit_message_parse_sim_slot_status (response, status, &error))
        g_task_return_pointer (task, status, g_free);
    else {
        g_free (status);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_sim_slot_status (MMSitClient        *self,
                                   GCancellable       *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIM_SLOT_STATUS,
                                        mm_sit_client_next_token (control_client (self)));
    mm_sit_client_command (control_client (self), request, 5, cancellable,
                           (GAsyncReadyCallback) get_sim_slot_status_ready, task);
}

gboolean
mm_sit_client_get_sim_slot_status_finish (MMSitClient        *self,
                                          GAsyncResult       *res,
                                          MMSitSimSlotStatus *out_status,
                                          GError            **error)
{
    MMSitSimSlotStatus *status;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_status != NULL, FALSE);

    status = g_task_propagate_pointer (G_TASK (res), error);
    if (!status)
        return FALSE;
    *out_status = *status;
    g_free (status);
    return TRUE;
}

void
mm_sit_client_set_logical_slot_mapping (MMSitClient                  *self,
                                        const MMSitLogicalSlotMapping *mappings,
                                        guint                         mapping_count,
                                        GCancellable                 *cancellable,
                                        GAsyncReadyCallback           callback,
                                        gpointer                      user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_logical_slot_mapping (mm_sit_client_next_token (self),
                                                            mappings,
                                                            mapping_count,
                                                            &error);
    if (!request) {
        GTask *task;

        task = g_task_new (self, cancellable, callback, user_data);
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_logical_slot_mapping_finish (MMSitClient  *self,
                                               GAsyncResult *res,
                                               GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/* A serving PLMN is also available while camped without registration. Keep the
 * raw registration state: the enable path provisions attachment before core
 * waits for registration, and a roaming subscription must remain roaming. */
static void get_registration_selection_ready (MMSitClient *self, GAsyncResult *res, GTask *task);

static void
get_registration_operator_ready (MMSitClient *self,
                                 GAsyncResult *res,
                                 GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GByteArray)   request = NULL;
    g_autoptr(GError)       error = NULL;
    MMSitRegistration      *registration = g_task_get_task_data (task);
    MMSitRegistration      *result;
    MMSitOperator           operator_info;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_operator (response, &operator_info, &error)) {
        mm_obj_dbg (self->priv->port, "registration check: reg state %u (registered %s), rat %u, serving plmn '%s'",
                    registration->state,
                    registration->registered ? "yes" : "no",
                    registration->radio_technology,
                    operator_info.plmn);

    } else {
        mm_obj_dbg (self->priv->port, "registration check: no operator response (%s); reg state %u",
                    error ? error->message : "parse failed", registration->state);
    }

    /*
     * And the selection mode, which stock issues as part of the same check --
     * 16-19 times on ch 0xf5 and 17-25 on 0xf6 across five boots, matching its
     * other three. We sent it once per boot, which was the single largest
     * count divergence left in the AP->CP stream.
     */
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_QUERY_NETWORK_SELECTION_MODE,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) get_registration_selection_ready,
                           task);
}

/* Last of the quartet; its answer is not needed, only the asking. */
static void
get_registration_selection_ready (MMSitClient  *self,
                                  GAsyncResult *res,
                                  GTask        *task)
{
    g_autoptr(MMSitMessage)  response = NULL;
    g_autoptr(GError)        error = NULL;
    MMSitRegistration       *registration = g_task_get_task_data (task);
    MMSitRegistration       *result;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response)
        mm_obj_dbg (self->priv->port, "registration check: no selection mode (%s)",
                    error->message);

    result = g_new (MMSitRegistration, 1);
    *result = *registration;
    g_task_return_pointer (task, result, g_free);
    g_object_unref (task);
}

static void
get_registration_ready (MMSitClient *self,
                        GAsyncResult *res,
                        GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GByteArray)   request = NULL;
    MMSitRegistration      *registration;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    registration = g_new0 (MMSitRegistration, 1);
    if (!response || !mm_sit_message_parse_registration (response, registration, &error)) {
        g_free (registration);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /*
     * Stock queries the operator on every registration check, not only when
     * the CP says it is not registered: across five boots on this baseband its
     * GET_OPERATOR count tracks its GET_DATA_REGISTRATION count almost exactly
     * (16-19 against 15-18 on ch 0xf5). Ours ran only on the unregistered
     * path, so it fell behind.
     */
    g_task_set_task_data (task, registration, g_free);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_OPERATOR,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) get_registration_operator_ready,
                           task);
}

/*
 * Registration queries written and still unanswered.
 *
 * Three call sites issue these and nothing serialises them, so with requests
 * pipelined they overlap -- up to six at once on hardware. Stock never has one
 * in flight at the moment it asks for an IMS context: across five captures its
 * companions there are setters (SET_PREFERRED_DATA_MODEM,
 * SET_FAST_DORMANCY_INFO) or nothing at all, and its answer comes back in
 * 1.0-2.6s against our 2.9-10.9s.
 *
 * Whether the overlap is what makes the CP slow is not established -- ours is
 * ~2.9s with two in flight and 10.9s with two in a different boot -- but
 * asking with them outstanding is a divergence from stock either way, and six
 * identical queries in flight is waste regardless.
 */
guint
mm_sit_client_registration_queries_in_flight (MMSitClient *self)
{
    static const guint16 ids[] = {
        MM_SIT_MESSAGE_GET_VOICE_REGISTRATION,
        MM_SIT_MESSAGE_GET_DATA_REGISTRATION,
    };

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), 0);
    if (!self->priv->port)
        return 0;
    return mm_port_serial_sit_pending_count (self->priv->port, ids, G_N_ELEMENTS (ids));
}

static void
reg_query_done (MMSitClient *self, GAsyncResult *res, gpointer kindp)
{
    RegQueryKind          kind = GPOINTER_TO_UINT (kindp);
    g_autoptr(GPtrArray)  waiters = NULL;
    g_autoptr(GError)     error = NULL;
    MMSitRegistration     reg = { 0 };
    gboolean              ok;
    guint                 i;

    ok = mm_sit_client_get_registration_finish (self, res, &reg, &error);

    /* Taken before completing anyone: a callback may ask again, and that must
     * start a fresh query rather than join the one that just finished. */
    waiters = g_steal_pointer (&self->priv->reg_waiters[kind]);
    if (!waiters)
        return;

    for (i = 0; i < waiters->len; i++) {
        RegWaiter *w = g_ptr_array_index (waiters, i);
        GTask     *task = g_task_new (self, NULL, w->callback, w->user_data);

        if (ok)
            g_task_return_pointer (task, g_memdup2 (&reg, sizeof (reg)), g_free);
        else
            g_task_return_error (task, g_error_copy (error));
        g_object_unref (task);
    }
}

/* See RegQueryKind: the first caller asks, the rest share the answer. */
static void
reg_query (MMSitClient        *self,
           RegQueryKind        kind,
           guint16             id,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    RegWaiter            *waiter;
    gboolean              first;

    waiter = g_new0 (RegWaiter, 1);
    waiter->callback = callback;
    waiter->user_data = user_data;

    first = (self->priv->reg_waiters[kind] == NULL);
    if (first)
        self->priv->reg_waiters[kind] = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (self->priv->reg_waiters[kind], waiter);
    if (!first)
        return;

    request = mm_sit_request_new_query (id, mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_registration_ready,
                           g_task_new (self, cancellable,
                                       (GAsyncReadyCallback) reg_query_done,
                                       GUINT_TO_POINTER (kind)));
}

void
mm_sit_client_get_registration (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    reg_query (self, REG_QUERY_DATA, MM_SIT_MESSAGE_GET_DATA_REGISTRATION,
               cancellable, callback, user_data);
}

gboolean
mm_sit_client_get_registration_finish (MMSitClient       *self,
                                       GAsyncResult      *res,
                                       MMSitRegistration *out_registration,
                                       GError           **error)
{
    MMSitRegistration *registration;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_registration != NULL, FALSE);

    registration = g_task_propagate_pointer (G_TASK (res), error);
    if (!registration)
        return FALSE;
    *out_registration = *registration;
    g_free (registration);
    return TRUE;
}

/*
 * Registration is only reported for the packet-switched domain by 0x0701. The CS
 * domain has its own query, and without it the core consolidates a permanent
 * 'unknown' for circuit-switched service. The two responses share a layout apart
 * from the radio-technology byte, which the codec accounts for.
 */
void
mm_sit_client_get_voice_registration (MMSitClient        *self,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data)
{
    reg_query (self, REG_QUERY_VOICE, MM_SIT_MESSAGE_GET_VOICE_REGISTRATION,
               cancellable, callback, user_data);
}

gboolean
mm_sit_client_get_voice_registration_finish (MMSitClient       *self,
                                             GAsyncResult      *res,
                                             MMSitRegistration *out_registration,
                                             GError           **error)
{
    return mm_sit_client_get_registration_finish (self, res, out_registration, error);
}

static void
get_operator_ready (MMSitClient *self,
                    GAsyncResult *res,
                    GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitOperator          *operator_info;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    operator_info = g_new0 (MMSitOperator, 1);
    if (response && mm_sit_message_parse_operator (response, operator_info, &error))
        g_task_return_pointer (task, operator_info, g_free);
    else {
        g_free (operator_info);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_operator (MMSitClient        *self,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_OPERATOR,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_operator_ready, task);
}

gboolean
mm_sit_client_get_operator_finish (MMSitClient   *self,
                                   GAsyncResult  *res,
                                   MMSitOperator *out_operator,
                                   GError       **error)
{
    MMSitOperator *operator_info;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_operator != NULL, FALSE);

    operator_info = g_task_propagate_pointer (G_TASK (res), error);
    if (!operator_info)
        return FALSE;
    *out_operator = *operator_info;
    g_free (operator_info);
    return TRUE;
}

static void
get_network_time_ready (MMSitClient  *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitNitzTime          *time;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    time = g_new0 (MMSitNitzTime, 1);
    if (!response || !mm_sit_message_parse_nitz_time (response, time, &error)) {
        g_free (time);
        g_task_return_error (task, error);
    } else if (!time->valid) {
        /* The CP answered, it just has no NITZ to report -- before the first
         * attach, or on a network that sends none. MM_CORE_ERROR_RETRY is what
         * the Time interface's timezone poll looks for to try again later. */
        g_free (time);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                                 "the modem has not received a network time yet");
    } else
        g_task_return_pointer (task, time, g_free);
    g_object_unref (task);
}

void
mm_sit_client_get_network_time (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_NITZ_TIME, mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_network_time_ready, task);
}

gboolean
mm_sit_client_get_network_time_finish (MMSitClient   *self,
                                       GAsyncResult  *res,
                                       MMSitNitzTime *out_time,
                                       GError       **error)
{
    MMSitNitzTime *time;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_time != NULL, FALSE);

    time = g_task_propagate_pointer (G_TASK (res), error);
    if (!time)
        return FALSE;
    *out_time = *time;
    g_free (time);
    return TRUE;
}

static void
get_radio_capability_ready (MMSitClient  *self,
                            GAsyncResult *res,
                            GTask        *task)
{
    g_autoptr(MMSitMessage)   response = NULL;
    GError                   *error = NULL;
    MMSitNetworkGenerations  *generations;

    response = mm_sit_client_command_finish (self, res, &error);
    generations = g_new0 (MMSitNetworkGenerations, 1);
    if (response && mm_sit_message_parse_radio_capability (response, generations, &error))
        g_task_return_pointer (task, generations, g_free);
    else {
        g_free (generations);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_radio_capability (MMSitClient        *self,
                                    GCancellable       *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_RADIO_CAPABILITY, mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_radio_capability_ready, task);
}

gboolean
mm_sit_client_get_radio_capability_finish (MMSitClient              *self,
                                           GAsyncResult             *res,
                                           MMSitNetworkGenerations  *out_generations,
                                           GError                  **error)
{
    MMSitNetworkGenerations *generations;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_generations != NULL, FALSE);

    generations = g_task_propagate_pointer (G_TASK (res), error);
    if (!generations)
        return FALSE;
    *out_generations = *generations;
    g_free (generations);
    return TRUE;
}

static void
get_base_band_version_ready (MMSitClient  *self,
                             GAsyncResult *res,
                             GTask        *task)
{
    g_autoptr(MMSitMessage)  response = NULL;
    GError                  *error = NULL;
    MMSitBaseBandVersion    *version;

    response = mm_sit_client_command_finish (self, res, &error);
    version = g_new0 (MMSitBaseBandVersion, 1);
    if (response && mm_sit_message_parse_base_band_version (response, version, &error))
        g_task_return_pointer (task, version, g_free);
    else {
        g_free (version);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_base_band_version (MMSitClient        *self,
                                     GCancellable       *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_base_band_version (mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_base_band_version_ready, task);
}

gboolean
mm_sit_client_get_base_band_version_finish (MMSitClient           *self,
                                            GAsyncResult          *res,
                                            MMSitBaseBandVersion  *out_version,
                                            GError               **error)
{
    MMSitBaseBandVersion *version;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_version != NULL, FALSE);

    version = g_task_propagate_pointer (G_TASK (res), error);
    if (!version)
        return FALSE;
    *out_version = *version;
    g_free (version);
    return TRUE;
}

static void
get_bands_ready (MMSitClient  *self,
                 GAsyncResult *res,
                 GTask        *task)
{
    g_autoptr(MMSitMessage)  response = NULL;
    GError                  *error = NULL;
    MMSitBandMode           *bands;

    response = mm_sit_client_command_finish (self, res, &error);
    bands = g_new0 (MMSitBandMode, 1);
    if (response && mm_sit_message_parse_band_mode (response, bands, &error))
        g_task_return_pointer (task, bands, g_free);
    else {
        g_free (bands);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
mm_sit_client_get_bands (MMSitClient        *self,
                         GCancellable       *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_MANUAL_BAND_MODE, mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 10, cancellable,
                           (GAsyncReadyCallback) get_bands_ready, task);
}

gboolean
mm_sit_client_get_bands_finish (MMSitClient    *self,
                                GAsyncResult   *res,
                                MMSitBandMode  *out_bands,
                                GError        **error)
{
    MMSitBandMode *bands;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_bands != NULL, FALSE);

    bands = g_task_propagate_pointer (G_TASK (res), error);
    if (!bands)
        return FALSE;
    *out_bands = *bands;
    g_free (bands);
    return TRUE;
}

/* Setting bands is read-modify-write: the request carries the CP's own
 * supported block back, so it has to be built from a fresh read rather than
 * from anything cached. */
typedef struct {
    GArray *lte;   /* guint16 3GPP LTE bands */
    GArray *wcdma; /* guint16 3GPP UTRAN bands; both empty lifts the restriction */
} SetBandsContext;

static void
set_bands_write_ready (MMSitClient  *self,
                       GAsyncResult *res,
                       GTask        *task)
{
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_bands_read_ready (MMSitClient  *self,
                      GAsyncResult *res,
                      GTask        *task)
{
    SetBandsContext       *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray)  request = NULL;
    MMSitBandMode          mode;
    GError                *error = NULL;

    if (!mm_sit_client_get_bands_finish (self, res, &mode, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    request = mm_sit_request_new_set_band_mode (mm_sit_client_next_token (self), &mode,
                                                (const guint16 *) ctx->lte->data, ctx->lte->len,
                                                (const guint16 *) ctx->wcdma->data, ctx->wcdma->len,
                                                &error);
    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, g_task_get_cancellable (task),
                         (GAsyncReadyCallback) set_bands_write_ready, task);
}

static void
set_bands_context_free (SetBandsContext *ctx)
{
    g_array_unref (ctx->lte);
    g_array_unref (ctx->wcdma);
    g_free (ctx);
}

/* 3GPP band numbers per RAT; both empty lifts any restriction. */
void
mm_sit_client_set_bands (MMSitClient        *self,
                         GArray             *lte,
                         GArray             *wcdma,
                         GCancellable       *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer            user_data)
{
    SetBandsContext *ctx;
    GTask           *task;

    task = g_task_new (self, cancellable, callback, user_data);
    ctx = g_new0 (SetBandsContext, 1);
    ctx->lte = g_array_ref (lte);
    ctx->wcdma = g_array_ref (wcdma);
    g_task_set_task_data (task, ctx, (GDestroyNotify) set_bands_context_free);

    mm_sit_client_get_bands (self, cancellable,
                             (GAsyncReadyCallback) set_bands_read_ready, task);
}

gboolean
mm_sit_client_set_bands_finish (MMSitClient   *self,
                                GAsyncResult  *res,
                                GError       **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
get_network_selection_mode_ready (MMSitClient  *self,
                                  GAsyncResult *res,
                                  GTask        *task)
{
    g_autoptr(MMSitMessage)    response = NULL;
    GError                    *error = NULL;
    MMSitNetworkSelectionMode  mode;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_network_selection_mode (response, &mode, &error))
        g_task_return_error (task, error);
    else
        g_task_return_int (task, mode);
    g_object_unref (task);
}

void
mm_sit_client_get_network_selection_mode (MMSitClient        *self,
                                          GCancellable       *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_QUERY_NETWORK_SELECTION_MODE,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 10, cancellable,
                           (GAsyncReadyCallback) get_network_selection_mode_ready, task);
}

gboolean
mm_sit_client_get_network_selection_mode_finish (MMSitClient               *self,
                                                 GAsyncResult              *res,
                                                 MMSitNetworkSelectionMode *out_mode,
                                                 GError                   **error)
{
    GError *inner_error = NULL;
    gssize  value;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_mode != NULL, FALSE);

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return FALSE;
    }
    *out_mode = (MMSitNetworkSelectionMode) value;
    return TRUE;
}

static void
get_imei_ready (MMSitClient  *self,
                GAsyncResult *res,
                GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    gchar                  *imei = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_imei (response, &imei, &error))
        g_task_return_error (task, error);
    else if (!imei)
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND,
                                 "the CP holds no IMEI");
    else
        g_task_return_pointer (task, imei, g_free);
    g_object_unref (task);
}

void
mm_sit_client_get_imei (MMSitClient        *self,
                        GCancellable       *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_IMEI, mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_imei_ready, task);
}

gchar *
mm_sit_client_get_imei_finish (MMSitClient   *self,
                               GAsyncResult  *res,
                               GError       **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);

    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
get_signal_ready (MMSitClient *self,
                  GAsyncResult *res,
                  GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    MMSitSignal            *signal;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    signal = g_new0 (MMSitSignal, 1);
    if (response && mm_sit_message_parse_signal (response, signal, &error))
        g_task_return_pointer (task, signal, g_free);
    else {
        g_free (signal);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

/* A layout the CP announced and we have no parser for. Refusing the query is
 * the point: sending it anyway buys a truncation at best, and for signal --
 * whose version is inferred from the payload length -- a future layout that
 * happened to be the same size would be decoded as the current one and answered
 * with plausible nonsense. See boot_config_device_info_ready. */
static gboolean
struct_version_refused (MMSitClient  *self,
                        gboolean      supported,
                        const gchar  *what,
                        guint8        version,
                        GTask        *task)
{
    /* Before the CP has answered 0x0922 there is no claim to hold against it. */
    if (!self->priv->struct_versions_known || supported)
        return FALSE;

    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "the CP speaks %s=%u, which this plugin cannot parse",
                             what, version);
    g_object_unref (task);
    return TRUE;
}

void
mm_sit_client_get_signal (MMSitClient        *self,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (struct_version_refused (self, self->priv->struct_versions.signal_strength_supported,
                                "SignalStrength", self->priv->struct_versions.signal_strength,
                                task))
        return;
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_SIGNAL_STRENGTH,
                                        mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_signal_ready, task);
}

gboolean
mm_sit_client_get_signal_finish (MMSitClient *self,
                                 GAsyncResult *res,
                                 MMSitSignal  *out_signal,
                                 GError      **error)
{
    MMSitSignal *signal;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_signal != NULL, FALSE);

    signal = g_task_propagate_pointer (G_TASK (res), error);
    if (!signal)
        return FALSE;
    *out_signal = *signal;
    g_free (signal);
    return TRUE;
}

static void
get_cell_info_ready (MMSitClient *self,
                     GAsyncResult *res,
                     GTask *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GArray                 *cells = NULL;
    GArray                 *nr_cells = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_cell_info_list (response, &cells, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* One response carries both views. NR records only appear under EN-DC, so an
     * absent NR list is the normal LTE-only case, not a failure. */
    if (!mm_sit_message_parse_nr_cell_info_list (response, &nr_cells, &error)) {
        mm_obj_dbg (self->priv->port, "cell info: NR records unparsable: %s", error->message);
        g_clear_error (&error);
        nr_cells = g_array_new (FALSE, FALSE, sizeof (MMSitCellInfoNr));
    }
    g_task_set_task_data (task, nr_cells, (GDestroyNotify) g_array_unref);
    g_task_return_pointer (task, cells, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

void
mm_sit_client_get_cell_info (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (struct_version_refused (self, self->priv->struct_versions.cell_info_supported,
                                "CellInfo", self->priv->struct_versions.cell_info, task) ||
        struct_version_refused (self, self->priv->struct_versions.cell_identity_supported,
                                "CellIdentity", self->priv->struct_versions.cell_identity, task))
        return;
    request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_CELL_INFO_LIST, mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_cell_info_ready, task);
}

GArray *
mm_sit_client_get_cell_info_finish (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GArray      **out_nr_cells,
                                    GError      **error)
{
    GArray *lte;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);

    lte = g_task_propagate_pointer (G_TASK (res), error);
    if (lte && out_nr_cells) {
        GArray *nr = g_task_get_task_data (G_TASK (res));

        *out_nr_cells = nr ? g_array_ref (nr) : NULL;
    }
    return lte;
}

static void
get_call_list_ready (MMSitClient  *self,
                     GAsyncResult *res,
                     GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GArray                 *calls = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_call_list (response, &calls, &error))
        g_task_return_pointer (task, calls, (GDestroyNotify) g_array_unref);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

void
mm_sit_client_get_call_list (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_call_list (mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_call_list_ready, task);
}

GArray *
mm_sit_client_get_call_list_finish (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

void
mm_sit_client_dial (MMSitClient        *self,
                    const gchar        *number,
                    GCancellable       *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GError               *error = NULL;

    request = mm_sit_request_new_dial (mm_sit_client_next_token (self), number, &error);
    if (!request) {
        GTask *task;

        task = g_task_new (self, cancellable, callback, user_data);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* The CP acks DIAL quickly; call *progress* then arrives as
     * CALL_STATE_CHANGED indications, not in this response. */
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_dial_finish (MMSitClient  *self,
                           GAsyncResult *res,
                           GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_answer (MMSitClient        *self,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_answer (mm_sit_client_next_token (self));
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_answer_finish (MMSitClient  *self,
                             GAsyncResult *res,
                             GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_send_dtmf (MMSitClient        *self,
                         const gchar        *digits,
                         GCancellable       *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GError               *error = NULL;

    request = mm_sit_request_new_dtmf (mm_sit_client_next_token (self), digits, &error);
    if (!request) {
        GTask *task;

        task = g_task_new (self, cancellable, callback, user_data);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_send_dtmf_finish (MMSitClient  *self,
                                GAsyncResult *res,
                                GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_hangup (MMSitClient        *self,
                      guint32             call_id,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_hangup (mm_sit_client_next_token (self), call_id);
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_hangup_finish (MMSitClient  *self,
                             GAsyncResult *res,
                             GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_indication_filter (MMSitClient        *self,
                                     guint32             filter,
                                     GCancellable       *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_indication_filter (mm_sit_client_next_token (self), filter);
    run_checked_command (self, request, 5, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_indication_filter_finish (MMSitClient  *self,
                                            GAsyncResult *res,
                                            GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_screen_state (MMSitClient        *self,
                                gboolean            screen_on,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_screen_state (mm_sit_client_next_token (self), screen_on);
    run_checked_command (self, request, 5, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_screen_state_finish (MMSitClient  *self,
                                       GAsyncResult *res,
                                       GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_carrier_manifest (MMSitClient         *self,
                                    const gchar         *directory,
                                    const guint8         sha1[MM_SIT_SHA1_LENGTH],
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GError               *error = NULL;

    request = mm_sit_request_new_carrier_manifest (mm_sit_client_next_token (self), directory, sha1, &error);
    if (!request) {
        g_task_report_error (self, callback, user_data,
                             mm_sit_client_set_carrier_manifest, error);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_carrier_manifest_finish (MMSitClient  *self,
                                           GAsyncResult *res,
                                           GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/* Identity handshake the downstream RIL performs once per CP boot before
 * turning the radio on: the CP parks a radio-on request that arrives before
 * it has digested this batch (and the carrier-config manifest), leaving the
 * radio in state "on" but never searching.  The CP answers 0x0922 with the
 * struct versions it speaks, e.g. "CellInfo=5,CellIdentity=5,SignalStrength=5".
 *
 * The three strings describe the *AP*, and they were the captured corpus values
 * -- "Pixel 9a" and an Android build id -- baked into a plugin that is not
 * specific to one board. They now come from the running system. Nothing in
 * 3GPP carries them: the network identifies a device by the IMEI's TAC, which
 * lives in the modem. HW-checked 2026-08-01 by sending
 * "Google Pixel 9a"/"postmarketOS edge"/"tegu": the CP answered status 0 with
 * the same version list, and registration, attach and data were unaffected.
 *
 * ID_MM_SIT_DEVICE_MODEL / _BUILD / _CODENAME override them, in case a CP or a
 * carrier profile ever turns out to care what it is told.
 *
 * 0x4605 SET_SW_VERSION_NUMBER, further down the batch, is the CP's own
 * two-digit firmware version rather than anything about the AP, so the batch
 * reads it back off GET_IMEISV first instead of carrying a constant. */
static void set_device_info (MMSitClient *self,
                             const gchar *model,
                             const gchar *build,
                             const gchar *codename,
                             const gchar *sw_version,
                             const gchar *cp_sw_version);

#define BOOT_CONFIG_DEVICE_MODEL_FALLBACK "Linux"
#define BOOT_CONFIG_DEVICE_BUILD_FALLBACK "unknown"

typedef enum {
    BOOT_CONFIG_STEP_AP_TIME,
    BOOT_CONFIG_STEP_DEVICE_INFO,
    BOOT_CONFIG_STEP_SOFTWARE_VERSION,
    BOOT_CONFIG_STEP_OEM_CONFIG,
    BOOT_CONFIG_STEP_MODEM_CONFIG,
    BOOT_CONFIG_STEP_BOOT_EFS,
    BOOT_CONFIG_STEP_UICC_RULES,
    BOOT_CONFIG_STEP_LAST,
} BootConfigStep;

typedef struct {
    BootConfigStep  step;
    gchar          *software_version;
} BootConfigContext;

static void
boot_config_context_free (BootConfigContext *ctx)
{
    g_free (ctx->software_version);
    g_free (ctx);
}

static void boot_config_step (GTask *task);

static void
boot_config_step_ready (MMSitClient *self,
                        GAsyncResult *res,
                        GTask *task)
{
    BootConfigContext *ctx = g_task_get_task_data (task);
    GError            *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx->step++;
    boot_config_step (task);
}

/* The 0x0922 reply is a version negotiation: the CP names the struct versions
 * it speaks for cell-info, cell-identity and signal-strength -- exactly the
 * three layouts the codec otherwise has to infer, cell-info by record size and
 * signal by payload length. Record them so a CP speaking a layout we cannot
 * read is refused by name here, rather than showing up much later as a
 * truncation at the first cell-info query, or (for signal, where a future
 * 200-byte V6 would measure as a V5) as a plausible wrong answer.
 *
 * A CP that does not name a layout has made no claim, so it is not held
 * against it; only an explicitly announced version we have no parser for is. */
static void
boot_config_device_info_ready (MMSitClient  *self,
                               GAsyncResult *res,
                               GTask        *task)
{
    BootConfigContext       *ctx = g_task_get_task_data (task);
    g_autoptr(MMSitMessage)  response = NULL;
    GError                  *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error) ||
        !mm_sit_message_parse_struct_versions (response, &self->priv->struct_versions, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    self->priv->struct_versions_known = TRUE;

    mm_obj_dbg (self, "CP struct versions: CellInfo=%u, CellIdentity=%u, SignalStrength=%u",
                self->priv->struct_versions.cell_info,
                self->priv->struct_versions.cell_identity,
                self->priv->struct_versions.signal_strength);
    if (!self->priv->struct_versions.cell_info_supported ||
        !self->priv->struct_versions.cell_identity_supported)
        mm_obj_warn (self, "no parser for the CP's cell-info layout "
                     "(CellInfo=%u, CellIdentity=%u); cell info will not be reported",
                     self->priv->struct_versions.cell_info,
                     self->priv->struct_versions.cell_identity);
    if (!self->priv->struct_versions.signal_strength_supported)
        mm_obj_warn (self, "no parser for the CP's signal layout (SignalStrength=%u); "
                     "signal strength will not be reported",
                     self->priv->struct_versions.signal_strength);

    ctx->step++;
    boot_config_step (task);
}

/* The software version 0x4605 carries is the CP's own, so a CP that will not
 * tell us has nothing for us to send back. Skip the message rather than fall
 * back on a constant -- a stale constant is the bug this replaced -- and say so,
 * because every other step of this batch answered. */
static void
boot_config_software_version_ready (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    BootConfigContext       *ctx = g_task_get_task_data (task);
    g_autoptr(MMSitMessage)  response = NULL;
    g_autoptr(GError)        error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_software_version (response, &ctx->software_version, &error))
        mm_obj_warn (self, "could not read the CP software version: %s", error->message);
    else if (!ctx->software_version)
        mm_obj_warn (self, "the CP holds no software version");

    ctx->step++;
    boot_config_step (task);
}

static void boot_config_step (GTask *task);

static void
boot_config_efs_ready (MMSitClient  *self,
                       GAsyncResult *res,
                       GTask        *task)
{
    BootConfigContext *ctx = g_task_get_task_data (task);

    mm_sit_client_read_boot_efs_finish (self, res, NULL);
    ctx->step++;
    boot_config_step (task);
}

static void
boot_config_uicc_ready (MMSitClient  *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    BootConfigContext *ctx = g_task_get_task_data (task);

    mm_sit_client_probe_uicc_access_rules_finish (self, res, NULL);
    ctx->step++;
    boot_config_step (task);
}

static void
boot_config_step (GTask *task)
{
    MMSitClient           *self = g_task_get_source_object (task);
    BootConfigContext     *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray)  request = NULL;
    GError                *error = NULL;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case BOOT_CONFIG_STEP_AP_TIME: {
        g_autoptr(GDateTime) now = NULL;

        /*
         * The AP's clock is the AP's, not a subscription's, and stock tells the
         * CP once: SET_AP_SYSTEM_TIME is its very first frame of the boot, on
         * ch 0xf5 with token 0, and never appears on 0xf6. We sent it on both.
         */
        if (self->priv->stack != 0) {
            ctx->step++;
            boot_config_step (task);
            return;
        }
        now = g_date_time_new_now_local ();
        request = mm_sit_request_new_set_ap_system_time (mm_sit_client_next_token (self), now, &error);
        break;
    }
    case BOOT_CONFIG_STEP_DEVICE_INFO:
        if (!self->priv->device_model)
            set_device_info (self, NULL, NULL, NULL, NULL, NULL);
        request = mm_sit_request_new_device_info (mm_sit_client_next_token (self),
                                                  self->priv->device_model,
                                                  self->priv->device_build,
                                                  self->priv->device_codename,
                                                  self->priv->device_sw_version,
                                                  &error);
        if (!request)
            break;
        mm_sit_client_command (self, request, 10, g_task_get_cancellable (task),
                               (GAsyncReadyCallback) boot_config_device_info_ready, task);
        return;
    case BOOT_CONFIG_STEP_SOFTWARE_VERSION:
        request = mm_sit_request_new_query (MM_SIT_MESSAGE_GET_IMEISV, mm_sit_client_next_token (self));
        mm_sit_client_command (self, request, 5, g_task_get_cancellable (task),
                               (GAsyncReadyCallback) boot_config_software_version_ready, task);
        return;
    case BOOT_CONFIG_STEP_OEM_CONFIG: {
        /*
         * Stock sends "67"; the SV digits of our GET_IMEISV are "64", so
         * echoing the modem back is right for the modem and wrong for parity.
         * Measured on identical firmware: IMEISV 35077975117992564 -> "64",
         * Android 0x4605 -> "67". A pinned value wins when the device package
         * supplies one; otherwise keep echoing the CP.
         */
        const gchar *version = self->priv->cp_sw_version ?: ctx->software_version;

        /* One CP, one software version: stock sends 0x4605 on ch 0xf5 only. */
        if (self->priv->stack != 0) {
            ctx->step++;
            boot_config_step (task);
            return;
        }
        if (!version) {
            ctx->step++;
            boot_config_step (task);
            return;
        }
        request = mm_sit_request_new_oem_config (mm_sit_client_next_token (self), version, &error);
        break;
    }
    case BOOT_CONFIG_STEP_MODEM_CONFIG:
        if (self->priv->cp && self->priv->stack == 1) {
            ctx->step++;
            boot_config_step (task);
            return;
        }
        request = mm_sit_request_new_set_modem_config_count (
            mm_sit_client_next_token (self), self->priv->cp ? 2 : 1);
        break;
    case BOOT_CONFIG_STEP_BOOT_EFS:
        read_isim_domain (self, g_task_get_cancellable (task));
        /* See mm_sit_client_read_boot_efs(). Cannot fail the boot. */
        mm_sit_client_read_boot_efs (self, g_task_get_cancellable (task),
                                     (GAsyncReadyCallback) boot_config_efs_ready,
                                     task);
        return;
    case BOOT_CONFIG_STEP_UICC_RULES:
        /* Stock's carrier-privilege discovery; see
         * mm_sit_client_probe_uicc_access_rules(). It cannot fail the boot. */
        mm_sit_client_probe_uicc_access_rules (self, g_task_get_cancellable (task),
                                               (GAsyncReadyCallback) boot_config_uicc_ready,
                                               task);
        return;
    case BOOT_CONFIG_STEP_LAST:
        self->priv->boot_config_done = TRUE;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    default:
        g_assert_not_reached ();
    }

    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, g_task_get_cancellable (task),
                         (GAsyncReadyCallback) boot_config_step_ready, task);
}

void
mm_sit_client_send_boot_config (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    BootConfigContext *ctx;
    GTask             *task;

    task = g_task_new (self, cancellable, callback, user_data);
    if (self->priv->boot_config_done) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    ctx = g_new0 (BootConfigContext, 1);
    g_task_set_task_data (task, ctx, (GDestroyNotify) boot_config_context_free);
    boot_config_step (task);
}

gboolean
mm_sit_client_send_boot_config_finish (MMSitClient  *self,
                                       GAsyncResult *res,
                                       GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * Connecting a bearer re-points the attach APN at that bearer's own APN, and it
 * has to, on this CP.
 *
 * SET_INITIAL_ATTACH_APN names the APN the CP puts in its ATTACH REQUEST and
 * the CP keeps whatever it was last told, so this is modem-wide state and not a
 * property of the context being brought up. Android never sends it from its
 * data-call path: it attaches on the carrier's IA entry once and asks for every
 * PDN it wants, IMS included, and the CP grants them.
 *
 * Ours will not. It refuses to create a PDN for the IMS DNN at all (see
 * attach_apn_resolve() in mm-broadband-modem-samsung-sit.c), so the only way to
 * get an IMS PDN is to attach on 'ims' and let the attach create it. That in
 * turn leaves the data APN with no context, and a SETUP_DATA_CALL for it is
 * refused the same way -- unless the attach is re-pointed at it first, which is
 * what this step does.
 *
 * So the two halves are a pair and neither works alone: 'ims' at power-up gets
 * IMS, and this step gets data. Removing this step with the IMS attach floor in
 * place costs mobile data on the stack that is attached for IMS; measured on
 * hardware, ESM-wise the CP simply answers 2253 for whichever APN it has no
 * context for. If the IMS refusal is ever understood and fixed, this step
 * should go and the floor with it.
 */
typedef enum {
    CONNECT_STEP_PREFERRED_MODEM,
    CONNECT_STEP_DATA_PROFILE,
    CONNECT_STEP_ATTACH_APN,
    CONNECT_STEP_CONFIG_0900,
    /*
     * Cell broadcast is not part of bringing up a context.
     *
     * There were two steps here that re-sent SET_BROADCAST_SMS_CONFIG and
     * SMS_BROADCAST_ACTIVATION on the way to every data call, and the config
     * they sent named one range, 4352-4356. The boot configuration has already
     * sent the full set by then, so this narrowed it: on whichever stack
     * carried data it switched CMAS 4370-4399 back off -- presidential,
     * extreme, severe and amber alerts -- for the rest of the boot.
     *
     * Stock sends neither here. Across five captures its SET_BROADCAST_SMS_CONFIG
     * and SMS_BROADCAST_ACTIVATION are always paired, always carry the full
     * channel set, and always land before the first data call: the last at
     * 7317ms against a first data call at 10387ms. See mm-sit-boot-config.c.
     */
    CONNECT_STEP_REGISTRATION,
    CONNECT_STEP_SETUP,
    CONNECT_STEP_FAST_DORMANCY,
    CONNECT_STEP_LAST,
} ConnectStep;

/*
 * Context ids
 *
 * The cid at SETUP_DATA_CALL @0x0c is the host's to pick and the CP echoes it
 * back; it is what keeps two concurrent contexts apart. Nothing in
 * ModemManager allocates one for us: the automatic allocator lives in
 * MMBroadbandBearer (select_profile_3gpp -> the profile manager), and our
 * bearer -- like MMBearerQmi -- derives from MMBaseBearer. QMI gets away with
 * that because a QMI modem allocates its own contexts from the settings passed
 * inline to WDS Start Network; this CP does not, so the allocation is ours.
 *
 * Hence this, rather than MMIfaceModem3gppProfileManager: the interface would
 * give us core's find_empty, but nothing would call it for our bearer class, so
 * we would be invoking it from here anyway -- and nothing consumes the D-Bus
 * profile list it adds (NetworkManager does not read profiles). See
 * docs/decisions/0005-ims-architecture.md.
 *
 * Allocation starts at 2 because SET_INITIAL_ATTACH_APN puts the attach context
 * on 1, and reuses per APN so that reconnecting the same APN does not burn a
 * context. Downstream uses 2 and 3 for ordinary calls on the first stack, which
 * is what this reproduces.
 *
 * The cid is no longer only an identifier: it picks the network interface. The
 * CP carries context n on PDP channel 180 + n, which the kernel exposes as
 * rmnet(n-1), so cid 1 owns rmnet0 and ordinary contexts land on rmnet1 up (see
 * get_context_data_port() in mm-bearer-samsung-sit.c for where that mapping is
 * read out of the vendor stack). Starting at 2 therefore stays right for a
 * second reason: rmnet0 belongs to the attach context. Note the *response* cid
 * is what selects the interface, so a CP that ever declined the cid we asked
 * for would still get the right netdev.
 *
 * Known gap: the table starts empty, so contexts left up by a previous
 * ModemManager are invisible to it. Closing that means seeding from
 * GET_DATA_CALL_LIST (0x0602) at enable; until then the failure mode is the one
 * we have always had -- SETUP_DATA_CALL hands back the existing context.
 */
#define CID_FIRST 2
/* Downstream was seen using 16 and 17, which look like the second stack's
 * range; staying below them costs nothing while one modem owns one stack.
 * TRACE NEEDED: the CP's real upper bound. The AT view says 1-11, but that
 * surface is a veneer that does not reflect SIT state, so it does not decide
 * this. */
#define CID_LAST 15

static guint8
cid_acquire (MMSitClient  *self,
             const gchar  *apn,
             GError      **error)
{
    guint cid;
    guint first = CID_FIRST + 15 * self->priv->stack;
    guint last = CID_LAST + 15 * self->priv->stack;

    if (!self->priv->cids)
        self->priv->cids = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, g_free);

    /* Reuse rather than duplicate: the same APN asked for twice is the same
     * context, which is also what core's find_best does on the AT path. */
    for (cid = first; cid <= last; cid++) {
        const gchar *held = g_hash_table_lookup (self->priv->cids,
                                                 GUINT_TO_POINTER (cid));
        if (held && g_str_equal (held, apn)) {
            mm_obj_dbg (self->priv->port, "reusing context id %u for APN '%s'", cid, apn);
            return (guint8) cid;
        }
    }

    for (cid = first; cid <= last; cid++) {
        if (!g_hash_table_contains (self->priv->cids, GUINT_TO_POINTER (cid))) {
            g_hash_table_insert (self->priv->cids, GUINT_TO_POINTER (cid),
                                 g_strdup (apn));
            mm_obj_dbg (self->priv->port, "context id %u allocated for APN '%s'", cid, apn);
            return (guint8) cid;
        }
    }

    g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_TOO_MANY,
                 "No free context id between %u and %u", first, last);
    return 0;
}

/*
 * Drop only this client's APN mapping, leaving the global reservation alone.
 * For the case where the reservation is held by something that is still using
 * it: cid_acquire() reuses a cid whose APN matches, so without this the next
 * attempt asks for the same blocked cid and is refused again, forever.
 */
static void
cid_forget (MMSitClient *self,
            guint8       cid)
{
    if (self->priv->cids &&
        g_hash_table_remove (self->priv->cids, GUINT_TO_POINTER ((guint) cid)))
        mm_obj_dbg (self->priv->port, "context id %u given up; it is reserved elsewhere", cid);
}

static void
cid_release (MMSitClient *self,
             guint8       cid)
{
    if (self->priv->cp)
        mm_sit_cp_release_data (self->priv->cp, cid);
    if (self->priv->cids &&
        g_hash_table_remove (self->priv->cids, GUINT_TO_POINTER ((guint) cid)))
        mm_obj_dbg (self->priv->port, "context id %u released", cid);
}

/*
 * The cid we asked for is a request; the CP's response carries the one it
 * actually used. They have always matched, but a reservation that tracks the
 * request rather than the answer would be quietly wrong the first time they
 * differ -- and the whole point of the table is that it agrees with the CP.
 */
static void
cid_confirm (MMSitClient *self,
             guint8       requested,
             guint8       assigned)
{
    g_autofree gchar *apn = NULL;

    if (requested == assigned || !self->priv->cids)
        return;

    apn = g_strdup (g_hash_table_lookup (self->priv->cids,
                                         GUINT_TO_POINTER ((guint) requested)));
    if (!apn)
        return;

    mm_obj_dbg (self->priv->port, "CP assigned context id %u, not the %u asked for",
                assigned, requested);
    g_hash_table_remove (self->priv->cids, GUINT_TO_POINTER ((guint) requested));
    g_hash_table_insert (self->priv->cids, GUINT_TO_POINTER ((guint) assigned),
                         g_steal_pointer (&apn));
}

typedef struct {
    ConnectStep    step;
    MMSitCp       *cp; /* keeps global ownership alive through asynchronous work */
    gboolean       setup_sent;
    gboolean       unexpected_cid;
    /* Reserved for the whole attempt, retries included, and released again if
     * the attempt does not end with a context up. */
    guint8         cid;
    gboolean       cid_kept;
    gchar         *apn;
    MMSitPdpType   pdp;
    /* Credentials, owned: the properties they came from need not outlive the
     * connect attempt. auth.user/password point into these two. */
    MMSitApnAuth   auth;
    gchar         *auth_user;
    gchar         *auth_password;
    guint32        apn_types;
    guint32        bearers;
    guint          registration_attempts;
    guint          setup_attempts;
    guint          retry_id;
    MMSitDataCall  call;
    GArray        *indicated_calls; /* latest DATA_CALL_LIST_CHANGED list */
    MMSitClient   *client;
    gulong         indication_handler_id;
} ConnectContext;

static void connect_step (GTask *task);

static void
connect_cleanup_ready (MMSitClient *client, GAsyncResult *res, MMSitCp *cp)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_disconnect_data_finish (client, res, &error))
        mm_obj_warn (client, "DSDS data cleanup failed; retaining ownership until radio-off: %s", error->message);
    g_object_unref (cp);
}

static void
connect_context_free (ConnectContext *ctx)
{
    /* Every failure path lands here -- cancelled, refused, timed out, retries
     * exhausted -- so releasing the reservation here is the one place that
     * cannot be forgotten. A context that did come up keeps its cid until the
     * bearer disconnects it.
     *
     * A SETUP that was sent but never produced an address produced no context
     * either, so there is nothing for the CP to deactivate: it answers
     * DEACTIVATE_DATA_CALL with status 2, the cleanup is treated as failed and
     * the cid is retained "until radio-off". Meanwhile the per-stack table
     * still maps that cid to the APN, so cid_acquire() hands the same one back
     * on the next attempt and mm_sit_cp_claim_data() refuses it -- every retry
     * for the rest of the process fails with "context N is already connected
     * or awaiting setup/cleanup", which is how one refused data call wedged
     * Fizz for a whole boot. Only tear down what actually came up. */
    if (!ctx->cid_kept) {
        if (ctx->cp && ctx->unexpected_cid) {
            /* Do not deactivate an unexpected context owned by another stack. */
            mm_obj_warn (ctx->client, "Unexpected DSDS CID; retaining data ownership until radio-off");
        } else if (ctx->cp && ctx->setup_sent &&
                   (ctx->call.has_ipv4 || ctx->call.has_ipv6))
            mm_sit_client_disconnect_data (ctx->client, ctx->cid, NULL,
                                           (GAsyncReadyCallback) connect_cleanup_ready,
                                           g_object_ref (ctx->cp));
        else
            cid_release (ctx->client, ctx->cid);
    }
    g_clear_object (&ctx->cp);
    if (ctx->retry_id)
        g_source_remove (ctx->retry_id);
    if (ctx->indication_handler_id)
        g_signal_handler_disconnect (ctx->client, ctx->indication_handler_id);
    g_clear_pointer (&ctx->indicated_calls, g_array_unref);
    g_clear_object (&ctx->client);
    g_free (ctx->apn);
    g_free (ctx->auth_user);
    g_free (ctx->auth_password);
    g_free (ctx);
}

/* Pick the connected call we set up. Prefer the entry whose cid matches the one
 * the SETUP response assigned; once known, a non-matching list must not be used
 * (a DATA_CALL_LIST_CHANGED may also carry an IMS/tethering PDN). Before the cid
 * is known (0), fall back to the first call that already has an address. */
static const MMSitDataCall *
connect_pick_call (GArray *calls, guint8 cid)
{
    const MMSitDataCall *fallback = NULL;
    guint                i;

    for (i = 0; i < calls->len; i++) {
        const MMSitDataCall *c = &g_array_index (calls, MMSitDataCall, i);

        if (!c->has_ipv4 && !c->has_ipv6)
            continue;
        if (cid && c->cid == cid)
            return c;
        if (!fallback)
            fallback = c;
    }
    return cid ? NULL : fallback;
}

static void
connect_indication (MMSitClient        *self,
                    const MMSitMessage *message,
                    GTask              *task)
{
    ConnectContext      *ctx = g_task_get_task_data (task);
    g_autoptr(GArray)    calls = NULL;
    const MMSitDataCall *chosen;

    if (message->id != MM_SIT_MESSAGE_DATA_CALL_LIST_CHANGED ||
        !mm_sit_message_parse_data_call_list (message, &calls, NULL))
        return;

    /* Keep the latest list so connect_setup_ready can match our cid once the
     * SETUP response assigns one. */
    g_clear_pointer (&ctx->indicated_calls, g_array_unref);
    ctx->indicated_calls = g_array_ref (calls);

    /* Advance a retrying SETUP only once our own call has an address. */
    if (ctx->step == CONNECT_STEP_SETUP && ctx->retry_id &&
        (chosen = connect_pick_call (calls, ctx->call.cid))) {
        g_source_remove (ctx->retry_id);
        ctx->retry_id = 0;
        ctx->call = *chosen;
        ctx->step++;
        connect_step (task);
    }
}

static gboolean
connect_retry (GTask *task)
{
    ConnectContext *ctx = g_task_get_task_data (task);

    ctx->retry_id = 0;
    connect_step (task);
    return G_SOURCE_REMOVE;
}

static void
connect_checked_ready (MMSitClient *self,
                       GAsyncResult *res,
                       GTask *task)
{
    GError *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ((ConnectContext *) g_task_get_task_data (task))->step++;
    connect_step (task);
}

static void
connect_registration_ready (MMSitClient *self,
                            GAsyncResult *res,
                            GTask *task)
{
    ConnectContext    *ctx = g_task_get_task_data (task);
    MMSitRegistration  registration;
    GError             *error = NULL;

    if (!mm_sit_client_get_registration_finish (self, res, &registration, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (registration.registered) {
        ctx->step++;
        connect_step (task);
        return;
    }

    ctx->registration_attempts++;
    if (ctx->registration_attempts >= 20) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                 "SIT modem did not register for packet data after 20 attempts");
        g_object_unref (task);
        return;
    }
    ctx->retry_id = g_timeout_add_seconds (2, (GSourceFunc) connect_retry, task);
}

static void
connect_setup_ready (MMSitClient *self,
                     GAsyncResult *res,
                     GTask *task)
{
    ConnectContext          *ctx = g_task_get_task_data (task);
    g_autoptr(MMSitMessage)  response = NULL;
    GError                  *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !mm_sit_message_parse_data_call (response, &ctx->call, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* The SETUP response assigns the cid but may not carry the address yet; take
     * it from the matching entry of the latest DATA_CALL_LIST_CHANGED list. */
    if (!ctx->call.has_ipv4 && !ctx->call.has_ipv6 && ctx->indicated_calls) {
        const MMSitDataCall *m = connect_pick_call (ctx->indicated_calls, ctx->call.cid);

        if (m)
            ctx->call = *m;
    }

    if (ctx->cp && ctx->call.cid != ctx->cid) {
        ctx->unexpected_cid = TRUE;
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "CP assigned unexpected DSDS context %u (requested %u)",
                                 ctx->call.cid, ctx->cid);
        g_object_unref (task);
        return;
    }
    if (!ctx->call.cause && (ctx->call.has_ipv4 || ctx->call.has_ipv6)) {
        ctx->step++;
        connect_step (task);
        return;
    }

    ctx->setup_attempts++;
    if (ctx->setup_attempts >= 6) {
        /* Tell the modem before failing: the attach APN it chose is what
         * decides whether this PDN could have been created. */
        if (ctx->call.cause)
            g_signal_emit (self, signals[SIGNAL_DATA_PDN_REFUSED], 0,
                           (guint) ctx->call.cause);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "SIT data call returned no address after 6 attempts (cause %u)",
                                 ctx->call.cause);
        g_object_unref (task);
        return;
    }
    ctx->retry_id = g_timeout_add_seconds (3, (GSourceFunc) connect_retry, task);
}

static GByteArray *
connect_build_request (MMSitClient   *self,
                       ConnectContext *ctx,
                       GError        **error)
{
    guint8 reserved[4] = { 0 };

    switch (ctx->step) {
    case CONNECT_STEP_PREFERRED_MODEM:
        /* See mm_sit_cp_note_data_stack(): this is the other sender. */
        mm_sit_cp_note_data_stack (self->priv->cp, self->priv->stack);
        return mm_sit_request_new_set_preferred_modem (mm_sit_client_next_token (control_client (self)), self->priv->stack);
    case CONNECT_STEP_DATA_PROFILE:
        return mm_sit_request_new_set_data_profile (mm_sit_client_next_token (self), ctx->apn, ctx->pdp, ctx->apn_types, ctx->bearers, &ctx->auth, error);
    case CONNECT_STEP_ATTACH_APN:
        return mm_sit_request_new_set_initial_attach_apn (mm_sit_client_next_token (self), ctx->apn, ctx->pdp, &ctx->auth, error);
    case CONNECT_STEP_CONFIG_0900:
        return mm_sit_request_new (0x0900, mm_sit_client_next_token (self), reserved, sizeof (reserved), error);
    case CONNECT_STEP_SETUP:
        return mm_sit_request_new_setup_data_call (mm_sit_client_next_token (self), ctx->cid,
                                                   ctx->apn, ctx->pdp, ctx->apn_types, &ctx->auth, error);
    case CONNECT_STEP_FAST_DORMANCY:
        return mm_sit_request_new_set_fast_dormancy_info (mm_sit_client_next_token (self));
    case CONNECT_STEP_REGISTRATION:
    case CONNECT_STEP_LAST:
    default:
        return NULL;
    }
}

static void
connect_step (GTask *task)
{
    MMSitClient            *self = g_task_get_source_object (task);
    ConnectContext         *ctx = g_task_get_task_data (task);
    GCancellable           *cancellable = g_task_get_cancellable (task);
    g_autoptr(GByteArray)   request = NULL;
    GError                 *error = NULL;

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    if (ctx->step == CONNECT_STEP_LAST) {
        MMSitDataCall *result = g_new (MMSitDataCall, 1);

        cid_confirm (self, ctx->cid, ctx->call.cid);
        ctx->cid_kept = TRUE;
        if (ctx->cp)
            mm_sit_cp_commit_data (ctx->cp, ctx->cid);
        *result = ctx->call;
        g_task_return_pointer (task, result, g_free);
        g_object_unref (task);
        return;
    }

    /* The IMS context is the attach context; re-pointing it would tear down
     * the very PDN this connect is for. */
    if (ctx->cp && ctx->step == CONNECT_STEP_ATTACH_APN)
        ctx->step++;

    if (ctx->step == CONNECT_STEP_REGISTRATION) {
        mm_sit_client_get_registration (self,
                                        cancellable,
                                        (GAsyncReadyCallback) connect_registration_ready,
                                        task);
        return;
    }

    request = connect_build_request (self, ctx, &error);
    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (ctx->step == CONNECT_STEP_SETUP) {
        ctx->setup_sent = TRUE;
        mm_sit_client_command (self, request, 10, cancellable,
                               (GAsyncReadyCallback) connect_setup_ready, task);
    } else
        run_checked_command (ctx->step == CONNECT_STEP_PREFERRED_MODEM ? control_client (self) : self,
                             request, 5, cancellable,
                             (GAsyncReadyCallback) connect_checked_ready, task);
}

void
mm_sit_client_connect_data (MMSitClient        *self,
                            const gchar        *apn,
                            MMSitPdpType        pdp,
                            guint32             apn_types,
                            const MMSitApnAuth *auth,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
    ConnectContext *ctx;
    GTask          *task;
    GError         *error = NULL;
    guint8          cid;

    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    task = g_task_new (self, cancellable, callback, user_data);
    if (!apn || !apn[0] || strlen (apn) > MM_SIT_MAX_APN_LENGTH) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                 "A valid APN of at most %u bytes is required",
                                 MM_SIT_MAX_APN_LENGTH);
        g_object_unref (task);
        return;
    }

    cid = cid_acquire (self, apn, &error);
    if (!cid) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (self->priv->cp && !mm_sit_cp_claim_data (self->priv->cp, self->priv->stack, cid, &error)) {
        if (!mm_sit_cp_data_reserved (self->priv->cp, cid))
            cid_release (self, cid);
        else
            cid_forget (self, cid);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx = g_new0 (ConnectContext, 1);
    ctx->cp = self->priv->cp ? g_object_ref (self->priv->cp) : NULL;
    ctx->cid = cid;
    ctx->apn = g_strdup (apn);
    ctx->pdp = pdp;
    ctx->apn_types = apn_types;
    if (auth) {
        ctx->auth_user = g_strdup (auth->user);
        ctx->auth_password = g_strdup (auth->password);
        ctx->auth.auth = auth->auth;
        ctx->auth.user = ctx->auth_user;
        ctx->auth.password = ctx->auth_password;
    }
    ctx->client = g_object_ref (self);
    g_task_set_task_data (task, ctx, (GDestroyNotify) connect_context_free);
    ctx->indication_handler_id = g_signal_connect (self, "indication",
                                                    G_CALLBACK (connect_indication), task);
    connect_step (task);
}

gboolean
mm_sit_client_connect_data_finish (MMSitClient    *self,
                                   GAsyncResult   *res,
                                   MMSitDataCall  *out_call,
                                   GError        **error)
{
    MMSitDataCall *call;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_call != NULL, FALSE);

    call = g_task_propagate_pointer (G_TASK (res), error);
    if (!call)
        return FALSE;
    *out_call = *call;
    g_free (call);
    return TRUE;
}

/* Second attempt at a teardown the CP refused, with the reason its own RIL
 * reserves for a context that is up but broken. */
static void
disconnect_reset_ready (MMSitClient  *self,
                        GAsyncResult *res,
                        GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error))
        g_task_return_error (task, error);
    else {
        cid_release (self, (guint8) GPOINTER_TO_UINT (g_task_get_task_data (task)));
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
disconnect_normal_ready (MMSitClient  *self,
                         GAsyncResult *res,
                         GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GByteArray)   retry = NULL;
    g_autoptr(GError)       error = NULL;
    guint8                  cid;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && message_check_success (response, &error)) {
        /* Only once the CP agrees the context is gone: handing the cid out
         * again while it is still up would collide, and a context the CP
         * refuses to drop is exactly the case the retry below exists for. */
        cid_release (self, (guint8) GPOINTER_TO_UINT (g_task_get_task_data (task)));
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /*
     * A NORMAL deactivate is refused (status 2, GENERIC_FAILURE) for a context
     * the CP will not drop on request -- seen after a long suspend, where the
     * network had degraded a dual-stack context to IPv4-only.  Leaving it up is
     * worse than it sounds: SETUP_DATA_CALL then keeps handing that same
     * context back and ignores the PDP type asked for, so the bearer can never
     * renegotiate its address families.  The RIL's own recovery for this is
     * DEACT_REASON_PDP_RESET, so escalate once before giving up.
     */
    cid = GPOINTER_TO_UINT (g_task_get_task_data (task));
    mm_obj_dbg (self->priv->port,
                "data call %u refused a normal deactivate (%s); retrying as PDP reset",
                cid, error->message);

    retry = mm_sit_request_new_deactivate_data_call (mm_sit_client_next_token (self), cid,
                                                     MM_SIT_DEACT_REASON_PDP_RESET);
    mm_sit_client_command (self, retry, 10, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) disconnect_reset_ready, task);
}

typedef struct {
    guint8        cid;
    gboolean      cid_kept;
    gchar        *apn;
    MMSitPdpType  pdp;
    guint32       apn_types;
    guint32       bearers;
    MMSitDataCall call;
} SetupContext;

static void
setup_context_free (SetupContext *ctx)
{
    g_free (ctx->apn);
    g_free (ctx);
}

static void setup_data_call_ready (MMSitClient  *self,
                                   GAsyncResult *res,
                                   GTask        *task);

/*
 * SETUP_DATA_CALL names a DNN; SET_DATA_PROFILE is what tells the CP that DNN
 * exists. Without the profile the CP has nothing to match the request against
 * and refuses it with cause 2253, "no URSP rule matched and the catch-all is
 * not allowed" -- the same answer it gives for an attach that named no APN,
 * which is why that cause read as an attach problem for so long.
 *
 * mm_sit_client_connect_data has always sent the profile first
 * (CONNECT_STEP_DATA_PROFILE); this entry point did not, so its one caller --
 * the IMS stack, for the ims PDN -- could never bring its context up on a
 * network whose policy does not permit a catch-all. Downstream sends the whole
 * profile list at boot, which is why Android has the ims profile in place long
 * before anything asks for that PDN.
 *
 * A refusal is not fatal: the CP answers status 2 here on hardware and grants
 * the context anyway, so the rejection is worth a line and nothing more.
 *
 * HW-confirmed on an s5400: `mm-sit-tool connect ims` (which goes through
 * connect_data) brought the same PDN up on the same CP seconds after the IMS
 * path had been refused, and the CP announced "IMS Registered" as it did.
 */

static void setup_issue_data_call (MMSitClient *self, GTask *task);

/* The PRE_PDN block is advisory: a refused row cannot fail the context. */
static void
setup_preamble_ready (MMSitClient  *self,
                      GAsyncResult *res,
                      GTask        *task)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_run_pdn_preamble_finish (self, res, &error))
        mm_obj_dbg (self->priv->port, "pdn preamble: %s; continuing",
                    error ? error->message : "failed");
    setup_issue_data_call (self, task);
}

static void
setup_profile_ready (MMSitClient  *self,
                     GAsyncResult *res,
                     GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GByteArray)   request = NULL;
    SetupContext           *ctx = g_task_get_task_data (task);
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error)) {
        mm_obj_dbg (self->priv->port,
                    "data profile for APN '%s' rejected (%s); attempting data call setup",
                    ctx->apn, error ? error->message : "no response");
        g_clear_error (&error);
    }


    /*
     * Stock's last writes before it asks for a context are queries, not
     * writes. Across five captures its SET_INITIAL_ATTACH_APN lands
     * 974-8171ms before the ask and its SET_DATA_PROFILE 518-7646ms, and
     * neither is in its final seven frames -- those are GET_OPERATOR,
     * QUERY_NETWORK_SELECTION_MODE, GET_DATA_REGISTRATION and
     * GET_VOICE_REGISTRATION, then the request.
     *
     * We wrote the attach APN 10-14ms and the profile 4-8ms before asking, so
     * the CP was still taking a profile change as it was asked to establish a
     * context against it. Running the PRE_PDN block here instead of before
     * this sequence puts those writes where stock has them and leaves the
     * queries last, which is the whole point of that block.
     */
    if (ctx->apn_types & MM_SIT_APN_TYPE_IMS) {
        mm_sit_client_run_pdn_preamble (self, g_task_get_cancellable (task),
                                        (GAsyncReadyCallback) setup_preamble_ready,
                                        task);
        return;
    }
    setup_issue_data_call (self, task);
}

static void
setup_issue_data_call (MMSitClient *self, GTask *task)
{
    SetupContext         *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;
    GError               *error = NULL;

    request = mm_sit_request_new_setup_data_call (mm_sit_client_next_token (self), ctx->cid,
                                                  ctx->apn, ctx->pdp, ctx->apn_types, NULL, &error);
    if (!request) {
        cid_release (self, ctx->cid);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* The CP takes its time on a fresh context -- the captured ims activation
     * ran to about 600 ms and a retry ladder belongs to the caller, not here. */
    mm_sit_client_command (self, request, 30, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) setup_data_call_ready, task);}

static void
setup_data_call_ready (MMSitClient  *self,
                       GAsyncResult *res,
                       GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    SetupContext           *ctx = g_task_get_task_data (task);
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error)) {
        cid_release (self, ctx->cid);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!mm_sit_message_parse_data_call (response, &ctx->call, &error)) {
        cid_release (self, ctx->cid);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (ctx->call.cause) {
        cid_release (self, ctx->cid);
        g_signal_emit (self, signals[SIGNAL_DATA_PDN_REFUSED], 0,
                       (guint) ctx->call.cause);
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "data call refused: cause %u", ctx->call.cause);
        g_object_unref (task);
        return;
    }

    cid_confirm (self, ctx->cid, ctx->call.cid);
    ctx->cid_kept = TRUE;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* Register the profile, then ask for the context; see setup_profile_ready. */
static void
setup_send_profile (MMSitClient *self,
                    GTask       *task)
{
    g_autoptr(GByteArray)  request = NULL;
    SetupContext          *ctx = g_task_get_task_data (task);
    MMSitApnAuth           auth = { 0 };
    GError                *error = NULL;

    request = mm_sit_request_new_set_data_profile (mm_sit_client_next_token (self), ctx->apn,
                                                   ctx->pdp, ctx->apn_types, ctx->bearers,
                                                   &auth, &error);
    if (!request) {
        cid_release (self, ctx->cid);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_sit_client_command (self, request, 10, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) setup_profile_ready, task);
}

static void
setup_reattach_ready (MMSitClient  *self,
                      GAsyncResult *res,
                      GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    /* Advisory. A CP that will not take the attach again is no reason not to
     * ask for the context -- that is exactly the state we were in before. */
    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error)) {
        mm_obj_dbg (self->priv->port, "the CP refused the re-asserted attach APN (%s); "
                    "asking for the context anyway", error ? error->message : "no response");
        g_clear_error (&error);
    }

    setup_send_profile (self, task);
}

/*
 * Build one boot-parity row into a frame.
 *
 * Lives here, not in the modem, because two callers need it now: the boot
 * replay and the PRE_PDN preamble that runs from the IMS sequence, which only
 * ever has a client. `mccmnc` is the one substitution the client cannot make
 * on its own -- the modem passes it, NULL otherwise.
 */
GByteArray *
mm_sit_client_build_boot_request (MMSitClient              *self,
                                    const MMSitBootRequest *req,
                                    const gchar              *mccmnc,
                                    GError                  **error)
{
    g_autofree guint8 *body = NULL;
    g_autofree guint8 *built = NULL;
    gsize              payload_len = req->len;
    gsize              len;

    /* A request whose payload is derived rather than stated. */
    if (req->sub == MM_SIT_BOOT_SUB_BUILD) {
        built = mm_sit_boot_build_payload (req, &payload_len);
        if (!built)
            return NULL;
    }

    len = 4 + payload_len;
    body = g_malloc0 (len);
    if (built)
        memcpy (body + 4, built, payload_len);
    else if (req->payload)
        memcpy (body + 4, req->payload, req->len);

    switch (req->sub) {
    case MM_SIT_BOOT_SUB_MCCMNC:
        /* Nothing to name the subscription with yet; the CP gets it next time. */
        if (!mccmnc || !mccmnc[0] || strlen (mccmnc) >= req->len)
            return NULL;
        memset (body + 4, 0, req->len);
        memcpy (body + 4, mccmnc, strlen (mccmnc));
        break;
    case MM_SIT_BOOT_SUB_STACK:
        if (req->len < 1)
            return NULL;
        body[4] = (guint8) self->priv->stack;
        break;
    case MM_SIT_BOOT_SUB_WALLCLOCK: {
        /*
         * Six bytes of UTC at payload+9: yy mm dd HH MM SS, yy = year - 2000,
         * each a plain byte rather than BCD.
         *
         * Stock's advances every frame (22:23:30, :31, :34, :35 across one
         * boot). The table is a verbatim replay, so before this it sent the
         * capture's own clock -- 2026-10-11 04:59:48, identical on every boot
         * and both stacks, which is what the CP was being told the time was.
         */
        g_autoptr(GDateTime) now = g_date_time_new_now_utc ();

        if (req->len < 15 || !now)
            return NULL;
        body[4 + 9]  = (guint8) (g_date_time_get_year (now) - 2000);
        body[4 + 10] = (guint8) g_date_time_get_month (now);
        body[4 + 11] = (guint8) g_date_time_get_day_of_month (now);
        body[4 + 12] = (guint8) g_date_time_get_hour (now);
        body[4 + 13] = (guint8) g_date_time_get_minute (now);
        body[4 + 14] = (guint8) g_date_time_get_second (now);
        break;
    }
    case MM_SIT_BOOT_SUB_BUILD:
    case MM_SIT_BOOT_SUB_NONE:
    default:
        break;
    }

    /*
     * The AIMS stack requests name their stack in payload byte 1, 1-based, and
     * the table does not cover it.
     *
     * mm-sit-boot-config.h is a verbatim replay of one cold boot captured on
     * **one stack**, and its own header says so. STACK_START (0x0d39),
     * STACK_STOP (0x0d3a), 0x0d3b and 0x0d3c all carry the stack, so replaying
     * them unmodified sent stack 0's index on both stacks: on ch 0xf6 we were
     * starting, stopping and configuring the IMS stack of the *other*
     * subscription.
     *
     * Confirmed against komodo's own Android, 2026-11-03: across every AP->CP
     * request in the capture, exactly these four ids have payload byte 1 equal
     * to 1 on ch 0xf5 and 2 on ch 0xf6, and no others do. In particular
     * **0x0d0b is 2 on both channels** -- it is not a stack index and must not
     * be touched.
     *
     * Done here rather than in the table so it survives regeneration.
     */
    switch (req->id) {
    case 0x0d39:
    case 0x0d3a:
    case 0x0d3b:
    case 0x0d3c:
        if (req->len < 2)
            return NULL;
        body[4 + 1] = (guint8) (self->priv->stack + 1);
        break;
    default:
        break;
    }

    return mm_sit_request_new (req->id, mm_sit_client_next_token (self),
                               body, len, error);
}

/*
 * Replay the PRE_PDN rows: the block stock runs between its last
 * SET_DATA_PROFILE and its IMS SETUP_DATA_CALL.
 *
 * Every row is advisory. The point is the traffic the CP sees immediately
 * before the request, so a refused row is still a row the CP saw, and nothing
 * here may fail the context that follows.
 */
/*
 * Stock runs this whole block in 518ms on ch 0xf5 and 618ms on 0xf6, so give it
 * a budget a few times that and no more. Nothing here may delay the context
 * that follows: every row is advisory, and a row whose reply the CP sits on
 * must cost the request that matters nothing. Per row, stock is never silent
 * for more than 124ms with a request outstanding during boot.
 */
#define PREAMBLE_BUDGET_MS   2000
#define PREAMBLE_ROW_TIMEOUT 1

typedef struct {
    guint  index;
    gint64 deadline_us;
} PreambleContext;

static void preamble_step (GTask *task);

static void
preamble_ready (MMSitClient  *self,
                GAsyncResult *res,
                GTask        *task)
{
    g_autoptr(MMSitMessage) message = NULL;
    g_autoptr(GError)       error = NULL;

    message = mm_sit_client_command_finish (self, res, &error);
    if (!message)
        mm_obj_dbg (self->priv->port, "pdn preamble: a row was refused (%s); continuing",
                    error ? error->message : "no response");
    preamble_step (task);
}

static void
preamble_step (GTask *task)
{
    MMSitClient              *self = g_task_get_source_object (task);
    PreambleContext          *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray)     request = NULL;
    g_autoptr(GError)         error = NULL;
    const MMSitBootRequest *table;
    gsize                     n_table = 0;

    table = mm_sit_boot_requests (&n_table);
    if (g_get_monotonic_time () >= ctx->deadline_us) {
        mm_obj_dbg (self->priv->port, "pdn preamble: out of budget after %u ms with "
                    "%u row(s) to go; going on to the context",
                    (guint) PREAMBLE_BUDGET_MS, (guint) (n_table - ctx->index));
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    while (ctx->index < n_table) {
        const MMSitBootRequest *req = &table[ctx->index];

        if (req->phase != MM_SIT_BOOT_PHASE_PRE_PDN ||
            !mm_sit_boot_request_wanted (req, self->priv->stack)) {
            ctx->index++;
            continue;
        }
        request = mm_sit_client_build_boot_request (self, req, NULL, &error);
        ctx->index++;
        if (request)
            break;
        if (error) {
            mm_obj_dbg (self->priv->port, "pdn preamble: couldn't build 0x%04x: %s",
                        req->id, error->message);
            g_clear_error (&error);
        }
    }

    if (!request) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    mm_sit_client_command (self, request, PREAMBLE_ROW_TIMEOUT,
                           g_task_get_cancellable (task),
                           (GAsyncReadyCallback) preamble_ready, task);
}

void
mm_sit_client_run_pdn_preamble (MMSitClient        *self,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    GTask *task;

    PreambleContext *ctx;

    task = g_task_new (self, cancellable, callback, user_data);
    ctx = g_new0 (PreambleContext, 1);
    ctx->deadline_us = g_get_monotonic_time () + (PREAMBLE_BUDGET_MS * 1000);
    g_task_set_task_data (task, ctx, g_free);
    preamble_step (task);
}

gboolean
mm_sit_client_run_pdn_preamble_finish (MMSitClient  *self,
                                       GAsyncResult *res,
                                       GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_setup_data_call (MMSitClient        *self,
                               const gchar        *apn,
                               MMSitPdpType        pdp,
                               guint32             apn_types,
                               guint32             bearers,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    SetupContext          *ctx;
    GTask                 *task;
    GError                *error = NULL;
    guint8                 cid;

    task = g_task_new (self, cancellable, callback, user_data);

    cid = cid_acquire (self, apn, &error);
    if (!cid) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /*
     * Name the stack that owns the data subscription -- but only for a context
     * that carries data. Which SIM mobile data belongs to is a subscription-wide
     * choice; it is not a property of whatever context happens to be going up,
     * and an IMS context says nothing about it.
     *
     * Asserting it from every setup made the two stacks fight over it. Each
     * one claimed data for itself on the way to its own IMS context, so a
     * two-SIM boot sent 0x0740 four times -- 01, 00, 01, 01 -- where stock
     * sends it twice, 00 then 01, and never again around either of its IMS
     * contexts. The CP does real work for a switch, and the redundant toggle
     * away and back cost 2690ms on the critical path to stack 0's IMS request
     * while stack 1's reply to the same message took 8ms.
     *
     * This was first added because an IMS context came back cause=0 and empty,
     * with no address and no P-CSCF, and the unsent preference was the theory.
     * That turned out to be truncated carrier config and is fixed at its own
     * root, so the theory this mirrored is gone; what remains is the real rule,
     * which is stock's. A bearer connect still names it as its own first step.
     */
    if (self->priv->cp && (apn_types & MM_SIT_APN_TYPE_DEFAULT))
        mm_sit_cp_prefer_data_stack (self->priv->cp, self->priv->stack);

    ctx = g_new0 (SetupContext, 1);
    ctx->cid = cid;
    ctx->apn = g_strdup (apn);
    ctx->pdp = pdp;
    ctx->apn_types = apn_types;
    ctx->bearers = bearers;
    g_task_set_task_data (task, ctx, (GDestroyNotify) setup_context_free);

    /*
     * Re-assert the attach APN before the first context of this attach epoch;
     * see the `attach_apn` comment on MMSitClientPrivate. Stock does this on
     * both stacks and does not scope it to the attach APN's own context -- its
     * ch 0xf5 re-assert is followed 1306 ms later by a context for `ims`.
     *
     * It was scoped that way for one revision, on the theory that re-asserting
     * ahead of a foreign APN had cost a grant. That was a false attribution:
     * measured across three boots an attach went out just before ch 0xf6's
     * first attempt every time -- once granted, twice refused -- so it does not
     * discriminate. Narrowing it only lost the parity, because ch 0xf5 never
     * asks for a context on its own attach APN at all.
     */
    if (self->priv->attach_apn && !self->priv->attach_reasserted) {
        g_autoptr(GByteArray) reattach = NULL;
        MMSitApnAuth          attach_auth = { 0 };

        attach_auth.auth     = self->priv->attach_auth_type;
        attach_auth.user     = self->priv->attach_auth_user;
        attach_auth.password = self->priv->attach_auth_password;

        self->priv->attach_reasserted = TRUE;
        reattach = mm_sit_request_new_set_initial_attach_apn_for_stack (
            mm_sit_client_next_token (self), self->priv->stack, self->priv->attach_apn,
            self->priv->attach_pdp, self->priv->attach_pcscf, &attach_auth, &error);
        if (reattach) {
            mm_obj_dbg (self->priv->port,
                        "re-asserting the attach APN '%s' before the context for '%s'",
                        self->priv->attach_apn, apn);
            mm_sit_client_command (self, reattach, 10, cancellable,
                                   (GAsyncReadyCallback) setup_reattach_ready, task);
            return;
        }
        /* Not worth failing the context over; the CP already has an attach. */
        mm_obj_dbg (self->priv->port, "could not rebuild the attach APN request (%s); "
                    "asking for the context anyway", error ? error->message : "unknown");
        g_clear_error (&error);
    }

    setup_send_profile (self, task);
}

gboolean
mm_sit_client_setup_data_call_finish (MMSitClient   *self,
                                      GAsyncResult  *res,
                                      MMSitDataCall *out_call,
                                      GError       **error)
{
    SetupContext *ctx;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_call != NULL, FALSE);

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;
    ctx = g_task_get_task_data (G_TASK (res));
    *out_call = ctx->call;
    return TRUE;
}

void
mm_sit_client_disconnect_data (MMSitClient        *self,
                               guint8              cid,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER ((guint) cid), NULL);
    request = mm_sit_request_new_deactivate_data_call (mm_sit_client_next_token (self), cid,
                                                       MM_SIT_DEACT_REASON_NORMAL);
    mm_sit_client_command (self, request, 10, cancellable,
                           (GAsyncReadyCallback) disconnect_normal_ready, task);
}

gboolean
mm_sit_client_disconnect_data_finish (MMSitClient  *self,
                                      GAsyncResult *res,
                                      GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * PLMN selection. Automatic selection is what recovers a modem that has fallen
 * out of service: the CP only starts searching on its own when radio power is
 * first enabled, so without this nothing re-triggers a search and the modem
 * stays unregistered indefinitely. The ack is empty; the outcome shows up as a
 * SERVING_NETWORK indication once the CP finds a network.
 *
 * **Only send this when the CP is not already selecting automatically.** The ack
 * comes back when the CP's search finishes, not when it accepts the request, so
 * asking a CP that is already mid-search costs the whole search: 181.7 s
 * measured on a boot where the request went out 40 s after radio-on, against
 * 5 ms, 11 ms and 3.4 s for the three downstream sends. MMPortSerial runs one
 * command at a time, so the port carries nothing else for the duration -- the
 * trace shows a dead link and then every queued command flushing out the
 * instant the deadline passes.
 *
 * The caller checks 0x0703 first, so by the time this is sent the CP is in
 * manual mode and the search is one it has not started yet.
 */
void
mm_sit_client_set_network_selection_auto (MMSitClient        *self,
                                          GCancellable       *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;

    request = mm_sit_request_new_set_network_selection_auto (mm_sit_client_next_token (self));
    run_checked_command (self, request, 30, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_network_selection_auto_finish (MMSitClient  *self,
                                                 GAsyncResult *res,
                                                 GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_network_selection_manual_ready (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    if (!response || !message_check_success (response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* A refusal is the CP's status byte -- captured ones carry 2
     * (GENERIC_FAILURE) and 7 (CANCELLED) -- so message_check_success above has
     * already caught it and there is nothing further to inspect. */
    if (!mm_sit_message_parse_network_selection_manual_ack (response, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*
 * Manual selection. Downstream stops any running scan first, then asks for the
 * PLMN on a specific RAT; a refused network is answered after a long delay (over
 * a minute in the capture), so the timeout here is generous.
 */
void
mm_sit_client_set_network_selection_manual (MMSitClient        *self,
                                            const gchar        *plmn,
                                            guint8              rat,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_set_network_selection_manual (mm_sit_client_next_token (self), plmn,
                                                              rat, &error);
    if (!request) {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    mm_sit_client_command (self, request, 180, cancellable,
                           (GAsyncReadyCallback) set_network_selection_manual_ready, task);
}

gboolean
mm_sit_client_set_network_selection_manual_finish (MMSitClient  *self,
                                                   GAsyncResult *res,
                                                   GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
get_facility_lock_ready (MMSitClient  *self,
                         GAsyncResult *res,
                         GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    GError                 *error = NULL;
    gboolean                locked = FALSE;

    response = mm_sit_client_command_finish (self, res, &error);
    if (response && mm_sit_message_parse_facility_lock (response, &locked, &error))
        g_task_return_boolean (task, locked);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

/*
 * Whether one facility is locked. The reply carries no retry counter -- like
 * classic RIL, the CP only reports remaining attempts in the response to an
 * actual PIN operation -- so this reads the lock state without spending one.
 */
void
mm_sit_client_get_facility_lock (MMSitClient        *self,
                                 guint8              facility,
                                 GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_facility_lock (mm_sit_client_next_token (self), facility,
                                                    self->priv->sim_aid_len ? self->priv->sim_aid : NULL,
                                                    self->priv->sim_aid_len);
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_facility_lock_ready, task);
}

/*
 * The UICC access-control probe, reproduced from stock.
 *
 * Measured on identical firmware, ch 0xf5, 5.74-5.86 s -- a block we sent none
 * of at all, and the largest single content gap left against the capture
 * (seven 0x0247, three 0x020f and one 0x020e on stack 0; nine, two and two on
 * stack 1):
 *
 *   0x0247 open by AID A0000001514143 4C ("..QACL", GSMA ARA-M) -> ffffffff 6a82
 *   0x0247 open by AID A000000063 "PKCS-15"      (ARF)          -> ffffffff 6a82
 *   0x0247 open by AID A000000018 "GPAC-15"                     -> ffffffff 6a82
 *   0x0247 open with NO AID                                     -> channel 1, 9000
 *   0x020f SELECT 3F00 (MF) on that channel
 *   0x020f SELECT 2F00 (EF_DIR)
 *   0x020f READ RECORD 1                        -> 4f 10 a0 00 00 00 87 10 02 ...
 *   0x020e close the channel
 *
 * It is carrier-privilege discovery: look for the access-rule applets, and
 * when they are absent read EF_DIR to enumerate the card's applications. All
 * reads, nothing written to the card.
 *
 * This cannot be a boot-parity table row, which is why it was missing for so
 * long: the channel number is handed back in the open response and every later
 * frame has to carry it, so a verbatim replay would transmit on a channel the
 * CP never gave us and leak the one it did. Hence the explicit sequence, which
 * always closes what it opened -- including on error.
 *
 * Request layouts, from the capture:
 *   0x0247  [u8 aid_len][aid, 16 bytes zero-padded][u8 pad]            = 18
 *   0x020f  [u32 ch][u32 cla][u32 ins][u32 p1][u32 p2][u32 p3]
 *           [u16 data_len][data...]
 *   0x020e  [u32 ch]                                                   = 4
 */
#define UICC_NO_CHANNEL 0xffffffffu

static const guint8 uicc_aid_ara_m[]  = { 0xa0, 0x00, 0x00, 0x01, 0x51, 0x41, 0x43, 0x4c };
static const guint8 uicc_aid_pkcs15[] = { 0xa0, 0x00, 0x00, 0x00, 0x63, 0x50, 0x4b, 0x43,
                                          0x53, 0x2d, 0x31, 0x35 };
static const guint8 uicc_aid_gpac[]   = { 0xa0, 0x00, 0x00, 0x00, 0x18, 0x47, 0x50, 0x41,
                                          0x43, 0x2d, 0x31, 0x35 };

typedef enum {
    UICC_STEP_OPEN_ARA_M,
    UICC_STEP_OPEN_PKCS15,
    UICC_STEP_OPEN_GPAC,
    UICC_STEP_OPEN_PLAIN,
    UICC_STEP_SELECT_MF,
    UICC_STEP_SELECT_EF_DIR,
    UICC_STEP_READ_RECORD,
    UICC_STEP_CLOSE,
    UICC_STEP_DONE,
} UiccStep;

typedef struct {
    UiccStep step;
    guint32  channel;
} UiccProbeContext;

static void uicc_probe_step (GTask *task);

static GByteArray *
uicc_build_open (MMSitClient *self, const guint8 *aid, gsize aid_len, GError **error)
{
    guint8 body[18] = { 0 };

    if (aid && aid_len) {
        body[0] = (guint8) aid_len;
        memcpy (&body[1], aid, MIN (aid_len, 16));
    }
    return mm_sit_request_new (MM_SIT_MESSAGE_OPEN_LOGICAL_CHANNEL,
                               mm_sit_client_next_token (self), body, sizeof (body), error);
}

static GByteArray *
uicc_build_apdu (MMSitClient *self, guint32 channel, guint8 ins, guint8 p1, guint8 p2,
                 guint8 p3, const guint8 *data, gsize data_len, GError **error)
{
    guint8 body[32] = { 0 };
    gsize  len = 26;

    memcpy (&body[0],  &channel, 4);
    body[4]  = 1;      /* the class byte stock uses on this channel */
    body[8]  = ins;
    body[12] = p1;
    body[16] = p2;
    body[20] = p3;
    body[24] = (guint8) data_len;
    if (data && data_len) {
        memcpy (&body[26], data, MIN (data_len, 4));
        len = 26 + MIN (data_len, 4);
    }
    return mm_sit_request_new (MM_SIT_MESSAGE_TRANSMIT_APDU_CHANNEL,
                               mm_sit_client_next_token (self), body, len, error);
}

static void
uicc_probe_ready (MMSitClient  *self,
                  GAsyncResult *res,
                  GTask        *task)
{
    g_autoptr(MMSitMessage) response = NULL;
    g_autoptr(GError)       error = NULL;
    UiccProbeContext       *ctx = g_task_get_task_data (task);

    response = mm_sit_client_command_finish (self, res, &error);

    if (ctx->step == UICC_STEP_OPEN_PLAIN) {
        /* The only answer this sequence reads: the channel to work on. The
         * named-AID opens before it are expected to fail with 6a82 on a card
         * that carries no access-rule applet, which is both of ours. */
        const guint8 *payload = NULL;
        gsize         payload_len = 0;

        if (response)
            payload = mm_sit_message_get_payload (response, &payload_len);
        /* get_payload() is frame+8, so the four status bytes come first and
         * the channel the CP assigned sits at +4, its SW at +8. */
        if (payload && payload_len >= 10) {
            guint32 channel;
            guint16 sw;

            memcpy (&channel, payload + 4, 4);
            memcpy (&sw, payload + 8, 2);
            if (channel != UICC_NO_CHANNEL && sw == 0x0090) {
                ctx->channel = channel;
                mm_obj_dbg (self->priv->port, "UICC access rules: reading EF_DIR on channel %u",
                            channel);
            } else {
                mm_obj_dbg (self->priv->port, "UICC access rules: no channel (sw %04x); done",
                            sw);
                ctx->step = UICC_STEP_DONE;
            }
        } else {
            ctx->step = UICC_STEP_DONE;
        }
    } else if (!response && ctx->channel != UICC_NO_CHANNEL && ctx->step < UICC_STEP_CLOSE) {
        /* Never leave a channel open because a read failed. */
        mm_obj_dbg (self->priv->port, "UICC access rules: step %d failed (%s); closing the channel",
                    (int) ctx->step, error ? error->message : "no response");
        ctx->step = UICC_STEP_CLOSE;
        uicc_probe_step (task);
        return;
    }

    ctx->step++;
    uicc_probe_step (task);
}

static void
uicc_probe_step (GTask *task)
{
    MMSitClient           *self = g_task_get_source_object (task);
    UiccProbeContext      *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray)  request = NULL;
    g_autoptr(GError)      error = NULL;
    static const guint8    ef_mf[2]  = { 0x3f, 0x00 };
    static const guint8    ef_dir[2] = { 0x2f, 0x00 };

    /* Nothing here is reported to anyone, so a step that cannot even be built
     * is skipped rather than failed -- except the close, which matters. */
    switch (ctx->step) {
    case UICC_STEP_OPEN_ARA_M:
        request = uicc_build_open (self, uicc_aid_ara_m, sizeof (uicc_aid_ara_m), &error);
        break;
    case UICC_STEP_OPEN_PKCS15:
        request = uicc_build_open (self, uicc_aid_pkcs15, sizeof (uicc_aid_pkcs15), &error);
        break;
    case UICC_STEP_OPEN_GPAC:
        request = uicc_build_open (self, uicc_aid_gpac, sizeof (uicc_aid_gpac), &error);
        break;
    case UICC_STEP_OPEN_PLAIN:
        request = uicc_build_open (self, NULL, 0, &error);
        break;
    case UICC_STEP_SELECT_MF:
        request = uicc_build_apdu (self, ctx->channel, 0xa4, 0x00, 0x04, 0x02,
                                   ef_mf, sizeof (ef_mf), &error);
        break;
    case UICC_STEP_SELECT_EF_DIR:
        request = uicc_build_apdu (self, ctx->channel, 0xa4, 0x00, 0x04, 0x02,
                                   ef_dir, sizeof (ef_dir), &error);
        break;
    case UICC_STEP_READ_RECORD:
        request = uicc_build_apdu (self, ctx->channel, 0xb2, 0x01, 0x04, 0x26,
                                   NULL, 0, &error);
        break;
    case UICC_STEP_CLOSE:
        if (ctx->channel == UICC_NO_CHANNEL)
            break;
        request = mm_sit_request_new (MM_SIT_MESSAGE_CLOSE_LOGICAL_CHANNEL,
                                      mm_sit_client_next_token (self),
                                      (const guint8 *) &ctx->channel, 4, &error);
        ctx->channel = UICC_NO_CHANNEL;
        break;
    case UICC_STEP_DONE:
    default:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    if (!request) {
        if (error)
            mm_obj_dbg (self->priv->port, "UICC access rules: couldn't build step %d: %s",
                        (int) ctx->step, error->message);
        ctx->step++;
        uicc_probe_step (task);
        return;
    }

    mm_sit_client_command (self, request, 10, g_task_get_cancellable (task),
                           (GAsyncReadyCallback) uicc_probe_ready, task);
}

/*
 * Read the elementary files stock reads at boot.
 *
 * The largest single divergence in the whole capture, and the one a filtered
 * parity score hid: stock issues 121 SIM_IO exchanges on ch 0xf5 and 167 on
 * 0xf6 before its IMS context, where we issued 27 and 25. Most of the volume is
 * two files -- EF_OPL (0x6fc6) and EF_PNN (0x6fc5) are read record by record,
 * 50 and 20-49 times -- and the rest is one GET_RESPONSE plus one read per file
 * across the standard set.
 *
 * mm_sit_client_read_ef() already does the whole shape of a file read, record
 * loops included, so this is the list and the order: ch 0xf5's sequence first,
 * then the files only ch 0xf6 touches. A card without one of them answers with
 * a status word and that file is skipped, so one list serves both stacks.
 *
 * Nothing is kept. The files the plugin actually uses are read by the code that
 * needs them; this exists so the CP sees the traffic it sees on stock.
 */
/*
 * Stock reads each of these once: GET_RESPONSE plus a single read. Only the
 * operator-name tables are read record by record, which is where the bulk of
 * its SIM_IO volume comes from (EF_OPL 50 reads, EF_PNN 20-49).
 *
 * Reading every record of everything overshot stock badly -- 206 exchanges
 * against its 121, with 31 records of EF_SMS (0x6f3c) and 25 of 0x6f3b that
 * stock never reads at all -- so `records` picks the two that want the whole
 * file and the rest stop after one pass.
 *
 * Files the plugin reads for its own purposes are deliberately absent: 0x6f07
 * (IMSI), 0x6f40 (MSISDN), 0x6f46, 0x6f60, 0x6f61, 0x6f3e, 0x6f3f, 0x6fad and
 * 0x6fb7 are read by the code that needs them, and listing them here read each
 * one twice where stock reads it once.
 */
typedef struct {
    guint16  file_id;
    gboolean records; /* read every record, not just the first pass */
} BootEf;

static const BootEf boot_ef_files[] = {
    { 0x2fe2, FALSE }, { 0x6fc9, FALSE }, { 0x6fca, FALSE }, { 0x6f11, FALSE },
    { 0x6fcb, FALSE }, { 0x6f13, FALSE }, { 0x6fcd, FALSE },
    { 0x6fc5, TRUE  }, { 0x6fc6, TRUE  },
    { 0x6f38, FALSE }, { 0x6f16, FALSE }, { 0x6f15, FALSE }, { 0x6f62, FALSE },
    { 0x6fd9, FALSE }, { 0x6f7b, FALSE }, { 0x6f05, FALSE }, { 0x2f05, FALSE },
    { 0x6f3c, FALSE }, { 0x6fe5, FALSE }, { 0x6f43, FALSE }, { 0x6fe7, FALSE },
    { 0x6fc7, FALSE }, { 0x6f14, FALSE }, { 0x6f18, FALSE }, { 0x6f3b, TRUE  },
    { 0x6f02, FALSE }, { 0x6f04, FALSE }, { 0x6f03, FALSE }, { 0x6f09, FALSE },
};

typedef struct {
    guint index;
} BootEfContext;

static void boot_ef_step (GTask *task);

static void
boot_ef_ready (MMSitClient  *self,
               GAsyncResult *res,
               GTask        *task)
{
    g_autoptr(GBytes) data = NULL;
    g_autoptr(GError) error = NULL;

    /* A file this card does not have is not a problem; stock gets refusals
     * here too. The read itself is the point. */
    data = mm_sit_client_read_ef_finish (self, res, NULL, &error);
    boot_ef_step (task);
}

static void
boot_ef_step (GTask *task)
{
    MMSitClient   *self = g_task_get_source_object (task);
    BootEfContext *ctx = g_task_get_task_data (task);
    const BootEf  *ef;

    if (ctx->index >= G_N_ELEMENTS (boot_ef_files)) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    ef = &boot_ef_files[ctx->index++];
    read_ef_capped (self, ef->file_id, NULL, 0, ef->records ? 0 : 1,
                    g_task_get_cancellable (task),
                    (GAsyncReadyCallback) boot_ef_ready, task);
}

void
mm_sit_client_read_boot_efs (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, g_new0 (BootEfContext, 1), g_free);
    mm_obj_dbg (self->priv->port, "reading the %u elementary files stock reads at boot",
                (guint) G_N_ELEMENTS (boot_ef_files));
    boot_ef_step (task);
}

gboolean
mm_sit_client_read_boot_efs_finish (MMSitClient  *self,
                                   GAsyncResult *res,
                                   GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_probe_uicc_access_rules (MMSitClient        *self,
                                       GCancellable       *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer            user_data)
{
    UiccProbeContext *ctx;
    GTask            *task;

    task = g_task_new (self, cancellable, callback, user_data);
    ctx = g_new0 (UiccProbeContext, 1);
    ctx->channel = UICC_NO_CHANNEL;
    g_task_set_task_data (task, ctx, g_free);
    uicc_probe_step (task);
}

gboolean
mm_sit_client_probe_uicc_access_rules_finish (MMSitClient  *self,
                                              GAsyncResult *res,
                                              GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

guint
mm_sit_client_get_aid_count (MMSitClient *self)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), 0);
    return self->priv->sim_aid_count;
}

/* The same query against one named application, for the per-AID pass stock
 * makes. Out-of-range falls back to whatever the card named first. */
void
mm_sit_client_get_facility_lock_for_aid (MMSitClient        *self,
                                         guint8              facility,
                                         guint               aid_index,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;
    const guint8         *aid = self->priv->sim_aid_len ? self->priv->sim_aid : NULL;
    gsize                 aid_len = self->priv->sim_aid_len;

    if (aid_index < self->priv->sim_aid_count && self->priv->sim_aid_lens[aid_index]) {
        aid = self->priv->sim_aids[aid_index];
        aid_len = self->priv->sim_aid_lens[aid_index];
    }

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_facility_lock (mm_sit_client_next_token (self), facility,
                                                    aid, aid_len);
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_facility_lock_ready, task);
}

gboolean
mm_sit_client_get_facility_lock_finish (MMSitClient  *self,
                                        GAsyncResult *res,
                                        gboolean     *out_locked,
                                        GError      **error)
{
    GError *inner = NULL;
    gboolean locked;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_locked != NULL, FALSE);

    /* The task's boolean *is* the lock state, so a FALSE result is only an error
     * when one was set. */
    locked = g_task_propagate_boolean (G_TASK (res), &inner);
    if (inner) {
        g_propagate_error (error, inner);
        return FALSE;
    }
    *out_locked = locked;
    return TRUE;
}

static void
get_allowed_network_types_ready (MMSitClient  *self,
                                 GAsyncResult *res,
                                 GTask        *task)
{
    g_autoptr(MMSitMessage)  response = NULL;
    MMSitNetworkGenerations *generations;
    GError                  *error = NULL;

    response = mm_sit_client_command_finish (self, res, &error);
    generations = g_new0 (MMSitNetworkGenerations, 1);
    if (response && mm_sit_message_parse_allowed_network_types (response, generations, &error))
        g_task_return_pointer (task, generations, g_free);
    else {
        g_free (generations);
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

/*
 * Which radio generations the CP is allowed to use. Downstream sets the
 * all-generations bitmap on every boot and narrows it only when the user picks a
 * specific mode; the CP keeps the value across our restarts, so it has to be
 * read back rather than assumed.
 */
void
mm_sit_client_get_allowed_network_types (MMSitClient        *self,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    request = mm_sit_request_new_get_allowed_network_types (mm_sit_client_next_token (self));
    mm_sit_client_command (self, request, 5, cancellable,
                           (GAsyncReadyCallback) get_allowed_network_types_ready, task);
}

gboolean
mm_sit_client_get_allowed_network_types_finish (MMSitClient             *self,
                                               GAsyncResult            *res,
                                               MMSitNetworkGenerations *out_generations,
                                               GError                 **error)
{
    MMSitNetworkGenerations *generations;

    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    g_return_val_if_fail (out_generations != NULL, FALSE);

    generations = g_task_propagate_pointer (G_TASK (res), error);
    if (!generations)
        return FALSE;
    *out_generations = *generations;
    g_free (generations);
    return TRUE;
}

void
mm_sit_client_set_initial_attach_apn (MMSitClient        *self,
                                      const gchar        *apn,
                                      MMSitPdpType        pdp,
                                      guint8              pcscf,
                                      const MMSitApnAuth *auth,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    /* Remember it verbatim: setup_data_call() re-asserts it before the first
     * context of this attach epoch, which is what stock does. */
    g_free (self->priv->attach_apn);
    g_free (self->priv->attach_auth_user);
    g_free (self->priv->attach_auth_password);
    self->priv->attach_apn = g_strdup (apn);
    self->priv->attach_pdp = pdp;
    self->priv->attach_pcscf = pcscf;
    self->priv->attach_auth_type = auth ? auth->auth : MM_SIT_AUTH_TYPE_NONE;
    self->priv->attach_auth_user = auth ? g_strdup (auth->user) : NULL;
    self->priv->attach_auth_password = auth ? g_strdup (auth->password) : NULL;
    self->priv->attach_reasserted = FALSE;

    request = mm_sit_request_new_set_initial_attach_apn_for_stack (
        mm_sit_client_next_token (self), self->priv->stack, apn, pdp, pcscf, auth, &error);
    if (!request) {
        GTask *task = g_task_new (self, cancellable, callback, user_data);

        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_initial_attach_apn_finish (MMSitClient  *self,
                                             GAsyncResult *res,
                                             GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_data_profile (MMSitClient        *self,
                                const gchar        *apn,
                                MMSitPdpType        pdp,
                                guint32             apn_types,
                                guint32             bearers,
                                const MMSitApnAuth *auth,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
    mm_sit_client_set_data_profile_roaming (self, apn, pdp, pdp, apn_types, bearers,
                                            auth, cancellable, callback, user_data);
}

/*
 * The profile push with the carrier's roaming protocol.
 *
 * Stock sends it separately from the home protocol -- frame+0x76, measured on
 * identical firmware: services.telus.com and sos go out pdp 2 / roaming 3
 * where copying the home protocol sent 2. `ims.db` has carried the column all
 * along; the value was simply never passed on.
 */
void
mm_sit_client_set_data_profile_roaming (MMSitClient        *self,
                                        const gchar        *apn,
                                        MMSitPdpType        pdp,
                                        MMSitPdpType        roaming_pdp,
                                        guint32             apn_types,
                                        guint32             bearers,
                                        const MMSitApnAuth *auth,
                                        GCancellable       *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_data_profile_roaming (mm_sit_client_next_token (self),
                                                           apn, pdp, roaming_pdp,
                                                           apn_types, bearers, auth, &error);
    if (!request) {
        GTask *task = g_task_new (self, cancellable, callback, user_data);

        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_data_profile_finish (MMSitClient  *self,
                                       GAsyncResult *res,
                                       GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_sit_client_set_allowed_network_types (MMSitClient                   *self,
                                         const MMSitNetworkGenerations *generations,
                                         GCancellable                  *cancellable,
                                         GAsyncReadyCallback            callback,
                                         gpointer                       user_data)
{
    g_autoptr(GByteArray) request = NULL;
    g_autoptr(GError)     error = NULL;

    request = mm_sit_request_new_set_allowed_network_types (mm_sit_client_next_token (self), generations, &error);
    if (!request) {
        GTask *task = g_task_new (self, cancellable, callback, user_data);

        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }
    run_checked_command (self, request, 10, cancellable, callback, user_data);
}

gboolean
mm_sit_client_set_allowed_network_types_finish (MMSitClient  *self,
                                                GAsyncResult *res,
                                                GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), FALSE);
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * Network scan. The CP has no "search complete" message: it pushes a
 * NET_SCAN_RESULT batch every few seconds, each one repeating every network
 * found so far, until it is told to stop. In the captures the batches arrive
 * 3-6 s apart and downstream stops the scan a few seconds after the last one, so
 * the scan is treated as finished once a batch has gone unanswered for
 * SCAN_QUIET_SECONDS, with SCAN_MAX_SECONDS as the hard stop.
 */
#define SCAN_QUIET_SECONDS 10
#define SCAN_MAX_SECONDS   70

typedef struct {
    GArray *networks;
    gulong  indication_id;
    guint   quiet_id;
    guint   deadline_id;
} ScanContext;

static void
scan_context_free (ScanContext *ctx)
{
    if (ctx->quiet_id)
        g_source_remove (ctx->quiet_id);
    if (ctx->deadline_id)
        g_source_remove (ctx->deadline_id);
    if (ctx->networks)
        g_array_unref (ctx->networks);
    g_free (ctx);
}

static void
scan_stop_ready (MMSitClient  *self,
                 GAsyncResult *res,
                 gpointer      user_data)
{
    g_autoptr(GError) error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error))
        mm_obj_dbg (self->priv->port, "network scan: stop request failed: %s", error->message);
}

/* Detach from the indication stream and stop the CP's search, then hand the
 * caller whatever the last batch held. */
static void
scan_complete (GTask *task)
{
    MMSitClient          *self = g_task_get_source_object (task);
    ScanContext          *ctx = g_task_get_task_data (task);
    g_autoptr(GByteArray) request = NULL;

    if (ctx->indication_id) {
        g_signal_handler_disconnect (self, ctx->indication_id);
        ctx->indication_id = 0;
    }
    if (ctx->quiet_id) {
        g_source_remove (ctx->quiet_id);
        ctx->quiet_id = 0;
    }
    if (ctx->deadline_id) {
        g_source_remove (ctx->deadline_id);
        ctx->deadline_id = 0;
    }

    request = mm_sit_request_new_stop_network_scan (mm_sit_client_next_token (self));
    run_checked_command (self, request, 10, NULL,
                         (GAsyncReadyCallback) scan_stop_ready, NULL);

    if (ctx->networks) {
        g_task_return_pointer (task, g_steal_pointer (&ctx->networks),
                               (GDestroyNotify) g_array_unref);
    } else {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "CP reported no networks before the scan timed out");
    }
    g_object_unref (task);
}

static gboolean
scan_quiet_elapsed (GTask *task)
{
    ScanContext *ctx = g_task_get_task_data (task);

    ctx->quiet_id = 0;
    scan_complete (task);
    return G_SOURCE_REMOVE;
}

static gboolean
scan_deadline_elapsed (GTask *task)
{
    ScanContext *ctx = g_task_get_task_data (task);

    ctx->deadline_id = 0;
    scan_complete (task);
    return G_SOURCE_REMOVE;
}

static void
scan_indication (MMSitClient        *self,
                 const MMSitMessage *message,
                 GTask              *task)
{
    ScanContext       *ctx = g_task_get_task_data (task);
    GArray            *networks = NULL;
    g_autoptr(GError)  error = NULL;

    if (message->id != MM_SIT_MESSAGE_NET_SCAN_RESULT)
        return;

    if (!mm_sit_message_parse_net_scan_result (message, &networks, &error)) {
        mm_obj_dbg (self->priv->port, "network scan: unparsable result batch: %s",
                    error->message);
        return;
    }

    mm_obj_dbg (self->priv->port, "network scan: batch of %u network(s)", networks->len);
    /* Each batch supersedes the previous one rather than adding to it. */
    if (ctx->networks)
        g_array_unref (ctx->networks);
    ctx->networks = networks;

    if (ctx->quiet_id)
        g_source_remove (ctx->quiet_id);
    ctx->quiet_id = g_timeout_add_seconds (SCAN_QUIET_SECONDS,
                                           (GSourceFunc) scan_quiet_elapsed, task);
}

static void
scan_start_ready (MMSitClient  *self,
                  GAsyncResult *res,
                  GTask        *task)
{
    ScanContext *ctx = g_task_get_task_data (task);
    GError      *error = NULL;

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->indication_id = g_signal_connect (self, "indication",
                                           G_CALLBACK (scan_indication), task);
    ctx->deadline_id = g_timeout_add_seconds (SCAN_MAX_SECONDS,
                                              (GSourceFunc) scan_deadline_elapsed, task);
}

void
mm_sit_client_scan_networks (MMSitClient        *self,
                             GCancellable       *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer            user_data)
{
    g_autoptr(GByteArray) request = NULL;
    GTask                *task;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, g_new0 (ScanContext, 1), (GDestroyNotify) scan_context_free);

    request = mm_sit_request_new_start_network_scan (mm_sit_client_next_token (self));
    run_checked_command (self, request, 10, cancellable,
                         (GAsyncReadyCallback) scan_start_ready, task);
}

GArray *
mm_sit_client_scan_networks_finish (MMSitClient  *self,
                                    GAsyncResult *res,
                                    GError      **error)
{
    g_return_val_if_fail (MM_IS_SIT_CLIENT (self), NULL);
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
dispose (GObject *object)
{
    MMSitClient *self = MM_SIT_CLIENT (object);

    mm_sit_client_close (self);
    g_clear_object (&self->priv->port);
    G_OBJECT_CLASS (mm_sit_client_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMSitClient *self = MM_SIT_CLIENT (object);

    if (self->priv->cp)
        g_object_remove_weak_pointer (G_OBJECT (self->priv->cp), (gpointer *) &self->priv->cp);
    g_free (self->priv->device);
    g_free (self->priv->proxy_fallback_device);
    g_free (self->priv->device_model);
    g_free (self->priv->device_build);
    g_free (self->priv->device_codename);
    g_free (self->priv->device_sw_version);
    g_free (self->priv->cp_sw_version);
    g_free (self->priv->device_manufacturer);
    g_free (self->priv->device_os_name);
    g_free (self->priv->device_os_version);
    g_free (self->priv->attach_apn);
    g_free (self->priv->attach_auth_user);
    g_free (self->priv->attach_auth_password);
    g_clear_pointer (&self->priv->cids, g_hash_table_unref);
    G_OBJECT_CLASS (mm_sit_client_parent_class)->finalize (object);
}

/* First line of a file, trimmed, or NULL. Device-tree properties are
 * NUL-terminated, which g_strstrip leaves alone but the length hides. */
static gchar *
read_first_line (const gchar *path)
{
    g_autofree gchar *contents = NULL;
    gchar            *newline;

    if (!g_file_get_contents (path, &contents, NULL, NULL))
        return NULL;
    newline = strchr (contents, '\n');
    if (newline)
        *newline = '\0';
    g_strstrip (contents);
    return contents[0] ? g_strdup (contents) : NULL;
}

/* The board codename, as the CP knows it: the machine part of the first
 * device-tree compatible, e.g. "google,zumapro-tegu" -> "tegu". */
static gchar *
read_board_codename (void)
{
    g_autofree gchar *compatible = NULL;
    const gchar      *dash;
    const gchar      *comma;

    compatible = read_first_line ("/proc/device-tree/compatible");
    if (!compatible)
        return NULL;
    /*
     * The first compatible is "<vendor>,<board>", so the board name is what
     * follows the last ',' or '-' -- whichever comes later. Splitting on '-'
     * alone left komodo's "google,komodo" whole and we told the CP
     * "google,komodo" where stock says "komodo" (measured, DEVICE_INFO field 3,
     * Android B-16035863 vs pmOS r32).
     */
    dash = strrchr (compatible, '-');
    comma = strrchr (compatible, ',');
    if (comma && (!dash || comma > dash))
        dash = comma;
    return g_strdup (dash ? dash + 1 : compatible);
}

/* os-release values are shell-quoted; GKeyFile hands back the quotes. */
static gchar *
os_release_value (GKeyFile    *keyfile,
                  const gchar *key)
{
    g_autofree gchar *raw = NULL;
    gchar            *unquoted;

    raw = g_key_file_get_string (keyfile, "os", key, NULL);
    if (!raw || !raw[0])
        return NULL;
    unquoted = g_shell_unquote (raw, NULL);
    return unquoted ?: g_strdup (raw);
}

/* The vendor part of the first device-tree compatible: "google,zumapro-tegu"
 * -> "Google". The SIP User-Agent leads with it, and the vendor writes it
 * capitalised where the device tree does not. */
static gchar *
read_vendor (void)
{
    g_autofree gchar *compatible = NULL;
    gchar            *vendor;
    const gchar      *comma;

    compatible = read_first_line ("/proc/device-tree/compatible");
    if (!compatible)
        return NULL;
    comma = strchr (compatible, ',');
    if (!comma)
        return NULL;
    vendor = g_strndup (compatible, comma - compatible);
    if (vendor[0])
        vendor[0] = g_ascii_toupper (vendor[0]);
    return vendor;
}


/* One os-release field, or NULL. */
static gchar *
read_os_field (const gchar *key)
{
    g_autoptr(GError)   error = NULL;
    g_autoptr(GKeyFile) keyfile = NULL;
    g_autofree gchar   *contents = NULL;
    g_autofree gchar   *wrapped = NULL;

    if (!g_file_get_contents ("/etc/os-release", &contents, NULL, NULL))
        return NULL;
    keyfile = g_key_file_new ();
    wrapped = g_strconcat ("[os]\n", contents, NULL);
    if (!g_key_file_load_from_data (keyfile, wrapped, -1, G_KEY_FILE_NONE, &error))
        return NULL;
    return os_release_value (keyfile, key);
}

/*
 * The OS version for the User-Agent. Android puts its platform version here --
 * the vendor's template calls it #AV# -- so it is a number on every device this
 * CP has been provisioned by. A rolling release has none: postmarketOS reports
 * VERSION_ID="edge". Fall back to the kernel release, which is the closest true
 * number this system has.
 */
static gchar *
read_os_version_number (void)
{
    g_autofree gchar *version = NULL;
    struct utsname    u;

    version = read_os_field ("VERSION_ID");
    if (version && g_ascii_isdigit (version[0]))
        return g_steal_pointer (&version);
    if (uname (&u) == 0 && u.release[0])
        return g_strdup (u.release);
    return g_steal_pointer (&version);
}

static gchar *
read_os_build (void)
{
    g_autoptr(GError)      error = NULL;
    g_autofree gchar      *name = NULL;
    g_autofree gchar      *version = NULL;
    g_autoptr(GKeyFile)    keyfile = NULL;
    g_autofree gchar      *contents = NULL;

    /* os-release is shell syntax, not a key file, but the values we want are
     * plain enough that GKeyFile reads them once a group header is added. */
    if (!g_file_get_contents ("/etc/os-release", &contents, NULL, NULL))
        return NULL;
    keyfile = g_key_file_new ();
    {
        g_autofree gchar *wrapped = g_strconcat ("[os]\n", contents, NULL);

        if (!g_key_file_load_from_data (keyfile, wrapped, -1, G_KEY_FILE_NONE, &error))
            return NULL;
    }
    name = os_release_value (keyfile, "ID");
    version = os_release_value (keyfile, "BUILD_ID");
    if (!version)
        version = os_release_value (keyfile, "VERSION_ID");
    if (!name && !version)
        return NULL;
    /*
     * One token, hyphenated -- "nura-edge", not "nura edge".
     *
     * This string is DEVICE_INFO's build field and the User-Agent's #BUILD#.
     * A vendor build id is always a single token (CP2A.260605.016), and the UA
     * path enforces that itself by replacing spaces, so a space here only ever
     * reached the CP through DEVICE_INFO.
     *
     * It is not a fix for anything: the spaced form was briefly suspected of
     * costing an IMS PDN and then exonerated -- the same build registered both
     * stacks on the next boot, so that run was variance. This is only a
     * well-formed value.
     */
    return g_strdup_printf ("%s-%s", name ?: "linux", version ?: "unknown");
}

static void
set_device_info (MMSitClient *self,
                 const gchar *model,
                 const gchar *build,
                 const gchar *codename,
                 const gchar *sw_version,
                 const gchar *cp_sw_version)
{
    g_free (self->priv->device_model);
    g_free (self->priv->device_build);
    g_free (self->priv->device_codename);
    g_free (self->priv->device_sw_version);
    g_free (self->priv->cp_sw_version);
    self->priv->device_sw_version = sw_version && sw_version[0] ? g_strdup (sw_version) : NULL;
    self->priv->cp_sw_version = cp_sw_version && cp_sw_version[0] ? g_strdup (cp_sw_version) : NULL;

    self->priv->device_model = model && model[0] ? g_strdup (model)
        : read_first_line ("/proc/device-tree/model");
    self->priv->device_build = build && build[0] ? g_strdup (build) : read_os_build ();
    self->priv->device_codename = codename && codename[0] ? g_strdup (codename)
        : read_board_codename ();

    if (!self->priv->device_model)
        self->priv->device_model = g_strdup (BOOT_CONFIG_DEVICE_MODEL_FALLBACK);
    if (!self->priv->device_build)
        self->priv->device_build = g_strdup (BOOT_CONFIG_DEVICE_BUILD_FALLBACK);
    if (!self->priv->device_codename)
        self->priv->device_codename = g_strdup ("");

    g_free (self->priv->device_manufacturer);
    g_free (self->priv->device_os_name);
    g_free (self->priv->device_os_version);
    self->priv->device_manufacturer = read_vendor ();
    self->priv->device_os_name = read_os_field ("NAME");
    self->priv->device_os_version = read_os_version_number ();

    /* 31 characters plus a NUL each; the builder refuses anything longer. */
    self->priv->device_model[MIN (strlen (self->priv->device_model), MM_SIT_MAX_DEVICE_INFO_FIELD - 1)] = '\0';
    self->priv->device_build[MIN (strlen (self->priv->device_build), MM_SIT_MAX_DEVICE_INFO_FIELD - 1)] = '\0';
    self->priv->device_codename[MIN (strlen (self->priv->device_codename), MM_SIT_MAX_DEVICE_INFO_FIELD - 1)] = '\0';

    mm_obj_dbg (self, "telling the CP the AP is '%s' / '%s' / '%s'",
                self->priv->device_model, self->priv->device_build,
                self->priv->device_codename);
}

/* Tell the CP what the AP is. Any argument may be NULL, in which case the
 * running system answers for it. Each field is truncated to what 0x0922
 * carries. */
void
mm_sit_client_set_device_info (MMSitClient *self,
                               const gchar *model,
                               const gchar *build,
                               const gchar *codename,
                               const gchar *sw_version,
                               const gchar *cp_sw_version)
{
    g_return_if_fail (MM_IS_SIT_CLIENT (self));

    set_device_info (self, model, build, codename, sw_version, cp_sw_version);
}

/* The client is its own log object rather than borrowing the port's. The port
 * only exists once the client is open, so logging through it was a crash
 * waiting for anyone who wrote the obvious mm_obj_dbg (self, ...) -- and with
 * two logical stacks the client, not the port, is the thing worth naming. */
static gchar *
log_object_build_id (MMLogObject *_self)
{
    MMSitClient *self = MM_SIT_CLIENT (_self);

    return g_strdup_printf ("%s/client", self->priv->device ?: "sit");
}

static void
log_object_iface_init (MMLogObjectInterface *iface)
{
    iface->build_id = log_object_build_id;
}

static void
mm_sit_client_init (MMSitClient *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_SIT_CLIENT,
                                              MMSitClientPrivate);
}

static void
mm_sit_client_class_init (MMSitClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSitClientPrivate));
    object_class->dispose = dispose;
    object_class->finalize = finalize;

    signals[SIGNAL_INDICATION] =
        g_signal_new ("indication",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_POINTER);

    /*
     * A data PDN the CP would not create, with the cause it gave. Whoever owns
     * the attach APN needs to know, because on this CP the choice of attach
     * APN is what decides whether a data call can be created at all.
     */
    signals[SIGNAL_DATA_PDN_REFUSED] =
        g_signal_new (MM_SIT_CLIENT_SIGNAL_DATA_PDN_REFUSED,
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE,
                      1,
                      G_TYPE_UINT);
}
