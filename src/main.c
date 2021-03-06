/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
//#include <sys/signalfd.h>

#include <gdbus.h>

#ifdef HAVE_CAPNG
#include <cap-ng.h>
#endif

#include "ofono.h"

#define SHUTDOWN_GRACE_SECONDS 10

static GMainLoop *event_loop;

void __ofono_exit(void)
{
	g_main_loop_quit(event_loop);
}

static gboolean quit_eventloop(gpointer user_data)
{
	__ofono_exit();
	return FALSE;
}

#if 0
static gboolean signal_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	static int terminated = 0;
	int signal_fd = GPOINTER_TO_INT(data);
	struct signalfd_siginfo si;
	ssize_t res;

	if (cond & (G_IO_NVAL | G_IO_ERR))
		return FALSE;

	res = read(signal_fd, &si, sizeof(si));
	if (res != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
	case SIGTERM:
		if (terminated == 0) {
			g_timeout_add_seconds(SHUTDOWN_GRACE_SECONDS,
						quit_eventloop, NULL);
			__ofono_modem_shutdown();
		}

		terminated++;
		break;
	default:
		break;
	}

	return TRUE;
}
#endif

static void system_bus_disconnected(DBusConnection *conn, void *user_data)
{
	ofono_error("System bus has disconnected!");

	g_main_loop_quit(event_loop);
}

static gchar *option_debug = NULL;
static gchar *option_plugin = NULL;
static gchar *option_noplugin = NULL;
static gboolean option_detach = TRUE;
static gboolean option_version = FALSE;

static gboolean parse_debug(const char *key, const char *value,
					gpointer user_data, GError **error)
{
	if (value)
		option_debug = g_strdup(value);
	else
		option_debug = g_strdup("*");

	return TRUE;
}

static GOptionEntry options[] = {
	{ "debug", 'd', G_OPTION_FLAG_OPTIONAL_ARG,
				G_OPTION_ARG_CALLBACK, parse_debug,
				"Specify debug options to enable", "DEBUG" },
	{ "plugin", 'p', 0, G_OPTION_ARG_STRING, &option_plugin,
				"Specify plugins to load", "NAME,..," },
	{ "noplugin", 'P', 0, G_OPTION_ARG_STRING, &option_noplugin,
				"Specify plugins not to load", "NAME,..." },
	{ "nodetach", 'n', G_OPTION_FLAG_REVERSE,
				G_OPTION_ARG_NONE, &option_detach,
				"Don't run as daemon in background" },
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version,
				"Show version information and exit" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	sigset_t mask;
	DBusConnection *conn;
	DBusError error;
	int signal_fd;
	GIOChannel *signal_io;
	int signal_source;

#ifdef HAVE_CAPNG
	/* Drop capabilities */
	capng_clear(CAPNG_SELECT_BOTH);
	capng_updatev(CAPNG_ADD, CAPNG_EFFECTIVE | CAPNG_PERMITTED,
				CAP_NET_BIND_SERVICE, CAP_NET_ADMIN,
				CAP_NET_RAW, CAP_SYS_ADMIN, -1);
	capng_apply(CAPNG_SELECT_BOTH);
#endif

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

#if 0
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Can't set signal mask");
		return 1;
	}
#endif

#if 0
	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd < 0) {
		perror("Can't create signal filedescriptor");
		return 1;
	}
	signal_io = g_io_channel_unix_new(signal_fd);
	g_io_channel_set_close_on_unref(signal_io, TRUE);
	signal_source = g_io_add_watch(signal_io,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			signal_cb, GINT_TO_POINTER(signal_fd));
	g_io_channel_unref(signal_io);
#endif

#ifdef NEED_THREADS
	if (g_thread_supported() == FALSE)
		g_thread_init(NULL);
#endif

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &err) == FALSE) {
		if (err != NULL) {
			g_printerr("%s\n", err->message);
			g_error_free(err);
			return 1;
		}

		g_printerr("An unknown error occurred\n");
		return 1;
	}

	g_option_context_free(context);

	if (option_version == TRUE) {
		printf("%s\n", VERSION);
		exit(0);
	}

	if (option_detach == TRUE) {
		if (daemon(0, 0)) {
			perror("Can't start daemon");
			return 1;
		}
	}

	event_loop = g_main_loop_new(NULL, FALSE);

#ifdef NEED_THREADS
	if (dbus_threads_init_default() == FALSE) {
		fprintf(stderr, "Can't init usage of threads\n");
		exit(1);
	}
#endif

	__ofono_log_init(option_debug, option_detach);

	dbus_error_init(&error);

	conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, OFONO_SERVICE, &error);
	if (conn == NULL) {
		if (dbus_error_is_set(&error) == TRUE) {
			ofono_error("Unable to hop onto D-Bus: %s",
					error.message);
			dbus_error_free(&error);
		} else {
			ofono_error("Unable to hop onto D-Bus");
		}

		goto cleanup;
	}

	g_dbus_set_disconnect_function(conn, system_bus_disconnected,
					NULL, NULL);

	__ofono_dbus_init(conn);

	__ofono_modemwatch_init();

	__ofono_manager_init();

	__ofono_plugin_init(option_plugin, option_noplugin);

	g_free(option_plugin);
	g_free(option_noplugin);

	g_main_loop_run(event_loop);

	__ofono_plugin_cleanup();

	__ofono_manager_cleanup();

	__ofono_modemwatch_cleanup();

	__ofono_dbus_cleanup();
	dbus_connection_unref(conn);

cleanup:
	g_source_remove(signal_source);
	g_main_loop_unref(event_loop);

	__ofono_log_cleanup();

	return 0;
}
