#
# Makefile.am
#
# Author: Mateu Batle Sastre <mateu.batle@collabora.com>
#
# Copyright (C) 2013 Collabora Ltd. All rights reserved.

ext_LTLIBRARIES		 = libgrlplaylist.la

libgrlplaylist_la_CFLAGS =	\
	$(DEPS_PLAYLIST_CFLAGS)

libgrlplaylist_la_LIBADD =	\
	$(DEPS_PLAYLIST_LIBS)

libgrlplaylist_la_LDFLAGS = \
	-no-undefined	      \
	-module		      \
	-avoid-version

libgrlplaylist_la_SOURCES = grl-playlist.c grl-playlist.h

extdir					= $(GRL_PLUGINS_DIR)
playlistxmldir		= $(GRL_PLUGINS_DIR)
playlistxml_DATA	= $(PLAYLIST_PLUGIN_ID).xml

EXTRA_DIST = $(playlistxml_DATA)

MAINTAINERCLEANFILES =	\
	*.in		\
	*~

DISTCLEANFILES = $(MAINTAINERCLEANFILES)
