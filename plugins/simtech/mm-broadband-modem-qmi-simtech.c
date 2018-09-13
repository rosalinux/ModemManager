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
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 * Copyright (C) 2018 Purism SPC
 */

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "ModemManager.h"
#include "mm-broadband-modem-qmi-simtech.h"
#include "mm-call-qmi-simtech.h"
#include "mm-iface-modem-voice.h"
#include "mm-base-modem-at.h"
#include "mm-log.h"

struct _MMBroadbandModemQmiSimtech
{
    MMBroadbandModemQmi parent_instance;

    /** Whether there is PCM audio support */
    gboolean pcm_audio;
};



static MMIfaceModemVoice *iface_modem_voice_parent;

static void iface_modem_voice_init (MMIfaceModemVoice *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemQmiSimtech,
                        mm_broadband_modem_qmi_simtech,
                        MM_TYPE_BROADBAND_MODEM_QMI, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_VOICE, iface_modem_voice_init))


static gboolean
propagate_boolean_finish (MMIfaceModemVoice *self,
                          GAsyncResult *res,
                          GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}


static gboolean
vmute_check_processor (MMBaseModem *base_modem,
                       gpointer none,
                       const gchar *command,
                       const gchar *response,
                       gboolean last_command,
                       const GError *error,
                       GVariant **result,
                       GError **result_error)
{
    if (error && error->domain != MM_MOBILE_EQUIPMENT_ERROR) {
        *result_error = g_error_copy (error);
        mm_dbg ("Got non-AT error checking VMUTE: %s", error->message);
        return FALSE;
    }

    if (error) {
        /* We got an AT error so no support */
        *result = g_variant_new_boolean (FALSE);
        g_variant_ref_sink (*result);
        return TRUE;
    }

    return FALSE;
}


static gboolean
pcmreg_check_processor (MMBaseModem *base_modem,
                        gpointer none,
                        const gchar *command,
                        const gchar *response,
                        gboolean last_command,
                        const GError *error,
                        GVariant **result,
                        GError **result_error)
{
    MMBroadbandModemQmiSimtech *self;

    if (error && error->domain != MM_MOBILE_EQUIPMENT_ERROR) {
        mm_dbg ("Got non-AT error checking PCMREG: %s", error->message);
        *result_error = g_error_copy (error);
        return FALSE;
    }

    /* We have voice call support regardless */
    *result = g_variant_new_boolean (TRUE);
    g_variant_ref_sink (*result);

    self = MM_BROADBAND_MODEM_QMI_SIMTECH (base_modem);
    self->pcm_audio = !error;

    mm_dbg ("%s have PCM audio support",
            error ? "Don't" : "Do");

    return TRUE;
}


/* First we check for voice call support with AT+VMUTE=? and if there
   is support we then check for PCM-audio-over-TTY with AT+CPCMREG=? */
static const MMBaseModemAtCommand voice_check_sequence[] = {
    { "+VMUTE=?", 10, /* FIXME: cache this? */ FALSE, vmute_check_processor },
    { "+CPCMREG=?", 10, FALSE, pcmreg_check_processor },
    { NULL }
};


static void
voice_check_sequence_ready (MMBaseModem *self,
                            GAsyncResult *res,
                            GTask *task)
{
    GError *error = NULL;
    GVariant *result;

    result = mm_base_modem_at_sequence_finish (self, res, NULL, &error);
    if (!result) {
        g_task_return_error (task, error);
    } else {
        g_task_return_boolean (task, g_variant_get_boolean (result));
        g_variant_unref (result);
    }

    g_object_unref (task);
}


static void
modem_voice_check_support (MMIfaceModemVoice *self,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    mm_base_modem_at_sequence (
        MM_BASE_MODEM (self),
        voice_check_sequence,
        NULL, /* response_processor_context */
        NULL, /* response_processor_context_free */
        (GAsyncReadyCallback)voice_check_sequence_ready,
        task);
}


/*****************************************************************************/
/* Enabling/disabling unsolicited events (Voice interface) */

static void
own_voice_enable_disable_unsolicited_events_ready (MMBaseModem *self,
                                                   GAsyncResult *res,
                                                   GTask *task)
{
    GError *error = NULL;

    mm_base_modem_at_command_full_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
    } else {
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
parent_voice_enable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                              GAsyncResult      *res,
                                              GTask             *task)
{
    GError *error = NULL;

    if (!iface_modem_voice_parent->enable_unsolicited_events_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Our own enable now */
    mm_base_modem_at_command_full (
        MM_BASE_MODEM (self),
        mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
        "+CLCC=1",
        5,
        FALSE, /* allow_cached */
        FALSE, /* raw */
        NULL, /* cancellable */
        (GAsyncReadyCallback)own_voice_enable_disable_unsolicited_events_ready,
        task);
}

static void
modem_voice_enable_unsolicited_events (MMIfaceModemVoice   *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's enable */
    iface_modem_voice_parent->enable_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_enable_unsolicited_events_ready,
        task);
}

static void
modem_voice_disable_unsolicited_events (MMIfaceModemVoice   *self,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Note: no parent disable method */

    mm_base_modem_at_command_full (
        MM_BASE_MODEM (self),
        mm_base_modem_peek_port_primary (MM_BASE_MODEM (self)),
        "+CLCC=0",
        5,
        FALSE, /* allow_cached */
        FALSE, /* raw */
        NULL, /* cancellable */
        (GAsyncReadyCallback)own_voice_enable_disable_unsolicited_events_ready,
        task);
}


/*****************************************************************************/

static MMBaseCall *
modem_voice_create_call (MMIfaceModemVoice *self,
                         MMCallDirection    direction,
                         const gchar       *number)
{
    return mm_call_qmi_simtech_new (MM_BASE_MODEM (self), direction, number,
                                    MM_BROADBAND_MODEM_QMI_SIMTECH (self)->pcm_audio);
}


MMBroadbandModemQmiSimtech *
mm_broadband_modem_qmi_simtech_new (const gchar *device,
                                    const gchar **drivers,
                                    const gchar *plugin,
                                    guint16 vendor_id,
                                    guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QMI_SIMTECH,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}


static void
mm_broadband_modem_qmi_simtech_init (MMBroadbandModemQmiSimtech *self)
{
}


static void
iface_modem_voice_init (MMIfaceModemVoice *iface)
{
    iface_modem_voice_parent = g_type_interface_peek_parent (iface);

    iface->check_support = modem_voice_check_support;
    iface->check_support_finish = propagate_boolean_finish;

    iface->enable_unsolicited_events = modem_voice_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = propagate_boolean_finish;
    iface->disable_unsolicited_events = modem_voice_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = propagate_boolean_finish;

    iface->create_call = modem_voice_create_call;
}


static void
mm_broadband_modem_qmi_simtech_class_init (MMBroadbandModemQmiSimtechClass *klass)
{
}
