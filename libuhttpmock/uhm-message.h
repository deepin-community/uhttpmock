/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * uhttpmock
 * Copyright (C) Igalia S.L. 2021 <dkolesa@igalia.com>
 *
 * uhttpmock is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * uhttpmock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with uhttpmock.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UHM_MESSAGE_H
#define UHM_MESSAGE_H

#include <glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define UHM_TYPE_MESSAGE		(uhm_message_get_type ())

G_DECLARE_FINAL_TYPE (UhmMessage, uhm_message, UHM, MESSAGE, GObject)

const gchar *uhm_message_get_method (UhmMessage *message);

SoupHTTPVersion uhm_message_get_http_version (UhmMessage *message);
void uhm_message_set_http_version (UhmMessage *message, SoupHTTPVersion version);

guint uhm_message_get_status (UhmMessage *message);
const gchar *uhm_message_get_reason_phrase (UhmMessage *message);
void uhm_message_set_status (UhmMessage *message, guint status, const gchar *reason_phrase);

GUri *uhm_message_get_uri (UhmMessage *message);

SoupMessageBody *uhm_message_get_request_body (UhmMessage *message);
SoupMessageBody *uhm_message_get_response_body (UhmMessage *message);

SoupMessageHeaders *uhm_message_get_request_headers (UhmMessage *message);
SoupMessageHeaders *uhm_message_get_response_headers (UhmMessage *message);

G_END_DECLS

#endif /* !UHM_MESSAGE_H */
