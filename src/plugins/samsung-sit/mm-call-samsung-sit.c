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

G_DEFINE_TYPE (MMCallSamsungSit, mm_call_samsung_sit, MM_TYPE_BASE_CALL)

struct _MMCallSamsungSitPrivate {
    MMBaseModem *modem;
};

/*****************************************************************************/

/* The CS call group is plain native SIT: DIAL/ANSWER/HANGUP are typed
 * requests on the same control port as everything else, so all three
 * operations are one client call apiece.  Progress is *not* in the response --
 * the CP pokes CALL_STATE_CHANGED and the modem re-polls GET_CALL_LIST -- so
 * these only report that the request itself was accepted. */

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
    mm_obj_dbg (self, "starting call");
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

    /* ANSWER is header-only: it accepts *the* incoming call, with no id, so
     * there is nothing to select here. */
    mm_obj_dbg (self, "accepting call");
    mm_sit_client_answer (client, NULL, (GAsyncReadyCallback) answer_ready, task);
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

    /* HANGUP takes the CP's call id.  ModemManager indices are the CP's ids
     * shifted by one (0 means "no index assigned" there, but is a valid id
     * here), so shift back.  A call dialled but not yet seen in a call list
     * still has index 0; fall back to id 0, the first call, and let the CP
     * reject it if that is wrong. */
    index = mm_base_call_get_index (_self);
    call_id = index ? index - 1 : 0;

    mm_obj_dbg (self, "hanging up call %u (MM index %u)", call_id, index);
    mm_sit_client_hangup (client, call_id, NULL,
                          (GAsyncReadyCallback) hangup_ready, task);
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

static void
mm_call_samsung_sit_init (MMCallSamsungSit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_CALL_SAMSUNG_SIT, MMCallSamsungSitPrivate);
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
    /* DTMF is left unimplemented: SIT has the messages but their payloads
     * are not decoded yet, and the base class reports "unsupported" for a
     * NULL vfunc. */
}
