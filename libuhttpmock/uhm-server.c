/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * uhttpmock
 * Copyright (C) Philip Withnall 2013, 2015 <philip@tecnocode.co.uk>
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

/**
 * SECTION:uhm-server
 * @short_description: mock HTTP(S) server
 * @stability: Unstable
 * @include: libuhttpmock/uhm-server.h
 *
 * This is a mock HTTPS server which can be used to run unit tests of network client code on a loopback interface rather than on the real Internet.
 * At its core, it's a simple HTTPS server which runs on a loopback address on an arbitrary port. The code under test must be modified to send its
 * requests to this port, although #UhmResolver may be used to transparently redirect all IP addresses to the mock server.
 * A convenience layer on the mock server provides loading of and recording to trace files, which are sequences of request–response HTTPS message pairs
 * where each request is expected by the server (in order). On receiving an expected request, the mock server will return the relevant response and move
 * to expecting the next request in the trace file.
 *
 * The mock server currently only operates on a single network interface, on HTTPS (if #UhmServer:tls-certificate is set) or HTTP otherwise.
 * This may change in future. Your own TLS certificate can be provided to authenticate the server using #UhmServer:tls-certificate, or a dummy
 * TLS certificate can be used by calling uhm_server_set_default_tls_certificate(). This certificate is not signed by a CA, so the
 * #SoupSession:ssl-strict property must be set to %FALSE in client code
 * during (and only during!) testing.
 *
 * The server can operate in three modes: logging, testing, and comparing. These are set by #UhmServer:enable-logging and #UhmServer:enable-online.
 *  • Logging mode (#UhmServer:enable-logging: %TRUE, #UhmServer:enable-online: %TRUE): Requests are sent to the real server online, and the
 *    request–response pairs recorded to a log file.
 *  • Testing mode (#UhmServer:enable-logging: %FALSE, #UhmServer:enable-online: %FALSE): Requests are sent to the mock server, which responds
 *    from the trace file.
 *  • Comparing mode (#UhmServer:enable-logging: %FALSE, #UhmServer:enable-online: %TRUE): Requests are sent to the real server online, and
 *    the request–response pairs are compared against those in an existing log file to see if the log file is up-to-date.
 *
 * Starting with version 0.11.0 hosts are automatically extracted and stored in hosts trace files. These files are
 * used during replay so no host configuration using uhm_resolver_add_A() is necessary in code any more.
 * Requires libsoup 3.5.1 or higher. If that version of libsoup is not available, uhm_resolver_add_A() must continue
 * to be used.
 *
 * Since: 0.1.0
 */

#include <glib.h>
#include <glib/gi18n.h>
#include <libsoup/soup.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "uhm-default-tls-certificate.h"
#include "uhm-resolver.h"
#include "uhm-server.h"
#include "uhm-message-private.h"

GQuark
uhm_server_error_quark (void)
{
	return g_quark_from_static_string ("uhm-server-error-quark");
}

static void uhm_server_dispose (GObject *object);
static void uhm_server_finalize (GObject *object);
static void uhm_server_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec);
static void uhm_server_set_property (GObject *object, guint property_id, const GValue *value, GParamSpec *pspec);

static gboolean real_handle_message (UhmServer *self, UhmMessage *message);
static gboolean real_compare_messages (UhmServer *self, UhmMessage *expected_message, UhmMessage *actual_message);

static void server_handler_cb (SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer user_data);
static void load_file_stream_thread_cb (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void load_file_iteration_thread_cb (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);

static GDataInputStream *load_file_stream (GFile *trace_file, GCancellable *cancellable, GError **error);
static UhmMessage *load_file_iteration (GDataInputStream *input_stream, GUri *base_uri, GCancellable *cancellable, GError **error);

static void apply_expected_domain_names (UhmServer *self);

struct _UhmServerPrivate {
	/* UhmServer is based around HTTP/HTTPS, and cannot be extended to support other application-layer protocols.
	 * If libuhttpmock is extended to support other protocols (e.g. IMAP) in future, a new UhmImapServer should be
	 * created. It may be possible to share code with UhmServer, but the APIs for the two would be sufficiently
	 * different to not be mergeable.
	 *
	 * The SoupServer is *not* thread safe, so all calls to it must be marshalled through the server_context, which
	 * it runs in. */
	SoupServer *server;
	UhmResolver *resolver;
	GThread *server_thread;
	GMainContext *server_context;
	GMainLoop *server_main_loop;

	/* TLS certificate. */
	GTlsCertificate *tls_certificate;

	/* Server interface. */
	GSocketAddress *address;  /* owned */
	gchar *address_string;  /* owned; cache */
	guint port;

	/* Expected resolver domain names. */
	gchar **expected_domain_names;

	GFile *trace_file;
	GDataInputStream *input_stream;
	GFileOutputStream *output_stream;
	UhmMessage *next_message;
	guint message_counter; /* ID of the message within the current trace file */

	GFile *trace_directory;
	gboolean enable_online;
	gboolean enable_logging;

	GFile *hosts_trace_file;
	GFileOutputStream *hosts_output_stream;
	GHashTable *hosts;

	GByteArray *comparison_message;
	enum {
		UNKNOWN,
		REQUEST_DATA,
		REQUEST_TERMINATOR,
		RESPONSE_DATA,
		RESPONSE_TERMINATOR,
	} received_message_state;
};

enum {
	PROP_TRACE_DIRECTORY = 1,
	PROP_ENABLE_ONLINE,
	PROP_ENABLE_LOGGING,
	PROP_ADDRESS,
	PROP_PORT,
	PROP_RESOLVER,
	PROP_TLS_CERTIFICATE,
};

enum {
	SIGNAL_HANDLE_MESSAGE = 1,
	SIGNAL_COMPARE_MESSAGES,
	LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE_WITH_PRIVATE (UhmServer, uhm_server, G_TYPE_OBJECT)

static void
uhm_server_class_init (UhmServerClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

	gobject_class->get_property = uhm_server_get_property;
	gobject_class->set_property = uhm_server_set_property;
	gobject_class->dispose = uhm_server_dispose;
	gobject_class->finalize = uhm_server_finalize;

	klass->handle_message = real_handle_message;
	klass->compare_messages = real_compare_messages;

	/**
	 * UhmServer:trace-directory:
	 *
	 * Directory relative to which all trace files specified in calls to uhm_server_start_trace() will be resolved.
	 * This is not used for any other methods, but must be non-%NULL if uhm_server_start_trace() is called.
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_TRACE_DIRECTORY,
	                                 g_param_spec_object ("trace-directory",
	                                                      "Trace Directory", "Directory relative to which all trace files will be resolved.",
	                                                      G_TYPE_FILE,
	                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer:enable-online:
	 *
	 * %TRUE if network traffic should reach the Internet as normal; %FALSE to redirect it to the local mock server.
	 * Use this in conjunction with #UhmServer:enable-logging to either log online traffic, or replay logged traffic locally.
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_ENABLE_ONLINE,
	                                 g_param_spec_boolean ("enable-online",
	                                                       "Enable Online", "Whether network traffic should reach the Internet as normal.",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer:enable-logging:
	 *
	 * %TRUE if network traffic should be logged to a trace file (specified by calling uhm_server_start_trace()). This operates independently
	 * of whether traffic is online or being handled locally by the mock server.
	 * Use this in conjunction with #UhmServer:enable-online to either log online traffic, or replay logged traffic locally.
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_ENABLE_LOGGING,
	                                 g_param_spec_boolean ("enable-logging",
	                                                       "Enable Logging", "Whether network traffic should be logged to a trace file.",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer:address:
	 *
	 * Address of the local mock server if it's running, or %NULL otherwise. This will be non-%NULL between calls to uhm_server_run() and
	 * uhm_server_stop(). The address is a physical IP address, e.g. <code class="literal">127.0.0.1</code>.
	 *
	 * This should not normally need to be passed into client code under test, unless the code references IP addresses specifically. The mock server
	 * runs a DNS resolver which automatically redirects client requests for known domain names to this address (#UhmServer:resolver).
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_ADDRESS,
	                                 g_param_spec_string ("address",
	                                                      "Server Address", "Address of the local mock server if it's running.",
	                                                      NULL,
	                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer:port:
	 *
	 * Port of the local mock server if it's running, or <code class="literal">0</code> otherwise. This will be non-<code class="literal">0</code> between
	 * calls to uhm_server_run() and uhm_server_stop().
	 *
	 * It is intended that this port be passed into the client code under test, to substitute for the default HTTPS port (443) which it would otherwise
	 * use.
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_PORT,
	                                 g_param_spec_uint ("port",
	                                                    "Server Port", "Port of the local mock server if it's running",
	                                                    0, G_MAXUINT, 0,
	                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer:resolver:
	 *
	 * Mock resolver used to redirect HTTP requests from specified domain names to the local mock server instance. This will always be set while the
	 * server is running (between calls to uhm_server_run() and uhm_server_stop()), and is %NULL otherwise.
	 *
	 * Use the resolver specified in this property to add domain names which are expected to be requested by the current trace. Domain names not added
	 * to the resolver will be rejected by the mock server. The set of domain names in the resolver will be reset when uhm_server_stop() is
	 * called.
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_RESOLVER,
	                                 g_param_spec_object ("resolver",
	                                                      "Resolver", "Mock resolver used to redirect HTTP requests to the local mock server instance.",
	                                                      UHM_TYPE_RESOLVER,
	                                                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer:tls-certificate:
	 *
	 * TLS certificate for the mock server to use when serving HTTPS pages. If this is non-%NULL, the server will always use HTTPS. If it is %NULL,
	 * the server will always use HTTP. The TLS certificate may be changed after constructing the #UhmServer, but changes to the property will not
	 * take effect until the next call to uhm_server_run().
	 *
	 * A certificate and private key may be generated by executing:
	 * <code>openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -nodes</code>. These files may then be used to construct a
	 * #GTlsCertificate by calling g_tls_certificate_new_from_files().
	 *
	 * Alternatively, a default #GTlsCertificate which wraps a dummy certificate (not signed by any certificate authority) may be set by
	 * calling uhm_server_set_default_tls_certificate(). This may be used as the #UhmServer:tls-certificate if the code under test has no specific
	 * requirements of the certificate used by the mock server it's tested against.
	 *
	 * Since: 0.1.0
	 */
	g_object_class_install_property (gobject_class, PROP_TLS_CERTIFICATE,
	                                 g_param_spec_object ("tls-certificate",
	                                                      "TLS Certificate", "TLS certificate for the mock server to use when serving HTTPS pages.",
	                                                      G_TYPE_TLS_CERTIFICATE,
	                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	/**
	 * UhmServer::handle-message:
	 * @self: a #UhmServer
	 * @message: a message containing the incoming HTTP(S) request, and which the outgoing HTTP(S) response should be set on
	 *
	 * Emitted whenever the mock server is running and receives a request from a client. Test code may connect to this signal and implement a handler
	 * which builds and returns a suitable response for a given message. The default handler reads a request–response pair from the current trace file,
	 * matches the requests and then returns the given response. If the requests don't match, an error is raised.
	 *
	 * Signal handlers should return %TRUE if they have handled the request and set an appropriate response; and %FALSE otherwise.
	 *
	 * Since: 0.1.0
	 */
	signals[SIGNAL_HANDLE_MESSAGE] = g_signal_new ("handle-message", G_OBJECT_CLASS_TYPE (klass), G_SIGNAL_RUN_LAST,
	                                               G_STRUCT_OFFSET (UhmServerClass, handle_message),
	                                               g_signal_accumulator_true_handled, NULL,
	                                               g_cclosure_marshal_generic,
	                                               G_TYPE_BOOLEAN, 1,
	                                               UHM_TYPE_MESSAGE);

	/**
	 * UhmServer::compare-messages:
	 * @self: a #UhmServer
	 * @expected_message: a message containing the expected HTTP(S) message provided by #UhmServer::handle-message
	 * @actual_message: a message containing the incoming HTTP(S) request
	 *
	 * Emitted whenever the mock server must compare two #UhmMessage<!-- -->s for equality; e.g. when in the testing or comparison modes.
	 * Test code may connect to this signal and implement a handler which checks custom properties of the messages. The default handler compares
	 * the URI and method of the messages, but no headers and not the message bodies.
	 *
	 * Signal handlers should return %TRUE if the messages match; and %FALSE otherwise. The first signal handler executed when
	 * this signal is emitted wins.
	 *
	 * Since: 1.0.0
	 */
	signals[SIGNAL_COMPARE_MESSAGES] = g_signal_new ("compare-messages", G_OBJECT_CLASS_TYPE (klass), G_SIGNAL_RUN_LAST,
	                                               G_STRUCT_OFFSET (UhmServerClass, compare_messages),
	                                               g_signal_accumulator_first_wins, NULL,
	                                               g_cclosure_marshal_generic,
	                                               G_TYPE_BOOLEAN, 2,
	                                               UHM_TYPE_MESSAGE, UHM_TYPE_MESSAGE);
}

static void
uhm_server_init (UhmServer *self)
{
	self->priv = uhm_server_get_instance_private (self);
	self->priv->hosts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static void
uhm_server_dispose (GObject *object)
{
	UhmServerPrivate *priv = UHM_SERVER (object)->priv;

	g_clear_object (&priv->resolver);
	g_clear_object (&priv->server);
	g_clear_pointer (&priv->server_context, g_main_context_unref);
	g_clear_pointer (&priv->hosts, g_hash_table_unref);
	g_clear_object (&priv->hosts_trace_file);
	g_clear_object (&priv->hosts_output_stream);
	g_clear_object (&priv->trace_file);
	g_clear_object (&priv->input_stream);
	g_clear_object (&priv->output_stream);
	g_clear_object (&priv->next_message);
	g_clear_object (&priv->trace_directory);
	g_clear_pointer (&priv->server_thread, g_thread_unref);
	g_clear_pointer (&priv->comparison_message, g_byte_array_unref);
	g_clear_object (&priv->tls_certificate);

	/* Chain up to the parent class */
	G_OBJECT_CLASS (uhm_server_parent_class)->dispose (object);
}

static void
uhm_server_finalize (GObject *object)
{
	UhmServerPrivate *priv = UHM_SERVER (object)->priv;

	g_strfreev (priv->expected_domain_names);

	/* Chain up to the parent class */
	G_OBJECT_CLASS (uhm_server_parent_class)->finalize (object);
}

static void
uhm_server_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	UhmServerPrivate *priv = UHM_SERVER (object)->priv;

	switch (property_id) {
		case PROP_TRACE_DIRECTORY:
			g_value_set_object (value, priv->trace_directory);
			break;
		case PROP_ENABLE_ONLINE:
			g_value_set_boolean (value, priv->enable_online);
			break;
		case PROP_ENABLE_LOGGING:
			g_value_set_boolean (value, priv->enable_logging);
			break;
		case PROP_ADDRESS:
			g_value_set_string (value, uhm_server_get_address (UHM_SERVER (object)));
			break;
		case PROP_PORT:
			g_value_set_uint (value, priv->port);
			break;
		case PROP_RESOLVER:
			g_value_set_object (value, priv->resolver);
			break;
		case PROP_TLS_CERTIFICATE:
			g_value_set_object (value, priv->tls_certificate);
			break;
		default:
			/* We don't have any other property... */
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
			break;
	}
}

static void
uhm_server_set_property (GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	UhmServer *self = UHM_SERVER (object);

	switch (property_id) {
		case PROP_TRACE_DIRECTORY:
			uhm_server_set_trace_directory (self, g_value_get_object (value));
			break;
		case PROP_ENABLE_ONLINE:
			uhm_server_set_enable_online (self, g_value_get_boolean (value));
			break;
		case PROP_ENABLE_LOGGING:
			uhm_server_set_enable_logging (self, g_value_get_boolean (value));
			break;
		case PROP_TLS_CERTIFICATE:
			uhm_server_set_tls_certificate (self, g_value_get_object (value));
			break;
		case PROP_ADDRESS:
		case PROP_PORT:
		case PROP_RESOLVER:
			/* Read-only. */
		default:
			/* We don't have any other property... */
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
			break;
	}
}

typedef struct {
	GDataInputStream *input_stream;
	GUri *base_uri;
} LoadFileIterationData;

static void
load_file_iteration_data_free (LoadFileIterationData *data)
{
	g_object_unref (data->input_stream);
	g_uri_unref (data->base_uri);
	g_slice_free (LoadFileIterationData, data);
}

static char *
uri_get_path_query (GUri *uri)
{
	const char *path = g_uri_get_path (uri);
	return g_strconcat (path[0] ? path : "/", g_uri_get_query (uri), NULL);
}

static GUri * /* transfer full */
build_base_uri (UhmServer *self)
{
	UhmServerPrivate *priv = self->priv;
	gchar *base_uri_string;
	GUri *base_uri;

	if (priv->enable_online == FALSE) {
		GSList *uris;  /* owned */
		uris = soup_server_get_uris (priv->server);
		if (uris == NULL) {
			return NULL;
		}
		base_uri_string = g_uri_to_string_partial (uris->data, G_URI_HIDE_PASSWORD);
		g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
	} else {
		base_uri_string = g_strdup ("https://localhost"); /* arbitrary */
	}

	base_uri = g_uri_parse (base_uri_string, SOUP_HTTP_URI_FLAGS, NULL);
	g_free (base_uri_string);

	return base_uri;
}

static inline gboolean
parts_equal (const char *one, const char *two, gboolean insensitive)
{
	if (!one && !two)
		return TRUE;
	if (!one || !two)
		return FALSE;
	return insensitive ? !g_ascii_strcasecmp (one, two) : !strcmp (one, two);
}

static gboolean
real_compare_messages (UhmServer *server, UhmMessage *expected_message, UhmMessage *actual_message)
{
	GUri *expected_uri, *actual_uri;

	/* Compare method. */
	if (g_strcmp0 (uhm_message_get_method (expected_message), uhm_message_get_method (actual_message)) != 0) {
		return FALSE;
	}

	/* Compare URIs. */
	expected_uri = uhm_message_get_uri (expected_message);
	actual_uri = uhm_message_get_uri (actual_message);

	if (!parts_equal (g_uri_get_user (expected_uri), g_uri_get_user (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_password (expected_uri), g_uri_get_password (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_path (expected_uri), g_uri_get_path (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_query (expected_uri), g_uri_get_query (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_fragment (expected_uri), g_uri_get_fragment (actual_uri), FALSE)) {
		return FALSE;
	}

	return TRUE;
}

/* strcmp()-like return value: 0 means the messages compare equal. */
static gint
compare_incoming_message (UhmServer *self, UhmMessage *expected_message, UhmMessage *actual_message)
{
	gboolean messages_equal = FALSE;

	g_signal_emit (self, signals[SIGNAL_COMPARE_MESSAGES], 0, expected_message, actual_message, &messages_equal);

	return (messages_equal == TRUE) ? 0 : 1;
}

static void
header_append_cb (const gchar *name, const gchar *value, gpointer user_data)
{
	UhmMessage *message = user_data;

	soup_message_headers_append (uhm_message_get_response_headers (message), name, value);
}

static void
server_response_append_headers (UhmServer *self, UhmMessage *message)
{
	UhmServerPrivate *priv = self->priv;
	gchar *trace_file_name, *trace_file_offset;

	trace_file_name = g_file_get_uri (priv->trace_file);
	soup_message_headers_append (uhm_message_get_response_headers (message), "X-Mock-Trace-File", trace_file_name);
	g_free (trace_file_name);

	trace_file_offset = g_strdup_printf ("%u", priv->message_counter);
	soup_message_headers_append (uhm_message_get_response_headers (message), "X-Mock-Trace-File-Offset", trace_file_offset);
	g_free (trace_file_offset);
}

static void
server_process_message (UhmServer *self, UhmMessage *message)
{
	UhmServerPrivate *priv = self->priv;
	g_autoptr(GBytes) message_body = NULL;
	goffset expected_content_length;
	g_autoptr(GError) error = NULL;
	const char *location_header = NULL;

	g_assert (priv->next_message != NULL);
	priv->message_counter++;

	if (compare_incoming_message (self, priv->next_message, message) != 0) {
		gchar *body, *next_uri, *actual_uri;

		/* Received message is not what we expected. Return an error. */
		uhm_message_set_status (message, SOUP_STATUS_BAD_REQUEST,
		                        "Unexpected request to mock server");

		next_uri = uri_get_path_query (uhm_message_get_uri (priv->next_message));
		actual_uri = uri_get_path_query (uhm_message_get_uri (message));
		body = g_strdup_printf ("Expected %s URI ‘%s’, but got %s ‘%s’.",
		                        uhm_message_get_method (priv->next_message),
		                        next_uri, uhm_message_get_method (message), actual_uri);
		g_free (actual_uri);
		g_free (next_uri);
		soup_message_body_append_take (uhm_message_get_response_body (message), (guchar *) body, strlen (body) + 1);

		server_response_append_headers (self, message);

		return;
	}

	/* The incoming message matches what we expected, so copy the headers and body from the expected response and return it. */
	uhm_message_set_http_version (message, uhm_message_get_http_version (priv->next_message));
	uhm_message_set_status (message, uhm_message_get_status (priv->next_message),
	                        uhm_message_get_reason_phrase (priv->next_message));

	/* Rewrite Location headers to use the uhttpmock server details */
	location_header = soup_message_headers_get_one (uhm_message_get_response_headers (priv->next_message), "Location");
	if (location_header) {
		g_autoptr(GUri) uri = NULL;
		g_autoptr(GUri) modified_uri = NULL;
		g_autofree char *uri_str = NULL;

		uri = g_uri_parse (location_header, G_URI_FLAGS_ENCODED, &error);
		if (uri) {
			modified_uri = g_uri_build (G_URI_FLAGS_ENCODED,
				                    g_uri_get_scheme (uri),
				                    g_uri_get_userinfo (uri),
				                    g_uri_get_host (uri),
				                    priv->port,
				                    g_uri_get_path (uri),
				                    g_uri_get_query (uri),
				                    g_uri_get_fragment (uri));

			uri_str = g_uri_to_string (modified_uri);
			soup_message_headers_replace (uhm_message_get_response_headers (priv->next_message), "Location", uri_str);
		} else {
			g_debug ("Failed to rewrite Location header ‘%s’ to use new port", location_header);
		}
	}
	soup_message_headers_foreach (uhm_message_get_response_headers (priv->next_message), header_append_cb, message);

	/* Add debug headers to identify the message and trace file. */
	server_response_append_headers (self, message);

	message_body = soup_message_body_flatten (uhm_message_get_response_body (priv->next_message));
	if (g_bytes_get_size (message_body) > 0)
		soup_message_body_append_bytes (uhm_message_get_response_body (message), message_body);

	/* If the log file doesn't contain the full response body (e.g. because it's a huge binary file containing a nul byte somewhere),
	 * make one up (all zeros). */
	expected_content_length = soup_message_headers_get_content_length (uhm_message_get_response_headers (message));
	if (expected_content_length > 0 && g_bytes_get_size (message_body) < (gsize) expected_content_length) {
		guint8 *buf;

		buf = g_malloc0 (expected_content_length - g_bytes_get_size (message_body));
		soup_message_body_append_take (uhm_message_get_response_body (message), buf, expected_content_length - g_bytes_get_size (message_body));
	}

	soup_message_body_complete (uhm_message_get_response_body (message));

	/* Clear the expected message. */
	g_clear_object (&priv->next_message);
}

static void
server_handler_cb (SoupServer *server, SoupServerMessage *message, const gchar *path, GHashTable *query, gpointer user_data)
{
	UhmServer *self = user_data;
	UhmMessage *umsg;
	gboolean message_handled = FALSE;

	soup_server_message_pause (message);
	umsg = uhm_message_new_from_server_message (message);

	g_signal_emit (self, signals[SIGNAL_HANDLE_MESSAGE], 0, umsg, &message_handled);

	soup_server_message_set_http_version (message, uhm_message_get_http_version (umsg));
	soup_server_message_set_status (message, uhm_message_get_status (umsg), uhm_message_get_reason_phrase (umsg));

	g_object_unref (umsg);
	soup_server_message_unpause (message);

	/* The message should always be handled by real_handle_message() at least. */
	g_assert (message_handled == TRUE);
}

static gboolean
real_handle_message (UhmServer *self, UhmMessage *message)
{
	UhmServerPrivate *priv = self->priv;
	gboolean handled = FALSE;

	/* Asynchronously load the next expected message from the trace file. */
	if (priv->next_message == NULL) {
		GTask *task;
		LoadFileIterationData *data;
		GError *child_error = NULL;

		data = g_slice_new (LoadFileIterationData);
		data->input_stream = g_object_ref (priv->input_stream);
		data->base_uri = build_base_uri (self);

		task = g_task_new (self, NULL, NULL, NULL);
		g_task_set_task_data (task, data, (GDestroyNotify) load_file_iteration_data_free);
		g_task_run_in_thread_sync (task, load_file_iteration_thread_cb);

		/* Handle the results. */
		priv->next_message = g_task_propagate_pointer (task, &child_error);

		g_object_unref (task);

		if (child_error != NULL) {
			gchar *body;

			uhm_message_set_status (message, SOUP_STATUS_INTERNAL_SERVER_ERROR,
			                        "Error loading expected request");

			body = g_strdup_printf ("Error: %s", child_error->message);
			soup_message_body_append_take (uhm_message_get_response_body (message), (guchar *) body, strlen (body) + 1);
			handled = TRUE;

			g_error_free (child_error);

			server_response_append_headers (self, message);
		} else if (priv->next_message == NULL) {
			gchar *body, *actual_uri;

			/* Received message is not what we expected. Return an error. */
			uhm_message_set_status (message, SOUP_STATUS_BAD_REQUEST,
			                        "Unexpected request to mock server");

			actual_uri = uri_get_path_query (uhm_message_get_uri (message));
			body = g_strdup_printf ("Expected no request, but got %s ‘%s’.", uhm_message_get_method (message), actual_uri);
			g_free (actual_uri);
			soup_message_body_append_take (uhm_message_get_response_body (message), (guchar *) body, strlen (body) + 1);
			handled = TRUE;

			server_response_append_headers (self, message);
		}
	}

	/* Process the actual message if we already know the expected message. */
	g_assert (priv->next_message != NULL || handled == TRUE);
	if (handled == FALSE) {
		server_process_message (self, message);
		handled = TRUE;
	}

	g_assert (handled == TRUE);
	return handled;
}

/**
 * uhm_server_new:
 *
 * Creates a new #UhmServer with default properties.
 *
 * Return value: (transfer full): a new #UhmServer; unref with g_object_unref()
 *
 * Since: 0.1.0
 */
UhmServer *
uhm_server_new (void)
{
	return g_object_new (UHM_TYPE_SERVER, NULL);
}

static gboolean
trace_to_soup_message_headers_and_body (SoupMessageHeaders *message_headers, SoupMessageBody *message_body, const gchar message_direction, const gchar **_trace)
{
	const gchar *i;
	const gchar *trace = *_trace;

	/* Parse headers. */
	while (TRUE) {
		gchar *header_name, *header_value;

		if (*trace == '\0') {
			/* No body. */
			goto done;
		} else if (*trace == ' ' && *(trace + 1) == ' ' && *(trace + 2) == '\n') {
			/* No body. */
			trace += 3;
			goto done;
		} else if (*trace != message_direction || *(trace + 1) != ' ') {
			g_warning ("Unrecognised start sequence ‘%c%c’.", *trace, *(trace + 1));
			goto error;
		}
		trace += 2;

		if (*trace == '\n') {
			/* Reached the end of the headers. */
			trace++;
			break;
		}

		i = strchr (trace, ':');
		if (i == NULL || *(i + 1) != ' ') {
			g_warning ("Missing spacer ‘: ’.");
			goto error;
		}

		header_name = g_strndup (trace, i - trace);
		trace += (i - trace) + 2;

		i = strchr (trace, '\n');
		if (i == NULL) {
			g_warning ("Missing spacer ‘\\n’.");
			goto error;
		}

		header_value = g_strndup (trace, i - trace);
		trace += (i - trace) + 1;

		/* Append the header. */
		soup_message_headers_append (message_headers, header_name, header_value);

		g_free (header_value);
		g_free (header_name);
	}

	/* Parse the body. */
	while (TRUE) {
		if (*trace == ' ' && *(trace + 1) == ' ' && *(trace + 2) == '\n') {
			/* End of the body. */
			trace += 3;
			break;
		} else if (*trace == '\0') {
			/* End of the body. */
			break;
		} else if (*trace != message_direction || *(trace + 1) != ' ') {
			g_warning ("Unrecognised start sequence ‘%c%c’.", *trace, *(trace + 1));
			goto error;
		}
		trace += 2;

		i = strchr (trace, '\n');
		if (i == NULL) {
			g_warning ("Missing spacer ‘\\n’.");
			goto error;
		}

		soup_message_body_append (message_body, SOUP_MEMORY_COPY, trace, i - trace + 1); /* include trailing \n */
		trace += (i - trace) + 1;
	}

done:
	/* Done. Update the output trace pointer. */
	soup_message_body_complete (message_body);
	*_trace = trace;

	return TRUE;

error:
	return FALSE;
}

/* base_uri is the base URI for the server, e.g. https://127.0.0.1:1431. */
static UhmMessage *
trace_to_soup_message (const gchar *trace, GUri *base_uri)
{
	UhmMessage *message = NULL;
	const gchar *i, *j, *method;
	gchar *uri_string, *response_message;
	SoupHTTPVersion http_version;
	guint response_status;
	g_autoptr(GUri) uri = NULL;

	g_return_val_if_fail (trace != NULL, NULL);

	/* The traces look somewhat like this:
	 * > POST /unauth HTTP/1.1
	 * > Soup-Debug-Timestamp: 1200171744
	 * > Soup-Debug: SoupSessionAsync 1 (0x612190), SoupMessage 1 (0x617000), SoupSocket 1 (0x612220)
	 * > Host: localhost
	 * > Content-Type: text/plain
	 * > Connection: close
	 * > 
	 * > This is a test.
	 *   
	 * < HTTP/1.1 201 Created
	 * < Soup-Debug-Timestamp: 1200171744
	 * < Soup-Debug: SoupMessage 1 (0x617000)
	 * < Date: Sun, 12 Jan 2008 21:02:24 GMT
	 * < Content-Length: 0
	 *
	 * This function parses a single request–response pair.
	 */

	/* Parse the method, URI and HTTP version first. */
	if (*trace != '>' || *(trace + 1) != ' ') {
		g_warning ("Unrecognised start sequence ‘%c%c’.", *trace, *(trace + 1));
		goto error;
	}
	trace += 2;

	/* Parse “POST /unauth HTTP/1.1”. */
	if (strncmp (trace, "POST", strlen ("POST")) == 0) {
		method = SOUP_METHOD_POST;
		trace += strlen ("POST");
	} else if (strncmp (trace, "GET", strlen ("GET")) == 0) {
		method = SOUP_METHOD_GET;
		trace += strlen ("GET");
	} else if (strncmp (trace, "DELETE", strlen ("DELETE")) == 0) {
		method = SOUP_METHOD_DELETE;
		trace += strlen ("DELETE");
	} else if (strncmp (trace, "PUT", strlen ("PUT")) == 0) {
		method = SOUP_METHOD_PUT;
		trace += strlen ("PUT");
	} else if (strncmp (trace, "PATCH", strlen ("PATCH")) == 0) {
		method = "PATCH";
		trace += strlen ("PATCH");
	} else if (strncmp (trace, "CONNECT", strlen ("CONNECT")) == 0) {
		method = "CONNECT";
		trace += strlen ("CONNECT");
	} else {
		g_warning ("Unknown method ‘%s’.", trace);
		goto error;
	}

	if (*trace != ' ') {
		g_warning ("Unrecognised spacer ‘%c’.", *trace);
		goto error;
	}
	trace++;

	i = strchr (trace, ' ');
	if (i == NULL) {
		g_warning ("Missing spacer ‘ ’.");
		goto error;
	}

	uri_string = g_strndup (trace, i - trace);
	trace += (i - trace) + 1;

	if (strncmp (trace, "HTTP/1.1", strlen ("HTTP/1.1")) == 0) {
		http_version = SOUP_HTTP_1_1;
		trace += strlen ("HTTP/1.1");
	} else if (strncmp (trace, "HTTP/1.0", strlen ("HTTP/1.0")) == 0) {
		http_version = SOUP_HTTP_1_0;
		trace += strlen ("HTTP/1.0");
	} else if (strncmp (trace, "HTTP/2", strlen ("HTTP/2")) == 0) {
		http_version = SOUP_HTTP_2_0;
		trace += strlen ("HTTP/2");
	} else {
		g_warning ("Unrecognised HTTP version ‘%s’.", trace);
		http_version = SOUP_HTTP_1_1;
	}

	if (*trace != '\n') {
		g_warning ("Unrecognised spacer ‘%c’.", *trace);
		goto error;
	}
	trace++;

	/* Build the message. */
	uri = g_uri_parse_relative (base_uri, uri_string, SOUP_HTTP_URI_FLAGS, NULL);
	message = uhm_message_new_from_uri (method, uri);

	if (message == NULL) {
		g_warning ("Invalid URI ‘%s’.", uri_string);
		goto error;
	}

	uhm_message_set_http_version (message, http_version);
	g_free (uri_string);

	/* Parse the request headers and body. */
	if (trace_to_soup_message_headers_and_body (uhm_message_get_request_headers (message), uhm_message_get_request_body (message), '>', &trace) == FALSE) {
		goto error;
	}

	/* Parse the response, starting with “HTTP/1.1 201 Created”. */
	if (*trace != '<' || *(trace + 1) != ' ') {
		g_warning ("Unrecognised start sequence ‘%c%c’.", *trace, *(trace + 1));
		goto error;
	}
	trace += 2;

	if (strncmp (trace, "HTTP/1.1", strlen ("HTTP/1.1")) == 0) {
		http_version = SOUP_HTTP_1_1;
		trace += strlen ("HTTP/1.1");
	} else if (strncmp (trace, "HTTP/1.0", strlen ("HTTP/1.0")) == 0) {
		http_version = SOUP_HTTP_1_0;
		trace += strlen ("HTTP/1.0");
	} else if (strncmp (trace, "HTTP/2", strlen ("HTTP/2")) == 0) {
		http_version = SOUP_HTTP_2_0;
		trace += strlen ("HTTP/2");
	} else {
		g_warning ("Unrecognised HTTP version ‘%s’.", trace);
	}

	if (*trace != ' ') {
		g_warning ("Unrecognised spacer ‘%c’.", *trace);
		goto error;
	}
	trace++;

	i = strchr (trace, ' ');
	if (i == NULL) {
		g_warning ("Missing spacer ‘ ’.");
		goto error;
	}

	response_status = g_ascii_strtoull (trace, (gchar **) &j, 10);
	if (j != i) {
		g_warning ("Invalid status ‘%s’.", trace);
		goto error;
	}
	trace += (i - trace) + 1;

	i = strchr (trace, '\n');
	if (i == NULL) {
		g_warning ("Missing spacer ‘\n’.");
		goto error;
	}

	response_message = g_strndup (trace, i - trace);
	trace += (i - trace) + 1;

	uhm_message_set_status (message, response_status, response_message);

	g_free (response_message);

	/* Parse the response headers and body. */
	if (trace_to_soup_message_headers_and_body (uhm_message_get_response_headers (message), uhm_message_get_response_body (message), '<', &trace) == FALSE) {
		goto error;
	}

	return message;

error:
	g_clear_object (&message);

	return NULL;
}

static GDataInputStream *
load_file_stream (GFile *trace_file, GCancellable *cancellable, GError **error)
{
	GFileInputStream *base_stream = NULL;  /* owned */
	GDataInputStream *data_stream = NULL;  /* owned */

	base_stream = g_file_read (trace_file, cancellable, error);

	if (base_stream == NULL)
		return NULL;

	data_stream = g_data_input_stream_new (G_INPUT_STREAM (base_stream));
	g_data_input_stream_set_byte_order (data_stream, G_DATA_STREAM_BYTE_ORDER_LITTLE_ENDIAN);
	g_data_input_stream_set_newline_type (data_stream, G_DATA_STREAM_NEWLINE_TYPE_LF);

	g_object_unref (base_stream);

	return data_stream;
}

static gboolean
load_message_half (GDataInputStream *input_stream, GString *current_message, GCancellable *cancellable, GError **error)
{
	gsize len;
	gchar *line = NULL;  /* owned */
	GError *child_error = NULL;

	while (TRUE) {
		line = g_data_input_stream_read_line (input_stream, &len, cancellable, &child_error);

		if (line == NULL && child_error != NULL) {
			/* Error. */
			g_propagate_error (error, child_error);
			return FALSE;
		} else if (line == NULL) {
			/* EOF. Try again to grab a response. */
			return TRUE;
		} else {
			gboolean reached_eom;

			reached_eom = (g_strcmp0 (line, "  ") == 0);

			g_string_append_len (current_message, line, len);
			g_string_append_c (current_message, '\n');

			g_free (line);

			if (reached_eom) {
				/* Reached the end of the message. */
				return TRUE;
			}
		}
	}
}

static UhmMessage *
load_file_iteration (GDataInputStream *input_stream, GUri *base_uri, GCancellable *cancellable, GError **error)
{
	UhmMessage *output_message = NULL;
	GString *current_message = NULL;

	current_message = g_string_new (NULL);

	do {
		/* Start loading from the stream. */
		g_string_truncate (current_message, 0);

		/* We should be at the start of a request; grab it. */
		if (!load_message_half (input_stream, current_message, cancellable, error) ||
		    !load_message_half (input_stream, current_message, cancellable, error)) {
			goto done;
		}

		if (current_message->len > 0) {
			output_message = trace_to_soup_message (current_message->str, base_uri);
		} else {
			/* Reached the end of the file. */
			output_message = NULL;
		}
	} while (output_message != NULL && uhm_message_get_status (output_message) == SOUP_STATUS_NONE);

done:
	/* Tidy up. */
	g_string_free (current_message, TRUE);

	/* Postcondition: (output_message != NULL) => (*error == NULL). */
	g_assert (output_message == NULL || (error == NULL || *error == NULL));

	return output_message;
}

static void
load_file_stream_thread_cb (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
	GFile *trace_file;
	GDataInputStream *input_stream;
	GError *child_error = NULL;

	trace_file = task_data;
	g_assert (G_IS_FILE (trace_file));

	input_stream = load_file_stream (trace_file, cancellable, &child_error);

	if (child_error != NULL) {
		g_task_return_error (task, child_error);
	} else {
		g_task_return_pointer (task, input_stream, g_object_unref);
	}
}

static void
load_file_iteration_thread_cb (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
	LoadFileIterationData *data = task_data;
	GDataInputStream *input_stream;
	UhmMessage *output_message;
	GUri *base_uri;
	GError *child_error = NULL;

	input_stream = data->input_stream;
	g_assert (G_IS_DATA_INPUT_STREAM (input_stream));
	base_uri = data->base_uri;

	output_message = load_file_iteration (input_stream, base_uri, cancellable, &child_error);

	if (child_error != NULL) {
		g_task_return_error (task, child_error);
	} else {
		g_task_return_pointer (task, output_message, g_object_unref);
	}
}

/**
 * uhm_server_unload_trace:
 * @self: a #UhmServer
 *
 * Unloads the current trace file of network messages, as loaded by uhm_server_load_trace() or uhm_server_load_trace_async().
 *
 * Since: 0.1.0
 */
void
uhm_server_unload_trace (UhmServer *self)
{
	UhmServerPrivate *priv = self->priv;

	g_return_if_fail (UHM_IS_SERVER (self));

	g_clear_object (&priv->next_message);
	g_clear_object (&priv->input_stream);
	g_clear_object (&priv->trace_file);
	g_clear_pointer (&priv->comparison_message, g_byte_array_unref);
	priv->message_counter = 0;
	priv->received_message_state = UNKNOWN;
}

/**
 * uhm_server_load_trace:
 * @self: a #UhmServer
 * @trace_file: trace file to load
 * @cancellable: (allow-none): a #GCancellable, or %NULL
 * @error: (allow-none): return location for a #GError, or %NULL
 *
 * Synchronously loads the given @trace_file of network messages, ready to simulate a network conversation by matching
 * requests against the file and returning the associated responses. Call uhm_server_run() to start the mock
 * server afterwards.
 *
 * Loading the trace file may be cancelled from another thread using @cancellable.
 *
 * On error, @error will be set and the state of the #UhmServer will not change. A #GIOError will be set if there is
 * a problem reading the trace file.
 *
 * Since: 0.1.0
 */
void
uhm_server_load_trace (UhmServer *self, GFile *trace_file, GCancellable *cancellable, GError **error)
{
	UhmServerPrivate *priv = self->priv;
	g_autoptr(GUri) base_uri = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree char *content = NULL;
	g_autofree char *trace_path = NULL;
	g_autofree char *trace_hosts = NULL;
	g_auto(GStrv) split = NULL;
	gsize len;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (G_IS_FILE (trace_file));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
	g_return_if_fail (error == NULL || *error == NULL);
	g_return_if_fail (priv->trace_file == NULL && priv->input_stream == NULL && priv->next_message == NULL);

	base_uri = build_base_uri (self);

	/* Trace File */
	priv->trace_file = g_object_ref (trace_file);
	priv->input_stream = load_file_stream (priv->trace_file, cancellable, error);

	if (priv->input_stream != NULL) {
		GError *child_error = NULL;

		priv->next_message = load_file_iteration (priv->input_stream, base_uri, cancellable, &child_error);
		priv->message_counter = 0;
		priv->comparison_message = g_byte_array_new ();
		priv->received_message_state = UNKNOWN;

		if (child_error != NULL) {
			g_clear_object (&priv->trace_file);
			g_propagate_error (error, child_error);
			return;
		}
	} else {
		/* Error. */
		g_clear_object (&priv->trace_file);
		return;
	}

	/* Host file */
	trace_path = g_file_get_path (trace_file);
	trace_hosts = g_strconcat (trace_path, ".hosts", NULL);
	priv->hosts_trace_file = g_file_new_for_path (trace_hosts);

	if (g_file_load_contents (priv->hosts_trace_file, cancellable, &content, &len, NULL, &local_error)) {
		split = g_strsplit (content, "\n", -1);
		for (gsize i = 0; split != NULL && split[i] != NULL; i++) {
			if (*(split[i]) != '\0') {
				uhm_resolver_add_A (priv->resolver, split[i], uhm_server_get_address (self));
			}
		}
	} else if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		/* It's not fatal that this file cannot be loaded as these hosts can be added in code */
		g_clear_error (&local_error);

	} else {
		/* Other I/O errors are fatal. */
		g_propagate_error (error, g_steal_pointer (&local_error));
		return;
	}
}

typedef struct {
	GAsyncReadyCallback callback;
	gpointer user_data;
	GUri *base_uri;
} LoadTraceData;

static void
load_trace_async_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
	UhmServer *self = UHM_SERVER (source_object);
	LoadTraceData *data = user_data;
	LoadFileIterationData *iteration_data;
	GTask *task;
	GError *child_error = NULL;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (G_IS_ASYNC_RESULT (result));
	g_return_if_fail (g_task_is_valid (result, self));

	self->priv->input_stream = g_task_propagate_pointer (G_TASK (result), &child_error);

	iteration_data = g_slice_new (LoadFileIterationData);
	iteration_data->input_stream = g_object_ref (self->priv->input_stream);
	iteration_data->base_uri = data->base_uri; /* transfer ownership */
	data->base_uri = NULL;

	task = g_task_new (g_task_get_source_object (G_TASK (result)), g_task_get_cancellable (G_TASK (result)), data->callback, data->user_data);
	g_task_set_task_data (task, iteration_data, (GDestroyNotify) load_file_iteration_data_free);

	if (child_error != NULL) {
		g_task_return_error (task, child_error);
	} else {
		g_task_run_in_thread (task, load_file_iteration_thread_cb);
	}

	g_object_unref (task);

	g_slice_free (LoadTraceData, data);
}

/**
 * uhm_server_load_trace_async:
 * @self: a #UhmServer
 * @trace_file: trace file to load
 * @cancellable: (allow-none): a #GCancellable, or %NULL
 * @callback: function to call once the async operation is complete
 * @user_data: (allow-none): user data to pass to @callback, or %NULL
 *
 * Asynchronous version of uhm_server_load_trace(). In @callback, call uhm_server_load_trace_finish() to complete the operation.
 *
 * Since: 0.1.0
 */
void
uhm_server_load_trace_async (UhmServer *self, GFile *trace_file, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;
	LoadTraceData *data;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (G_IS_FILE (trace_file));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
	g_return_if_fail (self->priv->trace_file == NULL && self->priv->input_stream == NULL && self->priv->next_message == NULL);

	self->priv->trace_file = g_object_ref (trace_file);

	data = g_slice_new (LoadTraceData);
	data->callback = callback;
	data->user_data = user_data;
	data->base_uri = build_base_uri (self);

	task = g_task_new (self, cancellable, load_trace_async_cb, data);
	g_task_set_task_data (task, g_object_ref (self->priv->trace_file), g_object_unref);
	g_task_run_in_thread (task, load_file_stream_thread_cb);
	g_object_unref (task);
}

/**
 * uhm_server_load_trace_finish:
 * @self: a #UhmServer
 * @result: asynchronous operation result passed to the callback
 * @error: (allow-none): return location for a #GError, or %NULL
 *
 * Finishes an asynchronous operation started by uhm_server_load_trace_async().
 *
 * On error, @error will be set and the state of the #UhmServer will not change.
 * See uhm_server_load_trace() for details on the error domains used.
 *
 * Since: 0.1.0
 */
void
uhm_server_load_trace_finish (UhmServer *self, GAsyncResult *result, GError **error)
{
	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (G_IS_ASYNC_RESULT (result));
	g_return_if_fail (error == NULL || *error == NULL);
	g_return_if_fail (g_task_is_valid (result, self));

	self->priv->next_message = g_task_propagate_pointer (G_TASK (result), error);
	self->priv->message_counter = 0;
	self->priv->comparison_message = g_byte_array_new ();
	self->priv->received_message_state = UNKNOWN;
}

/* Must only be called in the server thread. */
static gboolean
server_thread_quit_cb (gpointer user_data)
{
	UhmServer *self = user_data;
	UhmServerPrivate *priv = self->priv;

	g_main_loop_quit (priv->server_main_loop);

	return G_SOURCE_REMOVE;
}

static gpointer
server_thread_cb (gpointer user_data)
{
	UhmServer *self = user_data;
	UhmServerPrivate *priv = self->priv;

	g_main_context_push_thread_default (priv->server_context);

	/* Run the server. This will create a main loop and iterate the server_context until soup_server_quit() is called. */
	g_main_loop_run (priv->server_main_loop);

	g_main_context_pop_thread_default (priv->server_context);

	return NULL;
}

/**
 * uhm_server_run:
 * @self: a #UhmServer
 *
 * Runs the mock server, binding to a loopback TCP/IP interface and preparing a HTTPS server which is ready to accept requests.
 * The TCP/IP address and port number are chosen randomly out of the loopback addresses, and are exposed as #UhmServer:address and #UhmServer:port
 * once this function has returned. A #UhmResolver (exposed as #UhmServer:resolver) is set as the default #GResolver while the server is running.
 *
 * The server is started in a worker thread, so this function returns immediately and the server continues to run in the background. Use uhm_server_stop()
 * to shut it down.
 *
 * This function always succeeds.
 *
 * Since: 0.1.0
 */
void
uhm_server_run (UhmServer *self)
{
	UhmServerPrivate *priv = self->priv;
	g_autoptr(GError) error = NULL;
	GSList *sockets;  /* owned */
	GSocket *socket;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (priv->resolver == NULL);
	g_return_if_fail (priv->server == NULL);

	/* Set up the server. If (priv->tls_certificate != NULL) it will be a HTTPS server;
	 * otherwise it will be a HTTP server. */
	priv->server_context = g_main_context_new ();
	priv->server = soup_server_new ("tls-certificate", priv->tls_certificate,
	                                "raw-paths", TRUE,
	                                NULL);
	soup_server_add_handler (priv->server, "/", server_handler_cb, self, NULL);

	g_main_context_push_thread_default (priv->server_context);

	/* Try listening on either IPv4 or IPv6. If that fails, try on IPv4 only
	 * as listening on IPv6 while inside a Docker container (as happens in
	 * CI) can fail if the container isn’t bridged properly. */
	priv->server_main_loop = g_main_loop_new (priv->server_context, FALSE);
	if (!soup_server_listen_local (priv->server, 0, (priv->tls_certificate != NULL) ? SOUP_SERVER_LISTEN_HTTPS : 0, NULL))
		soup_server_listen_local (priv->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY | ((priv->tls_certificate != NULL) ? SOUP_SERVER_LISTEN_HTTPS : 0), &error);
	g_assert_no_error (error);  /* binding to localhost should never really fail */

	g_main_context_pop_thread_default (priv->server_context);

	/* Grab the randomly selected address and port. */
	sockets = soup_server_get_listeners (priv->server);
	g_assert (sockets != NULL);

	socket = sockets->data;
	priv->address = g_socket_get_local_address (socket, &error);
	g_assert_no_error (error);
	priv->port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (priv->address));

	g_slist_free (sockets);

	/* Set up the resolver. It is expected that callers will grab the resolver (by calling uhm_server_get_resolver())
	 * immediately after this function returns, and add some expected hostnames by calling uhm_resolver_add_A() one or
	 * more times, before starting the next test.Or they could call uhm_server_set_expected_domain_names() any time. */
	priv->resolver = uhm_resolver_new ();
	g_resolver_set_default (G_RESOLVER (priv->resolver));

	/* Note: This must be called before notify::resolver, so the user can add extra domain names in that callback if desired. */
	apply_expected_domain_names (self);

	g_object_freeze_notify (G_OBJECT (self));
	g_object_notify (G_OBJECT (self), "address");
	g_object_notify (G_OBJECT (self), "port");
	g_object_notify (G_OBJECT (self), "resolver");
	g_object_thaw_notify (G_OBJECT (self));

	/* Start the network thread. */
	priv->server_thread = g_thread_new ("mock-server-thread", server_thread_cb, self);
}

/**
 * uhm_server_stop:
 * @self: a #UhmServer
 *
 * Stops a mock server started by calling uhm_server_run(). This shuts down the server's worker thread and unbinds it from its TCP/IP socket.
 *
 * This unloads any trace file loaded by calling uhm_server_load_trace() (or its asynchronous counterpart). It also resets the set of domain
 * names loaded into the #UhmServer:resolver.
 *
 * This function always succeeds.
 *
 * Since: 0.1.0
 */
void
uhm_server_stop (UhmServer *self)
{
	UhmServerPrivate *priv = self->priv;
	GSource *idle;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (priv->server != NULL);
	g_return_if_fail (priv->resolver != NULL);

	/* Stop the server. */
	idle = g_idle_source_new ();
	g_source_set_callback (idle, server_thread_quit_cb, self, NULL);
	g_source_attach (idle, priv->server_context);
	g_source_unref (idle);

	g_thread_join (priv->server_thread);
	priv->server_thread = NULL;
	uhm_resolver_reset (priv->resolver);

	g_clear_pointer (&priv->server_main_loop, g_main_loop_unref);
	g_clear_pointer (&priv->server_context, g_main_context_unref);
	g_clear_object (&priv->server);
	g_clear_object (&priv->resolver);

	g_clear_object (&priv->address);
	g_free (priv->address_string);
	priv->address_string = NULL;
	priv->port = 0;

	g_object_freeze_notify (G_OBJECT (self));
	g_object_notify (G_OBJECT (self), "address");
	g_object_notify (G_OBJECT (self), "port");
	g_object_notify (G_OBJECT (self), "resolver");
	g_object_thaw_notify (G_OBJECT (self));

	/* Reset the trace file. */
	uhm_server_unload_trace (self);
}

/**
 * uhm_server_get_trace_directory:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:trace-directory property.
 *
 * Return value: (allow-none) (transfer none): the directory to load/store trace files from, or %NULL
 *
 * Since: 0.1.0
 */
GFile *
uhm_server_get_trace_directory (UhmServer *self)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), NULL);

	return self->priv->trace_directory;
}

/**
 * uhm_server_set_trace_directory:
 * @self: a #UhmServer
 * @trace_directory: (allow-none) (transfer none): a directory to load/store trace files from, or %NULL to unset it
 *
 * Sets the value of the #UhmServer:trace-directory property.
 *
 * Since: 0.1.0
 */
void
uhm_server_set_trace_directory (UhmServer *self, GFile *trace_directory)
{
	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (trace_directory == NULL || G_IS_FILE (trace_directory));

	if (trace_directory != NULL) {
		g_object_ref (trace_directory);
	}

	g_clear_object (&self->priv->trace_directory);
	self->priv->trace_directory = trace_directory;
	g_object_notify (G_OBJECT (self), "trace-directory");
}

/**
 * uhm_server_start_trace:
 * @self: a #UhmServer
 * @trace_name: name of the trace
 * @error: (allow-none): return location for a #GError, or %NULL
 *
 * Starts a mock server which follows the trace file of filename @trace_name in the #UhmServer:trace-directory directory.
 * See uhm_server_start_trace_full() for further documentation.
 *
 * This function has undefined behaviour if #UhmServer:trace-directory is %NULL.
 *
 * On failure, @error will be set and the #UhmServer state will remain unchanged. See uhm_server_start_trace_full() for
 * details of the error domains used.
 *
 * Since: 0.1.0
 */
void
uhm_server_start_trace (UhmServer *self, const gchar *trace_name, GError **error)
{
	GFile *trace_file;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (trace_name != NULL && *trace_name != '\0');
	g_return_if_fail (error == NULL || *error == NULL);

	g_assert (self->priv->trace_directory != NULL);

	trace_file = g_file_get_child (self->priv->trace_directory, trace_name);
	uhm_server_start_trace_full (self, trace_file, error);
	g_object_unref (trace_file);
}

/**
 * uhm_server_start_trace_full:
 * @self: a #UhmServer
 * @trace_file: a trace file to load
 * @error: (allow-none): return location for a #GError, or %NULL
 *
 * Convenience function to start logging to or reading from the given @trace_file, depending on the values of #UhmServer:enable-logging and
 * #UhmServer:enable-online.
 *
 * If #UhmServer:enable-logging is %TRUE, a log handler will be set up to redirect all client network activity into the given @trace_file.
 * If @trace_file already exists, it will be overwritten.
 *
 * If #UhmServer:enable-online is %FALSE, the given @trace_file is loaded using uhm_server_load_trace() and then a mock server is
 * started using uhm_server_run().
 *
 * On failure, @error will be set and the #UhmServer state will remain unchanged. A #GIOError will be set if logging is enabled
 * (#UhmServer:enable-logging) and there is a problem writing to the trace file; or if a trace needs to be loaded and there is a problem
 * reading from the trace file.
 *
 * Since: 0.1.0
 */
void
uhm_server_start_trace_full (UhmServer *self, GFile *trace_file, GError **error)
{
	UhmServerPrivate *priv = self->priv;
	GError *child_error = NULL;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (G_IS_FILE (trace_file));
	g_return_if_fail (error == NULL || *error == NULL);

	if (priv->output_stream != NULL) {
		g_warning ("%s: Nested trace files are not supported. Call uhm_server_end_trace() before calling %s again.", G_STRFUNC, G_STRFUNC);
	}
	g_return_if_fail (priv->output_stream == NULL);

	if (priv->enable_online == TRUE) {
		priv->message_counter = 0;
	    priv->comparison_message = g_byte_array_new ();
		priv->received_message_state = UNKNOWN;
	}

	/* Start writing out a trace file if logging is enabled. */
	if (priv->enable_logging == TRUE) {
		GFileOutputStream *output_stream;
		g_autofree char *trace_path = g_file_get_path (trace_file);
		g_autofree char *trace_hosts = g_strconcat (trace_path, ".hosts", NULL);
		priv->hosts_trace_file = g_file_new_for_path (trace_hosts);

		output_stream = g_file_replace (trace_file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &child_error);

		if (child_error != NULL) {
			g_propagate_prefixed_error (error, g_steal_pointer (&child_error),
			             "Error replacing trace file ‘%s’: ", trace_path);
			return;
		} else {
			/* Change state. */
			priv->output_stream = output_stream;
		}

		/* Host trace file */
		output_stream = g_file_replace (priv->hosts_trace_file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &child_error);
		if (child_error != NULL) {
			g_autofree char *hosts_trace_file_path = g_file_get_path (priv->hosts_trace_file);
			g_propagate_prefixed_error (error, g_steal_pointer (&child_error),
			             "Error replacing trace hosts file ‘%s’: ", hosts_trace_file_path);
			return;
		} else {
			/* Change state. */
			priv->hosts_output_stream = output_stream;
		}
	}

	/* Start reading from a trace file if online testing is disabled or if we need to compare server responses to the trace file. */
	if (priv->enable_online == FALSE) {
		uhm_server_run (self);
		uhm_server_load_trace (self, trace_file, NULL, &child_error);

		if (child_error != NULL) {
			gchar *trace_file_path = g_file_get_path (trace_file);
			g_set_error (error, child_error->domain, child_error->code,
			             "Error loading trace file ‘%s’: %s", trace_file_path, child_error->message);
			g_free (trace_file_path);

			g_error_free (child_error);

			uhm_server_stop (self);
			g_clear_object (&priv->output_stream);

			return;
		}
	} else if (priv->enable_online == TRUE && priv->enable_logging == FALSE) {
		uhm_server_load_trace (self, trace_file, NULL, &child_error);

		if (child_error != NULL) {
			gchar *trace_file_path = g_file_get_path (trace_file);
			g_set_error (error, child_error->domain, child_error->code,
			             "Error loading trace file ‘%s’: %s", trace_file_path, child_error->message);
			g_free (trace_file_path);

			g_error_free (child_error);

			g_clear_object (&priv->output_stream);

			return;
		}
	}
}

/**
 * uhm_server_end_trace:
 * @self: a #UhmServer
 *
 * Convenience function to finish logging to or reading from a trace file previously passed to uhm_server_start_trace() or
 * uhm_server_start_trace_full().
 *
 * If #UhmServer:enable-online is %FALSE, this will shut down the mock server (as if uhm_server_stop() had been called).
 *
 * Since: 0.1.0
 */
void
uhm_server_end_trace (UhmServer *self)
{
	UhmServerPrivate *priv = self->priv;

	g_return_if_fail (UHM_IS_SERVER (self));

	if (priv->enable_online == FALSE) {
		uhm_server_stop (self);
	} else if (priv->enable_online == TRUE && priv->enable_logging == FALSE) {
		uhm_server_unload_trace (self);
	}

	if (priv->enable_logging == TRUE) {
		g_clear_object (&self->priv->output_stream);
		g_clear_object (&self->priv->hosts_output_stream);
	}
}

/**
 * uhm_server_get_enable_online:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:enable-online property.
 *
 * Return value: %TRUE if the server does not intercept and handle network connections from client code; %FALSE otherwise
 *
 * Since: 0.1.0
 */
gboolean
uhm_server_get_enable_online (UhmServer *self)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), FALSE);

	return self->priv->enable_online;
}

/**
 * uhm_server_set_enable_online:
 * @self: a #UhmServer
 * @enable_online: %TRUE to not intercept and handle network connections from client code; %FALSE otherwise
 *
 * Sets the value of the #UhmServer:enable-online property.
 *
 * Since: 0.1.0
 */
void
uhm_server_set_enable_online (UhmServer *self, gboolean enable_online)
{
	g_return_if_fail (UHM_IS_SERVER (self));

	self->priv->enable_online = enable_online;
	g_object_notify (G_OBJECT (self), "enable-online");
}

/**
 * uhm_server_get_enable_logging:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:enable-logging property.
 *
 * Return value: %TRUE if client network traffic is being logged to a trace file; %FALSE otherwise
 *
 * Since: 0.1.0
 */
gboolean
uhm_server_get_enable_logging (UhmServer *self)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), FALSE);

	return self->priv->enable_logging;
}

/**
 * uhm_server_set_enable_logging:
 * @self: a #UhmServer
 * @enable_logging: %TRUE to log client network traffic to a trace file; %FALSE otherwise
 *
 * Sets the value of the #UhmServer:enable-logging property.
 *
 * Since: 0.1.0
 */
void
uhm_server_set_enable_logging (UhmServer *self, gboolean enable_logging)
{
	g_return_if_fail (UHM_IS_SERVER (self));

	self->priv->enable_logging = enable_logging;
	g_object_notify (G_OBJECT (self), "enable-logging");
}

/**
 * uhm_server_received_message_chunk:
 * @self: a #UhmServer
 * @message_chunk: single line of a message which was received
 * @message_chunk_length: length of @message_chunk in bytes
 * @error: (allow-none): return location for a #GError, or %NULL
 *
 * Indicates to the mock server that a single new line of a message was received from the real server. The message line may be
 * appended to the current trace file if logging is enabled (#UhmServer:enable-logging is %TRUE), adding a newline character
 * at the end. If logging is disabled but online mode is enabled (#UhmServer:enable-online is %TRUE), the message line will
 * be compared to the next expected line in the existing trace file. Otherwise, this function is a no-op.
 *
 * On failure, @error will be set and the #UhmServer state will remain unchanged apart from the parse state machine, which will remain
 * in the state reached after parsing @message_chunk. A %G_IO_ERROR will be returned if writing to the trace file failed. If in
 * comparison mode and the received message chunk corresponds to an unexpected message in the trace file, a %UHM_SERVER_ERROR will
 * be returned.
 *
 * <note><para>In common cases where message log data only needs to be passed to a #UhmServer and not (for example) logged to an
 * application-specific file or the command line as  well, it is simpler to use uhm_server_received_message_chunk_from_soup(), passing
 * it directly to soup_logger_set_printer(). See the documentation for uhm_server_received_message_chunk_from_soup() for details.</para></note>
 *
 * Since: 0.1.0
 */
void
uhm_server_received_message_chunk (UhmServer *self, const gchar *message_chunk, goffset message_chunk_length, GError **error)
{
	UhmServerPrivate *priv = self->priv;
	GError *child_error = NULL;
	g_autoptr(UhmMessage) online_message = NULL;
	g_autoptr(GUri) base_uri = NULL;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (message_chunk != NULL);
	g_return_if_fail (error == NULL || *error == NULL);

	/* Silently ignore the call if logging is disabled and we're offline, or if a trace file hasn't been specified. */
	if ((priv->enable_logging == FALSE && priv->enable_online == FALSE) || (priv->enable_logging == TRUE && priv->output_stream == NULL)) {
		return;
	}

	/* Simple state machine to track where we are in the soup log format. */
	switch (priv->received_message_state) {
		case UNKNOWN:
			if (strncmp (message_chunk, "> ", 2) == 0) {
				priv->received_message_state = REQUEST_DATA;
			}
			break;
		case REQUEST_DATA:
			if (strcmp (message_chunk, "  ") == 0) {
				priv->received_message_state = REQUEST_TERMINATOR;
			} else if (strncmp (message_chunk, "> ", 2) != 0) {
				priv->received_message_state = UNKNOWN;
			}
			break;
		case REQUEST_TERMINATOR:
			if (strncmp (message_chunk, "< ", 2) == 0) {
				priv->received_message_state = RESPONSE_DATA;
			} else {
				priv->received_message_state = UNKNOWN;
			}
			break;
		case RESPONSE_DATA:
			if (strcmp (message_chunk, "  ") == 0) {
				priv->received_message_state = RESPONSE_TERMINATOR;
			} else if (strncmp (message_chunk, "< ", 2) != 0) {
				priv->received_message_state = UNKNOWN;
			}
			break;
		case RESPONSE_TERMINATOR:
			if (strncmp (message_chunk, "> ", 2) == 0) {
				priv->received_message_state = REQUEST_DATA;
			} else {
				priv->received_message_state = UNKNOWN;
			}
			break;
		default:
			g_assert_not_reached ();
	}

	/* Silently ignore responses outputted by libsoup before the requests. This can happen when a SoupMessage is cancelled part-way through
	 * sending the request; in which case libsoup logs only a response of the form:
	 *     < HTTP/1.1 1 Cancelled
	 *     < Soup-Debug-Timestamp: 1375190963
	 *     < Soup-Debug: SoupMessage 0 (0x7fffe00261c0)
	 */
	if (priv->received_message_state == UNKNOWN) {
		return;
	}

	/* Append to the trace file. */
	if (priv->enable_logging == TRUE &&
	    (!g_output_stream_write_all (G_OUTPUT_STREAM (priv->output_stream), message_chunk, message_chunk_length, NULL, NULL, &child_error) ||
	     !g_output_stream_write_all (G_OUTPUT_STREAM (priv->output_stream), "\n", 1, NULL, NULL, &child_error))) {
		gchar *trace_file_path = g_file_get_path (priv->trace_file);
		g_set_error (error, child_error->domain, child_error->code,
		             "Error appending to log file ‘%s’: %s", trace_file_path, child_error->message);
		g_free (trace_file_path);

		g_error_free (child_error);

		return;
	}

	/* Update comparison message */
	if (priv->enable_online == TRUE) {
		/* Build up the message to compare. We explicitly don't escape nul bytes, because we want the trace
		 * files to be (pretty much) ASCII. File uploads are handled by zero-extending the responses according
		 * to the traced Content-Length. */
		g_byte_array_append (priv->comparison_message, (const guint8 *) message_chunk, message_chunk_length);
		g_byte_array_append (priv->comparison_message, (const guint8 *) "\n", 1);

		if (priv->received_message_state == RESPONSE_TERMINATOR) {
			/* End of a message. */
			base_uri = build_base_uri (self);
			online_message = trace_to_soup_message ((const gchar *) priv->comparison_message->data, base_uri);

			g_byte_array_set_size (priv->comparison_message, 0);
			priv->received_message_state = UNKNOWN;
		}
	}

	/* Append to the hosts file */
	if (online_message != NULL && priv->enable_online == TRUE && priv->enable_logging == TRUE) {
		const char *host = soup_message_headers_get_one (uhm_message_get_request_headers (online_message), "Soup-Host");

		if (!g_output_stream_write_all (G_OUTPUT_STREAM (priv->hosts_output_stream), host, strlen (host), NULL, NULL, &child_error)  ||
		    !g_output_stream_write_all (G_OUTPUT_STREAM (priv->hosts_output_stream), "\n", 1, NULL, NULL, &child_error)) {
			g_autofree gchar *hosts_trace_file_path = g_file_get_path (priv->hosts_trace_file);
			g_warning ("Error appending to host log file ‘%s’: %s", hosts_trace_file_path, child_error->message);
		}

		if (host != NULL)
			g_hash_table_add (priv->hosts, g_strdup (host));
	}

	/* Or compare to the existing trace file. */
	if (online_message != NULL && priv->enable_logging == FALSE && priv->enable_online == TRUE) {
		/* Received the last chunk of the response, so compare the message from the trace file and that from online. */
		g_assert (priv->next_message != NULL);

		/* Compare the message from the server with the message in the log file. */
		if (compare_incoming_message (self, online_message, priv->next_message) != 0) {
			gchar *next_uri, *actual_uri;

			next_uri = uri_get_path_query (uhm_message_get_uri (priv->next_message));
			actual_uri = uri_get_path_query (uhm_message_get_uri (online_message));
			g_set_error (error, UHM_SERVER_ERROR, UHM_SERVER_ERROR_MESSAGE_MISMATCH,
			             "Expected URI ‘%s’, but got ‘%s’.", next_uri, actual_uri);
			g_free (actual_uri);
			g_free (next_uri);

			g_object_unref (online_message);
			return;
		}
	}
}

/**
 * uhm_server_received_message_chunk_with_direction:
 * @self: a #UhmServer
 * @direction: single character indicating the direction of message transmission
 * @data: single line of a message which was received
 * @data_length: length of @data in bytes
 * @error: (allow-none): return location for a #GError, or %NULL
 *
 * Convenience version of uhm_server_received_message_chunk() which takes the
 * message @direction and @data separately, as provided by libsoup in a
 * #SoupLoggerPrinter callback.
 *
 * <informalexample><programlisting>
 * UhmServer *mock_server;
 * SoupSession *session;
 * SoupLogger *logger;
 *
 * static void
 * soup_log_printer (SoupLogger *logger, SoupLoggerLogLevel level, char direction, const char *data, gpointer user_data)
 * {
 * 	/<!-- -->* Pass the data to libuhttpmock. *<!-- -->/
 *	UhmServer *mock_server = UHM_SERVER (user_data);
 * 	uhm_server_received_message_chunk_with_direction (mock_server, direction, data, strlen (data), NULL);
 * }
 *
 * mock_server = uhm_server_new ();
 * session = soup_session_new ();
 *
 * logger = soup_logger_new (SOUP_LOGGER_LOG_BODY, -1);
 * soup_logger_set_printer (logger, (SoupLoggerPrinter) soup_log_printer, g_object_ref (mock_server), g_object_unref);
 * soup_session_add_feature (session, SOUP_SESSION_FEATURE (logger));
 * g_object_unref (logger);
 *
 * /<!-- -->* Do something with mock_server here. *<!-- -->/
 * </programlisting></informalexample>
 *
 * Since: 0.3.0
 */
void
uhm_server_received_message_chunk_with_direction (UhmServer *self, char direction, const gchar *data, goffset data_length, GError **error)
{
	gchar *message_chunk;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (direction == '<' || direction == '>' || direction == ' ');
	g_return_if_fail (data != NULL);
	g_return_if_fail (data_length >= -1);
	g_return_if_fail (error == NULL || *error == NULL);

	/* This is inefficient and not nul-safe, but it’ll do for now. */
	message_chunk = g_strdup_printf ("%c %s", direction, data);
	uhm_server_received_message_chunk (self, message_chunk, (data_length > -1) ? data_length + 2 : -1, error);
	g_free (message_chunk);
}

/**
 * uhm_server_received_message_chunk_from_soup:
 * @logger: a #SoupLogger
 * @level: the detail level of this log message
 * @direction: the transmission direction of the message
 * @data: message data
 * @user_data: (allow-none): user data passed to the #SoupLogger, or %NULL
 *
 * Convenience version of uhm_server_received_message_chunk() which can be passed directly to soup_logger_set_printer()
 * to forward all libsoup traffic logging to a #UhmServer. The #UhmServer must be passed to soup_logger_set_printer() as
 * its user data.
 *
 * <informalexample><programlisting>
 * UhmServer *mock_server;
 * SoupSession *session;
 * SoupLogger *logger;
 *
 * mock_server = uhm_server_new ();
 * session = soup_session_new ();
 *
 * logger = soup_logger_new (SOUP_LOGGER_LOG_BODY, -1);
 * soup_logger_set_printer (logger, uhm_server_received_message_chunk_from_soup, g_object_ref (mock_server), g_object_unref);
 * soup_session_add_feature (session, SOUP_SESSION_FEATURE (logger));
 * g_object_unref (logger);
 *
 * /<!-- -->* Do something with mock_server here. *<!-- -->/
 * </programlisting></informalexample>
 *
 * Since: 0.3.0
 */
void
uhm_server_received_message_chunk_from_soup (SoupLogger *logger, SoupLoggerLogLevel level, char direction, const char *data, gpointer user_data)
{
	/* Deliberately don’t do strict validation of parameters here, since we can’t be entirely sure what libsoup throws our way. */
	UhmServer *mock_server = UHM_SERVER (user_data);
	uhm_server_received_message_chunk_with_direction (mock_server, direction, data, strlen (data), NULL);
}

/**
 * uhm_server_get_address:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:address property.
 *
 * Return value: (allow-none) (transfer none): the physical address of the listening socket the server is currently bound to; or %NULL if the server is not running
 *
 * Since: 0.1.0
 */
const gchar *
uhm_server_get_address (UhmServer *self)
{
	GInetAddress *addr;

	g_return_val_if_fail (UHM_IS_SERVER (self), NULL);

	if (self->priv->address == NULL) {
		return NULL;
	}

	g_free (self->priv->address_string);
	addr = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (self->priv->address));
	self->priv->address_string = g_inet_address_to_string (addr);
	return self->priv->address_string;
}

/**
 * uhm_server_get_port:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:port property.
 *
 * Return value: the port of the listening socket the server is currently bound to; or <code class="literal">0</code> if the server is not running
 *
 * Since: 0.1.0
 */
guint
uhm_server_get_port (UhmServer *self)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), 0);

	return self->priv->port;
}

/**
 * uhm_server_get_resolver:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:resolver property.
 *
 * Return value: (allow-none) (transfer none): the mock resolver in use by the mock server, or %NULL if no resolver is active
 *
 * Since: 0.1.0
 */
UhmResolver *
uhm_server_get_resolver (UhmServer *self)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), NULL);

	return self->priv->resolver;
}

/**
 * uhm_server_get_tls_certificate:
 * @self: a #UhmServer
 *
 * Gets the value of the #UhmServer:tls-certificate property.
 *
 * Return value: (transfer none) (allow-none): the server's current TLS certificate; or %NULL if it's serving HTTP only
 *
 * Since: 0.1.0
 */
GTlsCertificate *
uhm_server_get_tls_certificate (UhmServer *self)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), NULL);

	return self->priv->tls_certificate;
}

/**
 * uhm_server_set_tls_certificate:
 * @self: a #UhmServer
 * @tls_certificate: (allow-none): TLS certificate for the server to use; or %NULL to serve HTTP only
 *
 * Sets the value of the #UhmServer:tls-certificate property.
 *
 * Since: 0.1.0
 */
void
uhm_server_set_tls_certificate (UhmServer *self, GTlsCertificate *tls_certificate)
{
	UhmServerPrivate *priv;

	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (tls_certificate == NULL || G_IS_TLS_CERTIFICATE (tls_certificate));

	priv = self->priv;

	if (tls_certificate != NULL) {
		g_object_ref (tls_certificate);
	}

	g_clear_object (&priv->tls_certificate);
	priv->tls_certificate = tls_certificate;
	g_object_notify (G_OBJECT (self), "tls-certificate");
}

/**
 * uhm_server_set_default_tls_certificate:
 * @self: a #UhmServer
 *
 * Sets the value of the #UhmServer:tls-certificate property to the default TLS certificate built in to libuhttpmock.
 * This default certificate is not signed by any certificate authority, and contains minimal metadata details. It may
 * be used by clients which have no special certificate requirements; clients which have special requirements should
 * construct a custom #GTlsCertificate and pass it to uhm_server_set_tls_certificate().
 *
 * Return value: (transfer none): the default certificate set as #UhmServer:tls-certificate
 *
 * Since: 0.1.0
 */
GTlsCertificate *
uhm_server_set_default_tls_certificate (UhmServer *self)
{
	GTlsCertificate *cert;
	GError *child_error = NULL;

	g_return_val_if_fail (UHM_IS_SERVER (self), NULL);

	/* Build the certificate. */
	cert = g_tls_certificate_new_from_pem (uhm_default_tls_certificate, -1, &child_error);
	g_assert_no_error (child_error);

	/* Set it as the property. */
	uhm_server_set_tls_certificate (self, cert);
	g_object_unref (cert);

	return cert;
}

static void
apply_expected_domain_names (UhmServer *self)
{
	UhmServerPrivate *priv = self->priv;
	const gchar *ip_address;
	guint i;

	if (priv->resolver == NULL) {
		return;
	}

	uhm_resolver_reset (priv->resolver);

	if (priv->expected_domain_names == NULL) {
		return;
	}

	ip_address = uhm_server_get_address (self);
	g_assert (ip_address != NULL);

	for (i = 0; priv->expected_domain_names[i] != NULL; i++) {
		uhm_resolver_add_A (priv->resolver, priv->expected_domain_names[i], ip_address);
	}
}

/**
 * uhm_server_set_expected_domain_names:
 * @self: a #UhmServer
 * @domain_names: (array zero-terminated=1) (allow-none) (element-type utf8): %NULL-terminated array of domain names to expect, or %NULL to not expect any
 *
 * Set the domain names which are expected to have requests made of them by the client code interacting with this #UhmServer.
 * This is a convenience method which calls uhm_resolver_add_A() on the server’s #UhmResolver for each of the domain names
 * listed in @domain_names. It associates them with the server’s current IP address, and automatically updates the mappings
 * if the IP address or resolver change.
 *
 * Note that this will reset all records on the server’s #UhmResolver, replacing all of them with the provided @domain_names.
 *
 * It is safe to add further domain names to the #UhmResolver in a callback for the #GObject::notify signal for #UhmServer:resolver;
 * that signal is emitted after the resolver is cleared and these @domain_names are added.
 *
 * Since: 0.3.0
 */
void
uhm_server_set_expected_domain_names (UhmServer *self, const gchar * const *domain_names)
{
	gchar **new_domain_names;

	g_return_if_fail (UHM_IS_SERVER (self));

	new_domain_names = g_strdupv ((gchar **) domain_names);  /* may be NULL */
	g_strfreev (self->priv->expected_domain_names);
	self->priv->expected_domain_names = new_domain_names;

	apply_expected_domain_names (self);
}

static gboolean
compare_messages_ignore_parameter_values_cb (UhmServer *server,
                                             UhmMessage *expected_message,
                                             UhmMessage *actual_message,
                                             gpointer user_data)
{
	GUri *expected_uri, *actual_uri;
	const gchar * const *ignore_query_param_values = user_data;
	gboolean retval;
	GHashTable/*<string, string>*/ *expected_params = NULL;  /* owned */
	GHashTable/*<string, string>*/ *actual_params = NULL;  /* owned */
	GHashTableIter iter;
	const gchar *key, *expected_value;
	guint i;

	/* Compare method. */
	if (g_strcmp0 (uhm_message_get_method (expected_message), uhm_message_get_method (actual_message)) != 0) {
		return FALSE;
	}

	/* Compare URIs, excluding query parameters. */
	expected_uri = uhm_message_get_uri (expected_message);
	actual_uri = uhm_message_get_uri (actual_message);

	if (!parts_equal (g_uri_get_user (expected_uri), g_uri_get_user (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_password (expected_uri), g_uri_get_password (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_path (expected_uri), g_uri_get_path (actual_uri), FALSE) ||
	    !parts_equal (g_uri_get_fragment (expected_uri), g_uri_get_fragment (actual_uri), FALSE)) {
		return FALSE;
	}

	/* Compare query parameters, excluding the ignored ones. Note that we
	 * expect the ignored parameters to exist, but their values may
	 * differ. */
	expected_params = soup_form_decode (g_uri_get_query (expected_uri));
	actual_params = soup_form_decode (g_uri_get_query (actual_uri));

	/* Check the presence of ignored parameters. */
	for (i = 0; ignore_query_param_values[i] != NULL; i++) {
		const gchar *name = ignore_query_param_values[i];

		if (g_hash_table_contains (expected_params, name) &&
		    !g_hash_table_contains (expected_params, name)) {
			retval = FALSE;
			goto done;
		}

		/* Remove them to simplify the comparison below. */
		g_hash_table_remove (expected_params, name);
		g_hash_table_remove (actual_params, name);
	}

	if (g_hash_table_size (actual_params) !=
	    g_hash_table_size (expected_params)) {
		retval = FALSE;
		goto done;
	}

	g_hash_table_iter_init (&iter, expected_params);

	while (g_hash_table_iter_next (&iter, (gpointer) &key,
	                               (gpointer) &expected_value)) {
		if (g_strcmp0 (expected_value,
		               g_hash_table_lookup (actual_params, key)) != 0) {
			retval = FALSE;
			goto done;
		}
	}

	retval = TRUE;

done:
	g_hash_table_unref (actual_params);
	g_hash_table_unref (expected_params);

	return retval;
}

static void
parameter_names_closure_notify (gpointer  data,
                                GClosure *closure)
{
	gchar **parameter_names = data;
	g_strfreev (parameter_names);
}

/**
 * uhm_server_filter_ignore_parameter_values:
 * @self: a #UhmServer
 * @parameter_names: (array zero-terminated=1): %NULL-terminated array of
 *    parameter names to ignore
 *
 * Install a #UhmServer:compare-messages filter function which will override the
 * default comparison function to one which ignores differences in the values of
 * the given query @parameter_names. The named parameters must still be present
 * in the query, however.
 *
 * The filter will remain in place for the lifetime of the #UhmServer, until
 * @uhm_server_compare_messages_remove_filter() is called with the returned
 * filter ID.
 *
 * Note that currently only one of the installed comparison functions will be
 * used. This may change in future.
 *
 * Returns: opaque filter ID used with
 *    uhm_server_compare_messages_remove_filter() to remove the filter later
 * Since: 0.5.0
 */
gulong
uhm_server_filter_ignore_parameter_values (UhmServer *self,
                                           const gchar * const *parameter_names)
{
	g_return_val_if_fail (UHM_IS_SERVER (self), 0);
	g_return_val_if_fail (parameter_names != NULL, 0);

	/* FIXME: What are the semantics of multiple installed compare-messages
	 * callbacks? Should they be aggregate-true? */
	return g_signal_connect_data (self, "compare-messages",
	                              (GCallback) compare_messages_ignore_parameter_values_cb,
	                              g_strdupv ((gchar **) parameter_names),
	                              parameter_names_closure_notify,
	                              0  /* connect flags */);
}

/**
 * uhm_server_compare_messages_remove_filter:
 * @self: a #UhmServer
 * @filter_id: filter ID returned by the filter addition function
 *
 * Remove a #UhmServer:compare-messages filter function installed previously by
 * calling something like uhm_server_filter_ignore_parameter_values().
 *
 * It is an error to call this function with an invalid @filter_id.
 *
 * Since: 0.5.0
 */
void
uhm_server_compare_messages_remove_filter (UhmServer *self,
                                           gulong filter_id)
{
	g_return_if_fail (UHM_IS_SERVER (self));
	g_return_if_fail (filter_id != 0);

	g_signal_handler_disconnect (self, filter_id);
}
