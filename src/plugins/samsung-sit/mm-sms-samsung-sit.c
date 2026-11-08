/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

/* Sending a message over SIT.
 *
 * ModemManager builds the TS 23.040 submit PDU and the CP takes it untouched,
 * so all this does is split the PDU into its service-centre prefix and its
 * TPDU and hand them over, part by part. Which message carries it depends on
 * the transport the CP is using: the AIMS one while IMS is registered, the
 * classic one otherwise.
 *
 * Every part but the last goes out as SEND_SMS_EXPECT_MORE, which is what the
 * capture shows the downstream RIL doing for a concatenated message, and the
 * CP's message reference runs on across all of them.
 *
 * Storing and deleting are not implemented: the CP has no message store of its
 * own, and messages on the SIM live in EF_SMS, which is a file access rather
 * than an SMS operation.
 */

#include <config.h>
#include <string.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-bind.h"
#include "mm-broadband-modem-samsung-sit.h"
#include "mm-iface-modem.h"
#include "mm-log-object.h"
#include "mm-sit-ims.h"
#include "mm-sms-part-3gpp.h"
#include "mm-sms-samsung-sit.h"

typedef struct {
    MMBaseModem *modem;
} MMSmsSamsungSitPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MMSmsSamsungSit, mm_sms_samsung_sit, MM_TYPE_BASE_SMS)

typedef struct {
    MMSitClient *client;
    GList       *parts;
    GList       *current;
    /* Which transport to send on. The CP carries messages over IMS while it is
     * registered there and over the CS group otherwise, and the capture only
     * ever shows each id on its own transport, so the first failure retries on
     * the other one rather than assuming. */
    gboolean     over_ims;
    gboolean     retried;
} SendContext;

/*
 * Whether a failed part is worth another try on the same transport.
 *
 * A timeout says nothing about the transport: the frame never reached the CP,
 * because the FMT ring was holding one the CP had not drained.  Switching
 * transports on that is the wrong answer twice over -- it spends the one retry
 * on a path we had already decided against, and if the ring then clears, the
 * message goes out over CS when IMS was correct.  A refusal from the CP is the
 * opposite: that transport was tried and declined, so the other one is exactly
 * what to reach for.
 */
static gboolean
send_failure_is_transport_blind (const GError *error)
{
    return g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT) ||
           g_error_matches (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED) ||
           g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
           g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY);
}

static void
send_context_free (SendContext *ctx)
{
    g_slice_free (SendContext, ctx);
}

static gboolean
sms_send_finish (MMBaseSms    *self,
                 GAsyncResult *res,
                 GError      **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void send_next_part (GTask *task);

static void
send_part_ready (MMSitClient  *client,
                 GAsyncResult *res,
                 GTask        *task)
{
    MMSmsSamsungSit *self = g_task_get_source_object (task);
    SendContext     *ctx = g_task_get_task_data (task);
    guint32          reference = 0;
    GError          *error = NULL;

    if (!mm_sit_client_send_sms_finish (client, res, &reference, &error)) {
        if (!ctx->retried) {
            gboolean blind = send_failure_is_transport_blind (error);

            /* The CP may not take the transport we guessed from the access
             * technology; try the other one once before giving up -- unless
             * the frame never got to the CP at all, in which case the
             * transport was never the question. */
            mm_obj_warn (self, "SMS send over %s failed (%s); retrying over %s",
                         ctx->over_ims ? "IMS" : "the CS group", error->message,
                         blind ? "the same transport"
                               : (ctx->over_ims ? "the CS group" : "IMS"));
            g_clear_error (&error);
            ctx->retried = TRUE;
            if (!blind)
                ctx->over_ims = !ctx->over_ims;
            send_next_part (task);
            return;
        }
        g_prefix_error (&error, "couldn't send SMS part: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "SMS part accepted, message reference %u", reference);
    mm_sms_part_set_message_reference ((MMSmsPart *) ctx->current->data, reference);

    ctx->current = g_list_next (ctx->current);
    send_next_part (task);
}

static void
send_next_part (GTask *task)
{
    MMSmsSamsungSit    *self = g_task_get_source_object (task);
    SendContext        *ctx = g_task_get_task_data (task);
    g_autofree guint8  *pdu = NULL;
    g_autoptr(GError)   error = NULL;
    guint               pdu_len = 0;
    guint               tpdu_at = 0;

    if (!ctx->current) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    pdu = mm_sms_part_3gpp_get_submit_pdu ((MMSmsPart *) ctx->current->data,
                                           &pdu_len, &tpdu_at, self, &error);
    if (!pdu) {
        g_task_return_error (task, error ? g_steal_pointer (&error) :
                             g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                                  "couldn't build the submit PDU"));
        g_object_unref (task);
        return;
    }
    if (tpdu_at >= pdu_len) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "the submit PDU carries no TPDU");
        g_object_unref (task);
        return;
    }

    /* The PDU's own service-centre prefix is a bare zero length byte unless the
     * message names one; in that case the client falls back to the SMSC the CP
     * reported, which is what the downstream RIL sends on every message. */
    mm_sit_client_send_sms (ctx->client,
                            (tpdu_at > 1) ? pdu : NULL,
                            (tpdu_at > 1) ? tpdu_at : 0,
                            pdu + tpdu_at,
                            pdu_len - tpdu_at,
                            g_list_next (ctx->current) != NULL,
                            ctx->over_ims,
                            NULL,
                            (GAsyncReadyCallback) send_part_ready,
                            task);
}

/* No cancellable in this vfunc: the core's send has none. */
static void
sms_send (MMBaseSms          *self,
          GAsyncReadyCallback callback,
          gpointer            user_data)
{
    MMSmsSamsungSitPrivate *priv;
    SendContext            *ctx;
    MMSitClient            *client;
    GError                 *error = NULL;
    GTask                  *task;

    priv = mm_sms_samsung_sit_get_instance_private (MM_SMS_SAMSUNG_SIT (self));
    task = g_task_new (self, NULL, callback, user_data);

    if (!priv->modem || !MM_IS_BROADBAND_MODEM_SAMSUNG_SIT (priv->modem)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "the SMS is not owned by a Samsung SIT modem");
        g_object_unref (task);
        return;
    }
    client = mm_broadband_modem_samsung_sit_peek_client (MM_BROADBAND_MODEM_SAMSUNG_SIT (priv->modem),
                                                         &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx = g_slice_new0 (SendContext);
    ctx->client = client;
    /*
     * Pick the transport on whether IMS is registered, which is what actually
     * decides whether the IMS group can carry a message -- and is how the
     * voice interface chooses, so the two now agree.
     *
     * This used to infer it from the access technology on the modem's D-Bus
     * skeleton. That is a cached value, and when the control plane is slow the
     * polls behind it time out and it reads as no technologies at all, which
     * silently means "not LTE" and sends over the CS group. Caught doing
     * exactly that on a registered LTE modem: the CS send was accepted 19
     * seconds later, long after the messaging app had shown it as failed.
     */
    {
        MMSitIms *ims = mm_broadband_modem_samsung_sit_peek_ims (MM_BROADBAND_MODEM_SAMSUNG_SIT (priv->modem));

        ctx->over_ims = ims && mm_sit_ims_is_registered (ims);
        mm_obj_dbg (self, "sending over %s", ctx->over_ims ? "IMS" : "the CS group");
    }
    ctx->parts = mm_base_sms_get_parts (self);
    ctx->current = ctx->parts;
    g_task_set_task_data (task, ctx, (GDestroyNotify) send_context_free);

    send_next_part (task);
}

/*****************************************************************************/

static gboolean
sms_delete_finish (MMBaseSms     *self,
                   GAsyncResult  *res,
                   GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

/*
 * Nothing is stored on the modem to delete.
 *
 * The CP hands a received message over as an indication, which is answered
 * with an SMS_ACK and then forgotten -- parts are built with
 * SMS_PART_INVALID_INDEX because there is no storage index to build them
 * with, and the modem keeps no copy. The message exists only in our own list,
 * which the core drops once this returns.
 *
 * Without this the core answers every delete with "Deleting SMS is not
 * supported by this modem", so a received message cannot be dismissed and the
 * attempt is logged twice per message for as long as it is in the list.
 */
static void
sms_delete (MMBaseSms           *self,
            GAsyncReadyCallback  callback,
            gpointer             user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/*****************************************************************************/

MMBaseSms *
mm_sms_samsung_sit_new (MMBaseModem *modem)
{
    MMSmsSamsungSitPrivate *priv;
    MMBaseSms              *sms;

    sms = MM_BASE_SMS (g_object_new (MM_TYPE_SMS_SAMSUNG_SIT,
                                     MM_BIND_TO, G_OBJECT (modem),
                                     MM_BASE_SMS_IS_3GPP, TRUE,
                                     MM_BASE_SMS_DEFAULT_STORAGE, MM_SMS_STORAGE_ME,
                                     NULL));
    priv = mm_sms_samsung_sit_get_instance_private (MM_SMS_SAMSUNG_SIT (sms));
    priv->modem = g_object_ref (modem);
    return sms;
}

static void
dispose (GObject *object)
{
    MMSmsSamsungSitPrivate *priv;

    priv = mm_sms_samsung_sit_get_instance_private (MM_SMS_SAMSUNG_SIT (object));
    g_clear_object (&priv->modem);
    G_OBJECT_CLASS (mm_sms_samsung_sit_parent_class)->dispose (object);
}

static void
mm_sms_samsung_sit_init (MMSmsSamsungSit *self)
{
}

static void
mm_sms_samsung_sit_class_init (MMSmsSamsungSitClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    MMBaseSmsClass *base_sms_class = MM_BASE_SMS_CLASS (klass);

    object_class->dispose = dispose;
    base_sms_class->send = sms_send;
    base_sms_class->send_finish = sms_send_finish;
    base_sms_class->delete = sms_delete;
    base_sms_class->delete_finish = sms_delete_finish;
}
