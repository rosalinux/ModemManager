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

#ifndef MM_BROADBAND_MODEM_QMI_SIMTECH_H
#define MM_BROADBAND_MODEM_QMI_SIMTECH_H

#include "mm-broadband-modem-qmi.h"

G_BEGIN_DECLS

#define MM_TYPE_BROADBAND_MODEM_QMI_SIMTECH (mm_broadband_modem_qmi_simtech_get_type ())

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MMBroadbandModemQmi, g_object_unref);
G_DECLARE_FINAL_TYPE (MMBroadbandModemQmiSimtech,
                      mm_broadband_modem_qmi_simtech,
                      MM, BROADBAND_MODEM_QMI_SIMTECH,
                      MMBroadbandModemQmi);

MMBroadbandModemQmiSimtech * mm_broadband_modem_qmi_simtech_new (const gchar  *device,
                                                                 const gchar **drivers,
                                                                 const gchar  *plugin,
                                                                 guint16       vendor_id,
                                                                 guint16       product_id);

G_END_DECLS

#endif /* MM_BROADBAND_MODEM_QMI_SIMTECH_H */
