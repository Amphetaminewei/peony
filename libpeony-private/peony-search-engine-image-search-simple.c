/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Copyright (C) 2005 Red Hat, Inc
 * Copyright (C) 2019 Tianjin KYLIN Information Technology Co., Ltd
 *
 * Peony is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Peony is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; see the file COPYING.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 * Modified by: Yue Lan <lanyue@kylinos.cn>
 *
 */

#include <config.h>
#include "peony-search-engine-image-search-simple.h"

#include <string.h>
#include <glib.h>

#include <eel/eel-gtk-macros.h>
#include <gio/gio.h>

#define BATCH_SIZE 500

typedef struct
{
    PeonySearchEngineImageSearchSimple *engine;
    GCancellable *cancellable;

    GList *mime_types;
    char **words;
    GList *found_list;

    GQueue *directories; 

    GHashTable *visited;

    gint n_processed_files;

    gchar *search_text;
    GList *uri_hits;
} SearchThreadData;


struct PeonySearchEngineImageSearchSimpleDetails
{
    PeonyQuery *query;

    SearchThreadData *active_search;

    gboolean query_finished;
};


static void  peony_search_engine_image_search_simple_class_init       (PeonySearchEngineImageSearchSimpleClass *class);
static void  peony_search_engine_image_search_simple_init             (PeonySearchEngineImageSearchSimple      *engine);

G_DEFINE_TYPE (PeonySearchEngineImageSearchSimple,
               peony_search_engine_image_search_simple,
               PEONY_TYPE_SEARCH_ENGINE);

static PeonySearchEngineClass *parent_class = NULL;

static void
finalize (GObject *object)
{
    PeonySearchEngineImageSearchSimple *simple;

    simple = PEONY_SEARCH_ENGINE_IMAGE_SEARCH_SIMPLE (object);

    if (simple->details->query)
    {
        g_object_unref (simple->details->query);
        simple->details->query = NULL;
    }

    g_free (simple->details);

    EEL_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

static SearchThreadData *
search_thread_data_new (PeonySearchEngineImageSearchSimple *engine,
                        PeonyQuery *query)
{
    SearchThreadData *data;
    char *text, *lower, *normalized, *uri;
    GFile *location;

    data = g_new0 (SearchThreadData, 1);

    data->engine = engine;
    data->directories = g_queue_new ();
    data->visited = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    data->search_text = peony_query_get_text (query);

    uri = peony_query_get_location (query);
    location = NULL;
    if (uri != NULL)
    {
        location = g_file_new_for_uri (uri);
        g_free (uri);
    }
    if (location == NULL)
    {
        location = g_file_new_for_path ("/");
    }
    g_queue_push_tail (data->directories, location);

    text = peony_query_get_text (query);
    normalized = g_utf8_normalize (text, -1, G_NORMALIZE_NFD);
    lower = g_utf8_strdown (normalized, -1);
    data->words = g_strsplit (lower, " ", -1);
    g_free (text);
    g_free (lower);
    g_free (normalized);

    data->mime_types = peony_query_get_mime_types (query);

    data->cancellable = g_cancellable_new ();

    return data;
}

static void
search_thread_data_free (SearchThreadData *data)
{
    g_queue_foreach (data->directories,
                     (GFunc)g_object_unref, NULL);
    g_queue_free (data->directories);
    g_hash_table_destroy (data->visited);
    g_object_unref (data->cancellable);
    g_strfreev (data->words);
    g_list_free_full (data->mime_types, g_free);
    g_list_free_full (data->uri_hits, g_free);

    g_free (data->search_text);

    g_free (data);
}

static gboolean
search_thread_done_idle (gpointer user_data)
{
    SearchThreadData *data;

    data = user_data;

    if (!g_cancellable_is_cancelled (data->cancellable))
    {
        peony_search_engine_finished (PEONY_SEARCH_ENGINE (data->engine));
        data->engine->details->active_search = NULL;
    }

    search_thread_data_free (data);

    return FALSE;
}

typedef struct
{
    GList *uris;
    SearchThreadData *thread_data;
} SearchHits;


static gboolean
search_thread_add_hits_idle (gpointer user_data)
{
    SearchHits *hits;

    hits = user_data;

    if (!g_cancellable_is_cancelled (hits->thread_data->cancellable))
    {
        peony_search_engine_hits_added (PEONY_SEARCH_ENGINE (hits->thread_data->engine),
                                       hits->uris);
    }

    g_list_free_full (hits->uris, g_free);
    g_free (hits);

    return FALSE;
}

static void
send_batch (SearchThreadData *data)
{
    SearchHits *hits;

    data->n_processed_files = 0;

    if (data->uri_hits)
    {
        hits = g_new (SearchHits, 1);
        hits->uris = data->uri_hits;
        hits->thread_data = data;
        g_idle_add (search_thread_add_hits_idle, hits);
    }
    data->uri_hits = NULL;
}

#define STD_ATTRIBUTES \
	G_FILE_ATTRIBUTE_STANDARD_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
	G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
	G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
	G_FILE_ATTRIBUTE_ID_FILE

static void
visit_directory (GFile *dir, SearchThreadData *data)
{
    /*
    TODO:
    the search text is the selected file's uri, we need use this uri get a file list from another program.
    this method has been located in a thread, so we don't need to care the user experience.
    */
    printf ("search_text: %s\n", data->search_text);
    //data->uri_hits = g_list_prepend (data->uri_hits, g_strdup ("file:///home/kylin/test/1.png"));
    data->uri_hits = g_list_prepend (data->uri_hits, g_strdup (data->search_text));
    return;
}


static gpointer
search_thread_func (gpointer user_data)
{
    SearchThreadData *data;
    GFile *dir;
    GFileInfo *info;
    const char *id;

    data = user_data;

    /* Insert id for toplevel directory into visited */
    dir = g_queue_peek_head (data->directories);
    info = g_file_query_info (dir, G_FILE_ATTRIBUTE_ID_FILE, 0, data->cancellable, NULL);
    if (info)
    {
        id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE);
        if (id)
        {
            g_hash_table_insert (data->visited, g_strdup (id), NULL);
        }
        g_object_unref (info);
    }

    while (!g_cancellable_is_cancelled (data->cancellable) &&
            (dir = g_queue_pop_head (data->directories)) != NULL)
    {
        visit_directory (dir, data);
        g_object_unref (dir);
    }
    send_batch (data);

    g_idle_add (search_thread_done_idle, data);

    return NULL;
}

static void
peony_search_engine_image_search_simple_start (PeonySearchEngine *engine)
{
    PeonySearchEngineImageSearchSimple *simple;
    SearchThreadData *data;
    GThread *thread;

    simple = PEONY_SEARCH_ENGINE_IMAGE_SEARCH_SIMPLE (engine);

    if (simple->details->active_search != NULL)
    {
        return;
    }

    if (simple->details->query == NULL)
    {
        return;
    }

    data = search_thread_data_new (simple, simple->details->query);

    thread = g_thread_new ("peony-search-image-search-simple", search_thread_func, data);
    simple->details->active_search = data;

    g_thread_unref (thread);
}

static void
peony_search_engine_image_search_simple_stop (PeonySearchEngine *engine)
{
    PeonySearchEngineImageSearchSimple *simple;

    simple = PEONY_SEARCH_ENGINE_IMAGE_SEARCH_SIMPLE (engine);

    if (simple->details->active_search != NULL)
    {
        g_cancellable_cancel (simple->details->active_search->cancellable);
        simple->details->active_search = NULL;
    }
}

static gboolean
peony_search_engine_image_search_simple_is_indexed (PeonySearchEngine *engine)
{
    return FALSE;
}

static void
peony_search_engine_image_search_simple_set_query (PeonySearchEngine *engine, PeonyQuery *query)
{
    PeonySearchEngineImageSearchSimple *simple;

    simple = PEONY_SEARCH_ENGINE_IMAGE_SEARCH_SIMPLE (engine);

    if (query)
    {
        g_object_ref (query);
    }

    if (simple->details->query)
    {
        g_object_unref (simple->details->query);
    }

    simple->details->query = query;
}

static void
peony_search_engine_image_search_simple_class_init (PeonySearchEngineImageSearchSimpleClass *class)
{
    GObjectClass *gobject_class;
    PeonySearchEngineClass *engine_class;

    parent_class = g_type_class_peek_parent (class);

    gobject_class = G_OBJECT_CLASS (class);
    gobject_class->finalize = finalize;

    engine_class = PEONY_SEARCH_ENGINE_CLASS (class);
    engine_class->set_query = peony_search_engine_image_search_simple_set_query;
    engine_class->start = peony_search_engine_image_search_simple_start;
    engine_class->stop = peony_search_engine_image_search_simple_stop;
    engine_class->is_indexed = peony_search_engine_image_search_simple_is_indexed;
}

static void
peony_search_engine_image_search_simple_init (PeonySearchEngineImageSearchSimple *engine)
{
    engine->details = g_new0 (PeonySearchEngineImageSearchSimpleDetails, 1);
}


PeonySearchEngine *
peony_search_engine_image_search_simple_new (void)
{
    PeonySearchEngine *engine;

    engine = g_object_new (PEONY_TYPE_SEARCH_ENGINE_IMAGE_SEARCH_SIMPLE, NULL);

    return engine;
}
