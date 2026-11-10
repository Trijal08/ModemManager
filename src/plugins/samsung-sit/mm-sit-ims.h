/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_IMS_H
#define MM_SIT_IMS_H

#include <glib-object.h>
#include <ModemManager.h>

#include "mm-sit-client.h"
#include "mm-sit-carrier-config.h"
#include "mm-sit-ims-config.h"

#define MM_TYPE_SIT_IMS            (mm_sit_ims_get_type ())
#define MM_SIT_IMS(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SIT_IMS, MMSitIms))
#define MM_IS_SIT_IMS(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SIT_IMS))

typedef struct _MMSitIms        MMSitIms;
typedef struct _MMSitImsClass   MMSitImsClass;
typedef struct _MMSitImsPrivate MMSitImsPrivate;

struct _MMSitIms {
    GObject          parent;
    MMSitImsPrivate *priv;
};

struct _MMSitImsClass {
    GObjectClass parent;
};

GType mm_sit_ims_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitIms, g_object_unref)

/*
 * Drives the CP's own IMS stack: bring up the ims PDN on its own context,
 * start the stack, push the configuration it cannot invent, hand it the address
 * and proxies the network granted, and follow what it reports back.
 *
 * `port_index` selects the stack, which on this CP *is* the SIT port -- there is
 * no in-band selector, so one of these belongs to one port.
 */
MMSitIms *mm_sit_ims_new (MMSitClient *client,
                          guint        port_index,
                          const gchar *ims_database);

/* What to substitute into the SIP User-Agent. Must be set before IMS can start;
 * the CP will not register without one. */
void mm_sit_ims_set_device_identity (MMSitIms                     *self,
                                     const MMSitImsDeviceIdentity *device);

/*
 * The trigger. IMS needs a packet-switched RAT, so this starts the sequence
 * when one appears and tears it down when it goes -- but whether a call can be
 * placed is [`mm_sit_ims_is_registered`], never this, because the network can
 * refuse IMS on a perfectly good LTE cell.
 */
/* Re-run the start gate with what is already known, for a condition that
 * became true between access-tech updates. */
void mm_sit_ims_reevaluate (MMSitIms *self);

void mm_sit_ims_update_access_tech (MMSitIms                   *self,
                                    MMModemAccessTechnology     act);

/*
 * Whether the modem is registered, i.e. whether the radio is up and settled.
 *
 * IMS does not wait for mobile data. It carries its own context on its own APN,
 * the way Android keeps VoLTE working with mobile data switched off -- gating on
 * the default connection meant IMS never registered without it.
 *
 * What it does wait for is the radio, because applying the attach APN
 * power-cycles it (SetInitialEpsBearerSettings is a power-down/set/restore in
 * MM core) and that drops every context, this one included. Registration is the
 * signal for both halves of that: it goes away while the radio is down, which
 * invalidates a stack whose PDN has just been pulled, and it comes back
 * afterwards, which starts a fresh one.
 */
void mm_sit_ims_set_registered (MMSitIms *self,
                                gboolean  registered);

/* Apply packet registration and its VoPS capability together, before MM core
 * publishes a state change that may start IMS. Unknown preserves the fallback
 * to RAT/registration and bounded provisioning attempts. */
void mm_sit_ims_update_registration (MMSitIms              *self,
                                    const MMSitRegistration *registration);

/* The serving network's MCCMNC, which selects the carrier configuration. */
/*
 * Which subscription's configuration to use. This is the SIM's identity, not
 * the serving network's: an o2 SIM (26207) camps on 26203, which is a different
 * record in Google's carrier list -- E-Plus, from before the two networks
 * merged -- so keying on where we are camped picks the wrong carrier in exactly
 * the case that matters. The modem builds this identity once and both the
 * carrier configuration and this share it.
 */
void mm_sit_ims_set_carrier_identity (MMSitIms                   *self,
                                      const MMSitCarrierIdentity *identity);

void mm_sit_ims_set_operator_code (MMSitIms    *self,
                                   const gchar *mccmnc);

/* Whether the CP has told us it is registered. The gate for routing a call to
 * IMS rather than CS. */
gboolean mm_sit_ims_is_registered (MMSitIms *self);

/*
 * IMS calls, as MMCallInfo, for merging into the Voice interface's list.
 *
 * AIMS has no equivalent of GET_CALL_LIST: the CP pushes CALL_RING and
 * CALL_STATUS and expects the AP to keep the state itself. So the CS half of
 * the list is polled and this half is tracked, and the interface sees one list
 * either way. Caller owns the returned list.
 */
GList *mm_sit_ims_get_call_list (MMSitIms *self);

/* Emitted when a tracked call changed, so the Voice interface re-reads the
 * list -- the same shape as the CS group's empty CALL_STATE_CHANGED poke. */
#define MM_SIT_IMS_SIGNAL_CALLS_CHANGED "calls-changed"
#define MM_SIT_IMS_SIGNAL_PDN_REFUSED  "pdn-refused"

/* The P-Associated-URI list from the last successful registration, or NULL. */
const gchar *mm_sit_ims_get_associated_uris (MMSitIms *self);

/*
 * Call control. Separate from the CS group's because AIMS is a separate
 * protocol: different requests, different ids, its own call numbering. The
 * Voice interface picks between them on [`mm_sit_ims_is_registered`].
 */
/*
 * AIMS media states, as observed on this hardware across a VoLTE call:
 * setup just after the dial, alerting before it connects, connected as it
 * becomes active, ended at hangup.  Values are what the CP was sent, not
 * names it told us.
 */
#define MM_SIT_IMS_MEDIA_SETUP     2
#define MM_SIT_IMS_MEDIA_ALERTING  4
#define MM_SIT_IMS_MEDIA_CONNECTED 0
#define MM_SIT_IMS_MEDIA_ENDED     6

/*
 * The APN the IMS PDN should use, from the carrier's own list -- the entry its
 * CarrierSettings marks IMS, with that entry's address family. Telus ships
 * 'ims' as IPv6-only; asking for both families works but describes the context
 * wrongly. Until this is set the built-in default stands, so a SIM with no
 * carrier record still gets a sensible attempt.
 */
void     mm_sit_ims_set_apn           (MMSitIms    *self,
                                       const gchar *apn,
                                       guint8       pdp,
                                       guint32      bearers);

void     mm_sit_ims_set_frame_time    (MMSitIms *self);
void     mm_sit_ims_set_rtp_threshold (MMSitIms *self);

void     mm_sit_ims_media_state   (MMSitIms            *self,
                                   guint8               state);

void     mm_sit_ims_dial          (MMSitIms            *self,
                                   const gchar         *number,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
gboolean mm_sit_ims_dial_finish   (MMSitIms            *self,
                                   GAsyncResult        *res,
                                   GError             **error);
void     mm_sit_ims_answer        (MMSitIms            *self,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
gboolean mm_sit_ims_answer_finish (MMSitIms            *self,
                                   GAsyncResult        *res,
                                   GError             **error);
/* `number` is the far end as the caller knows it, used only to name a
 * conference member being dropped; NULL is accepted. */
void     mm_sit_ims_hangup        (MMSitIms            *self,
                                   guint8               call_id,
                                   const gchar         *number,
                                   gboolean             declined,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
/* CALL_MANAGE operations, in the CP's numbering. */
#define MM_SIT_IMS_CALL_MANAGE_HOLD       1
#define MM_SIT_IMS_CALL_MANAGE_RESUME     2
#define MM_SIT_IMS_CALL_MANAGE_CONFERENCE 6
/* A conference is addressed to every call, not a particular one. */
#define MM_SIT_IMS_CALL_ID_ALL            0xff

/* CONF_CALL_ADD_REMOVE_USER operations. Dropping one member of a conference
 * goes through mm_sit_ims_hangup(), which picks this over a plain hangup. */
#define MM_SIT_IMS_CONF_CALL_ADD          0
#define MM_SIT_IMS_CONF_CALL_REMOVE       1

void     mm_sit_ims_call_manage        (MMSitIms            *self,
                                        guint8               op,
                                        guint8               call_id,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
gboolean mm_sit_ims_call_manage_finish (MMSitIms            *self,
                                        GAsyncResult        *res,
                                        GError             **error);

gboolean mm_sit_ims_hangup_finish (MMSitIms            *self,
                                   GAsyncResult        *res,
                                   GError             **error);

/*
 * DTMF on an established IMS call.
 *
 * The CP times nothing on the start/stop pair: the key is held down from
 * MM_SIT_IMS_DTMF_START until MM_SIT_IMS_DTMF_STOP names the same digit
 * again.  That is how Android drives its keypad -- the capture has the two
 * about 60 ms apart -- and it is what ModemManager's base call class already
 * sequences, so the tone duration stays the interface's to choose.
 * MM_SIT_IMS_DTMF_BURST is the CP's own self-timed tone, unused here.
 */
#define MM_SIT_IMS_DTMF_BURST 0
#define MM_SIT_IMS_DTMF_START 1
#define MM_SIT_IMS_DTMF_STOP  2

void     mm_sit_ims_send_dtmf        (MMSitIms            *self,
                                      guint8               call_id,
                                      guint8               kind,
                                      gchar                digit,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);
gboolean mm_sit_ims_send_dtmf_finish (MMSitIms            *self,
                                      GAsyncResult        *res,
                                      GError             **error);

/* Stop the stack and drop the PDN; used on radio-off and at dispose. */
void mm_sit_ims_stop (MMSitIms *self);

#endif /* MM_SIT_IMS_H */
