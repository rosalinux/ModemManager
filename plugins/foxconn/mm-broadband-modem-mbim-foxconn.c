/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2018-2019 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-location.h"
#include "mm-broadband-modem-mbim-foxconn.h"

#if defined WITH_QMI && QMI_MBIM_QMUX_SUPPORTED
# include "mm-iface-modem-firmware.h"
# include "mm-shared-qmi.h"
#endif

static void iface_modem_location_init (MMIfaceModemLocation *iface);

#if defined WITH_QMI && QMI_MBIM_QMUX_SUPPORTED
static void iface_modem_firmware_init (MMIfaceModemFirmware *iface);
#endif

static MMIfaceModemLocation *iface_modem_location_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMbimFoxconn, mm_broadband_modem_mbim_foxconn, MM_TYPE_BROADBAND_MODEM_MBIM, 0,
#if defined WITH_QMI && QMI_MBIM_QMUX_SUPPORTED
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_FIRMWARE, iface_modem_firmware_init)
#endif
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_LOCATION, iface_modem_location_init))

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED
} FeatureSupport;

struct _MMBroadbandModemMbimFoxconnPrivate {
    FeatureSupport unmanaged_gps_support;
};


#if defined WITH_QMI && QMI_MBIM_QMUX_SUPPORTED

/*****************************************************************************/
/* Firmware update settings
 *
 * We only support reporting firmware update settings when QMI support is built,
 * because this is the only clean way to get the expected firmware version to
 * report.
 */

static MMFirmwareUpdateSettings *
firmware_load_update_settings_finish (MMIfaceModemFirmware  *self,
                                      GAsyncResult          *res,
                                      GError               **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
foxconn_get_firmware_version_ready (QmiClientDms *client,
                                    GAsyncResult *res,
                                    GTask        *task)
{
    QmiMessageDmsFoxconnGetFirmwareVersionOutput *output;
    GError                                       *error = NULL;
    MMFirmwareUpdateSettings                     *update_settings = NULL;
    const gchar                                  *str;
    MMIfaceModemFirmware                         *self;
    guint                                         vendor_id;
    guint                                         product_id;

    output = qmi_client_dms_foxconn_get_firmware_version_finish (client, res, &error);
    if (!output || !qmi_message_dms_foxconn_get_firmware_version_output_get_result (output, &error))
        goto out;

    /* Create update settings now:
     * 0x105b is the T99W175 module, T99W175 supports QDU,
     * T99W265(0x0489:0xe0da ; 0x0489:0xe0db): supports QDU
     * else support FASTBOOT and QMI PDC.
     */
    self = g_task_get_source_object (task);
    vendor_id = mm_base_modem_get_vendor_id (MM_BASE_MODEM (self));
    product_id = mm_base_modem_get_product_id (MM_BASE_MODEM (self));
    if (vendor_id == 0x105b || (vendor_id == 0x0489 && (product_id  == 0xe0da || product_id == 0xe0db)))
        update_settings = mm_firmware_update_settings_new (MM_MODEM_FIRMWARE_UPDATE_METHOD_MBIM_QDU);
    else {
        update_settings = mm_firmware_update_settings_new (MM_MODEM_FIRMWARE_UPDATE_METHOD_FASTBOOT |
                                                           MM_MODEM_FIRMWARE_UPDATE_METHOD_QMI_PDC);
        mm_firmware_update_settings_set_fastboot_at (update_settings, "AT^FASTBOOT");
    }

    qmi_message_dms_foxconn_get_firmware_version_output_get_version (output, &str, NULL);
    mm_firmware_update_settings_set_version (update_settings, str);

 out:
    if (error)
        g_task_return_error (task, error);
    else {
        g_assert (update_settings);
        g_task_return_pointer (task, update_settings, g_object_unref);
    }
    g_object_unref (task);
    if (output)
        qmi_message_dms_foxconn_get_firmware_version_output_unref (output);
}

static void
firmware_load_update_settings (MMIfaceModemFirmware *self,
                               GAsyncReadyCallback   callback,
                               gpointer              user_data)
{
    GTask                                       *task;
    QmiMessageDmsFoxconnGetFirmwareVersionInput *input = NULL;
    QmiClient                                   *client = NULL;
    guint                                        vendor_id;
    guint                                        product_id;

    task = g_task_new (self, NULL, callback, user_data);

    client = mm_shared_qmi_peek_client (MM_SHARED_QMI (self),
                                        QMI_SERVICE_DMS,
                                        MM_PORT_QMI_FLAG_DEFAULT,
                                        NULL);
    if (!client) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "Unable to load version info: no QMI DMS client available");
        g_object_unref (task);
        return;
    }

    vendor_id = mm_base_modem_get_vendor_id (MM_BASE_MODEM (self));
    product_id = mm_base_modem_get_product_id (MM_BASE_MODEM (self));
    input = qmi_message_dms_foxconn_get_firmware_version_input_new ();
    /* 0x105b is the T99W175 module, T99W175/T99W265 need to compare the apps version. */
    if (vendor_id == 0x105b || (vendor_id == 0x0489 && (product_id  == 0xe0da || product_id == 0xe0db)))
        qmi_message_dms_foxconn_get_firmware_version_input_set_version_type (
            input,
            QMI_DMS_FOXCONN_FIRMWARE_VERSION_TYPE_FIRMWARE_MCFG_APPS,
            NULL);
    else
        qmi_message_dms_foxconn_get_firmware_version_input_set_version_type (
            input,
            QMI_DMS_FOXCONN_FIRMWARE_VERSION_TYPE_FIRMWARE_MCFG,
            NULL);
    qmi_client_dms_foxconn_get_firmware_version (
        QMI_CLIENT_DMS (client),
        input,
        10,
        NULL,
        (GAsyncReadyCallback)foxconn_get_firmware_version_ready,
        task);
    qmi_message_dms_foxconn_get_firmware_version_input_unref (input);
}

#endif

/*****************************************************************************/
/* Location capabilities loading (Location interface) */

static MMModemLocationSource
location_load_capabilities_finish (MMIfaceModemLocation  *self,
                                   GAsyncResult          *res,
                                   GError               **error)
{
    GError *inner_error = NULL;
    gssize value;

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_LOCATION_SOURCE_NONE;
    }
    return (MMModemLocationSource)value;
}

static void
custom_location_load_capabilities (GTask                 *task,
                                   MMModemLocationSource  sources)
{
    MMBroadbandModemMbimFoxconn *self;

    self = g_task_get_source_object (task);

    /* If we have a GPS port and an AT port, enable unmanaged GPS support */
    if (mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)) &&
        mm_base_modem_peek_port_gps (MM_BASE_MODEM (self))) {
        self->priv->unmanaged_gps_support = FEATURE_SUPPORTED;
        sources |= MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED;
    }

    /* So we're done, complete */
    g_task_return_int (task, sources);
    g_object_unref (task);
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult         *res,
                                GTask                *task)
{
    MMModemLocationSource  sources;
    GError                *error = NULL;

    sources = iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    custom_location_load_capabilities (task, sources);
}

static void
location_load_capabilities (MMIfaceModemLocation *self,
                            GAsyncReadyCallback   callback,
                            gpointer              user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's setup, if any. If MM is built without QMI support,
     * the MBIM modem won't have any location capabilities. */
    if (iface_modem_location_parent &&
        iface_modem_location_parent->load_capabilities &&
        iface_modem_location_parent->load_capabilities_finish) {
        iface_modem_location_parent->load_capabilities (self,
                                                        (GAsyncReadyCallback)parent_load_capabilities_ready,
                                                        task);
        return;
    }

    custom_location_load_capabilities (task, MM_MODEM_LOCATION_SOURCE_NONE);
}

/*****************************************************************************/
/* Disable location gathering (Location interface) */

static gboolean
disable_location_gathering_finish (MMIfaceModemLocation  *self,
                                   GAsyncResult          *res,
                                   GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_disable_location_gathering_ready (MMIfaceModemLocation *self,
                                         GAsyncResult         *res,
                                         GTask                *task)
{
    GError *error = NULL;

    if (!iface_modem_location_parent->disable_location_gathering_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_disable_location_gathering (GTask *task)
{
    MMIfaceModemLocation  *self;
    MMModemLocationSource  source;

    self = MM_IFACE_MODEM_LOCATION (g_task_get_source_object (task));
    source = GPOINTER_TO_UINT (g_task_get_task_data (task));

    if (iface_modem_location_parent &&
        iface_modem_location_parent->disable_location_gathering &&
        iface_modem_location_parent->disable_location_gathering_finish) {
        iface_modem_location_parent->disable_location_gathering (
            self,
            source,
            (GAsyncReadyCallback)parent_disable_location_gathering_ready,
            task);
        return;
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
unmanaged_gps_disabled_ready (MMBaseModem  *self,
                              GAsyncResult *res,
                              GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    parent_disable_location_gathering (task);
}

static void
disable_location_gathering (MMIfaceModemLocation  *_self,
                            MMModemLocationSource  source,
                            GAsyncReadyCallback    callback,
                            gpointer               user_data)
{
    MMBroadbandModemMbimFoxconn *self = MM_BROADBAND_MODEM_MBIM_FOXCONN (_self);
    GTask                       *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    /* We only support Unmanaged GPS at this level */
    if ((self->priv->unmanaged_gps_support != FEATURE_SUPPORTED) ||
        (source != MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        parent_disable_location_gathering (task);
        return;
    }

    mm_base_modem_at_command (MM_BASE_MODEM (_self),
                              "^NV=30007,01,\"00\"",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)unmanaged_gps_disabled_ready,
                              task);
}

/*****************************************************************************/
/* Enable location gathering (Location interface) */

static gboolean
enable_location_gathering_finish (MMIfaceModemLocation  *self,
                                  GAsyncResult          *res,
                                  GError               **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
unmanaged_gps_enabled_ready (MMBaseModem  *self,
                             GAsyncResult *res,
                             GTask        *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
custom_enable_location_gathering (GTask *task)
{
    MMBroadbandModemMbimFoxconn *self;
    MMModemLocationSource        source;

    self = g_task_get_source_object (task);
    source = GPOINTER_TO_UINT (g_task_get_task_data (task));

    /* We only support Unmanaged GPS at this level */
    if ((self->priv->unmanaged_gps_support != FEATURE_SUPPORTED) ||
        (source != MM_MODEM_LOCATION_SOURCE_GPS_UNMANAGED)) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "^NV=30007,01,\"01\"",
                              3,
                              FALSE,
                              (GAsyncReadyCallback)unmanaged_gps_enabled_ready,
                              task);
}

static void
parent_enable_location_gathering_ready (MMIfaceModemLocation *self,
                                        GAsyncResult         *res,
                                        GTask                *task)
{
    GError *error = NULL;

    if (!iface_modem_location_parent->enable_location_gathering_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    custom_enable_location_gathering (task);
}

static void
enable_location_gathering (MMIfaceModemLocation  *self,
                           MMModemLocationSource  source,
                           GAsyncReadyCallback    callback,
                           gpointer               user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (source), NULL);

    /* Chain up parent's gathering enable */
    if (iface_modem_location_parent &&
        iface_modem_location_parent->enable_location_gathering &&
        iface_modem_location_parent->enable_location_gathering_finish) {
        iface_modem_location_parent->enable_location_gathering (
            self,
            source,
            (GAsyncReadyCallback)parent_enable_location_gathering_ready,
            task);
        return;
    }

    custom_enable_location_gathering (task);
}

/*****************************************************************************/

MMBroadbandModemMbimFoxconn *
mm_broadband_modem_mbim_foxconn_new (const gchar  *device,
                                     const gchar **drivers,
                                     const gchar  *plugin,
                                     guint16       vendor_id,
                                     guint16       product_id)
{
    const gchar *carrier_config_mapping = NULL;

    /* T77W968 (DW5821e is also T77W968) modules use t77w968 carrier mapping table,
     * T99W175 modules use t99w175 carrier mapping table. */
    if ((vendor_id == 0x0489 && (product_id == 0xe0b4 || product_id == 0xe0b5)) ||
        (vendor_id == 0x413c && (product_id == 0x81d7 || product_id == 0x81e0)))
        carrier_config_mapping = PKGDATADIR "/mm-foxconn-t77w968-carrier-mapping.conf";
    else if (vendor_id == 0x105b && (product_id == 0xe0ab || product_id == 0xe0b0 || product_id == 0xe0b1))
        carrier_config_mapping = PKGDATADIR "/mm-foxconn-t99w175-carrier-mapping.conf";

    return g_object_new (MM_TYPE_BROADBAND_MODEM_MBIM_FOXCONN,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* MBIM bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED,              TRUE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_CONFIGURED,             FALSE,
                         MM_IFACE_MODEM_PERIODIC_SIGNAL_CHECK_DISABLED,      TRUE,
                         MM_IFACE_MODEM_LOCATION_ALLOW_GPS_UNMANAGED_ALWAYS, TRUE,
                         MM_IFACE_MODEM_CARRIER_CONFIG_MAPPING,              carrier_config_mapping,
                         NULL);
}

static void
mm_broadband_modem_mbim_foxconn_init (MMBroadbandModemMbimFoxconn *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BROADBAND_MODEM_MBIM_FOXCONN, MMBroadbandModemMbimFoxconnPrivate);
    self->priv->unmanaged_gps_support = FEATURE_SUPPORT_UNKNOWN;
}

static void
iface_modem_location_init (MMIfaceModemLocation *iface)
{
    iface_modem_location_parent = g_type_interface_peek_parent (iface);

    iface->load_capabilities = location_load_capabilities;
    iface->load_capabilities_finish = location_load_capabilities_finish;
    iface->enable_location_gathering = enable_location_gathering;
    iface->enable_location_gathering_finish = enable_location_gathering_finish;
    iface->disable_location_gathering = disable_location_gathering;
    iface->disable_location_gathering_finish = disable_location_gathering_finish;
}

#if defined WITH_QMI && QMI_MBIM_QMUX_SUPPORTED

static void
iface_modem_firmware_init (MMIfaceModemFirmware *iface)
{
    iface->load_update_settings = firmware_load_update_settings;
    iface->load_update_settings_finish = firmware_load_update_settings_finish;
}

#endif

static void
mm_broadband_modem_mbim_foxconn_class_init (MMBroadbandModemMbimFoxconnClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemMbimFoxconnPrivate));
}
