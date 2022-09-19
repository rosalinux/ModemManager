/*
 * Copyright (C) 2022 Rosa Linux
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-mtk.h"

#define EPDN_TAG "+EPDN: "
#define CGCONTRDP_TAG "+CGCONTRDP: "

const char* CCMNI_IFNAME = "ccmni";
#define TRANSACTION_ID_OFFSET 100

static gchar *
strip_quotes(gchar *quotes_str, gpointer log_object)
{
    /* Strip quotes */
    gchar *unquotes_str = NULL;
    gchar *unquotes_str_iter = NULL;
    gchar *quotes_str_iter = quotes_str;

    unquotes_str = unquotes_str_iter = g_malloc0 (strlen (quotes_str) + 1);
    while (*quotes_str_iter) {
        if (*quotes_str_iter != '"') {
            *unquotes_str_iter++ = *quotes_str_iter;
        }
        quotes_str_iter++;
    }

    return unquotes_str;
}

gboolean
mm_mtk_parse_ipdpaddr_response (const gchar *response,
                                guint expected_cid,
                                gchar **interface_name,
                                gchar **ip4_addr,
                                gchar **ip6_addr,
                                gpointer log_object,
                                GError **error)
{
    gboolean success = FALSE;
    char **items;
    guint num_items, i;
    guint num;
    guint interface_id;
    guint interface_id_index;
    guint ip4_addr_index;
    guint tmp;
    gchar *tmp_ip4_addr = NULL;

    if (!response || !g_str_has_prefix (response, EPDN_TAG)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing +EPDN prefix");
        return FALSE;
    }

    //     +EPDN:<aid>,"new",<rat type>,<interface id>,<mtu>,<address type>,<address1>[,<address2>]
    //     +EPDN:<aid>,"update",<interface id>,<address type>,<address1>[,<address2>]
    //     +EPDN:<aid>,"err",<err>

    response = mm_strip_tag (response, EPDN_TAG);
    items = g_strsplit_set (response, ",", 0);

    /* Strip any spaces on elements; inet_pton() doesn't like them */
    num_items = g_strv_length (items);
    for (i = 0; i < num_items; i++) {
        items[i] = g_strstrip (items[i]);
    }

    if (num_items < 2) {
        g_set_error_literal (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Malformed EPDN response (not enough items)");
        goto out;
    }

    if (!strcmp(items[1], "\"new\"") && num_items < 7) {
        g_set_error_literal (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Malformed EPDN response (not enough items)");
        goto out;
    }

    if (!strcmp(items[1], "\"update\"") && num_items < 5) {
        g_set_error_literal (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Malformed EPDN response (not enough items)");
        goto out;
    }

    /* Validate context ID */
    if (!mm_get_uint_from_str (items[0], &num) ||
        num != expected_cid) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unknown AID in EPDN response (got %d, expected %d)",
                     (guint) num,
                     expected_cid);
        goto out;
    }

    interface_id_index = 0;
    ip4_addr_index = 0;

    if (!strcmp(items[1], "\"new\"")) {
        interface_id_index = 3;
        ip4_addr_index = 6;
    } else if (!strcmp(items[1], "\"update\"")) {
        interface_id_index = 2;
        ip4_addr_index = 4;
    } else {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unknown type EPDN");
        goto out;
    }

    /* Interface name */

    if (!mm_get_uint_from_str (items[interface_id_index], &interface_id)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unknown interface id in EPDN response");
        goto out;
    }

    // this is transaction_id and interface_id combination,
    // i.e, trans_intf_id = (transaction_id * 100) + interface_id
    interface_id %= TRANSACTION_ID_OFFSET;

    *interface_name = g_strdup_printf ("%s%u", CCMNI_IFNAME, interface_id);

    /* IP4 address */

    /* Create unquated IP addr string */
    tmp_ip4_addr = strip_quotes(items[ip4_addr_index], log_object);

    tmp = 0;
    if (!inet_pton (AF_INET, tmp_ip4_addr, &tmp) || !tmp) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Couldn't parse IPv4 address '%s'",
                     tmp_ip4_addr);
        g_free(tmp_ip4_addr);
        goto out;
    }

    *ip4_addr = tmp_ip4_addr;

    // TODO IP6 address

    // ip6_addr

    success = TRUE;

out:
    g_strfreev (items);

    return success;
}

gboolean
mm_mtk_parse_cgcontrdp_response (const gchar *response,
                                 guint expected_cid,
                                 gchar **dns1_addr,
                                 gchar **dns2_addr,
                                 gchar **gw_addr,
                                 gpointer log_object,
                                 GError **error)
{
    gboolean success = FALSE;
    char **items;
    guint num_items, i;
    guint num;
    guint tmp;
    gchar *tmp_gw_addr = NULL;
    gchar *tmp_dns1_addr = NULL;
    gchar *tmp_dns2_addr = NULL;

    if (!response || !g_str_has_prefix (response, CGCONTRDP_TAG)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Missing +CGCONTRDP prefix");
        return FALSE;
    }

    //+CGCONTRDP response a list of [+CGCONTRDP: <aid>,<bearer_id>,<apn>[,<local_addr and subnet_mask>
    //[,<gw_addr>[,<DNS_prim_addr>[,<DNS_sec_addr>[,<P-CSCF_prim_addr>[,<P-CSCF_sec_addr>
    //[,<IM_CN_Signalling_Flag>[,<LIPA_indication>[,<IPv4_MTU>[,<WLAN_Offload>[,<Local_Addr_Ind>
    //[,<Non-IP_MTU>]]]]]]]]]]]]]

    response = mm_strip_tag (response, CGCONTRDP_TAG);
    items = g_strsplit_set (response, ",", 0);

    /* Strip any spaces on elements; inet_pton() doesn't like them */
    num_items = g_strv_length (items);
    for (i = 0; i < num_items; i++) {
        items[i] = g_strstrip (items[i]);
    }

    if (num_items < 7) {
        g_set_error_literal (error,
                             MM_CORE_ERROR,
                             MM_CORE_ERROR_FAILED,
                             "Malformed CGCONTRDP response (not enough items)");
        goto out;
    }

    /* Validate context ID */
    if (!mm_get_uint_from_str (items[0], &num) ||
        num != expected_cid) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Unknown AID in CGCONTRDP response (got %d, expected %d)",
                     (guint) num,
                     expected_cid);
        goto out;
    }

    /* gw address */

    /* Create unquated gw addr string */
    tmp_gw_addr = strip_quotes(items[4], log_object);

    tmp = 0;
    if (!inet_pton (AF_INET, tmp_gw_addr, &tmp) || !tmp) {
        mm_obj_warn (log_object,"gw address is not valid");
        g_free(tmp_gw_addr);
    } else {
        *gw_addr = tmp_gw_addr;
    }

    /* dns1 address */

    /* Create unquated dns1 addr string */
    tmp_dns1_addr = strip_quotes(items[5], log_object);

    tmp = 0;
    if (!inet_pton (AF_INET, tmp_dns1_addr, &tmp) || !tmp) {
        mm_obj_warn (log_object,"dns1 address is not valid");
        g_free(tmp_dns1_addr);
    } else {
        *dns1_addr = tmp_dns1_addr;
    }

    /* dns2 address */

    /* Create unquated dns2 addr string */
    tmp_dns2_addr = strip_quotes(items[6], log_object);

    tmp = 0;
    if (!inet_pton (AF_INET, tmp_dns2_addr, &tmp) || !tmp) {
        mm_obj_warn (log_object,"dns2 address is not valid");
        g_free(tmp_dns2_addr);
    } else {
        *dns2_addr = tmp_dns2_addr;
    }

    success = TRUE;

out:
    g_strfreev (items);

    return success;
}
