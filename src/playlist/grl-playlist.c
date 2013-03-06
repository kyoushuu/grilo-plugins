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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <grilo.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <pls/grl-pls.h>

#include "grl-playlist.h"

/* --------- Logging  -------- */

#define GRL_LOG_DOMAIN_DEFAULT playlist_log_domain
GRL_LOG_DOMAIN_STATIC(playlist_log_domain);

/* -------- File info ------- */

#define FILE_ATTRIBUTES                         \
  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","    \
  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","    \
  G_FILE_ATTRIBUTE_STANDARD_TYPE ","            \
  G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","       \
  G_FILE_ATTRIBUTE_TIME_MODIFIED ","            \
  G_FILE_ATTRIBUTE_THUMBNAIL_PATH ","           \
  G_FILE_ATTRIBUTE_THUMBNAILING_FAILED

#define FILE_ATTRIBUTES_FAST                    \
  G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN

/* ---- Emission chunks ----- */

#define BROWSE_IDLE_CHUNK_SIZE 5

/* --- Plugin information --- */

#define PLUGIN_ID   PLAYLIST_PLUGIN_ID

#define SOURCE_ID   "grl-playlist"
#define SOURCE_NAME "Playlist"
#define SOURCE_DESC "A source for browsing from a root playlist"

/* --- Grilo Playlist Private --- */

#define GRL_PLAYLIST_SOURCE_GET_PRIVATE(object)         \
  (G_TYPE_INSTANCE_GET_PRIVATE((object),                  \
			     GRL_PLAYLIST_SOURCE_TYPE,  \
			     GrlPlaylistSourcePrivate))

struct _GrlPlaylistSourcePrivate {
  gchar *base_path;
};

/* --- Data types --- */

static GrlPlaylistSource *grl_playlist_source_new (void);

static void grl_playlist_source_finalize (GObject *object);

gboolean grl_playlist_plugin_init (GrlRegistry *registry,
                                   GrlPlugin *plugin,
                                   GList *configs);

static const GList *grl_playlist_source_supported_keys (GrlSource *source);

static void grl_playlist_source_browse (GrlSource *source,
                                        GrlSourceBrowseSpec *bs);

/* =================== Playlist Plugin  =============== */

gboolean
grl_playlist_plugin_init (GrlRegistry *registry,
                          GrlPlugin *plugin,
                          GList *configs)
{
  GrlConfig *config;
  gchar *base_path = NULL;

  GRL_LOG_DOMAIN_INIT (playlist_log_domain, "playlist");

  GRL_DEBUG (__FUNCTION__);

  GrlPlaylistSource *source = grl_playlist_source_new ();

  for (; configs; configs = g_list_next (configs)) {
    gchar *path;
    config = GRL_CONFIG (configs->data);
    path = grl_config_get_string (config, GRILO_CONF_BASE_PLAYLIST);
    if (path) {
      base_path = path;
    }
  }
  source->priv->base_path = base_path;

  grl_registry_register_source (registry,
                                plugin,
                                GRL_SOURCE (source),
                                NULL);

  return TRUE;
}

GRL_PLUGIN_REGISTER (grl_playlist_plugin_init,
                     NULL,
                     PLUGIN_ID);

/* ================== Playlist GObject ================ */


G_DEFINE_TYPE (GrlPlaylistSource,
               grl_playlist_source,
               GRL_TYPE_SOURCE);

static GrlPlaylistSource *
grl_playlist_source_new (void)
{
  GRL_DEBUG (__FUNCTION__);

  return g_object_new (GRL_PLAYLIST_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       NULL);
}

static void
grl_playlist_source_class_init (GrlPlaylistSourceClass * klass)
{
  GObjectClass *g_class = G_OBJECT_CLASS (klass);
  GrlSourceClass *source_class = GRL_SOURCE_CLASS (klass);

  g_class->finalize = grl_playlist_source_finalize;

  source_class->supported_keys = grl_playlist_source_supported_keys;
  //source_class->get_caps = grl_playlist_source_get_caps;
  source_class->browse = grl_playlist_source_browse;

  g_type_class_add_private (klass, sizeof (GrlPlaylistSourcePrivate));
}

static void
grl_playlist_source_init (GrlPlaylistSource *source)
{
  source->priv = GRL_PLAYLIST_SOURCE_GET_PRIVATE (source);
}

static void
grl_playlist_source_finalize (GObject *object)
{
  GrlPlaylistSource *playlist_source = GRL_PLAYLIST_SOURCE (object);
  g_free (playlist_source->priv->base_path);
  G_OBJECT_CLASS (grl_playlist_source_parent_class)->finalize (object);
}

/* ================== API Implementation ================ */

static const GList *
grl_playlist_source_supported_keys (GrlSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = grl_metadata_key_list_new (GRL_METADATA_KEY_ID,
                                      GRL_METADATA_KEY_TITLE,
                                      GRL_METADATA_KEY_URL,
                                      GRL_METADATA_KEY_MIME,
                                      GRL_METADATA_KEY_GENRE,
                                      GRL_METADATA_KEY_AUTHOR,
                                      GRL_METADATA_KEY_ALBUM,
                                      GRL_METADATA_KEY_THUMBNAIL,
                                      GRL_METADATA_KEY_DURATION,
                                      GRL_METADATA_KEY_MODIFICATION_DATE,
                                      GRL_METADATA_KEY_CHILDCOUNT,
                                      NULL);
  }
  return keys;
}

static void
grl_playlist_source_browse (GrlSource *source,
                            GrlSourceBrowseSpec *bs)
{
  const gchar *id;

  GRL_DEBUG (__FUNCTION__);

  id = grl_media_get_id (bs->container);
  if (!id) {
    GRL_DEBUG ("Browsing the root");

    if (!grl_media_get_url (bs->container)) {
      gchar *base_path = GRL_PLAYLIST_SOURCE(source)->priv->base_path;
      GFile *file = g_file_new_for_path (base_path);
      gchar *url = g_file_get_uri (file);
      g_object_unref (file);
      grl_media_set_url (bs->container, url);
      g_free (url);
    }
  }

  if (grl_pls_media_is_playlist (bs->container)) {
    grl_pls_browse_by_spec (source, bs);
  } else {
    bs->callback (source,
                  bs->operation_id,
                  NULL,
                  0,
                  bs->user_data,
                  NULL);
  }
}
