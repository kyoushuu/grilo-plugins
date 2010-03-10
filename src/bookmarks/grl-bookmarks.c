/*
 * Copyright (C) 2010 Igalia S.L.
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
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>

#include "grl-bookmarks.h"

#define GRL_BOOKMARKS_GET_PRIVATE(object)			 \
  (G_TYPE_INSTANCE_GET_PRIVATE((object),			 \
                               GRL_BOOKMARKS_SOURCE_TYPE,        \
                               GrlBookmarksPrivate))

/* --------- Logging  -------- */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "grl-bookmarks"

/* --- Database --- */

#define GRL_SQL_DB        ".grl-bookmarks"

#define GRL_SQL_CREATE_TABLE_BOOKMARKS			 \
  "CREATE TABLE IF NOT EXISTS bookmarks ("		 \
  "id     INTEGER PRIMARY KEY AUTOINCREMENT,"		 \
  "parent INTEGER REFERENCES bookmarks (id),"		 \
  "type   INTEGER,"					 \
  "url    TEXT,"					 \
  "title  TEXT,"					 \
  "date   TEXT,"					 \
  "mime   TEXT,"					 \
  "desc   TEXT)"

#define GRL_SQL_GET_BOOKMARKS_BY_PARENT			\
  "SELECT b1.*, count(b2.parent <> '') "		\
  "FROM bookmarks b1 LEFT OUTER JOIN bookmarks b2 "	\
  "  ON b1.id = b2.parent "				\
  "WHERE b1.parent='%s' "				\
  "GROUP BY b1.id "					\
  "LIMIT %u OFFSET %u"

#define GRL_SQL_GET_BOOKMARK_BY_ID			\
  "SELECT b1.*, count(b2.parent <> '') "		\
  "FROM bookmarks b1 LEFT OUTER JOIN bookmarks b2 "	\
  "  ON b1.id = b2.parent "				\
  "WHERE b1.id='%s' "					\
  "GROUP BY b1.id "					\
  "LIMIT 1"

#define GRL_SQL_STORE_BOOKMARK				  \
  "INSERT INTO bookmarks "				  \
  "(parent, type, title, url, date, desc, mime) "	  \
  "VALUES (?, ?, ?, ?, ?, ?, ?)"

#define GRL_SQL_REMOVE_BOOKMARK			\
  "DELETE FROM bookmarks "			\
  "WHERE id='%s' or parent='%s'"

#define GRL_SQL_REMOVE_ORPHAN			\
  "DELETE FROM bookmarks "			\
  "WHERE id in ( "				\
  "  SELECT DISTINCT id FROM bookmarks "	\
  "  WHERE parent NOT IN ( "			\
  "    SELECT DISTINCT id FROM bookmarks) "	\
  "  and parent <> 0)"

#define GRL_SQL_GET_BOOKMARKS_BY_TEXT				\
  "SELECT b1.*, count(b2.parent <> '') "			\
  "FROM bookmarks b1 LEFT OUTER JOIN bookmarks b2 "		\
  "  ON b1.id = b2.parent "					\
  "WHERE b1.title LIKE '%%%s%%' OR b1.desc LIKE '%%%s%%' "	\
  "GROUP BY b1.id "						\
  "LIMIT %u OFFSET %u"

#define GRL_SQL_GET_BOOKMARKS_BY_QUERY				\
  "SELECT b1.*, count(b2.parent <> '') "			\
  "FROM bookmarks b1 LEFT OUTER JOIN bookmarks b2 "		\
  "  ON b1.id = b2.parent "					\
  "WHERE %s "							\
  "GROUP BY b1.id "						\
  "LIMIT %u OFFSET %u"

/* --- Plugin information --- */

#define PLUGIN_ID   "grl-bookmarks"
#define PLUGIN_NAME "Bookmarks"
#define PLUGIN_DESC "A plugin for organizing media bookmarks"

#define SOURCE_ID   "grl-bookmarks"
#define SOURCE_NAME "Bookmarks"
#define SOURCE_DESC "A source for organizing media bookmarks"

#define AUTHOR      "Igalia S.L."
#define LICENSE     "LGPL"
#define SITE        "http://www.igalia.com"


enum {
  BOOKMARK_TYPE_CATEGORY = 0,
  BOOKMARK_TYPE_STREAM,
};

enum {
  BOOKMARK_ID = 0,
  BOOKMARK_PARENT,
  BOOKMARK_TYPE,
  BOOKMARK_URL,
  BOOKMARK_TITLE,
  BOOKMARK_DATE,
  BOOKMARK_MIME,
  BOOKMARK_DESC,
  BOOKMARK_LAST
};

struct _GrlBookmarksPrivate {
  sqlite3 *db;
};

typedef struct {
  GrlMediaSource *source;
  guint operation_id;
  const gchar *media_id;
  guint skip;
  guint count;
  GrlMediaSourceResultCb callback;
  guint error_code;
  gboolean is_query;
  gpointer user_data;
} OperationSpec;

static GrlBookmarksSource *grl_bookmarks_source_new (void);

static void grl_bookmarks_source_finalize (GObject *plugin);

static const GList *grl_bookmarks_source_supported_keys (GrlMetadataSource *source);
static GrlSupportedOps grl_bookmarks_source_supported_operations (GrlMetadataSource *metadata_source);

static void grl_bookmarks_source_search (GrlMediaSource *source,
					 GrlMediaSourceSearchSpec *ss);
static void grl_bookmarks_source_query (GrlMediaSource *source,
					GrlMediaSourceQuerySpec *qs);
static void grl_bookmarks_source_browse (GrlMediaSource *source,
                                        GrlMediaSourceBrowseSpec *bs);
static void grl_bookmarks_source_metadata (GrlMediaSource *source,
					   GrlMediaSourceMetadataSpec *ms);
static void grl_bookmarks_source_store (GrlMediaSource *source,
                                       GrlMediaSourceStoreSpec *ss);
static void grl_bookmarks_source_remove (GrlMediaSource *source,
					 GrlMediaSourceRemoveSpec *rs);

/* =================== Bookmarks Plugin  =============== */

static gboolean
grl_bookmarks_plugin_init (GrlPluginRegistry *registry,
                           const GrlPluginInfo *plugin,
                           const GrlConfig *config)
{
  g_debug ("grl_bookmarks_plugin_init\n");

  GrlBookmarksSource *source = grl_bookmarks_source_new ();
  grl_plugin_registry_register_source (registry,
                                       plugin,
                                       GRL_MEDIA_PLUGIN (source));
  return TRUE;
}

GRL_PLUGIN_REGISTER (grl_bookmarks_plugin_init,
                     NULL,
                     PLUGIN_ID,
                     PLUGIN_NAME,
                     PLUGIN_DESC,
                     PACKAGE_VERSION,
                     AUTHOR,
                     LICENSE,
                     SITE);

/* ================== Bookmarks GObject ================ */

static GrlBookmarksSource *
grl_bookmarks_source_new (void)
{
  g_debug ("grl_bookmarks_source_new");
  return g_object_new (GRL_BOOKMARKS_SOURCE_TYPE,
		       "source-id", SOURCE_ID,
		       "source-name", SOURCE_NAME,
		       "source-desc", SOURCE_DESC,
		       NULL);
}

static void
grl_bookmarks_source_class_init (GrlBookmarksSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GrlMediaSourceClass *source_class = GRL_MEDIA_SOURCE_CLASS (klass);
  GrlMetadataSourceClass *metadata_class = GRL_METADATA_SOURCE_CLASS (klass);

  gobject_class->finalize = grl_bookmarks_source_finalize;

  metadata_class->supported_operations =
    grl_bookmarks_source_supported_operations;

  source_class->browse = grl_bookmarks_source_browse;
  source_class->search = grl_bookmarks_source_search;
  source_class->query = grl_bookmarks_source_query;
  source_class->store = grl_bookmarks_source_store;
  source_class->remove = grl_bookmarks_source_remove;
  source_class->metadata = grl_bookmarks_source_metadata;

  metadata_class->supported_keys = grl_bookmarks_source_supported_keys;

  g_type_class_add_private (klass, sizeof (GrlBookmarksPrivate));
}

static void
grl_bookmarks_source_init (GrlBookmarksSource *source)
{
  gint r;
  const gchar *home;
  gchar *db_path;
  gchar *sql_error = NULL;

  source->priv = GRL_BOOKMARKS_GET_PRIVATE (source);
  memset (source->priv, 0, sizeof (GrlBookmarksPrivate));

  home = g_getenv ("HOME");
  if (!home) {
    g_warning ("$HOME not set, cannot open database");
    return;
  }

  g_debug ("Opening database connection...");
  db_path = g_strconcat (home, G_DIR_SEPARATOR_S, GRL_SQL_DB, NULL);
  r = sqlite3_open (db_path, &source->priv->db);
  if (r) {
    g_critical ("Failed to open database '%s': %s",
		db_path, sqlite3_errmsg (source->priv->db));
    sqlite3_close (source->priv->db);
    return;
  }
  g_debug ("  OK");

  g_debug ("Checking database tables...");
  r = sqlite3_exec (source->priv->db, GRL_SQL_CREATE_TABLE_BOOKMARKS,
		    NULL, NULL, &sql_error);

  if (r) {
    if (sql_error) {
      g_warning ("Failed to create database tables: %s", sql_error);
      sqlite3_free (sql_error);
      sql_error = NULL;
    } else {
      g_warning ("Failed to create database tables.");
    }
    sqlite3_close (source->priv->db);
    return;
  }
  g_debug ("  OK");

  g_free (db_path);
}

G_DEFINE_TYPE (GrlBookmarksSource, grl_bookmarks_source, GRL_TYPE_MEDIA_SOURCE);

static void
grl_bookmarks_source_finalize (GObject *object)
{
  GrlBookmarksSource *source;

  g_debug ("grl_bookmarks_source_finalize");

  source = GRL_BOOKMARKS_SOURCE (object);

  sqlite3_close (source->priv->db);

  G_OBJECT_CLASS (grl_bookmarks_source_parent_class)->finalize (object);
}

/* ======================= Utilities ==================== */

static gboolean
mime_is_video (const gchar *mime)
{
  return mime && strstr (mime, "video") != NULL;
}

static gboolean
mime_is_audio (const gchar *mime)
{
  return mime && strstr (mime, "audio") != NULL;
}

static GrlMedia *
build_media_from_stmt (GrlMedia *content, sqlite3_stmt *sql_stmt)
{
  GrlMedia *media = NULL;
  gchar *id;
  gchar *title;
  gchar *url;
  gchar *desc;
  gchar *date;
  gchar *mime;
  guint type;
  guint childcount;

  if (content) {
    media = content;
  }

  id = (gchar *) sqlite3_column_text (sql_stmt, BOOKMARK_ID);
  title = (gchar *) sqlite3_column_text (sql_stmt, BOOKMARK_TITLE);
  url = (gchar *) sqlite3_column_text (sql_stmt, BOOKMARK_URL);
  desc = (gchar *) sqlite3_column_text (sql_stmt, BOOKMARK_DESC);
  date = (gchar *) sqlite3_column_text (sql_stmt, BOOKMARK_DATE);
  mime = (gchar *) sqlite3_column_text (sql_stmt, BOOKMARK_MIME);
  type = (guint) sqlite3_column_int (sql_stmt, BOOKMARK_TYPE);
  childcount = (guint) sqlite3_column_int (sql_stmt, BOOKMARK_LAST);

  if (!media) {
    if (type == BOOKMARK_TYPE_CATEGORY) {
      media = GRL_MEDIA (grl_media_box_new ());
    } else if (mime_is_audio (mime)) {
      media = GRL_MEDIA (grl_media_new ());
    } else if (mime_is_video (mime)) {
      media = GRL_MEDIA (grl_media_new ());
    } else {
      media = GRL_MEDIA (grl_media_new ());
    }
  }

  grl_media_set_id (media, id);
  grl_media_set_title (media, title);
  if (url) {
    grl_media_set_url (media, url);
  }
  if (desc) {
    grl_media_set_description (media, desc);
  }
  if (date) {
    grl_media_set_date (media, date);
  }

  if (type == BOOKMARK_TYPE_CATEGORY) {
    grl_media_box_set_childcount (GRL_MEDIA_BOX (media), childcount);
  }

  return media;
}

static void
bookmark_metadata (GrlMediaSourceMetadataSpec *ms)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  sqlite3 *db;
  GError *error = NULL;
  gchar *sql;
  const gchar *id;
  
  g_debug ("bookmark_metadata");

  db = GRL_BOOKMARKS_SOURCE (ms->source)->priv->db;

  id = grl_media_get_id (ms->media);
  if (!id) {
    /* Root category: special case */
    grl_media_set_title (ms->media, "");
    ms->callback (ms->source, ms->media, ms->user_data, NULL);
    return;
  }

  sql = g_strdup_printf (GRL_SQL_GET_BOOKMARK_BY_ID, id);
  g_debug ("%s", sql);
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL);
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to get bookmark: %s", sqlite3_errmsg (db));
    error = g_error_new (GRL_ERROR,
			 GRL_ERROR_METADATA_FAILED,
			 "Failed to get bookmark metadata");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
    return;
  }

  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  if (r == SQLITE_ROW) {
    build_media_from_stmt (ms->media, sql_stmt);
    ms->callback (ms->source, ms->media, ms->user_data, NULL);
  } else {
    g_warning ("Failed to get bookmark: %s", sqlite3_errmsg (db));
    error = g_error_new (GRL_ERROR,
			 GRL_ERROR_METADATA_FAILED,
			 "Failed to get bookmark metadata");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
  }

  sqlite3_finalize (sql_stmt);
}

static void
produce_bookmarks_from_sql (OperationSpec *os, const gchar *sql)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  sqlite3 *db;
  GrlMedia *media;
  GError *error = NULL;
  GList *medias = NULL;
  guint count = 0;
  GList *iter;

  g_debug ("produce_bookmarks_from_sql");

  g_debug ("%s", sql);
  db = GRL_BOOKMARKS_SOURCE (os->source)->priv->db;
  r = sqlite3_prepare_v2 (db, sql, strlen (sql), &sql_stmt, NULL);

  if (r != SQLITE_OK) {
    g_warning ("Failed to retrieve bookmarks: %s", sqlite3_errmsg (db));
    error = g_error_new (GRL_ERROR,
			 os->error_code,
			 "Failed to retrieve bookmarks list");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    goto free_resources;
  }

  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  while (r == SQLITE_ROW) {
    media = build_media_from_stmt (NULL, sql_stmt);
    medias = g_list_prepend (medias, media);
    count++;
    r = sqlite3_step (sql_stmt);
  }

  if (r != SQLITE_DONE) {
    g_warning ("Failed to retrieve bookmarks: %s", sqlite3_errmsg (db));
    error = g_error_new (GRL_ERROR,
			 os->error_code,
			 "Failed to retrieve bookmarks list");
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, error);
    g_error_free (error);
    goto free_resources;
  }

  if (count > 0) {
    medias = g_list_reverse (medias);
    iter = medias;
    while (iter) {
      media = GRL_MEDIA (iter->data);
      os->callback (os->source,
		    os->operation_id,
		    media,
		    --count,
		    os->user_data,
		    NULL);
      iter = g_list_next (iter);
    }
    g_list_free (medias);
  } else {
    os->callback (os->source, os->operation_id, NULL, 0, os->user_data, NULL);
  }

 free_resources:
  if (sql_stmt)
    sqlite3_finalize (sql_stmt);
}

static void
produce_bookmarks_by_query (OperationSpec *os, const gchar *query)
{
  gchar *sql;
  g_debug ("produce_bookmarks_by_query");
  sql = g_strdup_printf (GRL_SQL_GET_BOOKMARKS_BY_QUERY,
			 query, os->count, os->skip);    
  produce_bookmarks_from_sql (os, sql);
  g_free (sql);
}

static void
produce_bookmarks_by_text (OperationSpec *os, const gchar *text)
{
  gchar *sql;
  g_debug ("produce_bookmarks_by_text");
  sql = g_strdup_printf (GRL_SQL_GET_BOOKMARKS_BY_TEXT,
			 text, text, os->count, os->skip);    
  produce_bookmarks_from_sql (os, sql);
  g_free (sql);
}

static void
produce_bookmarks_from_category (OperationSpec *os, const gchar *category_id)
{
  gchar *sql;
  g_debug ("produce_bookmarks_from_category");
  sql = g_strdup_printf (GRL_SQL_GET_BOOKMARKS_BY_PARENT,
			 category_id, os->count, os->skip);    
  produce_bookmarks_from_sql (os, sql);
  g_free (sql);
}

static void
remove_bookmark (sqlite3 *db, const gchar *bookmark_id, GError **error)
{
  gint r;
  gchar *sql_error;
  gchar *sql;

  g_debug ("remove_bookmark");

  sql = g_strdup_printf (GRL_SQL_REMOVE_BOOKMARK, bookmark_id, bookmark_id);
  g_debug ("%s", sql);
  r = sqlite3_exec (db, sql, NULL, NULL, &sql_error);
  g_free (sql);

  if (r != SQLITE_OK) {
    g_warning ("Failed to remove bookmark '%s': %s", bookmark_id, sql_error);
    *error = g_error_new (GRL_ERROR,
			  GRL_ERROR_REMOVE_FAILED,
			  "Failed to remove bookmark");
    sqlite3_free (sql_error);
  }

  /* Remove orphan nodes from database */
  g_debug ("%s", GRL_SQL_REMOVE_ORPHAN);
  r = sqlite3_exec (db, GRL_SQL_REMOVE_ORPHAN, NULL, NULL, NULL);
}

static void
store_bookmark (sqlite3 *db,
		GrlMediaBox *parent,
		GrlMedia *bookmark,
		GError **error)
{
  gint r;
  sqlite3_stmt *sql_stmt = NULL;
  const gchar *title;
  const gchar *url;
  const gchar *desc;
  GTimeVal now;
  const gchar *parent_id;
  const gchar *mime;
  gchar *date;
  guint type;
  gchar *id;

  g_debug ("store_bookmark");

  title = grl_media_get_title (bookmark);
  url = grl_media_get_url (bookmark);
  desc = grl_media_get_description (bookmark);
  mime = grl_media_get_mime (bookmark);
  g_get_current_time (&now);
  date = g_time_val_to_iso8601 (&now);

  if (!parent) {
    parent_id = "0";
  } else {
    parent_id = grl_media_get_id (parent);
  }
  if (!parent_id) {
    parent_id = "0";
  }

  g_debug ("%s", GRL_SQL_STORE_BOOKMARK);
  r = sqlite3_prepare_v2 (db,
			  GRL_SQL_STORE_BOOKMARK,
			  strlen (GRL_SQL_STORE_BOOKMARK),
			  &sql_stmt, NULL);
  if (r != SQLITE_OK) {
    g_warning ("Failed to store bookmark '%s': %s", title, sqlite3_errmsg (db));
    *error = g_error_new (GRL_ERROR,
			  GRL_ERROR_STORE_FAILED,
			  "Failed to store bookmark '%s'", title);
    return;
  }

  g_debug ("URL: '%s'", url);

  if (GRL_IS_MEDIA_BOX (bookmark)) {
    type = BOOKMARK_TYPE_CATEGORY;
  } else {
    type = BOOKMARK_TYPE_STREAM;
  }

  sqlite3_bind_text (sql_stmt, 1, parent_id, -1, SQLITE_STATIC);
  sqlite3_bind_int (sql_stmt, 2, type);
  sqlite3_bind_text (sql_stmt, 3, title, -1, SQLITE_STATIC);
  if (type == BOOKMARK_TYPE_STREAM) {
    sqlite3_bind_text (sql_stmt, 4, url, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null (sql_stmt, 4);
  }
  if (date) {
    sqlite3_bind_text (sql_stmt, 5, date, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null (sql_stmt, 5);
  }
  if (mime) {
    sqlite3_bind_text (sql_stmt, 6, mime, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null (sql_stmt, 6);
  }
  if (desc) {
    sqlite3_bind_text (sql_stmt, 7, desc, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null (sql_stmt, 7);
  }

  while ((r = sqlite3_step (sql_stmt)) == SQLITE_BUSY);

  if (r != SQLITE_DONE) {
    g_warning ("Failed to store bookmark '%s': %s", title, sqlite3_errmsg (db));
    *error = g_error_new (GRL_ERROR,
			  GRL_ERROR_STORE_FAILED,
			  "Failed to store bookmark '%s'", title);
    sqlite3_finalize (sql_stmt);
    return;
  }

  sqlite3_finalize (sql_stmt);

  id = g_strdup_printf ("%llu", sqlite3_last_insert_rowid (db));
  grl_media_set_id (bookmark, id);
  g_free (id);
}

/* ================== API Implementation ================ */

static const GList *
grl_bookmarks_source_supported_keys (GrlMetadataSource *source)
{
  static GList *keys = NULL;
  if (!keys) {
    keys = grl_metadata_key_list_new (GRL_METADATA_KEY_ID,
                                      GRL_METADATA_KEY_TITLE,
                                      GRL_METADATA_KEY_URL,
                                      GRL_METADATA_KEY_CHILDCOUNT,
                                      GRL_METADATA_KEY_DESCRIPTION,
                                      GRL_METADATA_KEY_DATE,
                                      NULL);
  }
  return keys;
}

static void
grl_bookmarks_source_browse (GrlMediaSource *source,
                            GrlMediaSourceBrowseSpec *bs)
{
  g_debug ("grl_bookmarks_source_browse");

  OperationSpec *os;
  GrlBookmarksSource *bookmarks_source;
  GError *error = NULL;

  bookmarks_source = GRL_BOOKMARKS_SOURCE (source);
  if (!bookmarks_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (GRL_ERROR,
			 GRL_ERROR_BROWSE_FAILED,
			 "No database connection");
    bs->callback (bs->source, bs->browse_id, NULL, 0, bs->user_data, error);
    g_error_free (error);
  }

  /* Configure browse operation */
  os = g_new0 (OperationSpec, 1);
  os->source = bs->source;
  os->operation_id = bs->browse_id;
  os->media_id = grl_media_get_id (bs->container);
  os->count = bs->count;
  os->skip = bs->skip;
  os->callback = bs->callback;
  os->user_data = bs->user_data;
  os->error_code = GRL_ERROR_BROWSE_FAILED;

  produce_bookmarks_from_category (os, os->media_id ? os->media_id : "0");
  g_free (os);
}

static void
grl_bookmarks_source_search (GrlMediaSource *source,
			     GrlMediaSourceSearchSpec *ss)
{
  g_debug ("grl_bookmarks_source_search");

  GrlBookmarksSource *bookmarks_source;
  OperationSpec *os;
  GError *error = NULL;

  bookmarks_source = GRL_BOOKMARKS_SOURCE (source);
  if (!bookmarks_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (GRL_ERROR,
			 GRL_ERROR_QUERY_FAILED,
			 "No database connection");
    ss->callback (ss->source, ss->search_id, NULL, 0, ss->user_data, error);
    g_error_free (error);
  }

  os = g_new0 (OperationSpec, 1);
  os->source = ss->source;
  os->operation_id = ss->search_id;
  os->count = ss->count;
  os->skip = ss->skip;
  os->callback = ss->callback;
  os->user_data = ss->user_data;
  os->error_code = GRL_ERROR_SEARCH_FAILED;
  produce_bookmarks_by_text (os, ss->text);
  g_free (os);
}

static void
grl_bookmarks_source_query (GrlMediaSource *source,
			    GrlMediaSourceQuerySpec *qs)
{
  g_debug ("grl_bookmarks_source_query");

  GrlBookmarksSource *bookmarks_source;
  OperationSpec *os;
  GError *error = NULL;

  bookmarks_source = GRL_BOOKMARKS_SOURCE (source);
  if (!bookmarks_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (GRL_ERROR,
			 GRL_ERROR_QUERY_FAILED,
			 "No database connection");
    qs->callback (qs->source, qs->query_id, NULL, 0, qs->user_data, error);
    g_error_free (error);
  }

  os = g_new0 (OperationSpec, 1);
  os->source = qs->source;
  os->operation_id = qs->query_id;
  os->count = qs->count;
  os->skip = qs->skip;
  os->callback = qs->callback;
  os->user_data = qs->user_data;
  os->error_code = GRL_ERROR_SEARCH_FAILED;
  produce_bookmarks_by_query (os, qs->query);
  g_free (os);
}

static void
grl_bookmarks_source_store (GrlMediaSource *source, GrlMediaSourceStoreSpec *ss)
{
  g_debug ("grl_bookmarks_source_store");
  /* FIXME: Try to guess bookmark mime somehow */
  GError *error = NULL;
  store_bookmark (GRL_BOOKMARKS_SOURCE (ss->source)->priv->db,
		  ss->parent, ss->media, &error);
  ss->callback (ss->source, ss->parent, ss->media, ss->user_data, error);
  if (error) {
    g_error_free (error);
  }
}

static void grl_bookmarks_source_remove (GrlMediaSource *source,
					 GrlMediaSourceRemoveSpec *rs)
{
  g_debug ("grl_bookmarks_source_remove");
  GError *error = NULL;
  remove_bookmark (GRL_BOOKMARKS_SOURCE (rs->source)->priv->db,
		   rs->media_id, &error);
  rs->callback (rs->source, rs->media, rs->user_data, error);
  if (error) {
    g_error_free (error);
  }
}

static void
grl_bookmarks_source_metadata (GrlMediaSource *source,
			       GrlMediaSourceMetadataSpec *ms)
{
  g_debug ("grl_bookmarks_source_metadata");

  GrlBookmarksSource *bookmarks_source;
  GError *error = NULL;

  bookmarks_source = GRL_BOOKMARKS_SOURCE (source);
  if (!bookmarks_source->priv->db) {
    g_warning ("Can't execute operation: no database connection.");
    error = g_error_new (GRL_ERROR,
			 GRL_ERROR_METADATA_FAILED,
			 "No database connection");
    ms->callback (ms->source, ms->media, ms->user_data, error);
    g_error_free (error);
  }

  bookmark_metadata (ms);
}

static GrlSupportedOps
grl_bookmarks_source_supported_operations (GrlMetadataSource *metadata_source)
{
  GrlSupportedOps caps;
  GrlBookmarksSource *source;

  source = GRL_BOOKMARKS_SOURCE (metadata_source);
  caps = GRL_OP_BROWSE | GRL_OP_METADATA | GRL_OP_SEARCH | GRL_OP_QUERY |
    GRL_OP_STORE | GRL_OP_STORE_PARENT | GRL_OP_REMOVE;

  return caps;
}