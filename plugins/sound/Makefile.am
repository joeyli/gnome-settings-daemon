plugin_LTLIBRARIES = \
	libsound.la

libsound_la_SOURCES = \
	gsd-sound-plugin.h \
	gsd-sound-plugin.c \
	gsd-sound-manager.h \
	gsd-sound-manager.c

libsound_la_CPPFLAGS = \
	-I$(top_srcdir)/gnome-settings-daemon \
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\" \
	$(AM_CPPFLAGS)

libsound_la_CFLAGS = \
	$(SETTINGS_PLUGIN_CFLAGS) \
	$(GNOME_CFLAGS)	\
	$(PULSE_CFLAGS)	\
	$(AM_CFLAGS)

libsound_la_LDFLAGS = \
	$(GSD_PLUGIN_LDFLAGS)

libsound_la_LIBADD = \
	$(SETTINGS_PLUGIN_LIBS)	\
	$(PULSE_LIBS)

plugin_in_files = \
	sound.gnome-settings-plugin.in

plugin_DATA = $(plugin_in_files:.gnome-settings-plugin.in=.gnome-settings-plugin)

EXTRA_DIST = \
	$(plugin_in_files)

CLEANFILES = \
	$(plugin_DATA)

DISTCLEANFILES = \
	$(plugin_DATA)

@GSD_INTLTOOL_PLUGIN_RULE@
