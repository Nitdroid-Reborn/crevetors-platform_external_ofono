#  Copyright (C) 2010 The NitDroid Project
#  Copyright (C) 2008 The Android Open Source Project
#
#  Author: Alexey Roslyakov <alexey.roslyakov@newsycat.com>
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License version 2 as
#  published by the Free Software Foundation.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#

ifeq ($(BUILD_WITH_OFONO),true)

#
# ofonod
#

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_CFLAGS+=-DOFONO_PLUGIN_BUILTIN -DHAVE_CONFIG_H

LOCAL_SRC_FILES:= \
	gdbus/mainloop.c \
	gdbus/object.c \
	gdbus/polkit.c \
	gdbus/watch.c \
	gisi/message.c \
	gisi/netlink.c \
	gisi/socket.c \
	gisi/client.c \
	gisi/modem.c \
	gisi/pep.c \
	gisi/pipe.c \
	gisi/iter.c \
	gisi/server.c \
	drivers/isimodem/isimodem.c \
	drivers/isimodem/audio-settings.c \
	drivers/isimodem/debug.c \
	drivers/isimodem/radio-settings.c \
	drivers/isimodem/infoserver.c \
	drivers/isimodem/phonebook.c \
	drivers/isimodem/devinfo.c \
	drivers/isimodem/network-registration.c \
	drivers/isimodem/voicecall.c \
	drivers/isimodem/sms.c \
	drivers/isimodem/cbs.c \
	drivers/isimodem/sim.c \
	drivers/isimodem/ssn.c \
	drivers/isimodem/ussd.c \
	drivers/isimodem/gprs.c \
	drivers/isimodem/gprs-context.c \
	drivers/isimodem/call-forwarding.c \
	drivers/isimodem/call-settings.c \
	drivers/isimodem/call-barring.c \
	drivers/isimodem/call-meter.c \
	plugins/modemconf.c \
	plugins/n900.c \
	plugins/nokia-gpio.c \
	plugins/nokia-gpio.h \
	src/main.c \
	src/audio-settings.c \
	src/common.c \
	src/idmap.c \
	src/log.c \
	src/plugin.c \
	src/modem.c \
	src/manager.c \
	src/dbus.c \
	src/network.c \
	src/voicecall.c \
	src/ussd.c \
	src/message.c \
	src/sms.c \
	src/stk.c \
	src/stkagent.c \
	src/stkutil.c \
	src/smsutil.c \
	src/radio-settings.c \
	src/call-settings.c \
	src/call-forwarding.c \
	src/call-meter.c \
	src/ssn.c \
	src/call-barring.c \
	src/sim.c \
	src/simfs.c \
	src/simutil.c \
	src/nettime.c \
	src/phonebook.c \
	src/history.c \
	src/message-waiting.c \
	src/cbs.c \
	src/watch.c \
	src/call-volume.c \
	src/gprs-provision.c \
	src/gprs.c \
	src/storage.c \
	src/util.c \
	src/tsearch.c \
	src/missed_in_android.c
##



LOCAL_MODULE:= ofonod
LOCAL_MODULE_TAGS:=optional

LOCAL_C_INCLUDES := \
	$(KERNEL_HEADERS) \
	external/ofono/include \
	external/ofono/src \
	external/ofono/gisi \
	external/ofono/gatchat \
	external/ofono/gdbus \
	$(call include-path-for, dbus) \
	$(call include-path-for, glib)
##


LOCAL_SHARED_LIBRARIES := \
	liblog \
	libcutils \
	libdbus	\
	libglib-2.0 \
	libdl
##
LOCAL_LDLIBS += -ldl


include $(BUILD_EXECUTABLE)

endif
