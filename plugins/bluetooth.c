/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ProFUSION embedded systems
 *  Copyright (C) 2010  Gustavo F. Padovan <gustavo@padovan.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <glib.h>
#include <gdbus.h>
#include <ofono.h>

#include <ofono/dbus.h>

#include <btio.h>
#include "bluetooth.h"

static DBusConnection *connection;
static GHashTable *uuid_hash = NULL;
static GHashTable *adapter_address_hash = NULL;
static gint bluetooth_refcount;
static GSList *server_list = NULL;

#define TIMEOUT 60 /* Timeout for user response (seconds) */

struct server {
	guint8 channel;
	char *sdp_record;
	GIOChannel *io;
	GHashTable *adapter_hash;
	ConnectFunc connect_cb;
	gpointer user_data;
};

struct cb_data {
	struct server *server;
	char *path;
	guint source;
	GIOChannel *io;
};

void bluetooth_create_path(const char *dev_addr, const char *adapter_addr,
				char *buf, int size)
{
	int i, j;

	for (i = 0, j = 0; adapter_addr[j] && i < size - 1; j++)
		if (adapter_addr[j] >= '0' && adapter_addr[j] <= '9')
			buf[i++] = adapter_addr[j];
		else if (adapter_addr[j] >= 'A' && adapter_addr[j] <= 'F')
			buf[i++] = adapter_addr[j];

	if (i < size - 1)
		buf[i++] = '_';

	for (j = 0; dev_addr[j] && i < size - 1; j++)
		if (dev_addr[j] >= '0' && dev_addr[j] <= '9')
			buf[i++] = dev_addr[j];
		else if (dev_addr[j] >= 'A' && dev_addr[j] <= 'F')
			buf[i++] = dev_addr[j];

	buf[i] = '\0';
}

int bluetooth_send_with_reply(const char *path, const char *interface,
				const char *method,
				DBusPendingCallNotifyFunction cb,
				void *user_data, DBusFreeFunction free_func,
				int timeout, int type, ...)
{
	DBusMessage *msg;
	DBusPendingCall *call;
	va_list args;
	int err;

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, path,
						interface, method);
	if (msg == NULL) {
		ofono_error("Unable to allocate new D-Bus %s message", method);
		err = -ENOMEM;
		goto fail;
	}

	va_start(args, type);

	if (!dbus_message_append_args_valist(msg, type, args)) {
		va_end(args);
		err = -EIO;
		goto fail;
	}

	va_end(args);

	if (timeout > 0)
		timeout *= 1000;

	if (!dbus_connection_send_with_reply(connection, msg, &call, timeout)) {
		ofono_error("Sending %s failed", method);
		err = -EIO;
		goto fail;
	}

	dbus_pending_call_set_notify(call, cb, user_data, free_func);
	dbus_pending_call_unref(call);
	dbus_message_unref(msg);

	return 0;

fail:
	if (free_func && user_data)
		free_func(user_data);

	if (msg)
		dbus_message_unref(msg);

	return err;
}

typedef void (*PropertyHandler)(DBusMessageIter *iter, gpointer user_data);

struct property_handler {
	const char *property;
	PropertyHandler callback;
	gpointer user_data;
};

static gint property_handler_compare(gconstpointer a, gconstpointer b)
{
	const struct property_handler *handler = a;
	const char *property = b;

	return strcmp(handler->property, property);
}

void bluetooth_parse_properties(DBusMessage *reply, const char *property, ...)
{
	va_list args;
	GSList *prop_handlers = NULL;
	DBusMessageIter array, dict;

	va_start(args, property);

	while (property != NULL) {
		struct property_handler *handler =
					g_new0(struct property_handler, 1);

		handler->property = property;
		handler->callback = va_arg(args, PropertyHandler);
		handler->user_data = va_arg(args, gpointer);

		property = va_arg(args, const char *);

		prop_handlers = g_slist_prepend(prop_handlers, handler);
	}

	va_end(args);

	if (dbus_message_iter_init(reply, &array) == FALSE)
		goto done;

	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY)
		goto done;

	dbus_message_iter_recurse(&array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;
		GSList *l;

		dbus_message_iter_recurse(&dict, &entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto done;

		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);

		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			goto done;

		dbus_message_iter_recurse(&entry, &value);

		l = g_slist_find_custom(prop_handlers, key,
					property_handler_compare);

		if (l) {
			struct property_handler *handler = l->data;

			handler->callback(&value, handler->user_data);
		}

		dbus_message_iter_next(&dict);
	}

done:
	g_slist_foreach(prop_handlers, (GFunc)g_free, NULL);
	g_slist_free(prop_handlers);
}

static void has_uuid(DBusMessageIter *array, gpointer user_data)
{
	gboolean *profiles = user_data;
	DBusMessageIter value;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
		const char *uuid;

		dbus_message_iter_get_basic(&value, &uuid);

		if (!strcasecmp(uuid, HFP_AG_UUID))
			*profiles |= HFP_AG;

		dbus_message_iter_next(&value);
	}
}

static void parse_string(DBusMessageIter *iter, gpointer user_data)
{
	char **str = user_data;
	int arg_type = dbus_message_iter_get_arg_type(iter);

	if (arg_type != DBUS_TYPE_OBJECT_PATH && arg_type != DBUS_TYPE_STRING)
		return;

	dbus_message_iter_get_basic(iter, str);
}

static void device_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;
	int have_uuid = 0;
	const char *path = user_data;
	const char *adapter = NULL;
	const char *adapter_addr = NULL;
	const char *device_addr = NULL;
	const char *alias = NULL;
	struct bluetooth_profile *profile;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		if (!dbus_message_is_error(reply, DBUS_ERROR_UNKNOWN_METHOD))
			ofono_info("Error from GetProperties reply: %s",
					dbus_message_get_error_name(reply));

		goto done;
	}

	bluetooth_parse_properties(reply, "UUIDs", has_uuid, &have_uuid,
				"Adapter", parse_string, &adapter,
				"Address", parse_string, &device_addr,
				"Alias", parse_string, &alias, NULL);

	if (adapter)
		adapter_addr = g_hash_table_lookup(adapter_address_hash,
							adapter);

	if ((have_uuid & HFP_AG) && device_addr && adapter_addr) {
		profile = g_hash_table_lookup(uuid_hash, HFP_AG_UUID);
		if (profile == NULL || profile->create == NULL)
			goto done;

		profile->create(path, device_addr, adapter_addr, alias);
	}

done:
	dbus_message_unref(reply);
}

static void parse_devices(DBusMessageIter *array, gpointer user_data)
{
	DBusMessageIter value;
	GSList **device_list = user_data;

	DBG("");

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value)
			== DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&value, &path);

		*device_list = g_slist_prepend(*device_list, (gpointer) path);

		dbus_message_iter_next(&value);
	}
}

static gboolean property_changed(DBusConnection *connection, DBusMessage *msg,
				void *user_data)
{
	const char *property;
	DBusMessageIter iter;

	dbus_message_iter_init(msg, &iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return FALSE;

	dbus_message_iter_get_basic(&iter, &property);
	if (g_str_equal(property, "UUIDs") == TRUE) {
		int profiles = 0;
		const char *path = dbus_message_get_path(msg);
		DBusMessageIter variant;

		if (!dbus_message_iter_next(&iter))
			return FALSE;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return FALSE;

		dbus_message_iter_recurse(&iter, &variant);

		has_uuid(&variant, &profiles);

		/* We need the full set of properties to be able to create
		 * the modem properly, including Adapter and Alias, so
		 * refetch everything again
		 */
		if (profiles)
			bluetooth_send_with_reply(path, BLUEZ_DEVICE_INTERFACE,
					"GetProperties", device_properties_cb,
					g_strdup(path), g_free, -1,
					DBUS_TYPE_INVALID);
	} else if (g_str_equal(property, "Alias") == TRUE) {
		const char *path = dbus_message_get_path(msg);
		struct bluetooth_profile *profile;
		const char *alias = NULL;
		DBusMessageIter variant;
		GHashTableIter hash_iter;
		gpointer key, value;

		if (!dbus_message_iter_next(&iter))
			return FALSE;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
			return FALSE;

		dbus_message_iter_recurse(&iter, &variant);

		parse_string(&variant, &alias);

		g_hash_table_iter_init(&hash_iter, uuid_hash);
		while (g_hash_table_iter_next(&hash_iter, &key, &value)) {
			profile = value;
			if (profile->set_alias)
				profile->set_alias(path, alias);
		}
	}

	return TRUE;
}

static void adapter_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	const char *path = user_data;
	DBusMessage *reply;
	GSList *device_list = NULL;
	GSList *l;
	const char *addr;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	bluetooth_parse_properties(reply,
					"Devices", parse_devices, &device_list,
					"Address", parse_string, &addr,
					NULL);

	DBG("Adapter Address: %s, Path: %s", addr, path);
	g_hash_table_insert(adapter_address_hash,
				g_strdup(path), g_strdup(addr));

	for (l = device_list; l; l = l->next) {
		const char *device = l->data;

		bluetooth_send_with_reply(device, BLUEZ_DEVICE_INTERFACE,
					"GetProperties", device_properties_cb,
					g_strdup(device), g_free, -1,
					DBUS_TYPE_INVALID);
	}

done:
	g_slist_free(device_list);
	dbus_message_unref(reply);
}

static void get_adapter_properties(const char *path, const char *handle,
						gpointer user_data)
{
	bluetooth_send_with_reply(path, BLUEZ_ADAPTER_INTERFACE,
			"GetProperties", adapter_properties_cb,
			g_strdup(path), g_free, -1, DBUS_TYPE_INVALID);
}

static void remove_record(char *path, guint handle, struct server *server)
{
	DBusMessage *msg;

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, path,
					BLUEZ_SERVICE_INTERFACE,
					"RemoveRecord");
	if (msg == NULL) {
		ofono_error("Unable to allocate D-Bus RemoveRecord message");
		return;
	}

	dbus_message_append_args(msg, DBUS_TYPE_UINT32, &handle,
					DBUS_TYPE_INVALID);
	g_dbus_send_message(connection, msg);

	ofono_info("Unregistered handle for %s, channel %d: 0x%x", path,
			server->channel, handle);
}

static void server_stop(struct server *server)
{
	g_hash_table_foreach_remove(server->adapter_hash,
					(GHRFunc) remove_record, server);

	if (server->io != NULL) {
		g_io_channel_shutdown(server->io, TRUE, NULL);
		g_io_channel_unref(server->io);
		server->io = NULL;
	}
}

static void cb_data_destroy(gpointer data)
{
	struct cb_data *cb_data = data;

	if (cb_data->source != 0)
		g_source_remove(cb_data->source);

	g_free(cb_data->path);
	g_free(cb_data);
}

static void cancel_authorization(struct cb_data *user_data)
{
	DBusMessage *msg;

	msg = dbus_message_new_method_call(BLUEZ_SERVICE, user_data->path,
						BLUEZ_SERVICE_INTERFACE,
						"CancelAuthorization");

	if (msg == NULL) {
		ofono_error("Unable to allocate D-Bus CancelAuthorization"
				" message");
		return;
	}

	g_dbus_send_message(connection, msg);
}

static gboolean client_event(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	struct cb_data *cb_data = data;

	cancel_authorization(cb_data);
	cb_data->source = 0;

	return FALSE;
}

static void auth_cb(DBusPendingCall *call, gpointer user_data)
{
	struct cb_data *cb_data = user_data;
	struct server *server = cb_data->server;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError derr;
	GError *err = NULL;

	dbus_error_init(&derr);

	if (dbus_set_error_from_message(&derr, reply)) {
		ofono_error("RequestAuthorization error: %s, %s",
				derr.name, derr.message);

		if (dbus_error_has_name(&derr, DBUS_ERROR_NO_REPLY))
			cancel_authorization(cb_data);

		dbus_error_free(&derr);
	} else {
		ofono_info("RequestAuthorization succeeded");

		if (!bt_io_accept(cb_data->io, server->connect_cb,
					server->user_data, NULL, &err)) {
			ofono_error("%s", err->message);
			g_error_free(err);
		}
	}

	dbus_message_unref(reply);
}

static void new_connection(GIOChannel *io, gpointer user_data)
{
	struct server *server = user_data;
	struct cb_data *cbd;
	guint handle;
	const char *addr;
	GError *err = NULL;
	char laddress[18], raddress[18];
	guint8 channel;
	GHashTableIter iter;
	gpointer key, value;
	const char *path;
	gpointer handlep;

	bt_io_get(io, BT_IO_RFCOMM, &err, BT_IO_OPT_SOURCE, laddress,
					BT_IO_OPT_DEST, raddress,
					BT_IO_OPT_CHANNEL, &channel,
					BT_IO_OPT_INVALID);
	if (err) {
		ofono_error("%s", err->message);
		g_error_free(err);
		return;
	}

	ofono_info("New connection for %s on channel %u from: %s,", laddress,
							channel, raddress);

	path = NULL;
	g_hash_table_iter_init(&iter, adapter_address_hash);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (g_str_equal(laddress, value) == TRUE) {
			path = key;
			break;
		}
	}

	if (path == NULL)
		return;

	handlep = g_hash_table_lookup(server->adapter_hash, path);
	if (handlep == NULL)
		return;

	cbd = g_try_new0(struct cb_data, 1);
	if (cbd == NULL) {
		ofono_error("Unable to allocate client cb_data structure");
		return;
	}

	cbd->path = g_strdup(path);
	cbd->server = server;
	cbd->io = io;

	addr = raddress;
	handle = GPOINTER_TO_UINT(handlep);

	if (bluetooth_send_with_reply(path, BLUEZ_SERVICE_INTERFACE,
					"RequestAuthorization",
					auth_cb, cbd, cb_data_destroy,
					TIMEOUT, DBUS_TYPE_STRING, &addr,
					DBUS_TYPE_UINT32, &handle,
					DBUS_TYPE_INVALID) < 0) {
		ofono_error("Request Bluetooth authorization failed");
		return;
	}

	ofono_info("RequestAuthorization(%s, 0x%x)", raddress, handle);

	cbd->source = g_io_add_watch(io, G_IO_HUP | G_IO_ERR | G_IO_NVAL,
					client_event, cbd);
}

static void add_record_cb(DBusPendingCall *call, gpointer user_data)
{
	struct cb_data *cb_data = user_data;
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusError derr;
	guint32 handle;

	dbus_error_init(&derr);

	if (dbus_set_error_from_message(&derr, reply)) {
		ofono_error("Replied with an error: %s, %s",
					derr.name, derr.message);
		dbus_error_free(&derr);
		g_free(cb_data->path);
		goto done;
	}

	dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &handle,
					DBUS_TYPE_INVALID);

	g_hash_table_insert(cb_data->server->adapter_hash, cb_data->path,
				GUINT_TO_POINTER(handle));

	ofono_info("Registered handle for %s, channel %d: 0x%x", cb_data->path,
			cb_data->server->channel, handle);

done:
	/* Do not free cb_data->path, it is used in adapter_hash */
	g_free(cb_data);
	dbus_message_unref(reply);
}

static void add_record(gpointer data, gpointer user_data)
{
	struct server *server = data;
	const char *path = user_data;
	struct cb_data *cb_data;

	if (server->sdp_record == NULL)
		return;

	cb_data = g_try_new0(struct cb_data, 1);
	if (cb_data == NULL) {
		ofono_error("Unable to allocate cb_data structure");
		return;
	}

	cb_data->server = server;
	cb_data->path = g_strdup(path);

	bluetooth_send_with_reply(path, BLUEZ_SERVICE_INTERFACE, "AddRecord",
				add_record_cb, cb_data, NULL, -1,
				DBUS_TYPE_STRING, &server->sdp_record,
				DBUS_TYPE_INVALID);
}

static gboolean adapter_added(DBusConnection *connection, DBusMessage *message,
				void *user_data)
{
	const char *path;
	int ret;

	dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	ret = bluetooth_send_with_reply(path, BLUEZ_ADAPTER_INTERFACE,
			"GetProperties", adapter_properties_cb, g_strdup(path),
			g_free, -1, DBUS_TYPE_INVALID);

	g_slist_foreach(server_list, add_record, (gpointer) path);

	return TRUE;
}

static gboolean adapter_removed(DBusConnection *connection,
				DBusMessage *message, void *user_data)
{
	const char *path;
	GSList *l;

	if (dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID) == TRUE)
		g_hash_table_remove(adapter_address_hash, path);

	for (l = server_list; l; l = l->next) {
		struct server *server = l->data;

		/* Handle have already been removed, so removing related path */
		g_hash_table_remove(server->adapter_hash, path);
	}

	return TRUE;
}

static void parse_adapters(DBusMessageIter *array, gpointer user_data)
{
	DBusMessageIter value;

	DBG("");

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &value);

	while (dbus_message_iter_get_arg_type(&value)
			== DBUS_TYPE_OBJECT_PATH) {
		const char *path;

		dbus_message_iter_get_basic(&value, &path);

		DBG("Calling GetProperties on %s", path);

		bluetooth_send_with_reply(path, BLUEZ_ADAPTER_INTERFACE,
				"GetProperties", adapter_properties_cb,
				g_strdup(path), g_free, -1, DBUS_TYPE_INVALID);

		g_slist_foreach(server_list, add_record, (gpointer) path);

		dbus_message_iter_next(&value);
	}
}

static void manager_properties_cb(DBusPendingCall *call, gpointer user_data)
{
	DBusMessage *reply;

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_is_error(reply, DBUS_ERROR_SERVICE_UNKNOWN)) {
		DBG("Bluetooth daemon is apparently not available.");
		goto done;
	}

	DBG("");

	bluetooth_parse_properties(reply, "Adapters", parse_adapters, NULL,
						NULL);

done:
	dbus_message_unref(reply);
}

static void bluetooth_remove_all_modem(gpointer key, gpointer value,
					gpointer user_data)
{
	struct bluetooth_profile *profile = value;

	profile->remove_all();
}

static void bluetooth_disconnect(DBusConnection *connection, void *user_data)
{
	if (uuid_hash == NULL)
		return;

	g_hash_table_foreach(uuid_hash, bluetooth_remove_all_modem, NULL);
}

static guint bluetooth_watch;
static guint adapter_added_watch;
static guint adapter_removed_watch;
static guint property_watch;

static void bluetooth_ref(void)
{
	if (bluetooth_refcount > 0)
		goto increment;

	connection = ofono_dbus_get_connection();

	bluetooth_watch = g_dbus_add_service_watch(connection, BLUEZ_SERVICE,
					NULL, bluetooth_disconnect, NULL, NULL);

	adapter_added_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_MANAGER_INTERFACE,
						"AdapterAdded",
						adapter_added, NULL, NULL);

	adapter_removed_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_MANAGER_INTERFACE,
						"AdapterRemoved",
						adapter_removed, NULL, NULL);

	property_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						BLUEZ_DEVICE_INTERFACE,
						"PropertyChanged",
						property_changed, NULL, NULL);

	if (bluetooth_watch == 0 || adapter_added_watch == 0 ||
			adapter_removed_watch == 0 || property_watch == 0) {
		goto remove;
	}

	uuid_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);

	adapter_address_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, g_free);

	bluetooth_send_with_reply("/", BLUEZ_MANAGER_INTERFACE, "GetProperties",
				manager_properties_cb, NULL, NULL, -1,
				DBUS_TYPE_INVALID);

increment:
	g_atomic_int_inc(&bluetooth_refcount);

	return;

remove:
	g_dbus_remove_watch(connection, bluetooth_watch);
	g_dbus_remove_watch(connection, adapter_added_watch);
	g_dbus_remove_watch(connection, adapter_removed_watch);
	g_dbus_remove_watch(connection, property_watch);
}

static void bluetooth_unref(void)
{
	if (g_atomic_int_dec_and_test(&bluetooth_refcount) == FALSE)
		return;

	g_dbus_remove_watch(connection, bluetooth_watch);
	g_dbus_remove_watch(connection, adapter_added_watch);
	g_dbus_remove_watch(connection, adapter_removed_watch);
	g_dbus_remove_watch(connection, property_watch);

	g_hash_table_destroy(uuid_hash);
	g_hash_table_destroy(adapter_address_hash);
}

int bluetooth_register_uuid(const char *uuid, struct bluetooth_profile *profile)
{
	bluetooth_ref();

	g_hash_table_insert(uuid_hash, g_strdup(uuid), profile);

	g_hash_table_foreach(adapter_address_hash,
				(GHFunc) get_adapter_properties, NULL);

	return 0;
}

void bluetooth_unregister_uuid(const char *uuid)
{
	g_hash_table_remove(uuid_hash, uuid);

	bluetooth_unref();
}

struct server *bluetooth_register_server(guint8 channel, const char *sdp_record,
					ConnectFunc cb, gpointer user_data)
{
	struct server *server;
	GError *err;
	GHashTableIter iter;
	gpointer key, value;

	server = g_try_new0(struct server, 1);
	if (!server)
		return NULL;

	server->channel = channel;

	server->io = bt_io_listen(BT_IO_RFCOMM, NULL, new_connection,
					server, NULL, &err,
					BT_IO_OPT_CHANNEL, server->channel,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_INVALID);
	if (server->io == NULL) {
		g_error_free(err);
		g_free(server);
		return NULL;
	}

	bluetooth_ref();

	if (sdp_record != NULL)
		server->sdp_record = g_strdup(sdp_record);

	server->connect_cb = cb;
	server->user_data = user_data;
	server->adapter_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, NULL);

	g_hash_table_iter_init(&iter, adapter_address_hash);

	while (g_hash_table_iter_next(&iter, &key, &value))
		add_record(server, key);

	server_list = g_slist_prepend(server_list, server);

	return server;
}

void bluetooth_unregister_server(struct server *server)
{
	server_list = g_slist_remove(server_list, server);
	server_stop(server);
	g_hash_table_destroy(server->adapter_hash);
	g_free(server->sdp_record);
	g_free(server);

	bluetooth_unref();
}

OFONO_PLUGIN_DEFINE(bluetooth, "Bluetooth Utils Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, NULL, NULL)
