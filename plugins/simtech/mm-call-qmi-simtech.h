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

#ifndef MM_CALL_QMI_SIMTECH_H
#define MM_CALL_QMI_SIMTECH_H

#include <glib.h>
#include <glib-object.h>

#include "mm-base-call.h"

#define MM_CALL_QMI_SIMTECH_PCM_AUDIO_SUPPORTED "call-qmi-simtech-pcm-audio-supported"

#define MM_TYPE_CALL_QMI_SIMTECH (mm_call_qmi_simtech_get_type ())

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MMBaseCall, g_object_unref);
G_DECLARE_FINAL_TYPE (MMCallQmiSimtech,
                      mm_call_qmi_simtech,
                      MM, CALL_QMI_SIMTECH,
                      MMBaseCall);

MMBaseCall *mm_call_qmi_simtech_new (MMBaseModem     *modem,
                                     MMCallDirection  direction,
                                     const gchar     *number,
                                     gboolean         pcm_audio_supported);

#endif /* MM_CALL_QMI_SIMTECH_H */
