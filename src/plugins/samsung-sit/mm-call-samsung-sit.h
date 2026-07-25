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

#ifndef MM_CALL_SAMSUNG_SIT_H
#define MM_CALL_SAMSUNG_SIT_H

#include <glib.h>
#include <glib-object.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-call.h"
#include "mm-base-modem.h"

#define MM_TYPE_CALL_SAMSUNG_SIT            (mm_call_samsung_sit_get_type ())
#define MM_CALL_SAMSUNG_SIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_CALL_SAMSUNG_SIT, MMCallSamsungSit))
#define MM_CALL_SAMSUNG_SIT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), MM_TYPE_CALL_SAMSUNG_SIT, MMCallSamsungSitClass))
#define MM_IS_CALL_SAMSUNG_SIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_CALL_SAMSUNG_SIT))
#define MM_IS_CALL_SAMSUNG_SIT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), MM_TYPE_CALL_SAMSUNG_SIT))
#define MM_CALL_SAMSUNG_SIT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), MM_TYPE_CALL_SAMSUNG_SIT, MMCallSamsungSitClass))

typedef struct _MMCallSamsungSit        MMCallSamsungSit;
typedef struct _MMCallSamsungSitClass   MMCallSamsungSitClass;
typedef struct _MMCallSamsungSitPrivate MMCallSamsungSitPrivate;

struct _MMCallSamsungSit {
    MMBaseCall               parent;
    MMCallSamsungSitPrivate *priv;
};

struct _MMCallSamsungSitClass {
    MMBaseCallClass parent;
};

GType mm_call_samsung_sit_get_type (void);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MMCallSamsungSit, g_object_unref)

MMBaseCall *mm_call_samsung_sit_new (MMBaseModem     *modem,
                                     MMCallDirection  direction,
                                     const gchar     *number,
                                     const guint      dtmf_tone_duration);

#endif /* MM_CALL_SAMSUNG_SIT_H */
