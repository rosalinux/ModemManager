/*
 * Copyright (C) 2022 Rosa Linux
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-broadband-bearer-mtk.h"
#include "mm-base-modem-at.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-error-helpers.h"
#include "mm-daemon-enums-types.h"
#include "mm-modem-helpers-mtk.h"

G_DEFINE_TYPE (MMBroadbandBearerMtk, mm_broadband_bearer_mtk, MM_TYPE_BROADBAND_BEARER);

struct _MMBroadbandBearerMtkPrivate {
    guint uid;
};

static MMPortSerialAt *
common_get_at_data_port (MMBroadbandBearer  *self,
                         MMBaseModem        *modem,
                         GError            **error)
{
    MMPort *data;

    /* Look for best data port, NULL if none available. */
    data = mm_base_modem_peek_best_data_port (modem, MM_PORT_TYPE_AT);
    if (!data) {
        /* It may happen that the desired data port grabbed during probing was
         * actually a 'net' port, which the generic logic cannot handle, so if
         * that is the case, and we have no AT data ports specified, just
         fallback to the primary AT port. */
        data = (MMPort *) mm_base_modem_peek_port_primary (modem);
    }

    g_assert (MM_IS_PORT_SERIAL_AT (data));

    if (!mm_port_serial_open (MM_PORT_SERIAL (data), error)) {
        g_prefix_error (error, "Couldn't connect: cannot keep data port open.");
        return NULL;
    }

    mm_obj_dbg (self, "connection through a plain serial AT port: %s", mm_port_get_device (data));

    return MM_PORT_SERIAL_AT (g_object_ref (data));
}

/*****************************************************************************/
/* 3GPP disconnection */

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult *res,
                        GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
apn_diact_ready (MMBaseModem *modem,
                 GAsyncResult *res,
                 GTask *task)
{
    MMBroadbandBearerMtk *self;
    GError *error = NULL;

    self = g_task_get_source_object (task);

    mm_base_modem_at_command_full_finish (modem, res, &error);

    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    self->priv->uid = -1;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer *bearer,
                 MMBroadbandModem *modem,
                 MMPortSerialAt *primary,
                 MMPortSerialAt *secondary,
                 MMPort *data,
                 guint cid,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    MMBroadbandBearerMtk *self = MM_BROADBAND_BEARER_MTK (bearer);
    g_autofree gchar *command;
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    if (self->priv->uid < 0) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    command = g_strdup_printf ("+EAPNACT=0,%u,1", self->priv->uid);

    mm_base_modem_at_command_full (MM_BASE_MODEM (modem),
                                   primary,
                                   command,
                                   MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                   FALSE,
                                   FALSE, /* raw */
                                   NULL, /* cancellable */
                                   (GAsyncReadyCallback)apn_diact_ready,
                                   task);
}

/*****************************************************************************/
/* Profile set 3GPP */

typedef enum {
    PROFILE_SET_3GPP_CONTEXT_STEP_FIRST = 0,
    PROFILE_SET_3GPP_CONTEXT_STEP_UNLOCK,
    PROFILE_SET_3GPP_CONTEXT_STEP_RESET,
    PROFILE_SET_3GPP_CONTEXT_STEP_SET_APN,
    PROFILE_SET_3GPP_CONTEXT_STEP_SET_APN_PROTOCOL,
    PROFILE_SET_3GPP_CONTEXT_STEP_LOCK,
    PROFILE_SET_3GPP_CONTEXT_STEP_LAST
} ProfileSet3gppContextStep;

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    ProfileSet3gppContextStep step;
} ProfileSet3gppContext;

static void
profile_set_3gpp_context_free (ProfileSet3gppContext *ctx)
{
    g_object_unref (ctx->modem);
    g_clear_object (&ctx->primary);

    g_slice_free (ProfileSet3gppContext, ctx);
}

static gboolean
profile_set_3gpp_finish (MMBroadbandBearer *self,
                         GAsyncResult *res,
                         GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void profile_set_3gpp_context_step (GTask *task);

static void
apn_set (MMBaseModem *modem,
         GAsyncResult *res,
         GTask *task)
{
    MMBroadbandBearerMtk *self;
    ProfileSet3gppContext *ctx;
    GError *error = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    mm_base_modem_at_command_full_finish (modem, res, &error);

    if (error) {
        mm_obj_warn (self, "Profile set fail: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->step++;
    profile_set_3gpp_context_step(task);
}

static void
profile_set_3gpp_context_step (GTask *task)
{
    MMBroadbandBearerMtk *self;
    ProfileSet3gppContext *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
    case PROFILE_SET_3GPP_CONTEXT_STEP_FIRST: {
        mm_obj_dbg (self, "Start profile set");
        ctx->step++;
    }

    case PROFILE_SET_3GPP_CONTEXT_STEP_UNLOCK: {
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       "+EAPNLOCK=1",
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)apn_set,
                                       task);
        return;
    }

    case PROFILE_SET_3GPP_CONTEXT_STEP_RESET: {
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       "+EAPNSET",
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)apn_set,
                                       task);
        return;
    }

    case PROFILE_SET_3GPP_CONTEXT_STEP_SET_APN: {
        const gchar         *apn;
        const gchar         *user;
        const gchar         *passwd;
        g_autofree gchar    *apn_set_command;

        apn = mm_bearer_properties_get_apn (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
        user = mm_bearer_properties_get_user (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
        passwd = mm_bearer_properties_get_password (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));

        apn_set_command = g_strdup_printf ("+EAPNSET=\"%s\",1,\"%s\",\"%s\"", apn, user, passwd);

        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       apn_set_command,
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)apn_set,
                                       task);
        return;
    }

    case PROFILE_SET_3GPP_CONTEXT_STEP_SET_APN_PROTOCOL: {
        const gchar         *apn;
        g_autofree gchar    *apn_protocol_set_command;

        apn = mm_bearer_properties_get_apn (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));

        apn_protocol_set_command = g_strdup_printf ("+EAPNSET=\"%s\",2,\"type=default,supl,hipri;protocol=IPV4V6;roaming_protocol=IP;authtype=2;carrier_enabled=1;max_conns=0;max_conns_time=0;wait_time=0;bearer_bitmask=2147352575;inactive_timer=0\"", apn);
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       apn_protocol_set_command,
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)apn_set,
                                       task);
        return;
    }

    case PROFILE_SET_3GPP_CONTEXT_STEP_LOCK: {
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       "+EAPNLOCK=0",
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)apn_set,
                                       task);
        return;
    }

    case PROFILE_SET_3GPP_CONTEXT_STEP_LAST: {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    default:
        g_assert_not_reached ();
    }
}

/*****************************************************************************/
/* Connect 3GPP */

typedef enum {
    CONNECT_3GPP_CONTEXT_STEP_FIRST = 0,
    CONNECT_3GPP_CONTEXT_STEP_DATA_ALLOW,
    CONNECT_3GPP_CONTEXT_STEP_APNSET,
    CONNECT_3GPP_CONTEXT_STEP_APNACT,
    CONNECT_3GPP_CONTEXT_STEP_IP_CONFIG,
    CONNECT_3GPP_CONTEXT_STEP_DNS_CONFIG,
    CONNECT_3GPP_CONTEXT_STEP_LAST
} Connect3gppContextStep;

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    MMPortSerialAt *data;
    Connect3gppContextStep step;
    gchar *interface_name;
    MMBearerIpConfig *ipv4_config;
} Connect3gppContext;

static void
connect_3gpp_context_free (Connect3gppContext *ctx)
{
    g_object_unref (ctx->modem);

    g_free (ctx->interface_name);
    g_clear_object (&ctx->ipv4_config);
    g_clear_object (&ctx->data);
    g_clear_object (&ctx->primary);

    g_slice_free (Connect3gppContext, ctx);
}

static MMBearerConnectResult *
connect_3gpp_finish (MMBroadbandBearer *self,
                     GAsyncResult *res,
                     GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void connect_3gpp_context_step (GTask *task);

static void
data_allow_done (MMBaseModem *modem,
                 GAsyncResult *res,
                 GTask *task)
{
    Connect3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);

    mm_base_modem_at_command_full_finish (modem, res, &error);

    ctx->step++;
    connect_3gpp_context_step(task);
}

static void
apn_act_done (MMBaseModem *modem,
              GAsyncResult *res,
              GTask *task)
{
    MMBroadbandBearerMtk *self;
    Connect3gppContext *ctx;
    GError *error = NULL;
    GError *parse_error = NULL;
    const gchar *result = NULL;
    MM3gppCgev  type;
    guint   uid = 0;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    result = mm_base_modem_at_command_full_finish (MM_BASE_MODEM (modem), res, &error);

    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (result) {
        type = mm_3gpp_parse_cgev_indication_action (result);

        if (!mm_3gpp_parse_cgev_indication_primary (result, type, &uid, &parse_error)) {
            mm_obj_warn (self, "couldn't parse uid info from +CGEV indication '%s': %s", result, parse_error->message);
            g_task_return_error (task, parse_error);
            g_object_unref (task);
            return;
        }
        self->priv->uid = uid;
    }

    ctx->step++;
    connect_3gpp_context_step(task);
}

static MMPortNet *
port_net_new (const gchar *name)
{
    return MM_PORT_NET (g_object_new (MM_TYPE_PORT_NET,
                                      MM_PORT_DEVICE, name,
                                      MM_PORT_SUBSYS, MM_PORT_SUBSYS_NET,
                                      MM_PORT_TYPE,   MM_PORT_TYPE_NET,
                                      NULL));
}

static void
ip_info_response (MMBaseModem *modem,
                  GAsyncResult *res,
                  GTask *task)
{
    MMBroadbandBearerMtk *self;
    Connect3gppContext *ctx;
    gchar *ip4_addr = NULL;
    gchar *ip6_addr = NULL;
    GError *error = NULL;
    const gchar *response = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_full_finish (MM_BASE_MODEM (modem), res, &error);

    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!mm_mtk_parse_ipdpaddr_response (response,
                                         self->priv->uid,
                                         &ctx->interface_name,
                                         &ip4_addr,
                                         &ip6_addr,
                                         self,
                                         &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_bearer_ip_config_set_address(ctx->ipv4_config, ip4_addr);
    mm_bearer_ip_config_set_gateway (ctx->ipv4_config, ip4_addr); /* default gw */
    mm_bearer_ip_config_set_prefix (ctx->ipv4_config, 24); /* default prefix */
    g_free(ip4_addr);

    ctx->step++;
    connect_3gpp_context_step(task);
}

static void
cgcontrdp_response (MMBaseModem *modem,
                    GAsyncResult *res,
                    GTask *task) {
    MMBroadbandBearerMtk *self;
    Connect3gppContext *ctx;
    gchar *dns_addrs[3] = { NULL, NULL, NULL };
    guint n = 0;
    gchar *dns1_addr = NULL;
    gchar *dns2_addr = NULL;
    gchar *gw_addr = NULL;
    GError *error = NULL;
    const gchar *response = NULL;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_full_finish (MM_BASE_MODEM (modem), res, &error);

    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!mm_mtk_parse_cgcontrdp_response (response,
                                          self->priv->uid,
                                          &dns1_addr,
                                          &dns2_addr,
                                          &gw_addr,
                                          self,
                                          &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (gw_addr) {
        mm_bearer_ip_config_set_gateway (ctx->ipv4_config, gw_addr);
        g_free (gw_addr);
    }

    if (dns1_addr) {
        dns_addrs[n++] = dns1_addr;
    }

    if (dns2_addr) {
        dns_addrs[n++] = dns2_addr;
    }

    mm_bearer_ip_config_set_dns (ctx->ipv4_config, (const gchar **)dns_addrs);
    g_free (dns1_addr);
    g_free (dns2_addr);

    ctx->step++;
    connect_3gpp_context_step(task);
}

static void
profile_set_ready (MMBroadbandBearer *self,
                   GAsyncResult *res,
                   GTask *task)
{
    GError *error = NULL;
    Connect3gppContext *ctx;

    if (!profile_set_3gpp_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx = g_task_get_task_data (task);
    ctx->step++;
    connect_3gpp_context_step(task);
}

static void
connect_3gpp_context_step (GTask *task)
{
    MMBroadbandBearerMtk *self;
    Connect3gppContext *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
    case CONNECT_3GPP_CONTEXT_STEP_FIRST: {
        mm_obj_dbg (self, "CONNECT_3GPP_CONTEXT_STEP_FIRST");
        ctx->step++;
    } /* fall through */

    case CONNECT_3GPP_CONTEXT_STEP_DATA_ALLOW: {
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       "+EDALLOW=1",
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)data_allow_done,
                                       task);

        return;
    }

    case CONNECT_3GPP_CONTEXT_STEP_APNSET: {

        /* Setup profile set context */
        ProfileSet3gppContext *profile_ctx;
        GTask *profile_task;
        profile_ctx = g_slice_new0 (ProfileSet3gppContext);

        profile_ctx->modem = g_object_ref (ctx->modem);
        profile_ctx->primary = g_object_ref (ctx->primary);
        profile_ctx->step = PROFILE_SET_3GPP_CONTEXT_STEP_FIRST;

        profile_task = g_task_new (self, NULL, (GAsyncReadyCallback)profile_set_ready, task);
        g_task_set_task_data (profile_task, profile_ctx, (GDestroyNotify)profile_set_3gpp_context_free);
        g_task_set_check_cancellable (profile_task, FALSE);

        /* Start! */
        profile_set_3gpp_context_step (profile_task);
        return;
    }

    case CONNECT_3GPP_CONTEXT_STEP_APNACT: {
        const gchar         *apn;
        g_autofree gchar *apn_act_command;

        // TODO: wait +CIREPI:
        sleep(3);

        apn = mm_bearer_properties_get_apn (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
        apn_act_command = g_strdup_printf ("+EAPNACT=1,\"%s\",\"default\",0", apn);

        mm_base_modem_at_command_full (MM_BASE_MODEM (ctx->modem),
                                       ctx->data,
                                       apn_act_command,
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)apn_act_done,
                                       task);
        return;
    }

    case CONNECT_3GPP_CONTEXT_STEP_IP_CONFIG: {
        g_autofree gchar *ip_info_command;

        // AT+EPDN=<aid>, "ifst", <ifst_status + protocol_class_bitmap>
        // <ifst_status> :
        //       0     -> update interface status without waiting
        //       16    -> wait for interface up
        // <protocol_class_bitmap> :
        //       0     -> unknown
        //       1     -> wait for ipv4 address
        //       2     -> wait for ipv6 address
        //       3     -> wait for ipv4 and ipv6 address
        //       4     -> wait for any address
        //
        // Response:
        //     +EPDN:<aid>,"new",<rat type>,<interface id>,<mtu>,<address type>,<address1>[,<address2>]
        //     +EPDN:<aid>,"update",<interface id>,<address type>,<address1>[,<address2>]
        //     +EPDN:<aid>,"err",<err>

        // wait for interface up + wait for any address = 20
        ip_info_command = g_strdup_printf ("+EPDN=%u,\"ifst\",20", self->priv->uid);

        mm_base_modem_at_command_full (MM_BASE_MODEM (ctx->modem),
                                       ctx->data,
                                       ip_info_command,
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)ip_info_response,
                                       task);

        return;
    }

    case CONNECT_3GPP_CONTEXT_STEP_DNS_CONFIG: {
        g_autofree gchar *cgcontrdp_command;

        //+CGCONTRDP response a list of [+CGCONTRDP: <aid>,<bearer_id>,<apn>[,<local_addr and subnet_mask>
        //[,<gw_addr>[,<DNS_prim_addr>[,<DNS_sec_addr>[,<P-CSCF_prim_addr>[,<P-CSCF_sec_addr>
        //[,<IM_CN_Signalling_Flag>[,<LIPA_indication>[,<IPv4_MTU>[,<WLAN_Offload>[,<Local_Addr_Ind>
        //[,<Non-IP_MTU>]]]]]]]]]]]]]

        cgcontrdp_command = g_strdup_printf ("+CGCONTRDP=%u", self->priv->uid);

        mm_base_modem_at_command_full (MM_BASE_MODEM (ctx->modem),
                                       ctx->data,
                                       cgcontrdp_command,
                                       MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)cgcontrdp_response,
                                       task);

        return;
    }

    case CONNECT_3GPP_CONTEXT_STEP_LAST: {
        MMPortNet *net_port;

        net_port = port_net_new(ctx->interface_name);

        /* Setup result */
        g_task_return_pointer (
            task,
            mm_bearer_connect_result_new (MM_PORT (net_port), ctx->ipv4_config, NULL),
            (GDestroyNotify)mm_bearer_connect_result_unref);

        g_object_unref (task);
        g_object_unref (net_port);
        return;
    }

    default:
        g_assert_not_reached ();
    }
}

static void
connect_3gpp (MMBroadbandBearer *_self,
              MMBroadbandModem *modem,
              MMPortSerialAt *primary,
              MMPortSerialAt *secondary,
              GCancellable *cancellable,
              GAsyncReadyCallback callback,
              gpointer user_data)
{
    MMBroadbandBearerMtk *self = MM_BROADBAND_BEARER_MTK (_self);
    Connect3gppContext *ctx;
    GTask *task;
    GError *error = NULL;

    g_assert (primary != NULL);

    /* Setup connection context */
    ctx = g_slice_new0 (Connect3gppContext);
    ctx->modem = MM_BASE_MODEM (g_object_ref (modem));
    ctx->step = CONNECT_3GPP_CONTEXT_STEP_FIRST;
    ctx->primary = g_object_ref (primary);
    ctx->data = common_get_at_data_port (MM_BROADBAND_BEARER (self), ctx->modem, &error);

    ctx->ipv4_config = mm_bearer_ip_config_new ();
    mm_bearer_ip_config_set_method (ctx->ipv4_config, MM_BEARER_IP_METHOD_STATIC);

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)connect_3gpp_context_free);
    g_task_set_check_cancellable (task, FALSE);

    /* Run! */
    connect_3gpp_context_step (task);
}

MMBaseBearer *
mm_broadband_bearer_mtk_new_finish (GAsyncResult *res,
                                    GError **error)
{
    GObject *source;
    GObject *bearer;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    /* Only export valid bearers */
    mm_base_bearer_export (MM_BASE_BEARER (bearer));

    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_mtk_new (MMBroadbandModem *modem,
                             MMBearerProperties *config,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_MTK,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM, modem,
        MM_BASE_BEARER_CONFIG, config,
        NULL);
}

static void
mm_broadband_bearer_mtk_init (MMBroadbandBearerMtk *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_BEARER_MTK,
                                              MMBroadbandBearerMtkPrivate);

    /* Defaults */
    self->priv->uid = -1;
}

static void
mm_broadband_bearer_mtk_class_init (MMBroadbandBearerMtkClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBaseBearerClass *base_bearer_class = MM_BASE_BEARER_CLASS (klass);
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandBearerMtkPrivate));

    base_bearer_class->load_connection_status = NULL;
    base_bearer_class->load_connection_status_finish = NULL;
#if defined WITH_SYSTEMD_SUSPEND_RESUME
    base_bearer_class->reload_connection_status = NULL;
    base_bearer_class->reload_connection_status_finish = NULL;
#endif

    broadband_bearer_class->connect_3gpp = connect_3gpp;
    broadband_bearer_class->connect_3gpp_finish = connect_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;

}
