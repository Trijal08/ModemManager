/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 The ModemManager authors */

#ifndef MM_PORT_SERIAL_SIT_H
#define MM_PORT_SERIAL_SIT_H

#include "mm-port-serial.h"
#include "mm-sit-protocol.h"

#define MM_TYPE_PORT_SERIAL_SIT            (mm_port_serial_sit_get_type ())
#define MM_PORT_SERIAL_SIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_PORT_SERIAL_SIT, MMPortSerialSit))
#define MM_PORT_SERIAL_SIT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MM_TYPE_PORT_SERIAL_SIT, MMPortSerialSitClass))
#define MM_IS_PORT_SERIAL_SIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_PORT_SERIAL_SIT))
#define MM_IS_PORT_SERIAL_SIT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MM_TYPE_PORT_SERIAL_SIT))
#define MM_PORT_SERIAL_SIT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MM_TYPE_PORT_SERIAL_SIT, MMPortSerialSitClass))

typedef struct _MMPortSerialSit        MMPortSerialSit;
typedef struct _MMPortSerialSitClass   MMPortSerialSitClass;
typedef struct _MMPortSerialSitPrivate MMPortSerialSitPrivate;

struct _MMPortSerialSit {
    MMPortSerial parent;
    MMPortSerialSitPrivate *priv;
};

struct _MMPortSerialSitClass {
    MMPortSerialClass parent;
};

typedef void (*MMPortSerialSitUnsolicitedFn) (MMPortSerialSit    *self,
                                              const MMSitMessage *message,
                                              gpointer            user_data);

GType            mm_port_serial_sit_get_type (void);
/* Talk to the device node itself: @name is a /dev entry without the prefix. */
MMPortSerialSit *mm_port_serial_sit_new      (const gchar *name);
/* Talk to sit-proxy: @path is its AF_UNIX socket for one SIT port. */
MMPortSerialSit *mm_port_serial_sit_new_socket (const gchar *path);

guint16 mm_port_serial_sit_next_token (MMPortSerialSit *self);

guint mm_port_serial_sit_add_unsolicited_handler (MMPortSerialSit                *self,
                                                   MMPortSerialSitUnsolicitedFn     callback,
                                                   gpointer                        user_data,
                                                   GDestroyNotify                  notify);
void  mm_port_serial_sit_remove_unsolicited_handler (MMPortSerialSit *self,
                                                      guint            handler_id);

void          mm_port_serial_sit_send_indication (MMPortSerialSit *self,
                                                 GByteArray      *frame);

void          mm_port_serial_sit_command        (MMPortSerialSit     *self,
                                                 GByteArray          *request,
                                                 guint32              timeout_seconds,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
guint mm_port_serial_sit_pending_count (MMPortSerialSit *self,
                                        const guint16   *ids,
                                        gsize            n_ids);

MMSitMessage *mm_port_serial_sit_command_finish (MMPortSerialSit     *self,
                                                 GAsyncResult        *res,
                                                 GError             **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMPortSerialSit, g_object_unref)

#endif /* MM_PORT_SERIAL_SIT_H */
