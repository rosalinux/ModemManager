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
 * Copyright (C) 2015 Riccardo Vangelisti <riccardo.vangelisti@sadel.it>
 * Copyright (C) 2018 Purism SPC
 */

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem-at.h"
#include "mm-call-qmi-simtech.h"
#include "mm-log.h"


struct _MMCallQmiSimtech
{
    MMBaseCall parent_instance;

    /** Whether there is PCM audio support */
    gboolean pcm_audio_supported;

    /** The regular expression to match "VOICE CALL: ..." Unrequested
        Response Codes */
    GRegex *voice_call_regex;

    /** For CLCC URCs */
    GRegex *call_list_regex;
};

G_DEFINE_TYPE (MMCallQmiSimtech, mm_call_qmi_simtech, MM_TYPE_BASE_CALL)

enum {
    PROP_0,
    PROP_PCM_AUDIO_SUPPORTED,
    PROP_LAST
};

static GParamSpec *properties[PROP_LAST];


/*****************************************************************************/
/* Audio channel setup and cleanup */

static gboolean
setup_audio_channel_finish (MMBaseCall         *self,
                            GAsyncResult       *res,
                            MMPort            **audio_port,
                            MMCallAudioFormat **audio_format,
                            GError            **error)
{
    // FIXME: set audio_{port,format}
    return g_task_propagate_boolean (G_TASK (res), error);
}

static gboolean
cleanup_audio_channel_finish (MMBaseCall         *self,
                              GAsyncResult       *res,
                              GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
pcmreg_ready (MMBaseModem  *modem,
              GAsyncResult *res,
              GTask        *task)
{
    GError      *error = NULL;
    const gchar *response = NULL;
    const gchar *what = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        const gchar *what = g_task_get_task_data (task);
        mm_dbg ("Error %s audio streaming: '%s'", what, error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_dbg ("Successfully completed %s PCM audio for call", what);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
audio_channel_command (MMBaseCall           *_self,
                       const gchar          *command,
                       const gchar          *what,
                       GAsyncReadyCallback   callback,
                       gpointer              user_data)
{
    MMCallQmiSimtech  *self;
    GTask             *task;
    MMBaseModem       *modem = NULL;

    self = MM_CALL_QMI_SIMTECH (_self);

    task = g_task_new (self, NULL, callback, user_data);

    if (!self->pcm_audio_supported) {
        mm_dbg ("PCM audio not supported; not %s for call", what);
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_object_get (self,
                  MM_BASE_CALL_MODEM, &modem,
                  NULL);
    g_task_set_task_data (task, (gpointer)what, NULL);

    mm_dbg ("PCM audio supported, %s for call", what);
    mm_base_modem_at_command (modem,
                              command,
                              5,
                              FALSE,
                              (GAsyncReadyCallback)pcmreg_ready,
                              task);
}


static void
setup_audio_channel (MMBaseCall           *self,
                     GAsyncReadyCallback   callback,
                     gpointer              user_data)
{
    audio_channel_command (self, "+CPCMREG=1", "enabling",
                           callback, user_data);
}


static void
cleanup_audio_channel (MMBaseCall           *self,
                       GAsyncReadyCallback   callback,
                       gpointer              user_data)
{
    audio_channel_command (self, "+CPCMREG=0", "disabling",
                           callback, user_data);
}


/*****************************************************************************/
/* Unsolicited events */


static void
simtech_voice_call_received (MMPortSerialAt *port,
                             GMatchInfo *match_info,
                             MMCallQmiSimtech *self)
{
    mm_dbg ("(%s) Received voice call URC with argument `%s'",
            mm_port_get_device (MM_PORT (port)),
            g_match_info_fetch (match_info, 1));
}


static void
simtech_clcc_received (MMPortSerialAt   *port,
                       GMatchInfo       *match_info,
                       MMCallQmiSimtech *self)
{
    // We only support a single call so we ignore the id and just
    // treat every CLCC line as if it was for the single call

    static const gchar *call_stat_names[] = {
        [0] = "active",
        [1] = "held",
        [2] = "dialing (MO)",
        [3] = "alerting (MO)",
        [4] = "incoming (MT)",
        [5] = "waiting (MT)",
        [6] = "disconnect",
    };
    guint id, stat, call_state = 42;

    if (!mm_get_uint_from_match_info (match_info, 1, &id)) {
        return;
    }

    if (!mm_get_uint_from_match_info (match_info, 3, &stat)) {
        return;
    }

    if (stat < G_N_ELEMENTS (call_stat_names)) {
        mm_dbg ("SimTech call id '%u' state: '%s'", id, call_stat_names[stat]);
	} else {
        mm_dbg ("SimTech call id '%u' state unknown: '%u'", id, stat);
    }

    switch (stat) {
    case 1: /* ignore hold state, we don't support this yet. */
    case 2: /* ignore dialing state, we already handle this in the parent */
    case 4: /* ignore MT ringing state, we already handle this in the parent */
    case 5: /* ignore MT waiting state */
    case 6: /* ignore disconnect state, dealt with by NO CARRIER handler in super class */
        return;
    default:
        call_state = mm_gdbus_call_get_state (MM_GDBUS_CALL (self));
    }

    switch (stat) {
    case 0:
        /* ringing to active */
        if (call_state == MM_CALL_STATE_RINGING_OUT) {
            mm_base_call_change_state (MM_BASE_CALL (self), MM_CALL_STATE_ACTIVE,
                                       MM_CALL_STATE_REASON_ACCEPTED);
        }
        break;
    case 3:
        /* dialing to ringing */
        if (call_state == MM_CALL_STATE_DIALING) {
            mm_base_call_change_state (MM_BASE_CALL (self), MM_CALL_STATE_RINGING_OUT,
                                       MM_CALL_STATE_REASON_OUTGOING_STARTED);
        }
        break;
    }
}


static gboolean
common_setup_cleanup_unsolicited_events (MMCallQmiSimtech  *self,
                                         gboolean           enable,
                                         GError           **error)
{
    MMBaseModem    *modem = NULL;
    MMPortSerialAt *port;

    g_object_get (self,
                  MM_BASE_CALL_MODEM, &modem,
                  NULL);
    g_assert (MM_IS_BASE_MODEM (modem));

    port = mm_base_modem_peek_port_primary (modem);
    if (port) {
        const gchar *device;

        device = mm_port_get_device (MM_PORT (port));

        mm_dbg ("(%s) %s +CLCC URC handler",
                device, enable ? "Adding" : "Removing");
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->call_list_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)simtech_clcc_received : NULL,
            enable ? self : NULL,
            NULL);

        mm_dbg ("(%s) %s VOICE CALL URC handler",
                device, enable ? "Adding" : "Removing");
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->voice_call_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)simtech_voice_call_received : NULL,
            enable ? self : NULL,
            NULL);
    }

    return TRUE;
}


static gboolean
setup_unsolicited_events (MMBaseCall  *self,
                          GError     **error)
{
    MMBaseCallClass *parent = MM_BASE_CALL_CLASS (mm_call_qmi_simtech_parent_class);
    gboolean ok;

    ok = parent->setup_unsolicited_events (self, error);
    if (!ok) {
        return ok;
    }

    return common_setup_cleanup_unsolicited_events (MM_CALL_QMI_SIMTECH (self), TRUE, error);
}


static gboolean
cleanup_unsolicited_events (MMBaseCall  *self,
                            GError     **error)
{
    MMBaseCallClass *parent = MM_BASE_CALL_CLASS (mm_call_qmi_simtech_parent_class);
    gboolean ok;

    ok = parent->setup_unsolicited_events (self, error);

    return
        common_setup_cleanup_unsolicited_events (MM_CALL_QMI_SIMTECH (self), FALSE, error)
        && ok;
}

/*****************************************************************************/

MMBaseCall *
mm_call_qmi_simtech_new (MMBaseModem     *modem,
			 MMCallDirection  direction,
			 const gchar     *number,
			 gboolean         pcm_audio_supported)
{
    return MM_BASE_CALL (g_object_new (MM_TYPE_CALL_QMI_SIMTECH,
                                       MM_BASE_CALL_MODEM,                       modem,
                                       "direction",                              direction,
                                       "number",                                 number,
                                       MM_CALL_QMI_SIMTECH_PCM_AUDIO_SUPPORTED,  pcm_audio_supported,
                                       MM_BASE_CALL_SUPPORTS_DIALING_TO_RINGING, TRUE,
                                       MM_BASE_CALL_SUPPORTS_RINGING_TO_ACTIVE,  TRUE,
                                       NULL));
}


static void
mm_call_qmi_simtech_init (MMCallQmiSimtech *self)
{
    self->voice_call_regex = g_regex_new ("\\r\\n\\VOICE CALL:\\s*(.*)\\r\\n",
                                          G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    // +CLCC:<id1>,<dir>,<stat>,<mode>,<mpty>[,<number>,<type>[,<alpha>]][<CR><LF>
    // -- SIM7100 AT Command Manual, V1.00, p. 135
    self->call_list_regex = g_regex_new
        ("\\r\\n"
         "\\+CLCC:\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)"
         "(?:\\s*(.+)\\s*,\\s*(\\d+)(?:\\s*,\\s*(.+))?)?"
         "\\r\\n",
         G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}


static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
    MMCallQmiSimtech *self = MM_CALL_QMI_SIMTECH (object);

    switch (prop_id) {
    case PROP_PCM_AUDIO_SUPPORTED:
        self->pcm_audio_supported = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}


static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
    MMCallQmiSimtech *self = MM_CALL_QMI_SIMTECH (object);

    switch (prop_id) {
    case PROP_PCM_AUDIO_SUPPORTED:
        g_value_set_boolean (value, self->pcm_audio_supported);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}


static void
finalize (GObject *object)
{
    MMCallQmiSimtech *self = MM_CALL_QMI_SIMTECH (object);

    g_regex_unref (self->call_list_regex);
    g_regex_unref (self->voice_call_regex);

    G_OBJECT_CLASS (mm_call_qmi_simtech_parent_class)->finalize (object);
}


static void
mm_call_qmi_simtech_class_init (MMCallQmiSimtechClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMBaseCallClass *base_call_class = MM_BASE_CALL_CLASS (klass);

    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->finalize     = finalize;

    base_call_class->setup_unsolicited_events     = setup_unsolicited_events;
    base_call_class->cleanup_unsolicited_events   = cleanup_unsolicited_events;
    base_call_class->setup_audio_channel          = setup_audio_channel;
    base_call_class->setup_audio_channel_finish   = setup_audio_channel_finish;
    base_call_class->cleanup_audio_channel        = cleanup_audio_channel;
    base_call_class->cleanup_audio_channel_finish = cleanup_audio_channel_finish;

    properties[PROP_PCM_AUDIO_SUPPORTED] =
        g_param_spec_boolean (MM_CALL_QMI_SIMTECH_PCM_AUDIO_SUPPORTED,
			      "PCM Audio Supported",
			      "Whether the modem supported PCM audio over TTY",
			      FALSE,
			      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

    g_object_class_install_properties (object_class, PROP_LAST, properties);
}
