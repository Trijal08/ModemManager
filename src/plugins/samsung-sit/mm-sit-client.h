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
GArray *mm_sit_client_get_cell_info_finish (MMSitClient        *self,
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
