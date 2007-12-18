NULL =

SUBDIRS =		\
	actions		\
	$(NULL)

modules_flags = -export_dynamic -avoid-version -module -no-undefined

plugindir = $(libdir)/gnome-settings-daemon/plugins/media-keys

INCLUDES = \
	-I$(top_srcdir)/src		\
	-I$(top_builddir)/plugins/media-keys/actions	\
	-DGNOME_SETTINGS_LOCALEDIR=\""$(datadir)/locale"\"\
	-DPIXMAPDIR=\""$(plugindir)"\"			\
	-DGLADEDIR=\""$(plugindir)"\"			\
	$(SETTINGS_PLUGIN_CFLAGS)			\
	$(NULL)

plugin_LTLIBRARIES = \
	libmedia-keys.la		\
	$(NULL)

BUILT_SOURCES = 			\
	gsd-media-keys-manager-glue.h	\
	gsd-marshal.h			\
	gsd-marshal.c			\
	$(NULL)

gsd-media-keys-manager-glue.h: gsd-media-keys-manager.xml Makefile.am
	dbus-binding-tool --prefix=gsd_media_keys_manager --mode=glib-server --output=gsd-media-keys-manager-glue.h gsd-media-keys-manager.xml

gsd-marshal.c: gsd-marshal.list
	@GLIB_GENMARSHAL@ --prefix=gsd_marshal $< --header --body > $@

gsd-marshal.h: gsd-marshal.list
	@GLIB_GENMARSHAL@ --prefix=gsd_marshal $< --header > $@

libmedia_keys_la_SOURCES = 		\
	gsd-media-keys-plugin.h		\
	gsd-media-keys-plugin.c		\
	gsd-media-keys-manager.h	\
	gsd-media-keys-manager.c	\
	gsd-media-keys-window.h		\
	gsd-media-keys-window.c		\
	eggaccelerators.h		\
	eggaccelerators.c		\
	$(BUILT_SOURCES)		\
	$(NULL)

libmedia_keys_la_LDFLAGS = 		\
	$(PLUGIN_LIBTOOL_FLAGS)		\
	$(modules_flags)		\
	$(NULL)

libmedia_keys_la_LIBADD  = 		\
	$(SETTINGS_PLUGIN_LIBS)		\
	$(XF86MISC_LIBS)		\
	$(GST_LIBS)			\
	$(top_builddir)/plugins/media-keys/actions/libacme.la	\
	$(NULL)

libmedia_keys_la_CFLAGS = 		\
	$(NULL)

plugin_in_files = 		\
	media-keys.gnome-settings-plugin.desktop.in	\
	$(NULL)

%.gnome-settings-plugin: %.gnome-settings-plugin.desktop.in $(INTLTOOL_MERGE) $(wildcard $(top_srcdir)/po/*po) ; $(INTLTOOL_MERGE) $(top_srcdir)/po $< $@ -d -u -c $(top_builddir)/po/.intltool-merge-cache

plugin_DATA = $(plugin_in_files:.gnome-settings-plugin.desktop.in=.gnome-settings-plugin)

noinst_PROGRAMS =				\
	test-media-window			\
	$(NULL)

test_media_window_SOURCES =			\
	gsd-media-keys-window.c			\
	gsd-media-keys-window.h			\
	test-media-window.c			\
	$(NULL)

test_media_window_LDADD = \
	$(SETTINGS_DAEMON_LIBS)			\
	$(SETTINGS_PLUGIN_LIBS)			\
	$(XF86MISC_LIBS)			\
	$(GST_LIBS)				\
	$(NULL)

EXTRA_DIST = 				\
	gsd-media-keys-manager.xml	\
	gsd-marshal.list		\
	$(plugin_in_files)		\
	$(NULL)

CLEANFILES = 			\
	$(plugin_DATA)		\
	$(NULL)

DISTCLEANFILES =		\
	$(plugin_DATA)		\
	$(NULL)
