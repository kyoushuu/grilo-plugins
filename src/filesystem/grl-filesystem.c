/*
 * Copyright (C) 2010, 2011 Igalia S.L.
 * Copyright (C) 2011 Intel Corporation.
 *
 * Contact: Iago Toral Quiroga <itoral@igalia.com>
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
#include <glib/gi18n-lib.h>
#include <string.h>
#include <stdlib.h>
#include <pls/grl-pls.h>

#include "grl-filesystem.h"

/* --------- Logging  -------- */

#define GRL_LOG_DOMAIN_DEFAULT filesystem_log_domain
GRL_LOG_DOMAIN_STATIC(filesystem_log_domain);

/* -------- File info ------- */

#define FILE_ATTRIBUTES                         \
  G_FILE_ATTRIBUTE_STANDARD_NAME ","            \
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

/* ---- Default root ---- */

#define DEFAULT_ROOT "file:///"

/* --- Plugin information --- */

#define PLUGIN_ID   FILESYSTEM_PLUGIN_ID

#define SOURCE_ID   "grl-filesystem"
#define SOURCE_NAME _("Filesystem")
#define SOURCE_DESC _("A source for browsing the filesystem")

/* --- Grilo Filesystem Private --- */

#define GRL_FILESYSTEM_SOURCE_GET_PRIVATE(object)         \
  (G_TYPE_INSTANCE_GET_PRIVATE((object),                  \
			     GRL_FILESYSTEM_SOURCE_TYPE,  \
			     GrlFilesystemSourcePrivate))

struct _GrlFilesystemSourcePrivate {
  GList *chosen_uris;
  guint max_search_depth;
  /* a mapping operation_id -> GCancellable to cancel this operation */
  GHashTable *cancellables;
  /* Monitors for changes in directories */
  GList *monitors;
  GCancellable *cancellable_monitors;
};

/* --- Data types --- */

typedef struct _RecursiveOperation RecursiveOperation;

typedef gboolean (*RecursiveOperationCb) (GFileInfo *file_info,
                                          RecursiveOperation *operation);

typedef struct {
  GrlSourceBrowseSpec *spec;
  GList *entries;
  GList *current;
  const gchar *uri;
  guint remaining;
  GCancellable *cancellable;
  guint id;
}  BrowseIdleData;

struct _RecursiveOperation {
  RecursiveOperationCb on_cancel;
  RecursiveOperationCb on_finish;
  RecursiveOperationCb on_dir;
  RecursiveOperationCb on_file;
  gpointer on_dir_data;
  gpointer on_file_data;
  GCancellable *cancellable;
  GQueue *directories;
  guint max_depth;
};

typedef struct {
  guint depth;
  GFile *directory;
} RecursiveEntry;


static GrlFilesystemSource *grl_filesystem_source_new (void);

static void grl_filesystem_source_finalize (GObject *object);

gboolean grl_filesystem_plugin_init (GrlRegistry *registry,
                                     GrlPlugin *plugin,
                                     GList *configs);

static const GList *grl_filesystem_source_supported_keys (GrlSource *source);

static GrlCaps *grl_filesystem_source_get_caps (GrlSource *source,
                                                GrlSupportedOps operation);
static void grl_filesystem_source_resolve (GrlSource *source,
                                           GrlSourceResolveSpec *rs);

static void grl_filesystem_source_browse (GrlSource *source,
                                          GrlSourceBrowseSpec *bs);

static void grl_filesystem_source_search (GrlSource *source,
                                          GrlSourceSearchSpec *ss);

static gboolean grl_filesystem_test_media_from_uri (GrlSource *source,
                                                    const gchar *uri);

static void grl_filesystem_get_media_from_uri (GrlSource *source,
                                               GrlSourceMediaFromUriSpec *mfus);

static void grl_filesystem_source_cancel (GrlSource *source,
                                          guint operation_id);

static gboolean grl_filesystem_source_notify_change_start (GrlSource *source,
                                                           GError **error);

static gboolean grl_filesystem_source_notify_change_stop (GrlSource *source,
                                                          GError **error);

/* =================== Filesystem Plugin  =============== */

gboolean
grl_filesystem_plugin_init (GrlRegistry *registry,
                            GrlPlugin *plugin,
                            GList *configs)
{
  GrlConfig *config;
  GList *chosen_uris = NULL;
  guint max_search_depth = GRILO_CONF_MAX_SEARCH_DEPTH_DEFAULT;

  GRL_LOG_DOMAIN_INIT (filesystem_log_domain, "filesystem");

  GRL_DEBUG ("filesystem_plugin_init");

  /* Initialize i18n */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  GrlFilesystemSource *source = grl_filesystem_source_new ();

  for (; configs; configs = g_list_next (configs)) {
    gchar *uri;
    config = GRL_CONFIG (configs->data);
    uri = grl_config_get_string (config, GRILO_CONF_CHOSEN_URI);
    if (uri) {
      chosen_uris = g_list_append (chosen_uris, uri);
    }
    if (grl_config_has_param (config, GRILO_CONF_MAX_SEARCH_DEPTH)) {
      max_search_depth = (guint)grl_config_get_int (config, GRILO_CONF_MAX_SEARCH_DEPTH);
    }
  }
  source->priv->chosen_uris = chosen_uris;
  source->priv->max_search_depth = max_search_depth;

  grl_registry_register_source (registry,
                                plugin,
                                GRL_SOURCE (source),
                                NULL);

  return TRUE;
}

GRL_PLUGIN_REGISTER (grl_filesystem_plugin_init,
                     NULL,
                     PLUGIN_ID);

/* ================== Filesystem GObject ================ */


G_DEFINE_TYPE (GrlFilesystemSource,
               grl_filesystem_source,
               GRL_TYPE_SOURCE);

static GrlFilesystemSource *
grl_filesystem_source_new (void)
{
  GRL_DEBUG ("grl_filesystem_source_new");
  return g_object_new (GRL_FILESYSTEM_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       NULL);
}

static void
grl_filesystem_source_class_init (GrlFilesystemSourceClass * klass)
{
  GObjectClass *g_class = G_OBJECT_CLASS (klass);
  GrlSourceClass *source_class = GRL_SOURCE_CLASS (klass);

  g_class->finalize = grl_filesystem_source_finalize;

  source_class->supported_keys = grl_filesystem_source_supported_keys;
  source_class->cancel = grl_filesystem_source_cancel;
  source_class->get_caps = grl_filesystem_source_get_caps;
  source_class->browse = grl_filesystem_source_browse;
  source_class->search = grl_filesystem_source_search;
  source_class->notify_change_start = grl_filesystem_source_notify_change_start;
  source_class->notify_change_stop = grl_filesystem_source_notify_change_stop;
  source_class->resolve = grl_filesystem_source_resolve;
  source_class->test_media_from_uri = grl_filesystem_test_media_from_uri;
  source_class->media_from_uri = grl_filesystem_get_media_from_uri;

  g_type_class_add_private (klass, sizeof (GrlFilesystemSourcePrivate));
}

static void
grl_filesystem_source_init (GrlFilesystemSource *source)
{
  source->priv = GRL_FILESYSTEM_SOURCE_GET_PRIVATE (source);
  source->priv->cancellables = g_hash_table_new (NULL, NULL);
}

static void
grl_filesystem_source_finalize (GObject *object)
{
  GrlFilesystemSource *filesystem_source = GRL_FILESYSTEM_SOURCE (object);
  g_list_free_full (filesystem_source->priv->chosen_uris, g_free);
  g_hash_table_unref (filesystem_source->priv->cancellables);
  G_OBJECT_CLASS (grl_filesystem_source_parent_class)->finalize (object);
}

/* ======================= Utilities ==================== */

static void recursive_operation_next_entry (RecursiveOperation *operation);
static void add_monitor (GrlFilesystemSource *fs_source, GFile *dir);
static void cancel_monitors (GrlFilesystemSource *fs_source);

static gboolean
mime_is_video (const gchar *mime)
{
  return g_str_has_prefix (mime, "video/");
}

static gboolean
mime_is_audio (const gchar *mime)
{
  return g_str_has_prefix (mime, "audio/");
}

static gboolean
mime_is_image (const gchar *mime)
{
  return g_str_has_prefix (mime, "image/");
}

static gboolean
mime_is_media (const gchar *mime, GrlTypeFilter filter)
{
  if (!mime)
    return FALSE;
  if (!strcmp (mime, "inode/directory"))
    return TRUE;
  if (filter & GRL_TYPE_FILTER_AUDIO &&
      mime_is_audio (mime))
    return TRUE;
  if (filter & GRL_TYPE_FILTER_VIDEO &&
      mime_is_video (mime))
    return TRUE;
  if (filter & GRL_TYPE_FILTER_IMAGE &&
      mime_is_image (mime))
    return TRUE;
  return FALSE;
}

static gboolean
file_is_valid_content (GFileInfo *info, gboolean fast, GrlOperationOptions *options)
{
  const gchar *mime;
  const gchar *mime_filter = NULL;
  GValue *mime_filter_value = NULL;
  GValue *min_date_value = NULL;
  GValue *max_date_value = NULL;
  GDateTime *min_date = NULL;
  GDateTime *max_date = NULL;
  GDateTime *file_date = NULL;
  GrlTypeFilter type_filter;
  gboolean is_media = TRUE;
  GFileType type;

  /* Ignore hidden files */
  if (g_file_info_get_is_hidden (info)) {
      is_media = FALSE;
      goto end;
  }

  type = g_file_info_get_file_type (info);

  /* Directories are always accepted */
  if (type == G_FILE_TYPE_DIRECTORY) {
    goto end;
  }

  type_filter = options? grl_operation_options_get_type_filter (options): GRL_TYPE_FILTER_ALL;

  /* In fast mode we do not check mime-types, any non-hidden file is accepted */
  if (fast) {
    if (type_filter == GRL_TYPE_FILTER_NONE) {
      is_media = FALSE;
    }
    goto end;
  }

  /* Filter by type */
  mime = g_file_info_get_content_type (info);
  if (!mime_is_media (mime, type_filter)) {
    is_media = FALSE;
    goto end;
  }

  /* Filter by mime */
  mime_filter_value =
    options? grl_operation_options_get_key_filter (options,
                                                   GRL_METADATA_KEY_MIME): NULL;
  if (mime_filter_value) {
    mime_filter = g_value_get_string (mime_filter_value);
  }

  if (mime_filter && g_strcmp0 (mime, mime_filter) != 0) {
    is_media = FALSE;
    goto end;
  }

  /* Filter by date */
  if (options) {
    grl_operation_options_get_key_range_filter (options,
                                                GRL_METADATA_KEY_MODIFICATION_DATE,
                                                &min_date_value,
                                                &max_date_value);
  }

  if (min_date_value) {
    min_date = g_date_time_ref (g_value_get_boxed (min_date_value));
  }
  if (max_date_value) {
    max_date = g_date_time_ref (g_value_get_boxed (max_date_value));
  }

  if (min_date || max_date) {
    GTimeVal time = {0,};

    g_file_info_get_modification_time (info, &time);
    file_date = g_date_time_new_from_timeval_utc (&time);
  }

  if (min_date && file_date && g_date_time_compare (min_date, file_date) > 0) {
    is_media = FALSE;
    goto end;
  }

  if (max_date && file_date && g_date_time_compare (max_date, file_date) < 0) {
    is_media = FALSE;
    goto end;
  }

 end:
  if (file_date)
    g_date_time_unref (file_date);
  if (min_date)
    g_date_time_unref (min_date);
  if (max_date)
    g_date_time_unref (max_date);
  return is_media;
}

static void
set_container_childcount (GFile *file,
			  GrlMedia *media,
			  gboolean fast,
                          GrlOperationOptions *options)
{
  GFileEnumerator *e;
  GFileInfo *info;
  GError *error = NULL;
  gint count = 0;
  char *uri;

  /* in fast mode we don't compute  mime-types because it is slow,
     so we can only check if the directory is totally empty (no subdirs,
     and no files), otherwise we just say we do not know the actual
     childcount */
  if (fast) {
    grl_media_box_set_childcount (GRL_MEDIA_BOX (media),
                                  GRL_METADATA_KEY_CHILDCOUNT_UNKNOWN);
    return;
  }

  /* Open directory */
  uri = g_file_get_uri (file);
  GRL_DEBUG ("Opening directory '%s' for childcount", uri);
  g_free (uri);
  e = g_file_enumerate_children (file,
                                 FILE_ATTRIBUTES,
                                 G_FILE_QUERY_INFO_NONE,
                                 NULL,
                                 &error);
  if (!e) {
    GRL_DEBUG ("Failed to open directory: %s", error->message);
    g_error_free (error);
    return;
  }

  /* Count valid entries */
  count = 0;
  while ((info = g_file_enumerator_next_file (e, NULL, NULL)) != NULL) {
    if (file_is_valid_content (info, FALSE, options))
      count++;
    g_object_unref (info);
  }

  g_object_unref (e);

  grl_media_box_set_childcount (GRL_MEDIA_BOX (media), count);
}

static void
grl_media_set_id_from_file (GrlMedia *media,
			    GFile    *file)
{
  char *uri;

  uri = g_file_get_uri (file);
  grl_media_set_id (media, uri);
  g_free (uri);
}

static GrlMedia *
create_content (GrlMedia *content,
		GFile *file,
		GFileInfo *info,
                gboolean only_fast,
                GrlOperationOptions *options)
{
  GrlMedia *media = NULL;
  gchar *str;
  gchar *extension;
  const gchar *mime;
  GError *error = NULL;

  if (!info)
    info = g_file_query_info (file,
			      FILE_ATTRIBUTES,
			      0,
			      NULL,
			      &error);
  else
    info = g_object_ref (info);

  /* Update mode */
  if (content) {
    media = content;
  }

  if (info == NULL) {
    char *uri;

    uri = g_file_get_uri (file);
    GRL_DEBUG ("Failed to get info for file '%s': %s", uri,
               error->message);
    g_free (uri);

    if (!media) {
      media = grl_media_new ();
      grl_media_set_id_from_file (media, file);
    }

    /* Title */
    str = g_file_get_basename (file);

    /* Remove file extension */
    extension = g_strrstr (str, ".");
    if (extension) {
      *extension = '\0';
    }

    grl_media_set_title (media, str);
    g_error_free (error);
    g_free (str);
  } else {
    mime = g_file_info_get_content_type (info);

    if (!media) {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
	media = GRL_MEDIA (grl_media_box_new ());
      } else {
	if (mime_is_video (mime)) {
	  media = grl_media_video_new ();
	} else if (mime_is_audio (mime)) {
	  media = grl_media_audio_new ();
	} else if (mime_is_image (mime)) {
	  media = grl_media_image_new ();
	} else {
	  media = grl_media_new ();
	}
      }
      grl_media_set_id_from_file (media, file);
    }

    if (!GRL_IS_MEDIA_BOX (media)) {
      grl_media_set_mime (media, mime);
    }

    /* Title */
    str = g_strdup (g_file_info_get_display_name (info));

    /* Remove file extension */
    if (!GRL_IS_MEDIA_BOX (media)) {
      extension = g_strrstr (str, ".");
      if (extension) {
        *extension = '\0';
      }
    }

    grl_media_set_title (media, str);
    g_free (str);

    /* Date */
    GTimeVal time;
    GDateTime *date_time;
    g_file_info_get_modification_time (info, &time);
    date_time = g_date_time_new_from_timeval_utc (&time);
    grl_media_set_modification_date (media, date_time);
    g_date_time_unref (date_time);

    /* Thumbnail */
    gboolean thumb_failed =
      g_file_info_get_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_THUMBNAILING_FAILED);
    if (!thumb_failed) {
      const gchar *thumb =
        g_file_info_get_attribute_byte_string (info,
                                               G_FILE_ATTRIBUTE_THUMBNAIL_PATH);
      if (thumb) {
	gchar *thumb_uri = g_filename_to_uri (thumb, NULL, NULL);
	if (thumb_uri) {
	  grl_media_set_thumbnail (media, thumb_uri);
	  g_free (thumb_uri);
	}
      }
    }

    g_object_unref (info);
  }

  /* URL */
  str = g_file_get_uri (file);
  grl_media_set_url (media, str);
  g_free (str);

  /* Childcount */
  if (GRL_IS_MEDIA_BOX (media)) {
    set_container_childcount (file, media, only_fast, options);
  }

  return media;
}

static gboolean
browse_emit_idle (gpointer user_data)
{
  BrowseIdleData *idle_data;
  guint count;
  GrlFilesystemSource *fs_source;

  GRL_DEBUG ("browse_emit_idle");

  idle_data = (BrowseIdleData *) user_data;
  fs_source = GRL_FILESYSTEM_SOURCE (idle_data->spec->source);

  if (g_cancellable_is_cancelled (idle_data->cancellable)) {
    GRL_DEBUG ("Browse operation %d (\"%s\") has been cancelled",
               idle_data->id, idle_data->uri);
    idle_data->spec->callback(idle_data->spec->source,
                              idle_data->id, NULL, 0,
                              idle_data->spec->user_data, NULL);
    goto finish;
  }

  count = 0;
  do {
    gchar *uri;
    GrlMedia *content;
    GFile *file;
    GrlOperationOptions *options = idle_data->spec->options;

    uri = (gchar *) idle_data->current->data;
    file = g_file_new_for_uri (uri);
    g_free (uri);

    content = create_content (NULL,
                              file,
                              NULL,
                              grl_operation_options_get_flags (options)
                              & GRL_RESOLVE_FAST_ONLY,
                              options);
    g_object_unref (file);

    idle_data->spec->callback (idle_data->spec->source,
			       idle_data->spec->operation_id,
			       content,
			       idle_data->remaining--,
			       idle_data->spec->user_data,
			       NULL);

    idle_data->current = g_list_next (idle_data->current);
    count++;
  } while (count < BROWSE_IDLE_CHUNK_SIZE && idle_data->current);

  if (!idle_data->current)
    goto finish;

  return TRUE;

finish:
    g_list_free (idle_data->entries);
    g_hash_table_remove (fs_source->priv->cancellables,
                         GUINT_TO_POINTER (idle_data->id));
    g_object_unref (idle_data->cancellable);
    g_slice_free (BrowseIdleData, idle_data);
    return FALSE;
}

static void
produce_from_uri (GrlSourceBrowseSpec *bs, const gchar *uri, GrlOperationOptions *options)
{
  GFile *file;
  GFileEnumerator *e;
  GFileInfo *info;
  GError *error = NULL;
  guint skip, count;
  GList *entries = NULL;
  GList *iter;

  /* Open directory */
  GRL_DEBUG ("Opening directory '%s'", uri);
  file = g_file_new_for_uri (uri);
  e = g_file_enumerate_children (file,
                                 FILE_ATTRIBUTES,
                                 G_FILE_QUERY_INFO_NONE,
                                 NULL,
                                 &error);

  if (!e) {
    GRL_DEBUG ("Failed to open directory '%s': %s", uri, error->message);
    bs->callback (bs->source, bs->operation_id, NULL, 0, bs->user_data, error);
    g_error_free (error);
    g_object_unref (file);
    return;
  }

  /* Filter out media and directories */
  while ((info = g_file_enumerator_next_file (e, NULL, NULL)) != NULL) {
    if (file_is_valid_content (info, FALSE, options)) {
      GFile *entry;
      entry = g_file_get_child (file, g_file_info_get_name (info));
      entries = g_list_prepend (entries, g_file_get_uri (entry));
      g_object_unref (entry);
    }
    g_object_unref (info);
  }

  g_object_unref (e);
  g_object_unref (file);

  /* Apply skip and count */
  skip = grl_operation_options_get_skip (bs->options);
  count = grl_operation_options_get_count (bs->options);
  iter = entries;
  while (iter) {
    gboolean remove;
    GList *tmp;
    if (skip > 0)  {
      skip--;
      remove = TRUE;
    } else if (count > 0) {
      count--;
      remove = FALSE;
    } else {
      remove = TRUE;
    }
    if (remove) {
      tmp = iter;
      iter = g_list_next (iter);
      g_free (tmp->data);
      entries = g_list_delete_link (entries, tmp);
    } else {
      iter = g_list_next (iter);
    }
  }

  /* Emit results */
  if (entries) {
    /* Use the idle loop to avoid blocking for too long */
    BrowseIdleData *idle_data = g_slice_new (BrowseIdleData);
    gint global_count = grl_operation_options_get_count (bs->options);
    idle_data->spec = bs;
    idle_data->remaining = global_count - count - 1;
    idle_data->uri = uri;
    idle_data->entries = entries;
    idle_data->current = entries;
    idle_data->cancellable = g_cancellable_new ();
    idle_data->id = bs->operation_id;
    g_hash_table_insert (GRL_FILESYSTEM_SOURCE (bs->source)->priv->cancellables,
                         GUINT_TO_POINTER (bs->operation_id),
                         idle_data->cancellable);

    g_idle_add (browse_emit_idle, idle_data);
  } else {
    /* No results */
    bs->callback (bs->source,
		  bs->operation_id,
		  NULL,
		  0,
		  bs->user_data,
		  NULL);
  }
}

static RecursiveEntry *
recursive_entry_new (guint depth, GFile *directory)
{
  RecursiveEntry *entry;

  entry = g_slice_new(RecursiveEntry);
  entry->depth = depth;
  entry->directory = g_object_ref (directory);

  return entry;
}

static void
recursive_entry_free (RecursiveEntry *entry)
{
  g_object_unref (entry->directory);
  g_slice_free (RecursiveEntry, entry);
}

static RecursiveOperation *
recursive_operation_new (void)
{
  RecursiveOperation *operation;

  operation = g_slice_new0 (RecursiveOperation);
  operation->directories = g_queue_new ();
  operation->cancellable = g_cancellable_new ();

  return operation;
}

static void
recursive_operation_free (RecursiveOperation *operation)
{
  g_queue_foreach (operation->directories, (GFunc) recursive_entry_free, NULL);
  g_queue_free (operation->directories);
  g_object_unref (operation->cancellable);
  g_slice_free (RecursiveOperation, operation);
}

static void
recursive_operation_got_file (GFileEnumerator *enumerator, GAsyncResult *res, RecursiveOperation *operation)
{
  GList *files;
  GError *error = NULL;
  gboolean continue_operation = TRUE;

  GRL_DEBUG (__func__);

  files = g_file_enumerator_next_files_finish (enumerator, res, &error);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      GRL_WARNING ("Got error: %s", error->message);
    g_error_free (error);
    goto finished;
  }

  if (files) {
    GFileInfo *file_info;
    RecursiveEntry *entry;

    /* we assume there is only one GFileInfo in the list since that's what we ask
     * for when calling g_file_enumerator_next_files_async() */
    file_info = (GFileInfo *)files->data;
    g_list_free (files);
    /* Get the entry we are running now */
    entry = g_queue_peek_head (operation->directories);
    switch (g_file_info_get_file_type (file_info)) {
    case G_FILE_TYPE_SYMBOLIC_LINK:
      /* we're too afraid of infinite recursion to touch this for now */
      break;
    case G_FILE_TYPE_DIRECTORY:
        {
          if (entry->depth < operation->max_depth) {
            GFile *subdir;
            RecursiveEntry *subentry;

            if (operation->on_dir) {
              continue_operation = operation->on_dir(file_info, operation);
            }

            if (continue_operation) {
              subdir = g_file_get_child (entry->directory,
                                         g_file_info_get_name (file_info));
              subentry = recursive_entry_new (entry->depth + 1, subdir);
              g_queue_push_tail (operation->directories, subentry);
              g_object_unref (subdir);
            } else {
              g_object_unref (file_info);
              goto finished;
            }
          }
        }
      break;
    case G_FILE_TYPE_REGULAR:
      if (operation->on_file) {
        continue_operation = operation->on_file(file_info, operation);
        if (!continue_operation) {
          g_object_unref (file_info);
          goto finished;
        }
      }
      break;
    default:
      /* this file is a weirdo, we ignore it */
      break;
    }
    g_object_unref (file_info);
  } else {    /* end of enumerator */
    goto finished;
  }

  g_file_enumerator_next_files_async (enumerator, 1, G_PRIORITY_DEFAULT,
                                      operation->cancellable,
                                      (GAsyncReadyCallback)recursive_operation_got_file,
                                      operation);

  return;

finished:
  /* we're done with this dir/enumerator, let's treat the next one */
  g_object_unref (enumerator);
  recursive_entry_free (g_queue_pop_head (operation->directories));
  if (continue_operation) {
    recursive_operation_next_entry (operation);
  } else {
    recursive_operation_free (operation);
  }
}

static void
recursive_operation_got_entry (GFile *directory, GAsyncResult *res, RecursiveOperation *operation)
{
  GError *error = NULL;
  GFileEnumerator *enumerator;

  GRL_DEBUG (__func__);

  enumerator = g_file_enumerate_children_finish (directory, res, &error);
  if (error) {
    GRL_WARNING ("Got error: %s", error->message);
    g_error_free (error);
    /* we couldn't get the children of this directory, but we probably have
     * other directories to try */
    recursive_entry_free (g_queue_pop_head (operation->directories));
    recursive_operation_next_entry (operation);
    return;
  }

  g_file_enumerator_next_files_async (enumerator, 1, G_PRIORITY_DEFAULT,
                                      operation->cancellable,
                                      (GAsyncReadyCallback)recursive_operation_got_file,
                                      operation);
}

static void
recursive_operation_next_entry (RecursiveOperation *operation)
{
  RecursiveEntry *entry;

  GRL_DEBUG (__func__);

  if (g_cancellable_is_cancelled (operation->cancellable)) {
    /* We've been cancelled! */
    GRL_DEBUG ("Operation has been cancelled");
    if (operation->on_cancel) {
      operation->on_cancel(NULL, operation);
    }
    goto finished;
  }

  entry = g_queue_peek_head (operation->directories);
  if (!entry) { /* We've crawled everything, before reaching count */
    if (operation->on_finish) {
      operation->on_finish (NULL, operation);
    }
    goto finished;
  }

  g_file_enumerate_children_async (entry->directory, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                   G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                   G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   G_PRIORITY_DEFAULT,
                                   operation->cancellable,
                                   (GAsyncReadyCallback)recursive_operation_got_entry,
                                   operation);

  return;

finished:
  recursive_operation_free (operation);
}

static void
recursive_operation_initialize (RecursiveOperation *operation, GrlFilesystemSource *source)
{
  GList *chosen_uris, *uri;

  chosen_uris = source->priv->chosen_uris;
  if (chosen_uris) {
    for (uri = chosen_uris; uri; uri = g_list_next (uri)) {
      GFile *directory = g_file_new_for_uri (uri->data);
      g_queue_push_tail (operation->directories,
                         recursive_entry_new (0, directory));
      g_object_unref (directory);
    }
  } else {
    const gchar *home;
    GFile *directory;

    /* This is necessary for GLIB < 2.36 */
    home = g_getenv ("HOME");
    if (!home)
      home = g_get_home_dir ();
    directory = g_file_new_for_path (home);
    g_queue_push_tail (operation->directories,
                       recursive_entry_new (0, directory));
    g_object_unref (directory);
  }
}

static gboolean
cancel_cb (GFileInfo *file_info, RecursiveOperation *operation)
{
  GrlFilesystemSource *fs_source;

  if (operation->on_file_data) {
    GrlSourceSearchSpec *ss =
      (GrlSourceSearchSpec *) operation->on_file_data;
    fs_source = GRL_FILESYSTEM_SOURCE (ss->source);
    g_hash_table_remove (fs_source->priv->cancellables,
                         GUINT_TO_POINTER (ss->operation_id));
    ss->callback (ss->source, ss->operation_id, NULL, 0, ss->user_data, NULL);
  }

  if (operation->on_dir_data) {
    /* Remove all monitors */
    fs_source = GRL_FILESYSTEM_SOURCE (operation->on_dir_data);
    cancel_monitors (fs_source);
  }
  return FALSE;
}

static gboolean
finish_cb (GFileInfo *file_info, RecursiveOperation *operation)
{
  if (operation->on_file_data) {
    GrlSourceSearchSpec *ss =
      (GrlSourceSearchSpec *) operation->on_file_data;
    g_hash_table_remove (GRL_FILESYSTEM_SOURCE (ss->source)->priv->cancellables,
                         GUINT_TO_POINTER (ss->operation_id));
    ss->callback (ss->source, ss->operation_id, NULL, 0, ss->user_data, NULL);
  }

  if (operation->on_dir_data) {
    GRL_FILESYSTEM_SOURCE (operation->on_dir_data)->priv->cancellable_monitors = NULL;
  }

  return FALSE;
}

/* return TRUE if more files need to be returned, FALSE if we sent the count */
static gboolean
file_cb (GFileInfo *file_info, RecursiveOperation *operation)
{
  gchar *needle = NULL;
  gchar *haystack = NULL;
  gchar *normalized_needle = NULL;
  gchar *normalized_haystack = NULL;
  GrlSourceSearchSpec *ss = operation->on_file_data;
  gint remaining = -1;

  GRL_DEBUG (__func__);

  if (ss == NULL)
    return FALSE;

  if (ss->text) {
    haystack = g_utf8_casefold (g_file_info_get_display_name (file_info), -1);
    normalized_haystack = g_utf8_normalize (haystack, -1, G_NORMALIZE_ALL);

    needle = g_utf8_casefold (ss->text, -1);
    normalized_needle = g_utf8_normalize (needle, -1, G_NORMALIZE_ALL);
  }

  if (!ss->text ||
      strstr (normalized_haystack, normalized_needle)) {
    GrlMedia *media = NULL;
    RecursiveEntry *entry;
    GFile *file;
    GFileInfo *info;

    entry = g_queue_peek_head (operation->directories);
    file = g_file_get_child (entry->directory,
                             g_file_info_get_name (file_info));

    /* FIXME This is likely to block */
    info = g_file_query_info (file, FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, NULL, NULL);

    if (file_is_valid_content (info, FALSE, ss->options)) {
      guint skip = grl_operation_options_get_skip (ss->options);
      if (skip) {
        grl_operation_options_set_skip (ss->options, skip - 1);
      } else {
        media = create_content (NULL, file, info,
                                grl_operation_options_get_flags (ss->options)
                                  & GRL_RESOLVE_FAST_ONLY, ss->options);
      }
    }

    g_object_unref (info);
    g_object_unref (file);

    if (media) {
      gint count = grl_operation_options_get_count (ss->options);
      count--;
      grl_operation_options_set_count (ss->options, count);
      if (count == 0) {
        remaining = 0;
      }
      ss->callback (ss->source, ss->operation_id, media, remaining, ss->user_data, NULL);
    }
  }

  g_free (haystack);
  g_free (normalized_haystack);
  g_free (needle);
  g_free (normalized_needle);
  return remaining == -1;
}

static void
notify_parent_change (GrlSource *source, GFile *child, GrlSourceChangeType change)
{
  GFile *parent;
  GrlMedia *media;

  parent = g_file_get_parent (child);

  media = create_content (NULL, parent ? parent : child, NULL, GRL_RESOLVE_FAST_ONLY, NULL);
  grl_source_notify_change (source, media, change, FALSE);
  g_object_unref (media);

  if (parent) {
    g_object_unref (parent);
  }
}

static void
directory_changed (GFileMonitor *monitor,
                   GFile *file,
                   GFile *other_file,
                   GFileMonitorEvent event,
                   gpointer data)
{
  GrlSource *source = GRL_SOURCE (data);
  GFile *file_parent, *other_file_parent;

  if (event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event == G_FILE_MONITOR_EVENT_CREATED) {
    GFileInfo *info;
    info = g_file_query_info (file, FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info && file_is_valid_content (info, TRUE, NULL)) {
      notify_parent_change (source,
                            file,
                            (event == G_FILE_MONITOR_EVENT_CREATED)? GRL_CONTENT_ADDED: GRL_CONTENT_CHANGED);
      if (event == G_FILE_MONITOR_EVENT_CREATED) {
        if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
          add_monitor (GRL_FILESYSTEM_SOURCE (source), file);
        }
      }
    }
    g_object_unref (info);
  } else if (event == G_FILE_MONITOR_EVENT_DELETED) {
    notify_parent_change (source, file, GRL_CONTENT_REMOVED);
  } else if (event == G_FILE_MONITOR_EVENT_MOVED) {
    GFileInfo *info;
    info = g_file_query_info (file, FILE_ATTRIBUTES, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (file_is_valid_content (info, TRUE, NULL)) {
      file_parent = g_file_get_parent (file);
      other_file_parent = g_file_get_parent (other_file);

      if (g_file_equal (file_parent, other_file_parent) == 0) {
        notify_parent_change (source, file, GRL_CONTENT_CHANGED);
      } else {
        notify_parent_change (source, file, GRL_CONTENT_REMOVED);
        notify_parent_change (source, other_file, GRL_CONTENT_ADDED);
      }
      g_object_unref (file_parent);
      g_object_unref (other_file_parent);
    }
  }
}

static void
cancel_monitors (GrlFilesystemSource *fs_source)
{
  g_list_foreach (fs_source->priv->monitors,
                  (GFunc) g_file_monitor_cancel,
                  NULL);
  g_list_free_full (fs_source->priv->monitors, g_object_unref);
  fs_source->priv->monitors = NULL;
}

static void
add_monitor (GrlFilesystemSource *fs_source, GFile *dir)
{
  GFileMonitor *monitor;

  monitor = g_file_monitor_directory (dir, G_FILE_MONITOR_SEND_MOVED, NULL, NULL);
  if (monitor) {
    fs_source->priv->monitors = g_list_prepend (fs_source->priv->monitors,
                                                monitor);
    g_signal_connect (monitor,
                      "changed",
                      G_CALLBACK (directory_changed),
                      fs_source);
  } else {
    char *uri;
    uri = g_file_get_uri (dir);
    GRL_DEBUG ("Unable to set up monitor in %s\n", uri);
    g_free (uri);
  }
}

static gboolean
directory_cb (GFileInfo *dir_info, RecursiveOperation *operation)
{
  RecursiveEntry *entry;
  GFile *dir;
  GrlFilesystemSource *fs_source;

  fs_source = GRL_FILESYSTEM_SOURCE (operation->on_dir_data);
  entry = g_queue_peek_head (operation->directories);
  dir = g_file_get_child (entry->directory,
                          g_file_info_get_name (dir_info));

  add_monitor (fs_source, dir);
  g_object_unref (dir);

  return TRUE;
}

/* ================== API Implementation ================ */

static const GList *
grl_filesystem_source_supported_keys (GrlSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = grl_metadata_key_list_new (GRL_METADATA_KEY_ID,
                                      GRL_METADATA_KEY_TITLE,
                                      GRL_METADATA_KEY_URL,
                                      GRL_METADATA_KEY_MIME,
                                      GRL_METADATA_KEY_MODIFICATION_DATE,
                                      GRL_METADATA_KEY_CHILDCOUNT,
                                      NULL);
  }
  return keys;
}

static void
grl_filesystem_source_browse (GrlSource *source,
                              GrlSourceBrowseSpec *bs)
{
  const gchar *id;
  GList *chosen_uris;

  GRL_DEBUG (__FUNCTION__);

  if (grl_pls_media_is_playlist (bs->container)) {
    grl_pls_browse_by_spec (source, bs);
    return;
  }

  id = grl_media_get_id (bs->container);
  chosen_uris = GRL_FILESYSTEM_SOURCE(source)->priv->chosen_uris;
  if (!id && chosen_uris) {
    guint remaining = g_list_length (chosen_uris);

    if (remaining == 1) {
      produce_from_uri (bs, chosen_uris->data, bs->options);
    } else {
      for (; chosen_uris; chosen_uris = g_list_next (chosen_uris)) {
        GrlMedia *content;
        GFile *file;

        file = g_file_new_for_uri ((gchar *) chosen_uris->data);
        content = create_content (NULL,
                                  file,
                                  NULL,
                                  GRL_RESOLVE_FAST_ONLY,
                                  bs->options);
        g_object_unref (file);

        if (content) {
          bs->callback (source,
                        bs->operation_id,
                        content,
                        --remaining,
                        bs->user_data,
                        NULL);
        }
      }
    }
  } else {
    produce_from_uri (bs, id ? id : DEFAULT_ROOT, bs->options);
  }
}

static void grl_filesystem_source_search (GrlSource *source,
                                          GrlSourceSearchSpec *ss)
{
  RecursiveOperation *operation;
  GrlFilesystemSource *fs_source;

  GRL_DEBUG (__FUNCTION__);

  fs_source = GRL_FILESYSTEM_SOURCE (source);

  operation = recursive_operation_new ();
  operation->on_cancel = cancel_cb;
  operation->on_finish = finish_cb;
  operation->on_file = file_cb;
  operation->on_file_data = ss;
  operation->max_depth = fs_source->priv->max_search_depth;
  g_hash_table_insert (GRL_FILESYSTEM_SOURCE (source)->priv->cancellables,
                       GUINT_TO_POINTER (ss->operation_id),
                       operation->cancellable);

  recursive_operation_initialize (operation, fs_source);
  recursive_operation_next_entry (operation);
}

static void
grl_filesystem_source_resolve (GrlSource *source,
                               GrlSourceResolveSpec *rs)
{
  GFile *file;
  const gchar *id;
  GList *chosen_uris;

  GRL_DEBUG (__FUNCTION__);

  id = grl_media_get_id (rs->media);
  chosen_uris = GRL_FILESYSTEM_SOURCE(source)->priv->chosen_uris;

  if (!id && chosen_uris) {
    guint len;

    len = g_list_length (chosen_uris);
    if (len == 1) {
      file = g_file_new_for_uri (chosen_uris->data);
    } else {
      grl_media_set_title (rs->media, SOURCE_NAME);
      grl_media_box_set_childcount (GRL_MEDIA_BOX (rs->media), len);
      rs->callback (rs->source, rs->operation_id, rs->media, rs->user_data, NULL);
      return;
    }
  } else {
    file = g_file_new_for_uri (id ? id : DEFAULT_ROOT);
  }

  if (g_file_query_exists (file, NULL)) {
    create_content (rs->media, file, NULL,
                    grl_operation_options_get_flags (rs->options)
                    & GRL_RESOLVE_FAST_ONLY,
                    rs->options);
    rs->callback (rs->source, rs->operation_id, rs->media, rs->user_data, NULL);
  } else {
    GError *error = g_error_new (GRL_CORE_ERROR,
                                 GRL_CORE_ERROR_RESOLVE_FAILED,
                                 _("File %s does not exist"),
                                 id);
    rs->callback (rs->source, rs->operation_id, rs->media, rs->user_data, error);
    g_error_free (error);
  }

  g_object_unref (file);
}

static gboolean
is_supported_scheme (const char *scheme)
{
  GVfs *vfs;
  const gchar * const * schemes;
  guint i;

  if (g_strcmp0(scheme, "file") == 0)
    return TRUE;

  vfs = g_vfs_get_default ();
  schemes = g_vfs_get_supported_uri_schemes (vfs);
  for (i = 0; schemes[i] != NULL; i++) {
    if (strcmp (schemes[i], scheme) == 0)
      return TRUE;
  }

  return FALSE;
}

static gboolean
grl_filesystem_test_media_from_uri (GrlSource *source,
                                    const gchar *uri)
{
  GFile *file;
  GFileInfo *info;
  gchar *scheme;
  gboolean ret = FALSE;

  GRL_DEBUG (__FUNCTION__);

  scheme = g_uri_parse_scheme (uri);
  ret = is_supported_scheme (scheme);
  g_free (scheme);
  if (!ret)
    return ret;

  file = g_file_new_for_uri (uri);
  info = g_file_query_info (file, FILE_ATTRIBUTES_FAST, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_object_unref (file);

  if (!info)
    return FALSE;

  ret = file_is_valid_content (info, TRUE, NULL);

  g_object_unref (info);

  return ret;
}

static void grl_filesystem_get_media_from_uri (GrlSource *source,
                                               GrlSourceMediaFromUriSpec *mfus)
{
  gchar *scheme;
  GError *error = NULL;
  gboolean ret = FALSE;
  GrlMedia *media;
  GFile *file;

  GRL_DEBUG (__FUNCTION__);

  scheme = g_uri_parse_scheme (mfus->uri);
  ret = is_supported_scheme (scheme);
  g_free (scheme);
  if (!ret) {
    error = g_error_new (GRL_CORE_ERROR,
                         GRL_CORE_ERROR_MEDIA_FROM_URI_FAILED,
                         _("Cannot get media from %s"), mfus->uri);
    mfus->callback (source, mfus->operation_id, NULL, mfus->user_data, error);
    g_clear_error (&error);
    return;
  }

  /* FIXME: this is a blocking call, not sure we want that in here */
  /* Note: we assume create_content() never returns NULL, which seems to be true */
  file = g_file_new_for_uri (mfus->uri);
  media =
      create_content (NULL, file, NULL,
                      grl_operation_options_get_flags (mfus->options)
                       & GRL_RESOLVE_FAST_ONLY,
                      mfus->options);
  g_object_unref (file);
  mfus->callback (source, mfus->operation_id, media, mfus->user_data, NULL);
}

static void
grl_filesystem_source_cancel (GrlSource *source, guint operation_id)
{
  GCancellable *cancellable;
  GrlFilesystemSourcePrivate *priv;

  priv = GRL_FILESYSTEM_SOURCE (source)->priv;

  cancellable =
      G_CANCELLABLE (g_hash_table_lookup (priv->cancellables,
                                          GUINT_TO_POINTER (operation_id)));
  if (cancellable)
    g_cancellable_cancel (cancellable);
}

static gboolean
grl_filesystem_source_notify_change_start (GrlSource *source,
                                           GError **error)
{
  GrlFilesystemSource *fs_source;
  RecursiveOperation *operation;

  GRL_DEBUG (__func__);

  fs_source = GRL_FILESYSTEM_SOURCE (source);
  operation = recursive_operation_new ();
  operation->on_cancel = cancel_cb;
  operation->on_finish = finish_cb;
  operation->on_dir = directory_cb;
  operation->on_dir_data = fs_source;
  operation->max_depth = fs_source->priv->max_search_depth;

  fs_source->priv->cancellable_monitors = operation->cancellable;

  recursive_operation_initialize (operation, fs_source);
  recursive_operation_next_entry (operation);

  return TRUE;
}

static gboolean
grl_filesystem_source_notify_change_stop (GrlSource *source,
                                          GError **error)
{
  GrlFilesystemSource *fs_source = GRL_FILESYSTEM_SOURCE (source);

  /* Check if notifying is being initialized */
  if (fs_source->priv->cancellable_monitors) {
    g_cancellable_cancel (fs_source->priv->cancellable_monitors);
    fs_source->priv->cancellable_monitors = NULL;
  } else {
    /* Cancel and remove all monitors */
    cancel_monitors (fs_source);
  }

  return TRUE;
}

static GrlCaps *
grl_filesystem_source_get_caps (GrlSource *source,
                                GrlSupportedOps operation)
{
  GList *keys;
  static GrlCaps *caps = NULL;

  if (caps == NULL) {
   caps = grl_caps_new ();
   grl_caps_set_type_filter (caps, GRL_TYPE_FILTER_ALL);
   keys = grl_metadata_key_list_new (GRL_METADATA_KEY_MIME,
                                     NULL);
   grl_caps_set_key_filter (caps, keys);
   g_list_free (keys);
   keys = grl_metadata_key_list_new (GRL_METADATA_KEY_MODIFICATION_DATE,
                                     NULL);
   grl_caps_set_key_range_filter (caps, keys);
   g_list_free (keys);
  }

  return caps;
}
