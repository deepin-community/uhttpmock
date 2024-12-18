/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * uhttpmock
 * Copyright (C) Philip Withnall 2013, 2015 <philip@tecnocode.co.uk>
 * Copyright (C) Collabora Ltd. 2009
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
 *
 * Original author: Vivek Dasmohapatra <vivek@collabora.co.uk>
 */

/*
 * This code is heavily based on code originally by Vivek Dasmohapatra, found here:
 * http://cgit.collabora.com/git/user/sjoerd/telepathy-gabble.git/plain/tests/twisted/test-resolver.c
 * It was originally licenced under LGPLv2.1+.
 */

/**
 * SECTION:uhm-resolver
 * @short_description: mock DNS resolver
 * @stability: Unstable
 * @include: libuhttpmock/uhm-resolver.h
 *
 * A mock DNS resolver which resolves according to specified host-name–IP-address pairs, and raises an error for all non-specified host name requests.
 * This allows network connections for expected services to be redirected to a different server, such as a local mock server on a loopback interface.
 *
 * Since: 0.1.0
 */

#include <stdio.h>
#include <glib.h>
#include <gio/gio.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "uhm-resolver.h"

static void uhm_resolver_finalize (GObject *object);

static GList *uhm_resolver_lookup_by_name_with_flags (GResolver *resolver, const gchar *hostname, GResolverNameLookupFlags flags, GCancellable *cancellable, GError **error);
static GList *uhm_resolver_lookup_by_name (GResolver *resolver, const gchar *hostname, GCancellable *cancellable, GError **error);
static void uhm_resolver_lookup_by_name_with_flags_async (GResolver *resolver, const gchar *hostname, GResolverNameLookupFlags flags, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
static void uhm_resolver_lookup_by_name_async (GResolver *resolver, const gchar *hostname, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
static GList *uhm_resolver_lookup_by_name_finish (GResolver *resolver, GAsyncResult *result, GError **error);
static GList *uhm_resolver_lookup_service (GResolver *resolver, const gchar *rrname, GCancellable *cancellable, GError **error);
static void uhm_resolver_lookup_service_async (GResolver *resolver, const gchar *rrname, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
static GList *uhm_resolver_lookup_service_finish (GResolver *resolver, GAsyncResult *result, GError **error);

typedef struct {
	gchar *key;
	gchar *addr;
} FakeHost;

typedef struct {
	char *key;
	GSrvTarget *srv;
} FakeService;

struct _UhmResolverPrivate {
	GList *fake_A;
	GList *fake_SRV;
};

G_DEFINE_TYPE_WITH_PRIVATE (UhmResolver, uhm_resolver, G_TYPE_RESOLVER)

static void
uhm_resolver_class_init (UhmResolverClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GResolverClass *resolver_class = G_RESOLVER_CLASS (klass);

	gobject_class->finalize = uhm_resolver_finalize;

	resolver_class->lookup_by_name = uhm_resolver_lookup_by_name;
	resolver_class->lookup_by_name_async = uhm_resolver_lookup_by_name_async;
	resolver_class->lookup_by_name_finish = uhm_resolver_lookup_by_name_finish;
	resolver_class->lookup_by_name_with_flags = uhm_resolver_lookup_by_name_with_flags;
	resolver_class->lookup_by_name_with_flags_async = uhm_resolver_lookup_by_name_with_flags_async;
	resolver_class->lookup_by_name_with_flags_finish = uhm_resolver_lookup_by_name_finish;
	resolver_class->lookup_service = uhm_resolver_lookup_service;
	resolver_class->lookup_service_async = uhm_resolver_lookup_service_async;
	resolver_class->lookup_service_finish = uhm_resolver_lookup_service_finish;
}

static void
uhm_resolver_init (UhmResolver *self)
{
	self->priv = uhm_resolver_get_instance_private (self);
}

static void
uhm_resolver_finalize (GObject *object)
{
	uhm_resolver_reset (UHM_RESOLVER (object));

	/* Chain up to the parent class */
	G_OBJECT_CLASS (uhm_resolver_parent_class)->finalize (object);
}

static gchar *
_service_rrname (const char *service, const char *protocol, const char *domain)
{
	gchar *rrname, *ascii_domain;

	ascii_domain = g_hostname_to_ascii (domain);
	rrname = g_strdup_printf ("_%s._%s.%s", service, protocol, ascii_domain);
	g_free (ascii_domain);

	return rrname;
}

static GList *
find_fake_services (UhmResolver *self, const char *name)
{
	GList *fake = NULL;
	GList *rval = NULL;

	for (fake = self->priv->fake_SRV; fake != NULL; fake = g_list_next (fake)) {
		FakeService *entry = fake->data;
		if (entry != NULL && !g_strcmp0 (entry->key, name)) {
			rval = g_list_append (rval, g_srv_target_copy (entry->srv));
		}
	}

	return rval;
}

static void
fake_services_free (GList/*<owned GSrvTarget>*/ *services)
{
	g_list_free_full (services, (GDestroyNotify) g_object_unref);
}

static GList *
find_fake_hosts (UhmResolver *self, const char *name, GResolverNameLookupFlags flags)
{
	GList *fake = NULL;
	GList *rval = NULL;

	for (fake = self->priv->fake_A; fake != NULL; fake = g_list_next (fake)) {
		FakeHost *entry = fake->data;
		if (entry != NULL && !g_strcmp0 (entry->key, name)) {
			g_autoptr (GInetAddress) addr;
			GSocketFamily fam;
			addr = g_inet_address_new_from_string (entry->addr);
			fam = g_inet_address_get_family (addr);
			switch (flags) {
				case G_RESOLVER_NAME_LOOKUP_FLAGS_IPV4_ONLY:
					if (fam == G_SOCKET_FAMILY_IPV6)
						continue;
					break;
				case G_RESOLVER_NAME_LOOKUP_FLAGS_IPV6_ONLY:
					if (fam == G_SOCKET_FAMILY_IPV4)
						continue;
					break;
				case G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT:
				default:
					break;
			}
			rval = g_list_append (rval, g_steal_pointer (&addr));
		}
	}

	return rval;
}

static void
fake_hosts_free (GList/*<owned GInetAddress>*/ *addrs)
{
	g_list_free_full (addrs, (GDestroyNotify) g_object_unref);
}

static GList *
uhm_resolver_lookup_by_name_with_flags (GResolver *resolver, const gchar *hostname, GResolverNameLookupFlags flags, GCancellable *cancellable, GError **error)
{
	GList *result;

	result = find_fake_hosts (UHM_RESOLVER (resolver), hostname, flags);

	if (result == NULL) {
		g_set_error (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "No fake hostname record registered for ‘%s’.", hostname);
	}

	return result;
}

static GList *
uhm_resolver_lookup_by_name (GResolver *resolver, const gchar *hostname, GCancellable *cancellable, GError **error)
{
	return uhm_resolver_lookup_by_name_with_flags (resolver, hostname, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable, error);
}

static void
uhm_resolver_lookup_by_name_with_flags_async (GResolver *resolver, const gchar *hostname, GResolverNameLookupFlags flags, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task = NULL;  /* owned */
	GList/*<owned GInetAddress>*/ *addr = NULL;  /* owned */
	GError *error = NULL;

	task = g_task_new (resolver, cancellable, callback, user_data);
	g_task_set_source_tag (task, uhm_resolver_lookup_by_name_async);

	addr = uhm_resolver_lookup_by_name_with_flags (resolver, hostname, flags, NULL, &error);

	if (addr != NULL) {
		g_task_return_pointer (task, addr,
		                       (GDestroyNotify) fake_hosts_free);
	} else {
		g_task_return_error (task, error);
	}

	g_object_unref (task);
}

static void
uhm_resolver_lookup_by_name_async (GResolver *resolver, const gchar *hostname, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	uhm_resolver_lookup_by_name_with_flags_async (resolver, hostname, G_RESOLVER_NAME_LOOKUP_FLAGS_DEFAULT, cancellable, callback, user_data);
}

static GList *
uhm_resolver_lookup_by_name_finish (GResolver *resolver, GAsyncResult *result, GError **error)
{
	GTask *task;  /* unowned */

	g_return_val_if_fail (g_task_is_valid (result, resolver), NULL);
	g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) ==
	                      uhm_resolver_lookup_by_name_async, NULL);

	task = G_TASK (result);

	return g_task_propagate_pointer (task, error);
}

static GList *
uhm_resolver_lookup_service (GResolver *resolver, const gchar *rrname, GCancellable *cancellable, GError **error)
{
	GList *result;

	result = find_fake_services (UHM_RESOLVER (resolver), rrname);

	if (result == NULL) {
		g_set_error (error, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND, "No fake service records registered for ‘%s’.", rrname);
	}

	return result;
}

static void
uhm_resolver_lookup_service_async (GResolver *resolver, const gchar *rrname, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	GList/*<owned GSrvTarget>*/ *addrs = NULL;  /* owned */
	GTask *task = NULL;  /* owned */
	GError *error = NULL;

	task = g_task_new (resolver, cancellable, callback, user_data);
	g_task_set_source_tag (task, uhm_resolver_lookup_service_async);

	addrs = uhm_resolver_lookup_service (resolver, rrname,
	                                     cancellable, &error);

	if (addrs != NULL) {
		g_task_return_pointer (task, addrs,
		                       (GDestroyNotify) fake_services_free);
	} else {
		g_task_return_error (task, error);
	}

	g_object_unref (task);
}

static GList *
uhm_resolver_lookup_service_finish (GResolver *resolver, GAsyncResult *result, GError **error)
{
	GTask *task;  /* unowned */

	g_return_val_if_fail (g_task_is_valid (result, resolver), NULL);
	g_return_val_if_fail (g_task_get_source_tag (G_TASK (result)) ==
	                      uhm_resolver_lookup_service_async, NULL);

	task = G_TASK (result);

	return g_task_propagate_pointer (task, error);
}

/**
 * uhm_resolver_new:
 *
 * Creates a new #UhmResolver with default property values.
 *
 * Return value: (transfer full): a new #UhmResolver; unref with g_object_unref()
 */
UhmResolver *
uhm_resolver_new (void)
{
	return g_object_new (UHM_TYPE_RESOLVER, NULL);
}

/**
 * uhm_resolver_reset:
 * @self: a #UhmResolver
 *
 * Resets the state of the #UhmResolver, deleting all records added with uhm_resolver_add_A() and uhm_resolver_add_SRV().
 */
void
uhm_resolver_reset (UhmResolver *self)
{
	GList *fake = NULL;

	g_return_if_fail (UHM_IS_RESOLVER (self));

	for (fake = self->priv->fake_A; fake != NULL; fake = g_list_next (fake)) {
		FakeHost *entry = fake->data;
		g_free (entry->key);
		g_free (entry->addr);
		g_free (entry);
	}
	g_list_free (self->priv->fake_A);
	self->priv->fake_A = NULL;

	for (fake = self->priv->fake_SRV; fake != NULL; fake = g_list_next (fake)) {
		FakeService *entry = fake->data;
		g_free (entry->key);
		g_srv_target_free (entry->srv);
		g_free (entry);
	}
	g_list_free (self->priv->fake_SRV);
	self->priv->fake_SRV = NULL;
}

/**
 * uhm_resolver_add_A:
 * @self: a #UhmResolver
 * @hostname: the hostname to match
 * @addr: the IP address to resolve to
 *
 * Adds a resolution mapping from the host name @hostname to the IP address @addr.
 *
 * Return value: %TRUE on success; %FALSE otherwise
 *
 * Since: 0.1.0
 */
gboolean
uhm_resolver_add_A (UhmResolver *self, const gchar *hostname, const gchar *addr)
{
	FakeHost *entry;

	g_return_val_if_fail (UHM_IS_RESOLVER (self), FALSE);
	g_return_val_if_fail (hostname != NULL && *hostname != '\0', FALSE);
	g_return_val_if_fail (addr != NULL && *addr != '\0', FALSE);

	entry = g_new0 (FakeHost, 1);
	entry->key = g_strdup (hostname);
	entry->addr = g_strdup (addr);
	self->priv->fake_A = g_list_append (self->priv->fake_A, entry);

	return TRUE;
}

/**
 * uhm_resolver_add_SRV:
 * @self: a #UhmResolver
 * @service: the service name to match
 * @protocol: the protocol name to match
 * @domain: the domain name to match
 * @addr: the IP address to resolve to
 * @port: the port to resolve to
 *
 * Adds a resolution mapping the given @service (on @protocol and @domain) to the IP address @addr and given @port.
 *
 * Return value: %TRUE on success; %FALSE otherwise
 *
 * Since: 0.1.0
 */
gboolean
uhm_resolver_add_SRV (UhmResolver *self, const gchar *service, const gchar *protocol, const gchar *domain, const gchar *addr, guint16 port)
{
	gchar *key;
	GSrvTarget *serv;
	FakeService *entry;

	g_return_val_if_fail (UHM_IS_RESOLVER (self), FALSE);
	g_return_val_if_fail (service != NULL && *service != '\0', FALSE);
	g_return_val_if_fail (protocol != NULL && *protocol != '\0', FALSE);
	g_return_val_if_fail (domain != NULL && *domain != '\0', FALSE);
	g_return_val_if_fail (addr != NULL && *addr != '\0', FALSE);
	g_return_val_if_fail (port > 0, FALSE);

	key = _service_rrname (service, protocol, domain);
	entry = g_new0 (FakeService, 1);
	serv = g_srv_target_new (addr, port, 0, 0);
	entry->key = key;
	entry->srv = serv;
	self->priv->fake_SRV = g_list_append (self->priv->fake_SRV, entry);

	return TRUE;
}
