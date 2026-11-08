/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_SIT_CP_H
#define MM_SIT_CP_H

#include "mm-sit-client.h"

#define MM_TYPE_SIT_CP (mm_sit_cp_get_type ())
G_DECLARE_FINAL_TYPE (MMSitCp, mm_sit_cp, MM, SIT_CP, GObject)

/* Shared by the two MM objects, keyed by the real physical device path. */
MMSitCp *mm_sit_cp_get (const gchar *physdev, const gchar *device,
                        gboolean use_proxy, GError **error);
/* Also used by socket-based protocol tests. Takes references to both clients. */
MMSitCp *mm_sit_cp_new (MMSitClient *first, MMSitClient *second);
MMSitClient *mm_sit_cp_peek_client (MMSitCp *self, guint stack);
void mm_sit_cp_prepare (MMSitCp *self, GAsyncReadyCallback callback, gpointer user_data);
gboolean mm_sit_cp_prepare_finish (MMSitCp *self, GAsyncResult *res, GError **error);

/* Reservations cover pending connects as well as connected internet contexts.
 * Internal IMS setup does not claim the internet-data preference. */
gboolean mm_sit_cp_claim_data (MMSitCp *self, guint stack, guint8 cid, GError **error);
/* Name the stack that owns mobile data to the CP; a no-op if unchanged. */
/* The inventory prepare() already polled, so the modems need not ask again;
 * FALSE when nothing is cached. Dropped on SIM_SLOT_STATUS_CHANGED. */
gboolean mm_sit_cp_peek_sim_slot_status (MMSitCp *self, MMSitSimSlotStatus *out);
void mm_sit_cp_forget_sim_slot_status (MMSitCp *self);

void mm_sit_cp_prefer_data_stack (MMSitCp *self, guint stack);

/* Whether the CP already believes @stack owns mobile data. */
gboolean mm_sit_cp_data_stack_is (MMSitCp *self, guint stack);
void mm_sit_cp_note_data_stack   (MMSitCp *self, guint stack);
gboolean mm_sit_cp_data_reserved (MMSitCp *self, guint8 cid);
void mm_sit_cp_commit_data (MMSitCp *self, guint8 cid);
void mm_sit_cp_release_data (MMSitCp *self, guint8 cid);
void mm_sit_cp_release_stack_data (MMSitCp *self, guint stack);
void mm_sit_cp_update_data_calls (MMSitCp *self, guint stack, GArray *calls);

#endif
