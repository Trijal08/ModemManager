/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Copyright (C) 2026 Steffen Deusch
 */

#include <stdlib.h>
#include <string.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem.h"
#include "mm-bind.h"
#include "mm-iface-modem-voice.h"
#include "mm-broadband-modem-samsung-sit.h"
#include "mm-call-samsung-sit.h"
#include "mm-log-object.h"
#include "mm-sit-client.h"
#include "mm-sit-ims.h"

G_DEFINE_TYPE (MMCallSamsungSit, mm_call_samsung_sit, MM_TYPE_BASE_CALL)

struct _MMCallSamsungSitPrivate {
    MMBaseModem *modem;
    /* The key currently held down, so the matching release names the same
     * digit.  0 when no key is down. */
    gchar        dtmf_pressed;
};

/*****************************************************************************/

/* The CS call group is plain native SIT: DIAL/ANSWER/HANGUP are typed
 * requests on the same control port as everything else, so all three
 * operations are one client call apiece.  Progress is *not* in the response --
 * the CP pokes CALL_STATE_CHANGED and the modem re-polls GET_CALL_LIST -- so
 * these only report that the request itself was accepted. */

/*
 * Whether this call goes over IMS.
 *
 * Decided per call rather than once, and from the CP's own registration state
 * rather than from the RAT: the two stacks are separate protocols with separate
 * requests, and which one can carry a call is exactly what the CP last told us.
 * A call already under way keeps the answer it started with, since its id
 * belongs to that stack.
 */
static gboolean
call_is_ims (MMCallSamsungSit *self)
{
    MMSitIms *ims;

    if (!self->priv->modem)
        return FALSE;
    ims = mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem));
    return ims && mm_sit_ims_is_registered (ims);
}

static MMSitClient *
peek_client (MMCallSamsungSit  *self,
             GError           **error)
{
    MMSitClient *client;

    if (!self->priv->modem) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Call is not bound to a modem");
        return NULL;
    }

    client = mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem),
                                                         error);
    return client;
}

/*****************************************************************************/
/* Start the call */

static gboolean
call_start_finish (MMBaseCall    *self,
                   GAsyncResult  *res,
                   GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
dial_ready (MMSitClient  *client,
            GAsyncResult *res,
            GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_dial_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
ims_dial_ready (MMSitIms     *ims,
                GAsyncResult *res,
                GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_ims_dial_finish (ims, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* Answer and hang up report only that the CP took the request; the call's own
 * state arrives as a CALL_STATUS indication, exactly as on the CS side. */
static void
ims_simple_ready (MMSitIms     *ims,
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
call_start (MMBaseCall          *_self,
            GCancellable        *cancellable,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
    MMCallSamsungSit *self = MM_CALL_SAMSUNG_SIT (_self);
    MMSitClient      *client;
    GTask            *task;
    GError           *error = NULL;

    task = g_task_new (self, cancellable, callback, user_data);

    client = peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* No call index is assigned here: unlike QMI's DialCall, the SIT DIAL
     * response carries no id.  The index arrives with the first
     * GET_CALL_LIST poll, which the voice interface matches to this object
     * by number. */
    if (call_is_ims (self)) {
        mm_obj_dbg (self, "starting IMS call");
        {
            MMSitIms *ims = mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem));

            mm_sit_ims_dial (ims,
                             mm_gdbus_call_get_number (MM_GDBUS_CALL (self)),
                             cancellable,
                             (GAsyncReadyCallback) ims_dial_ready,
                             task);
            /*
             * Android's order after the dial: the frame time, then the setup
             * media state.  Both go out behind the dial request because the
             * SIT port serialises commands.
             */
            mm_sit_ims_set_frame_time (ims);
            mm_sit_ims_media_state (ims, MM_SIT_IMS_MEDIA_SETUP);
        }
        return;
    }

    mm_obj_dbg (self, "starting CS call");
    mm_sit_client_dial (client,
                        mm_gdbus_call_get_number (MM_GDBUS_CALL (self)),
                        cancellable,
                        (GAsyncReadyCallback) dial_ready,
                        task);
}

/*****************************************************************************/
/* Accept the call */

static gboolean
call_accept_finish (MMBaseCall    *self,
                    GAsyncResult  *res,
                    GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
answer_ready (MMSitClient  *client,
              GAsyncResult *res,
              GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_answer_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
call_accept (MMBaseCall          *_self,
             GAsyncReadyCallback  callback,
             gpointer             user_data)
{
    MMCallSamsungSit *self = MM_CALL_SAMSUNG_SIT (_self);
    MMSitClient      *client;
    GTask            *task;
    GError           *error = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    client = peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (call_is_ims (self)) {
        /* AIMS ANSWER names the media it accepts but not the call: like CS, it
         * takes the one that is ringing. */
        mm_obj_dbg (self, "accepting IMS call");
        mm_sit_ims_answer (mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem)),
                           (GAsyncReadyCallback) ims_simple_ready, task);
        return;
    }

    /* ANSWER is header-only: it accepts *the* incoming call, with no id, so
     * there is nothing to select here. */
    mm_obj_dbg (self, "accepting CS call");
    mm_sit_client_answer (client, NULL, (GAsyncReadyCallback) answer_ready, task);
}

/*
 * The CP's id for this call.  ModemManager indices are the CP's ids shifted by
 * one (0 means "no index assigned" there, but is a valid id here), so shift
 * back.  A call dialled but not yet seen in a call list still has index 0;
 * fall back to id 0, the first call, and let the CP reject it if that is
 * wrong.
 */
static guint8
call_cp_id (MMBaseCall *self)
{
    guint index = mm_base_call_get_index (self);

    return (guint8) (index ? index - 1 : 0);
}

/*****************************************************************************/
/* Hang up the call */

static gboolean
call_hangup_finish (MMBaseCall    *self,
                    GAsyncResult  *res,
                    GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
hangup_ready (MMSitClient  *client,
              GAsyncResult *res,
              GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_hangup_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
call_hangup (MMBaseCall          *_self,
             GAsyncReadyCallback  callback,
             gpointer             user_data)
{
    MMCallSamsungSit *self = MM_CALL_SAMSUNG_SIT (_self);
    MMSitClient      *client;
    GTask            *task;
    GError           *error = NULL;
    guint             index;
    guint             call_id;

    task = g_task_new (self, NULL, callback, user_data);

    client = peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    index = mm_base_call_get_index (_self);
    call_id = call_cp_id (_self);

    if (call_is_ims (self)) {
        /* A call that was never answered is declined rather than hung up, and
         * the CP is told which of the two it was. */
        gboolean declined = (mm_base_call_get_state (_self) == MM_CALL_STATE_RINGING_IN);

        mm_obj_dbg (self, "%s IMS call %u (MM index %u)",
                    declined ? "declining" : "hanging up", call_id, index);
        mm_sit_ims_hangup (mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem)),
                           (guint8) call_id, declined,
                           (GAsyncReadyCallback) ims_simple_ready, task);
        return;
    }

    mm_obj_dbg (self, "hanging up CS call %u (MM index %u)", call_id, index);
    mm_sit_client_hangup (client, call_id, NULL,
                          (GAsyncReadyCallback) hangup_ready, task);
}

/*****************************************************************************/
/* DTMF */

/*
 * The two stacks spell a key press differently, so a call is served by the one
 * it started on.
 *
 * AIMS carries it as two requests -- down, then up -- naming the same digit,
 * and holds the tone in between.  That is the shape ModemManager's base call
 * class already drives: send_dtmf, wait the call's configured tone duration,
 * stop_dtmf, and it sequences a multi-character string itself.
 *
 * CS has no such pair.  Its DTMF (0x0003) is a self-timed burst: the CP is
 * handed the digits and times each tone itself, so there is nothing to release
 * and stop_dtmf has nothing to do.  The group does define DTMF_START (0x0005)
 * and DTMF_STOP (0x0006), but the vendor builder for START writes a boolean
 * nothing in any available library sets, so what it selects is unknown and
 * using it would put an undefined byte on the wire.
 *
 * Either way one call here is one key, so these return 1 and let the base class
 * advance the string.
 */

static gssize
call_send_dtmf_finish (MMBaseCall    *self,
                       GAsyncResult  *res,
                       GError       **error)
{
    return g_task_propagate_int (G_TASK (res), error);
}

static void
ims_send_dtmf_ready (MMSitIms     *ims,
                     GAsyncResult *res,
                     GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_ims_send_dtmf_finish (ims, res, &error))
        g_task_return_error (task, error);
    else
        /* One key per request; the base class advances the string by this. */
        g_task_return_int (task, 1);
    g_object_unref (task);
}

static void
cs_send_dtmf_ready (MMSitClient  *client,
                    GAsyncResult *res,
                    GTask        *task)
{
    GError *error = NULL;

    if (!mm_sit_client_send_dtmf_finish (client, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_int (task, 1);
    g_object_unref (task);
}

static void
call_send_dtmf (MMBaseCall          *_self,
                const gchar         *dtmf,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    MMCallSamsungSit *self = MM_CALL_SAMSUNG_SIT (_self);
    MMSitClient      *client;
    GTask            *task;
    GError           *error = NULL;
    gchar             key[2];

    task = g_task_new (self, NULL, callback, user_data);

    if (!dtmf || !dtmf[0]) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                                 "No DTMF tone to send");
        g_object_unref (task);
        return;
    }

    if (call_is_ims (self)) {
        MMSitIms *ims = mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem));

        if (!ims) {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                     "No IMS stack to send DTMF on");
            g_object_unref (task);
            return;
        }

        /* Remember the key so the release names the same digit. */
        self->priv->dtmf_pressed = dtmf[0];
        mm_obj_dbg (self, "pressing DTMF key '%c' on IMS call %u",
                    dtmf[0], call_cp_id (_self));
        mm_sit_ims_send_dtmf (ims, call_cp_id (_self), MM_SIT_IMS_DTMF_START, dtmf[0],
                              (GAsyncReadyCallback) ims_send_dtmf_ready, task);
        return;
    }

    client = peek_client (self, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* A burst needs no release, so leave dtmf_pressed clear. */
    self->priv->dtmf_pressed = 0;
    key[0] = dtmf[0];
    key[1] = '\0';
    mm_obj_dbg (self, "sending DTMF key '%c' on CS call %u", key[0], call_cp_id (_self));
    mm_sit_client_send_dtmf (client, key, NULL,
                             (GAsyncReadyCallback) cs_send_dtmf_ready, task);
}

static gboolean
call_stop_dtmf_finish (MMBaseCall    *self,
                       GAsyncResult  *res,
                       GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
call_stop_dtmf (MMBaseCall          *_self,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    MMCallSamsungSit *self = MM_CALL_SAMSUNG_SIT (_self);
    MMSitIms         *ims;
    GTask            *task;
    gchar             digit;

    task = g_task_new (self, NULL, callback, user_data);

    /* Nothing is being held: either the key was never pressed, or this is a CS
     * burst the CP has already timed for us. */
    digit = self->priv->dtmf_pressed;
    if (!digit) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    self->priv->dtmf_pressed = 0;

    ims = self->priv->modem
        ? mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem))
        : NULL;
    if (!ims) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "No IMS stack to release DTMF on");
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "releasing DTMF key '%c' on IMS call %u",
                digit, call_cp_id (_self));
    mm_sit_ims_send_dtmf (ims, call_cp_id (_self), MM_SIT_IMS_DTMF_STOP, digit,
                          (GAsyncReadyCallback) ims_simple_ready, task);
}

/*****************************************************************************/

MMBaseCall *
mm_call_samsung_sit_new (MMBaseModem     *modem,
                         MMCallDirection  direction,
                         const gchar     *number,
                         const guint      dtmf_tone_duration)
{
    MMBaseCall *call;

    call = MM_BASE_CALL (g_object_new (MM_TYPE_CALL_SAMSUNG_SIT,
                                       MM_BASE_CALL_IFACE_MODEM_VOICE,           modem,
                                       MM_BIND_TO,                               modem,
                                       MM_CALL_DIRECTION,                        direction,
                                       MM_CALL_NUMBER,                           number,
                                       MM_CALL_DTMF_TONE_DURATION,               dtmf_tone_duration,
                                       /* The CP reports incoming calls through
                                        * the call list, and drives both
                                        * transitions itself. */
                                       MM_BASE_CALL_SKIP_INCOMING_TIMEOUT,       TRUE,
                                       MM_BASE_CALL_SUPPORTS_DIALING_TO_RINGING, TRUE,
                                       MM_BASE_CALL_SUPPORTS_RINGING_TO_ACTIVE,  TRUE,
                                       NULL));
    MM_CALL_SAMSUNG_SIT (call)->priv->modem = g_object_ref (modem);
    return call;
}

/*
 * Tell the CP what the call's media is doing, at every state change.
 *
 * Android raises AIMS_MEDIA_STATE_IND throughout a call and the CP answers
 * nothing; we never sent it at all.  Without it the CP publishes no CallInfo
 * block, and the AoC -- which learns the call's codec, session and sample
 * rate from that block and from nowhere else -- sits logging "Unknown codec
 * type (0xffffffff)" for the whole call with the downlink silent.
 *
 * CS calls are left alone: this is an IMS message and the CP has no IMS
 * session to describe for them.
 */
static void
notify_media_state (MMCallSamsungSit *self)
{
    MMSitIms   *ims;
    MMCallState state;
    guint8      media;

    if (!self->priv->modem || !call_is_ims (self))
        return;

    state = mm_gdbus_call_get_state (MM_GDBUS_CALL (self));
    switch (state) {
    case MM_CALL_STATE_DIALING:
        /*
         * Not from here: ModemManager marks the call dialing before the DIAL
         * request goes out, so announcing it here describes a call's media to
         * the CP before the call exists.  Android's order is DIAL, then the
         * frame time, then the setup state -- so call_dial() sends it.
         */
        return;
    case MM_CALL_STATE_RINGING_OUT:
    case MM_CALL_STATE_RINGING_IN:
        media = MM_SIT_IMS_MEDIA_ALERTING;
        break;
    case MM_CALL_STATE_ACTIVE:
        media = MM_SIT_IMS_MEDIA_CONNECTED;
        break;
    case MM_CALL_STATE_TERMINATED:
        media = MM_SIT_IMS_MEDIA_ENDED;
        break;
    default:
        /* Nothing observed on the wire for the other states. */
        return;
    }

    ims = mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (self->priv->modem));
    if (!ims)
        return;

    mm_sit_ims_media_state (ims, media);

    /* Android gives the CP its RTP thresholds just after the connected state. */
    if (media == MM_SIT_IMS_MEDIA_CONNECTED)
        mm_sit_ims_set_rtp_threshold (ims);
}

static void
state_changed (MMCallSamsungSit *self,
               GParamSpec       *pspec,
               gpointer          user_data)
{
    notify_media_state (self);
}

static void
mm_call_samsung_sit_init (MMCallSamsungSit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_CALL_SAMSUNG_SIT, MMCallSamsungSitPrivate);
    g_signal_connect (self, "notify::state", G_CALLBACK (state_changed), NULL);
}

static void
dispose (GObject *object)
{
    MMCallSamsungSit *self = MM_CALL_SAMSUNG_SIT (object);

    g_clear_object (&self->priv->modem);

    G_OBJECT_CLASS (mm_call_samsung_sit_parent_class)->dispose (object);
}

static void
mm_call_samsung_sit_class_init (MMCallSamsungSitClass *klass)
{
    GObjectClass    *object_class = G_OBJECT_CLASS (klass);
    MMBaseCallClass *base_call_class = MM_BASE_CALL_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMCallSamsungSitPrivate));

    object_class->dispose = dispose;

    base_call_class->start         = call_start;
    base_call_class->start_finish  = call_start_finish;
    base_call_class->accept        = call_accept;
    base_call_class->accept_finish = call_accept_finish;
    base_call_class->hangup        = call_hangup;
    base_call_class->hangup_finish = call_hangup_finish;
    base_call_class->send_dtmf        = call_send_dtmf;
    base_call_class->send_dtmf_finish = call_send_dtmf_finish;
    base_call_class->stop_dtmf        = call_stop_dtmf;
    base_call_class->stop_dtmf_finish = call_stop_dtmf_finish;
}
