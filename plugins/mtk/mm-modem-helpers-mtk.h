/*
 * Copyright (C) 2022 Rosa Linux
 */

#ifndef MM_MODEM_HELPERS_MTK_H
#define MM_MODEM_HELPERS_MTK_H

#include "glib.h"

gboolean mm_mtk_parse_ipdpaddr_response (const gchar *response,
                                         guint expected_cid,
                                         gchar **interface_name,
                                         gchar **ip4_addr,
                                         gchar **ip6_addr,
                                         gpointer log_object,
                                         GError **error);

gboolean mm_mtk_parse_cgcontrdp_response (const gchar *response,
                                          guint expected_cid,
                                          gchar **dns1_addr,
                                          gchar **dns2_addr,
                                          gchar **gw_addr,
                                          gpointer log_object,
                                          GError **error);

#endif  /* MM_MODEM_HELPERS_MTK_H */
