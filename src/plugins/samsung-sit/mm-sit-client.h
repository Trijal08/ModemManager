/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_SIT_CLIENT_H
#define MM_SIT_CLIENT_H

#include "mm-port-serial-sit.h"

#define MM_TYPE_SIT_CLIENT            (mm_sit_client_get_type ())
#define MM_SIT_CLIENT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_SIT_CLIENT, MMSitClient))
#define MM_SIT_CLIENT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MM_TYPE_SIT_CLIENT, MMSitClientClass))
#define MM_IS_SIT_CLIENT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_SIT_CLIENT))
#define MM_IS_SIT_CLIENT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MM_TYPE_SIT_CLIENT))
#define MM_SIT_CLIENT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MM_TYPE_SIT_CLIENT, MMSitClientClass))

typedef struct _MMSitClient        MMSitClient;
typedef struct _MMSitClientClass   MMSitClientClass;
typedef struct _MMSitClientPrivate MMSitClientPrivate;

struct _MMSitClient {
    GObject parent;
    MMSitClientPrivate *priv;
};

struct _MMSitClientClass {
    GObjectClass parent;
};

GType        mm_sit_client_get_type   (void);
MMSitClient *mm_sit_client_new        (const gchar *device);
MMSitClient *mm_sit_client_new_from_port (MMPortSerialSit *port);
const gchar *mm_sit_client_get_device (MMSitClient *self);

gboolean mm_sit_client_open    (MMSitClient  *self,
                                GError      **error);
void     mm_sit_client_close   (MMSitClient  *self);
gboolean mm_sit_client_is_open (MMSitClient  *self);
gboolean mm_sit_client_get_carrier_sim_info (MMSitClient         *self,
                                             MMSitCarrierSimInfo *out_info);
gboolean mm_sit_client_radio_state_trusted  (MMSitClient         *self);

void          mm_sit_client_command        (MMSitClient        *self,
                                            GByteArray         *request,
                                            guint32             timeout_seconds,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
MMSitMessage *mm_sit_client_command_finish (MMSitClient        *self,
                                            GAsyncResult       *res,
                                            GError            **error);

void            mm_sit_client_get_radio_state        (MMSitClient        *self,
                                                       GCancellable       *cancellable,
                                                       GAsyncReadyCallback callback,
                                                       gpointer            user_data);
MMSitRadioState mm_sit_client_get_radio_state_finish (MMSitClient        *self,
                                                       GAsyncResult       *res,
                                                       GError            **error);
void            mm_sit_client_set_radio_state        (MMSitClient        *self,
                                                       MMSitRadioState     state,
                                                       GCancellable       *cancellable,
                                                       GAsyncReadyCallback callback,
                                                       gpointer            user_data);
gboolean        mm_sit_client_set_radio_state_finish (MMSitClient        *self,
                                                       GAsyncResult       *res,
                                                       GError            **error);

void     mm_sit_client_get_sim_status        (MMSitClient        *self,
                                              GCancellable       *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer            user_data);
gboolean mm_sit_client_get_sim_status_finish (MMSitClient        *self,
                                              GAsyncResult       *res,
                                              MMSitSimStatus     *out_status,
                                              GError            **error);

/* Send one SMS part. 'smsc' may be NULL, which lets the CP use the service
 * centre the card configured. 'over_ims' picks the transport: the AIMS group
 * while IMS carries messages, the classic one otherwise. 'expect_more' marks
 * every part of a concatenated message but the last. */
void     mm_sit_client_send_sms                   (MMSitClient        *self,
                                                   const guint8       *smsc,
                                                   gsize               smsc_len,
                                                   const guint8       *tpdu,
                                                   gsize               tpdu_len,
                                                   gboolean            expect_more,
                                                   gboolean            over_ims,
                                                   GCancellable       *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer            user_data);
gboolean mm_sit_client_send_sms_finish            (MMSitClient        *self,
                                                   GAsyncResult       *res,
                                                   guint32            *out_reference,
                                                   GError            **error);

/* Acknowledge a delivered message on the transport that carried it. The CP
 * holds the message until this is sent. */
void     mm_sit_client_ack_sms                    (MMSitClient        *self,
                                                   gboolean            over_ims,
                                                   GCancellable       *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer            user_data);
gboolean mm_sit_client_ack_sms_finish             (MMSitClient        *self,
                                                   GAsyncResult       *res,
                                                   GError            **error);

/* Read one elementary file: GET_RESPONSE for its shape, then as many
 * READ_BINARY/READ_RECORD passes as it takes. 'path' is the chain of file ids
 * from the MF down to the file's parent. The result is the whole file, records
 * concatenated, with the geometry in *out_info so a caller can slice them; a
 * card refusal comes back as an error naming the status word. */
void     mm_sit_client_read_ef                    (MMSitClient        *self,
                                                   guint16             file_id,
                                                   const guint16      *path,
                                                   guint               path_len,
                                                   GCancellable       *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer            user_data);
GBytes  *mm_sit_client_read_ef_finish             (MMSitClient        *self,
                                                   GAsyncResult       *res,
                                                   MMSitEfInfo        *out_info,
                                                   GError            **error);

void     mm_sit_client_get_sim_slot_status        (MMSitClient        *self,
                                                   GCancellable       *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer            user_data);
gboolean mm_sit_client_get_sim_slot_status_finish (MMSitClient        *self,
                                                   GAsyncResult       *res,
                                                   MMSitSimSlotStatus *out_status,
                                                   GError            **error);

void     mm_sit_client_set_logical_slot_mapping        (MMSitClient                  *self,
                                                        const MMSitLogicalSlotMapping *mappings,
                                                        guint                         mapping_count,
                                                        GCancellable                 *cancellable,
                                                        GAsyncReadyCallback           callback,
                                                        gpointer                      user_data);
gboolean mm_sit_client_set_logical_slot_mapping_finish (MMSitClient                  *self,
                                                        GAsyncResult                 *res,
                                                        GError                      **error);

void     mm_sit_client_get_registration        (MMSitClient        *self,
                                                GCancellable       *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer            user_data);
gboolean mm_sit_client_get_registration_finish (MMSitClient        *self,
                                                GAsyncResult       *res,
                                                MMSitRegistration  *out_registration,
                                                GError            **error);

/* CS-domain counterpart of mm_sit_client_get_registration(); without it the core
 * has no circuit-switched registration state at all. */
void     mm_sit_client_get_voice_registration        (MMSitClient        *self,
                                                      GCancellable       *cancellable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer            user_data);
gboolean mm_sit_client_get_voice_registration_finish (MMSitClient        *self,
                                                      GAsyncResult       *res,
                                                      MMSitRegistration  *out_registration,
                                                      GError            **error);

/* Ask the CP to search for a network. Nothing else re-triggers a search once it
 * has fallen out of service. */
void     mm_sit_client_set_network_selection_auto        (MMSitClient        *self,
                                                          GCancellable       *cancellable,
                                                          GAsyncReadyCallback callback,
                                                          gpointer            user_data);
gboolean mm_sit_client_set_network_selection_auto_finish (MMSitClient        *self,
                                                          GAsyncResult       *res,
                                                          GError            **error);

void     mm_sit_client_set_network_selection_manual        (MMSitClient        *self,
                                                            const gchar        *plmn,
                                                            guint8              rat,
                                                            GCancellable       *cancellable,
                                                            GAsyncReadyCallback callback,
                                                            gpointer            user_data);
gboolean mm_sit_client_set_network_selection_manual_finish (MMSitClient        *self,
                                                            GAsyncResult       *res,
                                                            GError            **error);

/* Whether one facility (MM_SIT_FACILITY_*) is locked. Carries no retry counter:
 * the CP only reports remaining attempts in a PIN operation's response. */
void     mm_sit_client_get_facility_lock        (MMSitClient        *self,
                                                 guint8              facility,
                                                 GCancellable       *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer            user_data);
gboolean mm_sit_client_get_facility_lock_finish (MMSitClient        *self,
                                                 GAsyncResult       *res,
                                                 gboolean           *out_locked,
                                                 GError            **error);

/* The radio generations the CP may use. Persisted by the CP across our
 * restarts, so the current value has to be read back. */
void     mm_sit_client_get_allowed_network_types        (MMSitClient        *self,
                                                         GCancellable       *cancellable,
                                                         GAsyncReadyCallback callback,
                                                         gpointer            user_data);
gboolean mm_sit_client_get_allowed_network_types_finish (MMSitClient        *self,
                                                         GAsyncResult       *res,
                                                         MMSitNetworkGenerations *out_generations,
                                                         GError            **error);

void     mm_sit_client_set_allowed_network_types        (MMSitClient        *self,
                                                         const MMSitNetworkGenerations *generations,
                                                         GCancellable       *cancellable,
                                                         GAsyncReadyCallback callback,
                                                         gpointer            user_data);
gboolean mm_sit_client_set_allowed_network_types_finish (MMSitClient        *self,
                                                         GAsyncResult       *res,
                                                         GError            **error);

/* Runs a PLMN search to completion and returns a GArray of MMSitScannedNetwork;
 * the CP has no "search done" message, so this settles once its result batches
 * stop arriving. */
void    mm_sit_client_scan_networks        (MMSitClient        *self,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
GArray *mm_sit_client_scan_networks_finish (MMSitClient        *self,
                                            GAsyncResult       *res,
                                            GError            **error);

void     mm_sit_client_get_operator        (MMSitClient        *self,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
gboolean mm_sit_client_get_operator_finish (MMSitClient        *self,
                                             GAsyncResult       *res,
                                             MMSitOperator      *out_operator,
                                             GError            **error);

void     mm_sit_client_get_signal        (MMSitClient        *self,
                                          GCancellable       *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer            user_data);
gboolean mm_sit_client_get_signal_finish (MMSitClient        *self,
                                          GAsyncResult       *res,
                                          MMSitSignal        *out_signal,
                                          GError            **error);

void    mm_sit_client_get_cell_info        (MMSitClient        *self,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
/* Returns the LTE records; *out_nr_cells (optional, transfer full) receives the
 * NR ones from the same response, empty unless the CP is reporting NR in reach. */
GArray *mm_sit_client_get_cell_info_finish (MMSitClient        *self,
                                            GAsyncResult       *res,
                                            GArray            **out_nr_cells,
                                            GError            **error);

/* CS voice call control. Progress is reported out-of-band as
 * CALL_STATE_CHANGED indications, not in these responses. */
void     mm_sit_client_get_call_list        (MMSitClient        *self,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            user_data);
GArray  *mm_sit_client_get_call_list_finish (MMSitClient        *self,
                                             GAsyncResult       *res,
                                             GError            **error);

void     mm_sit_client_dial                 (MMSitClient        *self,
                                             const gchar        *number,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            user_data);
gboolean mm_sit_client_dial_finish          (MMSitClient        *self,
                                             GAsyncResult       *res,
                                             GError            **error);

void     mm_sit_client_answer               (MMSitClient        *self,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            user_data);
gboolean mm_sit_client_answer_finish        (MMSitClient        *self,
                                             GAsyncResult       *res,
                                             GError            **error);

void     mm_sit_client_hangup               (MMSitClient        *self,
                                             guint32             call_id,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            user_data);
gboolean mm_sit_client_hangup_finish        (MMSitClient        *self,
                                             GAsyncResult       *res,
                                             GError            **error);

void     mm_sit_client_set_indication_filter        (MMSitClient        *self,
                                                     guint32             filter,
                                                     GCancellable       *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer            user_data);
gboolean mm_sit_client_set_indication_filter_finish (MMSitClient        *self,
                                                     GAsyncResult       *res,
                                                     GError            **error);

void     mm_sit_client_set_screen_state        (MMSitClient        *self,
                                                gboolean            screen_on,
                                                GCancellable       *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer            user_data);
gboolean mm_sit_client_set_screen_state_finish (MMSitClient        *self,
                                                GAsyncResult       *res,
                                                GError            **error);

void     mm_sit_client_set_carrier_manifest        (MMSitClient        *self,
                                                    const gchar        *directory,
                                                    const guint8        sha1[MM_SIT_SHA1_LENGTH],
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);
gboolean mm_sit_client_set_carrier_manifest_finish (MMSitClient        *self,
                                                    GAsyncResult       *res,
                                                    GError            **error);

void     mm_sit_client_send_boot_config        (MMSitClient        *self,
                                                GCancellable       *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer            user_data);
gboolean mm_sit_client_send_boot_config_finish (MMSitClient        *self,
                                                GAsyncResult       *res,
                                                GError            **error);

void     mm_sit_client_connect_data        (MMSitClient        *self,
                                            const gchar        *apn,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
gboolean mm_sit_client_connect_data_finish (MMSitClient        *self,
                                            GAsyncResult       *res,
                                            MMSitDataCall      *out_call,
                                            GError            **error);

void     mm_sit_client_disconnect_data        (MMSitClient        *self,
                                               guint8              cid,
                                               GCancellable       *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer            user_data);
gboolean mm_sit_client_disconnect_data_finish (MMSitClient        *self,
                                               GAsyncResult       *res,
                                               GError            **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMSitClient, g_object_unref)

#endif /* MM_SIT_CLIENT_H */
