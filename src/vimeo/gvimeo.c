/*
 * Copyright (C) 2010, 2011 Igalia S.L.
 *
 * Contact: Iago Toral Quiroga <itoral@igalia.com>
 *
 * Authors: Joaquim Rocha <jrocha@igalia.com>
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

#include "gvimeo.h"

#include <gcrypt.h>
#include <net/grl-net.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <quvi/quvi.h>

#define G_VIMEO_GET_PRIVATE(object)             \
  (G_TYPE_INSTANCE_GET_PRIVATE((object),        \
                               G_VIMEO_TYPE,    \
                               GVimeoPrivate))

#define PLUGIN_USER_AGENT             "Grilo Vimeo Plugin"

#define VIMEO_HOST                    "http://vimeo.com"
#define VIMEO_ENDPOINT                VIMEO_HOST "/api/rest/v2"
#define VIMEO_VIDEO_LOAD_URL          VIMEO_HOST "/moogaloop/load/clip:"
#define VIMEO_VIDEO_PLAY_URL          VIMEO_HOST "/moogaloop/play/clip:"

#define VIMEO_VIDEO_SEARCH_METHOD     "vimeo.videos.search"
#define VIMEO_API_OAUTH_SIGN_METHOD   "HMAC-SHA1"
#define VIMEO_API_OAUTH_SIGNATURE_PARAM "&oauth_signature=%s"

#define VIMEO_VIDEO_SEARCH					\
  "full_response=yes"						\
  "&method=%s"							\
  "&oauth_consumer_key=%s"					\
  "&oauth_nonce=%s"						\
  "&oauth_signature_method=" VIMEO_API_OAUTH_SIGN_METHOD	\
  "&oauth_timestamp=%s"						\
  "&oauth_token="						\
  "&page=%d"							\
  "&per_page=%d"						\
  "&query=%s"

enum {
  PROP_0,
  PROP_QUVI_FORMAT
};

typedef struct {
  GVimeo *vimeo;
  GVimeoVideoSearchCb search_cb;
  gpointer user_data;
} GVimeoVideoSearchData;

typedef struct {
  GVimeo *vimeo;
  gchar *vimeo_url;
  GVimeoURLCb callback;
  gpointer user_data;
} GVimeoVideoURLData;

struct _GVimeoPrivate {
  gchar *api_key;
  gchar *auth_token;
  gchar *auth_secret;
  gint per_page;
  quvi_t quvi_handler;
  GrlNetWc *wc;
};

enum InfoType {SIMPLE, EXTENDED};

typedef struct {
  enum InfoType type;
  gchar *name;
} VideoInfo;

static VideoInfo video_info[] = {{SIMPLE, VIMEO_VIDEO_TITLE},
				 {SIMPLE, VIMEO_VIDEO_DESCRIPTION},
				 {SIMPLE, VIMEO_VIDEO_UPLOAD_DATE},
				 {SIMPLE, VIMEO_VIDEO_WIDTH},
				 {SIMPLE, VIMEO_VIDEO_HEIGHT},
				 {SIMPLE, VIMEO_VIDEO_OWNER},
				 {SIMPLE, VIMEO_VIDEO_URL},
				 {SIMPLE, VIMEO_VIDEO_THUMBNAIL},
				 {SIMPLE, VIMEO_VIDEO_DURATION},
				 {EXTENDED, VIMEO_VIDEO_OWNER}};

static void g_vimeo_finalize (GObject *object);
static void g_vimeo_dispose (GObject *object);
static void g_vimeo_set_property (GObject *object,
                                  guint propid,
                                  const GValue *value,
                                  GParamSpec *spc);
static gchar * encode_uri (const gchar *uri);

/* -------------------- GOBJECT -------------------- */

G_DEFINE_TYPE (GVimeo, g_vimeo, G_TYPE_OBJECT);

static void
g_vimeo_class_init (GVimeoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = g_vimeo_finalize;
  gobject_class->dispose = g_vimeo_dispose;
  gobject_class->set_property = g_vimeo_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_QUVI_FORMAT,
                                   g_param_spec_string ("quvi-format",
                                                        "quvi-format",
                                                        "URL requested format",
                                                        NULL,
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME));

  g_type_class_add_private (klass, sizeof (GVimeoPrivate));
}

static void
g_vimeo_init (GVimeo *vimeo)
{
  vimeo->priv = G_VIMEO_GET_PRIVATE (vimeo);
  vimeo->priv->per_page = 50;
  if (quvi_init (&(vimeo->priv->quvi_handler)) != QUVI_OK) {
    vimeo->priv->quvi_handler = NULL;
  } else {
    quvi_setopt (vimeo->priv->quvi_handler, QUVIOPT_NOVERIFY);
  }
  vimeo->priv->wc = grl_net_wc_new ();
  g_object_set (vimeo->priv->wc, "user-agent", PLUGIN_USER_AGENT, NULL);
}

static void
g_vimeo_dispose (GObject *object)
{
  GVimeo *vimeo = G_VIMEO (object);

  if (vimeo->priv->wc) {
    g_object_unref (vimeo->priv->wc);
    vimeo->priv->wc = NULL;
  }

  G_OBJECT_CLASS (g_vimeo_parent_class)->dispose (object);
}

static void
g_vimeo_finalize (GObject *object)
{
  GVimeo *vimeo = G_VIMEO (object);
  g_free (vimeo->priv->api_key);
  g_free (vimeo->priv->auth_secret);
  if (vimeo->priv->quvi_handler) {
    quvi_close (&(vimeo->priv->quvi_handler));
  }

  G_OBJECT_CLASS (g_vimeo_parent_class)->finalize (object);
}

GVimeo *
g_vimeo_new (const gchar *api_key, const gchar *auth_secret)
{
  GVimeo *vimeo = g_object_new (G_VIMEO_TYPE, NULL);
  vimeo->priv->api_key = g_strdup (api_key);
  vimeo->priv->auth_secret = g_strdup (auth_secret);

  return vimeo;
}

static void
g_vimeo_set_property (GObject *object,
                      guint propid,
                      const GValue *value,
                      GParamSpec *pspec)
{
  switch (propid) {
  case PROP_QUVI_FORMAT:
    quvi_setopt (G_VIMEO (object)->priv->quvi_handler,
                 QUVIOPT_FORMAT,
                 g_value_get_string (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

/* -------------------- PRIVATE API -------------------- */

static gchar *
get_timestamp (void)
{
  time_t t = time (NULL);
  return g_strdup_printf ("%d", (gint) t);
}

static gchar *
get_nonce (void)
{
  gchar *timestamp = get_timestamp();
  guint rnd_number = g_random_int ();
  gchar *rnd_str = g_strdup_printf ("%d_%s", rnd_number, timestamp);
  gchar *nonce = g_compute_checksum_for_string (G_CHECKSUM_MD5, rnd_str, -1);
  g_free (timestamp);
  g_free (rnd_str);

  return nonce;
}

static gchar *
get_videos_search_params (GVimeo *vimeo, const gchar *text, gint page) {
  gchar *encoded_text = encode_uri (text);
  gchar *timestamp = get_timestamp ();
  gchar *nonce = get_nonce ();
  gchar *params = g_strdup_printf (VIMEO_VIDEO_SEARCH,
				   VIMEO_VIDEO_SEARCH_METHOD,
				   vimeo->priv->api_key,
				   nonce,
				   timestamp,
				   page,
				   vimeo->priv->per_page,
				   encoded_text);
  g_free (timestamp);
  g_free (nonce);
  g_free (encoded_text);

  return params;
}

static gchar *
sign_string (gchar *message, gchar *key)
{
  gchar *signed_message = NULL;
  gcry_md_hd_t digest_obj;
  unsigned char *hmac_digest;
  guint digest_len;

  gcry_md_open(&digest_obj,
	       GCRY_MD_SHA1,
	       GCRY_MD_FLAG_SECURE | GCRY_MD_FLAG_HMAC);
  gcry_md_setkey(digest_obj, key, strlen (key));
  gcry_md_write (digest_obj, message, strlen (message));
  gcry_md_final (digest_obj);
  hmac_digest = gcry_md_read (digest_obj, 0);

  digest_len = gcry_md_get_algo_dlen (GCRY_MD_SHA1);
  signed_message = g_base64_encode (hmac_digest, digest_len);

  gcry_md_close (digest_obj);

  return signed_message;
}

static gboolean
result_is_correct (xmlNodePtr node)
{
  gboolean correct = FALSE;
  xmlChar *stat;

  if (xmlStrcmp (node->name, (const xmlChar *) "rsp") == 0)
  {
    stat = xmlGetProp (node, (const xmlChar *) "stat");
    if (stat && xmlStrcmp (stat, (const xmlChar *) "ok") == 0)
    {
      correct = TRUE;
      xmlFree (stat);
    }
  }

  return correct;
}

static void
add_node (xmlNodePtr node, GHashTable *video)
{
  xmlAttrPtr attr;

  for (attr = node->properties; attr != NULL; attr = attr->next)
  {
    g_hash_table_insert (video,
                         g_strconcat ((const gchar *) node->name,
                                      "_",
                                      (const gchar *) attr->name,
                                      NULL),
                         (gchar *) xmlGetProp (node, attr->name));
  }
}

static xmlNodePtr
xpath_get_node (xmlXPathContextPtr context, gchar *xpath_expr)
{
  xmlNodePtr node = NULL;
  xmlXPathObjectPtr xpath_obj;
  xpath_obj = xmlXPathEvalExpression ((xmlChar *) xpath_expr, context);

  if (xpath_obj && xpath_obj->nodesetval->nodeTab)
  {
    node = xpath_obj->nodesetval->nodeTab[0];
  }
  xmlXPathFreeObject (xpath_obj);

  return node;
}

static GHashTable *
get_video (xmlNodePtr node)
{
  gint i;
  gint array_length;
  xmlXPathContextPtr context;
  gchar *video_id;
  GHashTable *video = g_hash_table_new_full (g_str_hash,
                                             g_str_equal,
                                             g_free,
                                             g_free);

  /* Adds the video node's properties */
  add_node (node, video);

  context = xmlXPathNewContext (node->doc);
  video_id = (gchar *) xmlGetProp (node, (xmlChar *) "id");

  array_length = G_N_ELEMENTS (video_info);
  for (i = 0; i < array_length; i++)
  {
    /* Look for the wanted information only under the current video */
    gchar *xpath_name = g_strdup_printf ("//video[@id=%s]//%s",
					 video_id,
					 video_info[i].name);
    xmlNodePtr info_node = xpath_get_node (context, xpath_name);
    if (info_node)
    {
      if (video_info[i].type == EXTENDED) {
	add_node (info_node, video);
      }
      else
      {
	g_hash_table_insert (video,
			     g_strdup ((const gchar *) info_node->name),
			     (gchar *) xmlNodeGetContent (info_node));
      }
    }
    g_free (xpath_name);
  }
  g_free (video_id);

  xmlXPathFreeContext (context);

  return video;
}


static void
process_video_search_result (const gchar *xml_result, gpointer user_data)
{
  xmlDocPtr doc;
  xmlNodePtr node;
  GList *video_list = NULL;
  GVimeoVideoSearchData *data = (GVimeoVideoSearchData *) user_data;

  doc = xmlReadMemory (xml_result,
		       xmlStrlen ((xmlChar *) xml_result),
		       NULL,
		       NULL,
		       XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
  node = xmlDocGetRootElement (doc);

  /* Check result is ok */
  if (!node || !result_is_correct (node))
  {
    data->search_cb (data->vimeo, NULL, data->user_data);
  }
  else
  {
    node = node->xmlChildrenNode;

    /* Now we're at "video pages" node */
    node = node->xmlChildrenNode;
    while (node)
    {
      video_list = g_list_prepend (video_list, get_video (node));
      node = node->next;
    }

    data->search_cb (data->vimeo, g_list_reverse (video_list), data->user_data);
    g_list_free_full (video_list, (GDestroyNotify) g_hash_table_unref);
  }
  g_slice_free (GVimeoVideoSearchData, data);
  xmlFreeDoc (doc);
}

static void
search_videos_complete_cb (GObject *source_object,
                           GAsyncResult *res,
                           gpointer data)
{
  gchar *content = NULL;

  GVimeoVideoSearchCb *search_data = (GVimeoVideoSearchCb *) data;
  grl_net_wc_request_finish (GRL_NET_WC (source_object),
                             res,
                             &content,
                             NULL,
                             NULL);

  process_video_search_result (content, search_data);
}

static gboolean
get_video_play_url_cb (GVimeoVideoURLData *url_data)
{
  QUVIcode rc;
  gchar *url = NULL;
  quvi_media_t v;

  if (url_data->vimeo->priv->quvi_handler) {
    rc = quvi_parse (url_data->vimeo->priv->quvi_handler,
                     url_data->vimeo_url,
                     &v);
    if (rc == QUVI_OK) {
      rc = quvi_getprop (v, QUVIPROP_MEDIAURL, &url);
      url_data->callback (url, url_data->user_data);
      quvi_parse_close (&v);
    } else {
      url_data->callback (NULL, url_data->user_data);
    }
  } else {
    url_data->callback (NULL, url_data->user_data);
  }

  g_object_unref (url_data->vimeo);
  g_free (url_data->vimeo_url);
  g_slice_free (GVimeoVideoURLData, url_data);

  return FALSE;
}

static gchar *
encode_uri (const gchar *uri)
{
  return g_uri_escape_string (uri, NULL, TRUE);
}

static gchar *
build_request (GVimeo *vimeo, const gchar *query, gint page)
{
  gchar *params;
  gchar *endpoint_encoded;
  gchar *key;
  gchar *escaped_str;
  gchar *tmp_str;
  gchar *signature;

  g_return_val_if_fail (G_IS_VIMEO (vimeo), NULL);

  params = get_videos_search_params (vimeo, query, page);
  endpoint_encoded = encode_uri (VIMEO_ENDPOINT);
  key = g_strdup_printf ("%s&", vimeo->priv->auth_secret);
  escaped_str = encode_uri (params);
  tmp_str = g_strdup_printf ("GET&%s&%s", endpoint_encoded, escaped_str);
  signature = sign_string (tmp_str, key);
  g_free (escaped_str);
  g_free (tmp_str);
  escaped_str = encode_uri (signature);
  tmp_str = g_strdup_printf ("%s?%s" VIMEO_API_OAUTH_SIGNATURE_PARAM,
			     VIMEO_ENDPOINT,
			     params,
			     escaped_str);

  g_free (endpoint_encoded);
  g_free (params);
  g_free (key);
  g_free (escaped_str);
  g_free (signature);

  return tmp_str;
}

/* -------------------- PUBLIC API -------------------- */

void
g_vimeo_set_per_page (GVimeo *vimeo, gint per_page)
{
  g_return_if_fail (G_IS_VIMEO (vimeo));
  vimeo->priv->per_page = per_page;
}

void
g_vimeo_videos_search (GVimeo *vimeo,
		       const gchar *text,
		       gint page,
		       GVimeoVideoSearchCb callback,
		       gpointer user_data)
{
  GVimeoVideoSearchData *search_data;
  gchar *request;

  g_return_if_fail (G_IS_VIMEO (vimeo));

  request = build_request (vimeo, text, page);
  search_data = g_slice_new (GVimeoVideoSearchData);
  search_data->vimeo = vimeo;
  search_data->search_cb = callback;
  search_data->user_data = user_data;

  grl_net_wc_request_async (vimeo->priv->wc,
                            request,
                            NULL,
                            search_videos_complete_cb,
                            search_data);
  g_free (request);
}

void
g_vimeo_video_get_play_url (GVimeo *vimeo,
			    gint id,
			    GVimeoURLCb callback,
			    gpointer user_data)
{
  GVimeoVideoURLData *data;

  data = g_slice_new (GVimeoVideoURLData);
  data->vimeo = g_object_ref (vimeo);
  data->vimeo_url = g_strdup_printf (VIMEO_HOST "/%d", id);
  data->callback = callback;
  data->user_data = user_data;

  g_idle_add ((GSourceFunc) get_video_play_url_cb, data);
}
