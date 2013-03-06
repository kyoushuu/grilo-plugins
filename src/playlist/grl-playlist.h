/*
 * Copyright (C) 2013 Collabora Ltd.
 *
 * Contact: Mateu Batle Sastre <mateu.batle@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef _GRL_PLAYLIST_SOURCE_H_
#define _GRL_PLAYLIST_SOURCE_H_

#include <grilo.h>

#define GRL_PLAYLIST_SOURCE_TYPE                \
  (grl_playlist_source_get_type ())

#define GRL_PLAYLIST_SOURCE(obj)                                \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj),                           \
                               GRL_PLAYLIST_SOURCE_TYPE,        \
                               GrlPlaylistSource))

#define GRL_IS_PLAYLIST_SOURCE(obj)                             \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj),                           \
                               GRL_PLAYLIST_SOURCE_TYPE))

#define GRL_PLAYLIST_SOURCE_CLASS(klass)                \
  (G_TYPE_CHECK_CLASS_CAST((klass),                     \
                           GRL_PLAYLIST_SOURCE_TYPE,    \
                           GrlPlaylistSourceClass))

#define GRL_IS_PLAYLIST_SOURCE_CLASS(klass)             \
  (G_TYPE_CHECK_CLASS_TYPE((klass)                      \
                           GRL_PLAYLIST_SOURCE_TYPE))

#define GRL_PLAYLIST_SOURCE_GET_CLASS(obj)                      \
  (G_TYPE_INSTANCE_GET_CLASS ((obj),                            \
                              GRL_PLAYLIST_SOURCE_TYPE,         \
                              GrlPlaylistSourceClass))

/* --- Grilo Configuration --- */
#define GRILO_CONF_BASE_PLAYLIST "base-playlist"

typedef struct _GrlPlaylistSource GrlPlaylistSource;
typedef struct _GrlPlaylistSourcePrivate GrlPlaylistSourcePrivate;

struct _GrlPlaylistSource {

  GrlSource parent;

  /*< private >*/
  GrlPlaylistSourcePrivate *priv;
};

typedef struct _GrlPlaylistSourceClass GrlPlaylistSourceClass;

struct _GrlPlaylistSourceClass {

  GrlSourceClass parent_class;

};

GType grl_playlist_source_get_type (void);

#endif /* _GRL_PLAYLIST_SOURCE_H_ */
