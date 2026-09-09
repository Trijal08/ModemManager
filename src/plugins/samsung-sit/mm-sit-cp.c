/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-sit-cp.h"

struct _MMSitCp {
    GObject parent;
    gchar *physdev;
    MMSitClient *clients[2];
    GPtrArray *waiters;
    GHashTable *data_cids; /* CID -> 1 pending, 2 connected */
    gboolean ready;
    gboolean mapping_sent;
    gboolean secondary_off;
    guint attempts;
    MMSitLogicalSlotMapping mapping[2];
};

G_DEFINE_TYPE (MMSitCp, mm_sit_cp, G_TYPE_OBJECT)
static GHashTable *instances; /* weak; all access is on MM's main context */

static void
finalize (GObject *object)
{
    MMSitCp *self = MM_SIT_CP (object);

    if (self->physdev)
        g_hash_table_remove (instances, self->physdev);
    g_clear_object (&self->clients[0]);
    g_clear_object (&self->clients[1]);
    g_clear_pointer (&self->waiters, g_ptr_array_unref);
    g_hash_table_unref (self->data_cids);
    g_free (self->physdev);
    G_OBJECT_CLASS (mm_sit_cp_parent_class)->finalize (object);
}

static void
mm_sit_cp_class_init (MMSitCpClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = finalize;
}

static void
mm_sit_cp_init (MMSitCp *self)
{
    self->data_cids = g_hash_table_new (g_direct_hash, g_direct_equal);
}

MMSitCp *
mm_sit_cp_new (MMSitClient *first, MMSitClient *second)
{
    MMSitCp *self = g_object_new (MM_TYPE_SIT_CP, NULL);

    self->clients[0] = g_object_ref (first);
    self->clients[1] = g_object_ref (second);
    mm_sit_client_set_stack (first, self, 0);
    mm_sit_client_set_stack (second, self, 1);
    return self;
}

MMSitCp *
mm_sit_cp_get (const gchar *physdev, const gchar *device,
               gboolean use_proxy, GError **error)
{
    g_autoptr(MMSitClient) first = NULL;
    g_autoptr(MMSitClient) second = NULL;
    MMSitClient **clients[2] = { &first, &second };
    g_autofree gchar *prefix = NULL;
    const gchar *suffix = g_strrstr (device, "sit");
    MMSitCp *self;
    guint i;

    if (!physdev || !suffix ||
        (g_strcmp0 (suffix, "sit0") && g_strcmp0 (suffix, "sit1"))) {
        g_set_error_literal (error, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
                             "DSDS requires a physical CP path and two numbered SIT ports");
        return NULL;
    }
    if (!instances)
        instances = g_hash_table_new (g_str_hash, g_str_equal);
    self = g_hash_table_lookup (instances, physdev);
    if (self)
        return g_object_ref (self);

    prefix = g_strndup (device, suffix - device);
    for (i = 0; i < 2; i++) {
        g_autofree gchar *name = g_strdup_printf ("%ssit%u", prefix, i);

        *clients[i] = use_proxy ? mm_sit_client_new_proxy (name) : mm_sit_client_new (name);
        if (!mm_sit_client_open (*clients[i], error))
            return NULL;
    }
    self = mm_sit_cp_new (first, second);
    self->physdev = g_strdup (physdev);
    g_hash_table_insert (instances, self->physdev, self);
    return self;
}

MMSitClient *
mm_sit_cp_peek_client (MMSitCp *self, guint stack)
{
    g_return_val_if_fail (stack < 2, NULL);
    return self->clients[stack];
}

static void
prepare_complete (MMSitCp *self, const GError *error)
{
    g_autoptr(GPtrArray) waiters = g_steal_pointer (&self->waiters);
    guint i;

    self->ready = !error;
    for (i = 0; i < waiters->len; i++) {
        GTask *task = g_ptr_array_index (waiters, i);

        if (error)
            g_task_return_error (task, g_error_copy (error));
        else
            g_task_return_boolean (task, TRUE);
    }
}

static gboolean query_slots (MMSitCp *self);
static void second_radio_off (MMSitClient *client, GAsyncResult *res, MMSitCp *self);

static void
mapping_ready (MMSitClient *client, GAsyncResult *res, MMSitCp *self)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MMSitMessage) response = mm_sit_client_command_finish (client, res, &error);

    if (!response || !response->has_status) {
        if (!error)
            error = g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                         "CP refused the initial DSDS slot mapping");
        prepare_complete (self, error);
        return;
    }
    /* Changing the live-modem count can apply the same mapping asynchronously.
     * Even a rejected setter must be followed by an inventory check: hardware
     * returned status 7 while the requested two-stack assignment appeared. */
    g_timeout_add_seconds (1, (GSourceFunc) query_slots, self);
}

static void
slots_ready (MMSitClient *client, GAsyncResult *res, MMSitCp *self)
{
    MMSitSimSlotStatus status;
    gboolean found[2] = { FALSE, FALSE };
    g_autoptr(GError) error = NULL;
    guint i, j;

    if (!mm_sit_client_get_sim_slot_status_finish (client, res, &status, &error)) {
        prepare_complete (self, error);
        return;
    }
    for (i = 0; i < status.slot_count; i++) {
        for (j = 0; j < status.slots[i].port_count; j++) {
            const MMSitSimPort *port = &status.slots[i].ports[j];
            guint stack = port->logical_slot_id;

            /* A mapped stack exists even with an empty tray or no enabled
             * eSIM profile. Subscription readiness belongs to its MM modem. */
            if (stack < 2 && port->state == 1) {
                found[stack] = TRUE;
                if (!self->mapping_sent) {
                    self->mapping[stack].physical_slot_id = i;
                    self->mapping[stack].port_id = j;
                } else if (self->mapping[stack].physical_slot_id != i ||
                           self->mapping[stack].port_id != j)
                    found[stack] = FALSE;
            }
        }
    }
    if (found[1] && !self->secondary_off) {
        mm_sit_client_set_radio_state (self->clients[1], MM_SIT_RADIO_STATE_OFF, NULL,
                                       (GAsyncReadyCallback) second_radio_off, self);
        return;
    }
    if (found[0] && found[1]) {
        prepare_complete (self, NULL);
        return;
    }

    /* Keep any existing assignment. Only add the missing logical stack, once,
     * while both radios are off and before either MM modem initializes its SIM.
     * Ordinary data handover never calls this mapping setter. */
    if (!self->mapping_sent && self->attempts >= 5 && found[0] != found[1]) {
        guint missing = found[0] ? 1 : 0;
        guint present = 1 - missing;

        for (i = 0; i < status.slot_count; i++) {
            if (i == self->mapping[present].physical_slot_id)
                continue;
            for (j = 0; j < status.slots[i].port_count; j++) {
                g_autoptr(GByteArray) request = NULL;

                self->mapping[missing].physical_slot_id = i;
                self->mapping[missing].port_id = j;
                request = mm_sit_request_new_set_logical_slot_mapping (
                    mm_sit_client_next_token (client), self->mapping, 2, &error);
                if (!request) {
                    prepare_complete (self, error);
                    return;
                }
                self->mapping_sent = TRUE;
                mm_sit_client_command (client, request, 30, NULL,
                                       (GAsyncReadyCallback) mapping_ready, self);
                return;
            }
        }
    }
    if (++self->attempts < 20) {
        g_timeout_add_seconds (1, (GSourceFunc) query_slots, self);
        return;
    }
    error = g_error_new_literal (MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "DSDS requires two mapped logical SIM ports");
    prepare_complete (self, error);
}

static gboolean
query_slots (MMSitCp *self)
{
    mm_sit_client_get_sim_slot_status (self->clients[0], NULL,
                                      (GAsyncReadyCallback) slots_ready, self);
    return G_SOURCE_REMOVE;
}

static void
second_radio_off (MMSitClient *client, GAsyncResult *res, MMSitCp *self)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_set_radio_state_finish (client, res, &error))
        prepare_complete (self, error);
    else {
        self->secondary_off = TRUE;
        query_slots (self);
    }
}

static void
first_radio_off (MMSitClient *client, GAsyncResult *res, MMSitCp *self)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_set_radio_state_finish (client, res, &error)) {
        prepare_complete (self, error);
        return;
    }
    query_slots (self);
}

static void
boot_ready (MMSitClient *client, GAsyncResult *res, MMSitCp *self)
{
    g_autoptr(GError) error = NULL;

    if (!mm_sit_client_send_boot_config_finish (client, res, &error)) {
        prepare_complete (self, error);
        return;
    }
    /* Starting the experiment deliberately drops contexts inherited from a
     * previous MM process. They cannot be trusted by a fresh ownership table. */
    mm_sit_client_set_radio_state (self->clients[0], MM_SIT_RADIO_STATE_OFF, NULL,
                                   (GAsyncReadyCallback) first_radio_off, self);
}

void
mm_sit_cp_prepare (MMSitCp *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);

    if (self->ready) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    if (self->waiters) {
        g_ptr_array_add (self->waiters, task);
        return;
    }
    self->waiters = g_ptr_array_new_with_free_func (g_object_unref);
    g_ptr_array_add (self->waiters, task);
    self->attempts = 0;
    self->mapping_sent = FALSE;
    self->secondary_off = FALSE;
    mm_sit_client_send_boot_config (self->clients[0], NULL,
                                    (GAsyncReadyCallback) boot_ready, self);
}

gboolean
mm_sit_cp_prepare_finish (MMSitCp *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

gboolean
mm_sit_cp_claim_data (MMSitCp *self, guint stack, guint8 cid, GError **error)
{
    GHashTableIter iter;
    gpointer key;

    g_return_val_if_fail (stack < 2 && cid >= 2 + stack * 15 && cid <= 15 + stack * 15, FALSE);
    g_hash_table_iter_init (&iter, self->data_cids);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        guint owner = (GPOINTER_TO_UINT (key) - 1) / 15;

        if (owner != stack) {
            g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
                         "SIT stack %u owns mobile data; disconnect it before connecting stack %u",
                         owner, stack);
            return FALSE;
        }
    }
    if (mm_sit_cp_data_reserved (self, cid)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
                     "SIT context %u is already connected or awaiting setup/cleanup", cid);
        return FALSE;
    }
    g_hash_table_insert (self->data_cids, GUINT_TO_POINTER ((guint) cid), GUINT_TO_POINTER (1));
    return TRUE;
}

gboolean
mm_sit_cp_data_reserved (MMSitCp *self, guint8 cid)
{
    return g_hash_table_contains (self->data_cids, GUINT_TO_POINTER ((guint) cid));
}

void
mm_sit_cp_commit_data (MMSitCp *self, guint8 cid)
{
    g_hash_table_insert (self->data_cids, GUINT_TO_POINTER ((guint) cid), GUINT_TO_POINTER (2));
}

void
mm_sit_cp_release_data (MMSitCp *self, guint8 cid)
{
    g_hash_table_remove (self->data_cids, GUINT_TO_POINTER ((guint) cid));
}

void
mm_sit_cp_release_stack_data (MMSitCp *self, guint stack)
{
    guint cid;

    for (cid = 2 + 15 * stack; cid <= 15 + 15 * stack; cid++)
        mm_sit_cp_release_data (self, cid);
}

void
mm_sit_cp_update_data_calls (MMSitCp *self, guint stack, GArray *calls)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, self->data_cids);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        guint cid = GPOINTER_TO_UINT (key);
        gboolean present = FALSE;
        guint i;

        if ((cid - 1) / 15 != stack || GPOINTER_TO_UINT (value) != 2)
            continue;
        for (i = 0; i < calls->len; i++) {
            MMSitDataCall *call = &g_array_index (calls, MMSitDataCall, i);

            if (call->cid == cid && call->active)
                present = TRUE;
        }
        if (!present)
            g_hash_table_iter_remove (&iter);
    }
}
