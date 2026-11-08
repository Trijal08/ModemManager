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
    gint data_pref;        /* stack last named to the CP, -1 if never */
    gboolean secondary_off;
    gboolean primary_off;
    /*
     * The slot inventory this object already polled, shared with the modems.
     * Stock reads it once per boot; we read it three times inside 40 ms -- once
     * here and once per modem from load_sim_slots(). prepare() polls until both
     * logical stacks are mapped, so it holds the authoritative answer by the
     * time either modem gets that far. Dropped on SIM_SLOT_STATUS_CHANGED.
     */
    gboolean slot_status_valid;
    MMSitSimSlotStatus slot_status;
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
    self->data_pref = -1;
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

/*
 * Name the stack that owns mobile data to the CP (SET_PREFERRED_DATA_MODEM).
 *
 * This has to be in place *before* anything asks for a PDN. The stock stack
 * sends it twice during boot and re-asserts it seconds before it sets up its
 * IMS context; we only ever sent it as the first step of a data bearer
 * connect, which on a captured boot landed twelve seconds *after* the IMS
 * SETUP_DATA_CALL had already been answered. That answer came back cause=0
 * and empty -- no address, no P-CSCF -- where the stock stack's carries both
 * on a byte-identical request, so the context was created without the CP
 * having been told which stack the data subscription belongs to.
 *
 * Called before a PDN is asked for, so the CP is never asked to create a
 * context without knowing who owns data. That is what the stock stack does --
 * it re-asserts the preference three seconds before its own IMS context --
 * and is why this is not done once at coordinator-ready time, where there is
 * no live port to send on. A bearer connect names it separately as its own
 * first step, so this is the IMS path's counterpart to that.
 *
 * Sent on stack 0's client because that is where the capture shows it, and
 * the payload names the stack rather than the port it arrives on. Only on a
 * change: the CP is told once and then left alone.
 *
 * Fire and forget. If the CP refuses there is nothing useful to do about it
 * here, and the caller is in the middle of something else.
 */
gboolean
mm_sit_cp_peek_sim_slot_status (MMSitCp *self, MMSitSimSlotStatus *out)
{
    if (!self || !self->slot_status_valid)
        return FALSE;
    *out = self->slot_status;
    return TRUE;
}

void
mm_sit_cp_forget_sim_slot_status (MMSitCp *self)
{
    if (self)
        self->slot_status_valid = FALSE;
}

void
mm_sit_cp_prefer_data_stack (MMSitCp *self, guint stack)
{
    g_autoptr(GByteArray) request = NULL;

    if (stack >= 2 || !self->clients[0] || self->data_pref == (gint) stack)
        return;

    request = mm_sit_request_new_set_preferred_modem (
        mm_sit_client_next_token (self->clients[0]), stack);
    if (!request)
        return;
    self->data_pref = (gint) stack;
    mm_sit_client_command (self->clients[0], request, 10, NULL, NULL, NULL);
}

/*
 * A bearer connect names the preference as its own first step, on its own
 * client, without coming through here. Record it so the check above is reading
 * what the CP was actually told: left stale, the next data context would see a
 * preference it disagreed with and send a redundant switch, which is the flap
 * this is all here to avoid.
 */
void
mm_sit_cp_note_data_stack (MMSitCp *self, guint stack)
{
    if (!self || stack >= 2)
        return;
    self->data_pref = (gint) stack;
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
static void first_radio_off (MMSitClient *client, GAsyncResult *res, MMSitCp *self);

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
    self->slot_status = status;
    self->slot_status_valid = TRUE;
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

        /* The mapping setter needs both radios down, and this is the only
         * place that needs them down at all; see boot_ready(). */
        if (!self->secondary_off) {
            mm_sit_client_set_radio_state (self->clients[1], MM_SIT_RADIO_STATE_OFF, NULL,
                                           (GAsyncReadyCallback) second_radio_off, self);
            return;
        }
        if (!self->primary_off) {
            mm_sit_client_set_radio_state (self->clients[0], MM_SIT_RADIO_STATE_OFF, NULL,
                                           (GAsyncReadyCallback) first_radio_off, self);
            return;
        }

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
    self->primary_off = TRUE;
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
    /*
     * Straight to the inventory. This used to take stack 0's radio down first,
     * to drop contexts inherited from a previous MM process -- but stock never
     * sends SET_RADIO_POWER OFF at all. Measured on identical firmware, its
     * whole boot is one request per stack, `02 00 00 00 00 00`, and nothing
     * else for the life of the capture; we sent `01` then `02`. The radios only
     * have to be down to *write* a slot mapping, so that cost moved to the one
     * branch that writes one.
     */
    query_slots (self);
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
    /*
     * The preference is not sent from here. A bearer connect already names it
     * as its own first step (CONNECT_STEP_PREFERRED_MODEM), so doing it on the
     * claim as well put a second, unexpected frame ahead of that one.
     */
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
