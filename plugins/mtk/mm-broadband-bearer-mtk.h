/*
 * Copyright (C) 2022 Rosa Linux
 */

#ifndef MM_BROADBAND_BEARER_MTK_H
#define MM_BROADBAND_BEARER_MTK_H

#include <glib.h>
#include <glib-object.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-bearer.h"

#define MM_TYPE_BROADBAND_BEARER_MTK            (mm_broadband_bearer_mtk_get_type ())
#define MM_BROADBAND_BEARER_MTK(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_BEARER_MTK, MMBroadbandBearerMtk))
#define MM_BROADBAND_BEARER_MTK_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_BEARER_MTK, MMBroadbandBearerMtkClass))
#define MM_IS_BROADBAND_BEARER_MTK(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_BEARER_MTK))
#define MM_IS_BROADBAND_BEARER_MTK_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_BEARER_MTK))
#define MM_BROADBAND_BEARER_MTK_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_BEARER_MTK, MMBroadbandBearerMtkClass))

typedef struct _MMBroadbandBearerMtk MMBroadbandBearerMtk;
typedef struct _MMBroadbandBearerMtkClass MMBroadbandBearerMtkClass;
typedef struct _MMBroadbandBearerMtkPrivate MMBroadbandBearerMtkPrivate;

struct _MMBroadbandBearerMtk {
    MMBroadbandBearer parent;
    MMBroadbandBearerMtkPrivate *priv;
};

struct _MMBroadbandBearerMtkClass {
    MMBroadbandBearerClass parent;
};

GType mm_broadband_bearer_mtk_get_type (void);

/* Default bearer creation implementation */
void      mm_broadband_bearer_mtk_new            (MMBroadbandModem *modem,
                                                  MMBearerProperties *config,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data);
MMBaseBearer *mm_broadband_bearer_mtk_new_finish (GAsyncResult *res,
                                                  GError **error);

#endif /* MM_BROADBAND_BEARER_MTK_H */
