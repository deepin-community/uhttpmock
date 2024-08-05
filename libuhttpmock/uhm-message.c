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

#include "uhm-message-private.h"

struct _UhmMessage {
	GObject parent;

	char *method;
	SoupHTTPVersion http_version;
	guint status_code;
	char *reason_phrase;
	GUri *uri;
	SoupMessageBody *request_body;
	SoupMessageHeaders *request_headers;
	SoupMessageBody *response_body;
	SoupMessageHeaders *response_headers;
};

struct _UhmMessageClass {
	GObjectClass parent_class;
};

G_DEFINE_FINAL_TYPE (UhmMessage, uhm_message, G_TYPE_OBJECT)

enum {
	PROP_URI = 1,
	PROP_METHOD
};

static void
uhm_message_init (UhmMessage *msg)
{
	msg->http_version = SOUP_HTTP_1_0;
	msg->status_code = SOUP_STATUS_NONE;

	msg->request_body = soup_message_body_new ();
	msg->request_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);
	msg->response_body = soup_message_body_new ();
	msg->response_headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);
}

static void
free_request_and_response (UhmMessage *msg)
{
	soup_message_body_unref (msg->request_body);
	soup_message_headers_unref (msg->request_headers);
	soup_message_body_unref (msg->response_body);
	soup_message_headers_unref (msg->response_headers);
}

static void
uhm_message_finalize (GObject *obj)
{
	UhmMessage *msg = UHM_MESSAGE (obj);

	g_free (msg->method);
	g_free (msg->reason_phrase);
	g_clear_pointer (&msg->uri, g_uri_unref);

	free_request_and_response (msg);

	G_OBJECT_CLASS (uhm_message_parent_class)->finalize (obj);
}

static void
uhm_message_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	UhmMessage *msg = UHM_MESSAGE (object);

	switch (property_id) {
	case PROP_URI:
		g_value_set_boxed (value, g_uri_ref (msg->uri));
		break;
	case PROP_METHOD:
		g_value_set_string (value, msg->method);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
uhm_message_set_property (GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	UhmMessage *msg = UHM_MESSAGE (object);

	switch (property_id) {
	case PROP_URI:
		g_clear_pointer (&msg->uri, g_uri_unref);
		msg->uri = g_value_dup_boxed (value);
		break;
	case PROP_METHOD:
		g_clear_pointer (&msg->method, g_free);
		msg->method = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
uhm_message_class_init (UhmMessageClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->finalize = uhm_message_finalize;
	gobject_class->set_property = uhm_message_set_property;
	gobject_class->get_property = uhm_message_get_property;

	g_object_class_install_property (gobject_class, PROP_URI,
	                                 g_param_spec_boxed ("uri",
							     "URI", "URI.",
							     G_TYPE_URI,
							     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_class, PROP_METHOD,
	                                 g_param_spec_string ("method",
	                                                      "Method", "Method.",
	                                                      "POST",
	                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

/* private */

UhmMessage *
uhm_message_new_from_uri (const gchar *method, GUri *uri)
{
	return g_object_new (UHM_TYPE_MESSAGE,
			     "method", method,
			     "uri", uri,
			     NULL);
}

UhmMessage *
uhm_message_new_from_server_message (SoupServerMessage *smsg)
{
	UhmMessage *msg;

	msg = g_object_new (UHM_TYPE_MESSAGE,
			    "method", soup_server_message_get_method (smsg),
			    "uri", soup_server_message_get_uri (smsg),
			    NULL);

	msg->http_version = soup_server_message_get_http_version (smsg);
	msg->status_code = soup_server_message_get_status (smsg);
	msg->reason_phrase = g_strdup (soup_server_message_get_reason_phrase (smsg));

	free_request_and_response (msg);
	msg->request_body = soup_message_body_ref (soup_server_message_get_request_body (smsg));
	msg->request_headers = soup_message_headers_ref (soup_server_message_get_request_headers (smsg));
	msg->response_body = soup_message_body_ref (soup_server_message_get_response_body (smsg));
	msg->response_headers = soup_message_headers_ref (soup_server_message_get_response_headers (smsg));

	return msg;
}

void uhm_message_set_status (UhmMessage *message, guint status, const char *reason_phrase)
{
	message->status_code = status;
	g_clear_pointer (&message->reason_phrase, g_free);
	message->reason_phrase = g_strdup (reason_phrase);
}

void uhm_message_set_http_version (UhmMessage *message, SoupHTTPVersion version)
{
	message->http_version = version;
}

/* public */

const gchar *
uhm_message_get_method (UhmMessage *message)
{
	return message->method;
}

SoupHTTPVersion
uhm_message_get_http_version (UhmMessage *message)
{
	return message->http_version;
}

guint uhm_message_get_status (UhmMessage *message)
{
	return message->status_code;
}

const gchar *uhm_message_get_reason_phrase (UhmMessage *message)
{
	return message->reason_phrase;
}

GUri *uhm_message_get_uri (UhmMessage *message)
{
	return message->uri;
}

SoupMessageBody *uhm_message_get_request_body (UhmMessage *message)
{
	return message->request_body;
}

SoupMessageBody *uhm_message_get_response_body (UhmMessage *message)
{
	return message->response_body;
}

SoupMessageHeaders *uhm_message_get_request_headers (UhmMessage *message)
{
	return message->request_headers;
}

SoupMessageHeaders *uhm_message_get_response_headers (UhmMessage *message)
{
	return message->response_headers;
}

