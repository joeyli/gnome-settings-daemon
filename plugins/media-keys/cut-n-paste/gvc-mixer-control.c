/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006-2008 Lennart Poettering
 * Copyright (C) 2008 Sjoerd Simons <sjoerd@luon.net>
 * Copyright (C) 2008 William Jon McCann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <pulse/ext-stream-restore.h>

#include "gvc-mixer-control.h"
#include "gvc-mixer-sink.h"
#include "gvc-mixer-source.h"
#include "gvc-mixer-sink-input.h"
#include "gvc-mixer-source-output.h"
#include "gvc-mixer-event-role.h"

#define GVC_MIXER_CONTROL_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GVC_TYPE_MIXER_CONTROL, GvcMixerControlPrivate))

struct GvcMixerControlPrivate
{
        pa_glib_mainloop *pa_mainloop;
        pa_mainloop_api  *pa_api;
        pa_context       *pa_context;
        int               n_outstanding;

        gboolean          default_sink_is_set;
        guint             default_sink_id;
        char             *default_sink_name;
        gboolean          default_source_is_set;
        guint             default_source_id;
        char             *default_source_name;

        gboolean          event_sink_input_is_set;
        guint             event_sink_input_id;

        GHashTable       *all_streams;
        GHashTable       *sinks; /* fixed outputs */
        GHashTable       *sources; /* fixed inputs */
        GHashTable       *sink_inputs; /* routable output streams */
        GHashTable       *source_outputs; /* routable input streams */
        GHashTable       *clients;
};

enum {
        READY,
        STREAM_ADDED,
        STREAM_REMOVED,
        DEFAULT_SINK_CHANGED,
        DEFAULT_SOURCE_CHANGED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     gvc_mixer_control_class_init (GvcMixerControlClass *klass);
static void     gvc_mixer_control_init       (GvcMixerControl      *mixer_control);
static void     gvc_mixer_control_finalize   (GObject              *object);

G_DEFINE_TYPE (GvcMixerControl, gvc_mixer_control, G_TYPE_OBJECT)

pa_context *
gvc_mixer_control_get_pa_context (GvcMixerControl *control)
{
        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);
        return control->priv->pa_context;
}

GvcMixerStream *
gvc_mixer_control_get_event_sink_input (GvcMixerControl *control)
{
        GvcMixerStream *stream;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        stream = g_hash_table_lookup (control->priv->all_streams,
                                      GUINT_TO_POINTER (control->priv->event_sink_input_id));

        return stream;
}

gboolean
gvc_mixer_control_set_default_sink (GvcMixerControl *control,
                                    GvcMixerStream  *stream)
{
        pa_operation *o;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), FALSE);
        g_return_val_if_fail (GVC_IS_MIXER_STREAM (stream), FALSE);

        o = pa_context_set_default_sink (control->priv->pa_context,
                                         gvc_mixer_stream_get_name (stream),
                                         NULL,
                                         NULL);
        if (o == NULL) {
                g_warning ("pa_context_set_default_sink() failed");
                return FALSE;
        }

        pa_operation_unref (o);

        return TRUE;
}

gboolean
gvc_mixer_control_set_default_source (GvcMixerControl *control,
                                      GvcMixerStream  *stream)
{
        pa_operation *o;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), FALSE);
        g_return_val_if_fail (GVC_IS_MIXER_STREAM (stream), FALSE);

        o = pa_context_set_default_source (control->priv->pa_context,
                                           gvc_mixer_stream_get_name (stream),
                                           NULL,
                                           NULL);
        if (o == NULL) {
                g_warning ("pa_context_set_default_source() failed");
                return FALSE;
        }

        pa_operation_unref (o);

        return TRUE;
}

GvcMixerStream *
gvc_mixer_control_get_default_sink (GvcMixerControl *control)
{
        GvcMixerStream *stream;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        if (control->priv->default_sink_is_set) {
                stream = g_hash_table_lookup (control->priv->all_streams,
                                              GUINT_TO_POINTER (control->priv->default_sink_id));
        } else {
                stream = NULL;
        }

        return stream;
}

GvcMixerStream *
gvc_mixer_control_get_default_source (GvcMixerControl *control)
{
        GvcMixerStream *stream;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        if (control->priv->default_source_is_set) {
                stream = g_hash_table_lookup (control->priv->all_streams,
                                              GUINT_TO_POINTER (control->priv->default_source_id));
        } else {
                stream = NULL;
        }

        return stream;
}

GvcMixerStream *
gvc_mixer_control_lookup_stream_id (GvcMixerControl *control,
                                    guint            id)
{
        GvcMixerStream *stream;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        stream = g_hash_table_lookup (control->priv->all_streams,
                                      GUINT_TO_POINTER (id));
        return stream;
}

static void
listify_hash_values_hfunc (gpointer key,
                           gpointer value,
                           gpointer user_data)
{
        GSList **list = user_data;

        *list = g_slist_prepend (*list, value);
}

static int
gvc_stream_collate (GvcMixerStream *a,
                    GvcMixerStream *b)
{
        const char *namea;
        const char *nameb;

        g_return_val_if_fail (a == NULL || GVC_IS_MIXER_STREAM (a), 0);
        g_return_val_if_fail (b == NULL || GVC_IS_MIXER_STREAM (b), 0);

        namea = gvc_mixer_stream_get_name (a);
        nameb = gvc_mixer_stream_get_name (b);

        return g_utf8_collate (namea, nameb);
}

GSList *
gvc_mixer_control_get_streams (GvcMixerControl *control)
{
        GSList *retval;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        retval = NULL;
        g_hash_table_foreach (control->priv->all_streams,
                              listify_hash_values_hfunc,
                              &retval);
        return g_slist_sort (retval, (GCompareFunc) gvc_stream_collate);
}

GSList *
gvc_mixer_control_get_sinks (GvcMixerControl *control)
{
        GSList *retval;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        retval = NULL;
        g_hash_table_foreach (control->priv->sinks,
                              listify_hash_values_hfunc,
                              &retval);
        return g_slist_sort (retval, (GCompareFunc) gvc_stream_collate);
}

GSList *
gvc_mixer_control_get_sources (GvcMixerControl *control)
{
        GSList *retval;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        retval = NULL;
        g_hash_table_foreach (control->priv->sources,
                              listify_hash_values_hfunc,
                              &retval);
        return g_slist_sort (retval, (GCompareFunc) gvc_stream_collate);
}

GSList *
gvc_mixer_control_get_sink_inputs (GvcMixerControl *control)
{
        GSList *retval;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        retval = NULL;
        g_hash_table_foreach (control->priv->sink_inputs,
                              listify_hash_values_hfunc,
                              &retval);
        return g_slist_sort (retval, (GCompareFunc) gvc_stream_collate);
}

GSList *
gvc_mixer_control_get_source_outputs (GvcMixerControl *control)
{
        GSList *retval;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

        retval = NULL;
        g_hash_table_foreach (control->priv->source_outputs,
                              listify_hash_values_hfunc,
                              &retval);
        return g_slist_sort (retval, (GCompareFunc) gvc_stream_collate);
}

static void
dec_outstanding (GvcMixerControl *control)
{
        if (control->priv->n_outstanding <= 0) {
                return;
        }

        if (--control->priv->n_outstanding <= 0) {
                g_signal_emit (G_OBJECT (control), signals[READY], 0);
        }
}

gboolean
gvc_mixer_control_is_ready (GvcMixerControl *control)
{
        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), FALSE);

        return (control->priv->n_outstanding == 0);
}


static void
_set_default_source (GvcMixerControl *control,
                     GvcMixerStream  *stream)
{
        guint new_id;

        new_id = 0;

        if (stream != NULL) {
                new_id = gvc_mixer_stream_get_id (stream);
        }

        if (control->priv->default_source_id != new_id) {
                control->priv->default_source_id = new_id;
                control->priv->default_source_is_set = TRUE;
                g_signal_emit (control,
                               signals[DEFAULT_SOURCE_CHANGED],
                               0,
                               new_id);
        }
}

static void
_set_default_sink (GvcMixerControl *control,
                   GvcMixerStream  *stream)
{
        guint new_id;

        new_id = 0;

        if (stream != NULL) {
                new_id = gvc_mixer_stream_get_id (stream);
        }

        if (control->priv->default_sink_id != new_id) {
                control->priv->default_sink_id = new_id;
                control->priv->default_sink_is_set = TRUE;

                g_signal_emit (control,
                               signals[DEFAULT_SINK_CHANGED],
                               0,
                               new_id);
        }
}

static gboolean
_stream_has_name (gpointer        key,
                  GvcMixerStream *stream,
                  const char     *name)
{
        const char *t_name;

        t_name = gvc_mixer_stream_get_name (stream);

        if (t_name != NULL
            && name != NULL
            && strcmp (t_name, name) == 0) {
                return TRUE;
        }

        return FALSE;
}

static GvcMixerStream  *
find_stream_for_name (GvcMixerControl *control,
                      const char      *name)
{
        GvcMixerStream *stream;

        stream = g_hash_table_find (control->priv->all_streams,
                                    (GHRFunc)_stream_has_name,
                                    (char *)name);
        return stream;
}

static void
update_default_source_from_name (GvcMixerControl *control,
                                 const char      *name)
{
        gboolean changed;

        if ((control->priv->default_source_name == NULL
             && name != NULL)
            || (control->priv->default_source_name != NULL
                && name == NULL)
            || strcmp (control->priv->default_source_name, name) != 0) {
                changed = TRUE;
        }

        if (changed) {
                GvcMixerStream *stream;

                g_free (control->priv->default_source_name);
                control->priv->default_source_name = g_strdup (name);

                stream = find_stream_for_name (control, name);
                _set_default_source (control, stream);
        }
}

static void
update_default_sink_from_name (GvcMixerControl *control,
                               const char      *name)
{
        gboolean changed;

        if ((control->priv->default_sink_name == NULL
             && name != NULL)
            || (control->priv->default_sink_name != NULL
                && name == NULL)
            || strcmp (control->priv->default_sink_name, name) != 0) {
                changed = TRUE;
        }

        if (changed) {
                GvcMixerStream *stream;
                g_free (control->priv->default_sink_name);
                control->priv->default_sink_name = g_strdup (name);

                stream = find_stream_for_name (control, name);
                _set_default_sink (control, stream);
        }
}

static void
update_server (GvcMixerControl      *control,
               const pa_server_info *info)
{
        if (info->default_source_name != NULL) {
                update_default_source_from_name (control, info->default_source_name);
        }
        if (info->default_sink_name != NULL) {
                update_default_sink_from_name (control, info->default_sink_name);
        }
}

static void
remove_stream (GvcMixerControl *control,
               GvcMixerStream  *stream)
{
        guint id;

        g_object_ref (stream);

        id = gvc_mixer_stream_get_id (stream);

        if (id == control->priv->default_sink_id) {
                _set_default_sink (control, NULL);
        } else if (id == control->priv->default_source_id) {
                _set_default_source (control, NULL);
        }

        g_hash_table_remove (control->priv->all_streams,
                             GUINT_TO_POINTER (id));
        g_signal_emit (G_OBJECT (control),
                       signals[STREAM_REMOVED],
                       0,
                       gvc_mixer_stream_get_id (stream));
        g_object_unref (stream);
}

static void
add_stream (GvcMixerControl *control,
            GvcMixerStream  *stream)
{
        g_hash_table_insert (control->priv->all_streams,
                             GUINT_TO_POINTER (gvc_mixer_stream_get_id (stream)),
                             stream);
        g_signal_emit (G_OBJECT (control),
                       signals[STREAM_ADDED],
                       0,
                       gvc_mixer_stream_get_id (stream));
}

static void
update_sink (GvcMixerControl    *control,
             const pa_sink_info *info)
{
        GvcMixerStream *stream;
        gboolean        is_new;
        pa_volume_t     max_volume;
        char            map_buff[PA_CHANNEL_MAP_SNPRINT_MAX];

        pa_channel_map_snprint (map_buff, PA_CHANNEL_MAP_SNPRINT_MAX, &info->channel_map);
#if 1
        g_debug ("Updating sink: index=%u name='%s' description='%s' map='%s'",
                 info->index,
                 info->name,
                 info->description,
                 map_buff);
#endif

        /* for now completely ignore virtual streams */
        if (!(info->flags & PA_SINK_HARDWARE)) {
                return;
        }

        is_new = FALSE;
        stream = g_hash_table_lookup (control->priv->sinks,
                                      GUINT_TO_POINTER (info->index));
        if (stream == NULL) {
                GvcChannelMap *map;
                map = gvc_channel_map_new_from_pa_channel_map (&info->channel_map);
                stream = gvc_mixer_sink_new (control->priv->pa_context,
                                             info->index,
                                             map);
                g_object_unref (map);
                is_new = TRUE;
        }

        max_volume = pa_cvolume_max (&info->volume);
        gvc_mixer_stream_set_name (stream, info->name);
        gvc_mixer_stream_set_description (stream, info->description);
        gvc_mixer_stream_set_icon_name (stream, "audio-card");
        gvc_mixer_stream_set_volume (stream, (guint)max_volume);
        gvc_mixer_stream_set_is_muted (stream, info->mute);
        gvc_mixer_stream_set_can_decibel (stream, !!(info->flags & PA_SINK_DECIBEL_VOLUME));
        if (!!(info->flags & PA_SINK_DECIBEL_VOLUME)) {
                gdouble db;
                db = pa_sw_volume_to_dB (max_volume);
                gvc_mixer_stream_set_decibel (stream, db);
        }

        if (is_new) {
                g_hash_table_insert (control->priv->sinks,
                                     GUINT_TO_POINTER (info->index),
                                     g_object_ref (stream));
                add_stream (control, stream);
        }

        if (control->priv->default_sink_name != NULL
            && info->name != NULL
            && strcmp (control->priv->default_sink_name, info->name) == 0) {
                _set_default_sink (control, stream);
        }
}

static void
update_source (GvcMixerControl      *control,
               const pa_source_info *info)
{
        GvcMixerStream *stream;
        gboolean        is_new;
        pa_volume_t     max_volume;

#if 1
        g_debug ("Updating source: index=%u name='%s' description='%s'",
                 info->index,
                 info->name,
                 info->description);
#endif

        /* for now completely ignore virtual streams */
        if (!(info->flags & PA_SOURCE_HARDWARE)) {
                return;
        }

        is_new = FALSE;

        stream = g_hash_table_lookup (control->priv->sources,
                                      GUINT_TO_POINTER (info->index));
        if (stream == NULL) {
                GvcChannelMap *map;
                map = gvc_channel_map_new_from_pa_channel_map (&info->channel_map);
                stream = gvc_mixer_source_new (control->priv->pa_context,
                                               info->index,
                                               map);
                g_object_unref (map);
                is_new = TRUE;
        }

        max_volume = pa_cvolume_max (&info->volume);

        gvc_mixer_stream_set_name (stream, info->name);
        gvc_mixer_stream_set_description (stream, info->description);
        gvc_mixer_stream_set_icon_name (stream, "audio-input-microphone");
        gvc_mixer_stream_set_volume (stream, (guint)max_volume);
        gvc_mixer_stream_set_is_muted (stream, info->mute);
        gvc_mixer_stream_set_can_decibel (stream, !!(info->flags & PA_SOURCE_DECIBEL_VOLUME));
        if (!!(info->flags & PA_SINK_DECIBEL_VOLUME)) {
                gdouble db;
                db = pa_sw_volume_to_dB (max_volume);
                gvc_mixer_stream_set_decibel (stream, db);
        }

        if (is_new) {
                g_hash_table_insert (control->priv->sources,
                                     GUINT_TO_POINTER (info->index),
                                     g_object_ref (stream));
                add_stream (control, stream);
        }

        if (control->priv->default_source_name != NULL
            && info->name != NULL
            && strcmp (control->priv->default_source_name, info->name) == 0) {
                _set_default_source (control, stream);
        }
}

static void
set_icon_name_from_proplist (GvcMixerStream *stream,
                             pa_proplist    *l,
                             const char     *default_icon_name)
{
        const char *t;

        if ((t = pa_proplist_gets (l, PA_PROP_MEDIA_ICON_NAME))) {
                goto finish;
        }

        if ((t = pa_proplist_gets (l, PA_PROP_WINDOW_ICON_NAME))) {
                goto finish;
        }

        if ((t = pa_proplist_gets (l, PA_PROP_APPLICATION_ICON_NAME))) {
                goto finish;
        }

        if ((t = pa_proplist_gets (l, PA_PROP_MEDIA_ROLE))) {

                if (strcmp (t, "video") == 0 ||
                    strcmp (t, "phone") == 0) {
                        goto finish;
                }

                if (strcmp (t, "music") == 0) {
                        t = "audio";
                        goto finish;
                }

                if (strcmp (t, "game") == 0) {
                        t = "applications-games";
                        goto finish;
                }

                if (strcmp (t, "event") == 0) {
                        t = "dialog-information";
                        goto finish;
                }
        }

        t = default_icon_name;

 finish:
        gvc_mixer_stream_set_icon_name (stream, t);
}

static void
update_sink_input (GvcMixerControl          *control,
                   const pa_sink_input_info *info)
{
        GvcMixerStream *stream;
        gboolean        is_new;
        pa_volume_t     max_volume;
        const char     *name;

#if 0
        g_debug ("Updating sink input: index=%u name='%s' client=%u sink=%u",
                 info->index,
                 info->name,
                 info->client,
                 info->sink);
#endif

        is_new = FALSE;

        stream = g_hash_table_lookup (control->priv->sink_inputs,
                                      GUINT_TO_POINTER (info->index));
        if (stream == NULL) {
                GvcChannelMap *map;
                map = gvc_channel_map_new_from_pa_channel_map (&info->channel_map);
                stream = gvc_mixer_sink_input_new (control->priv->pa_context,
                                                   info->index,
                                                   map);
                g_object_unref (map);
                is_new = TRUE;
        }

        max_volume = pa_cvolume_max (&info->volume);

        name = (const char *)g_hash_table_lookup (control->priv->clients,
                                                  GUINT_TO_POINTER (info->client));
        gvc_mixer_stream_set_name (stream, name);
        gvc_mixer_stream_set_description (stream, info->name);

        set_icon_name_from_proplist (stream, info->proplist, "applications-multimedia");
        gvc_mixer_stream_set_volume (stream, (guint)max_volume);
        gvc_mixer_stream_set_is_muted (stream, info->mute);

        if (is_new) {
                g_hash_table_insert (control->priv->sink_inputs,
                                     GUINT_TO_POINTER (info->index),
                                     g_object_ref (stream));
                add_stream (control, stream);
        }
}

static void
update_source_output (GvcMixerControl             *control,
                      const pa_source_output_info *info)
{
        GvcMixerStream *stream;
        gboolean        is_new;
        const char     *name;

#if 1
        g_debug ("Updating source output: index=%u name='%s' client=%u source=%u",
                 info->index,
                 info->name,
                 info->client,
                 info->source);
#endif

        is_new = FALSE;
        stream = g_hash_table_lookup (control->priv->source_outputs,
                                      GUINT_TO_POINTER (info->index));
        if (stream == NULL) {
                GvcChannelMap *map;
                map = gvc_channel_map_new_from_pa_channel_map (&info->channel_map);
                stream = gvc_mixer_source_output_new (control->priv->pa_context,
                                                      info->index,
                                                      map);
                g_object_unref (map);
                is_new = TRUE;
        }

        name = (const char *)g_hash_table_lookup (control->priv->clients,
                                                  GUINT_TO_POINTER (info->client));

        gvc_mixer_stream_set_name (stream, name);
        gvc_mixer_stream_set_description (stream, info->name);
        set_icon_name_from_proplist (stream, info->proplist, "applications-multimedia");

        if (is_new) {
                g_hash_table_insert (control->priv->source_outputs,
                                     GUINT_TO_POINTER (info->index),
                                     g_object_ref (stream));
                add_stream (control, stream);
        }
}

static void
update_client (GvcMixerControl      *control,
               const pa_client_info *info)
{
#if 1
        g_debug ("Updating client: index=%u name='%s'",
                 info->index,
                 info->name);
#endif
        g_hash_table_insert (control->priv->clients,
                             GUINT_TO_POINTER (info->index),
                             g_strdup (info->name));
}

static void
_pa_context_get_sink_info_cb (pa_context         *context,
                              const pa_sink_info *i,
                              int                 eol,
                              void               *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (eol < 0) {
                if (pa_context_errno (context) == PA_ERR_NOENTITY) {
                        return;
                }

                g_warning ("Sink callback failure");
                return;
        }

        if (eol > 0) {
                dec_outstanding (control);
                return;
        }

        update_sink (control, i);
}


static void
_pa_context_get_source_info_cb (pa_context           *context,
                                const pa_source_info *i,
                                int                   eol,
                                void                 *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (eol < 0) {
                if (pa_context_errno (context) == PA_ERR_NOENTITY) {
                        return;
                }

                g_warning ("Source callback failure");
                return;
        }

        if (eol > 0) {
                dec_outstanding (control);
                return;
        }

        update_source (control, i);
}

static void
_pa_context_get_sink_input_info_cb (pa_context               *context,
                                    const pa_sink_input_info *i,
                                    int                       eol,
                                    void                     *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (eol < 0) {
                if (pa_context_errno (context) == PA_ERR_NOENTITY) {
                        return;
                }

                g_warning ("Sink input callback failure");
                return;
        }

        if (eol > 0) {
                dec_outstanding (control);
                return;
        }

        update_sink_input (control, i);
}

static void
_pa_context_get_source_output_info_cb (pa_context                  *context,
                                       const pa_source_output_info *i,
                                       int                          eol,
                                       void                        *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (eol < 0) {
                if (pa_context_errno (context) == PA_ERR_NOENTITY) {
                        return;
                }

                g_warning ("Source output callback failure");
                return;
        }

        if (eol > 0)  {
                dec_outstanding (control);
                return;
        }

        update_source_output (control, i);
}

static void
_pa_context_get_client_info_cb (pa_context           *context,
                                const pa_client_info *i,
                                int                   eol,
                                void                 *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (eol < 0) {
                if (pa_context_errno (context) == PA_ERR_NOENTITY) {
                        return;
                }

                g_warning ("Client callback failure");
                return;
        }

        if (eol > 0) {
                dec_outstanding (control);
                return;
        }

        update_client (control, i);
}

static void
_pa_context_get_server_info_cb (pa_context           *context,
                                const pa_server_info *i,
                                void                 *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (i == NULL) {
                g_warning ("Server info callback failure");
                return;
        }

        update_server (control, i);
        dec_outstanding (control);
}

static void
remove_event_role_stream (GvcMixerControl *control)
{
        g_debug ("Removing event role");
}

static void
update_event_role_stream (GvcMixerControl                  *control,
                          const pa_ext_stream_restore_info *info)
{
        GvcMixerStream *stream;
        gboolean        is_new;
        pa_volume_t     max_volume;

        if (strcmp (info->name, "sink-input-by-media-role:event") != 0) {
                return;
        }

#if 0
        g_debug ("Updating event role: name='%s' device='%s'",
                 info->name,
                 info->device);
#endif

        is_new = FALSE;

        if (!control->priv->event_sink_input_is_set) {
                stream = gvc_mixer_event_role_new (control->priv->pa_context,
                                                   info->device);
                control->priv->event_sink_input_id = gvc_mixer_stream_get_id (stream);
                control->priv->event_sink_input_is_set = TRUE;

                is_new = TRUE;
        } else {
                stream = g_hash_table_lookup (control->priv->all_streams,
                                              GUINT_TO_POINTER (control->priv->event_sink_input_id));
        }

        max_volume = pa_cvolume_max (&info->volume);

        gvc_mixer_stream_set_name (stream, _("System Sounds"));
        gvc_mixer_stream_set_icon_name (stream, "multimedia-volume-control");
        gvc_mixer_stream_set_volume (stream, (guint)max_volume);
        gvc_mixer_stream_set_is_muted (stream, info->mute);

        if (is_new) {
                add_stream (control, stream);
        }
}

static void
_pa_ext_stream_restore_read_cb (pa_context                       *context,
                                const pa_ext_stream_restore_info *i,
                                int                               eol,
                                void                             *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        if (eol < 0) {
                g_debug ("Failed to initialized stream_restore extension: %s",
                         pa_strerror (pa_context_errno (context)));
                remove_event_role_stream (control);
                return;
        }

        if (eol > 0) {
                dec_outstanding (control);
                return;
        }

        update_event_role_stream (control, i);
}

static void
_pa_ext_stream_restore_subscribe_cb (pa_context *context,
                                     void       *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);
        pa_operation    *o;

        o = pa_ext_stream_restore_read (context,
                                        _pa_ext_stream_restore_read_cb,
                                        control);
        if (o == NULL) {
                g_warning ("pa_ext_stream_restore_read() failed");
                return;
        }

        pa_operation_unref (o);
}

static void
req_update_server_info (GvcMixerControl *control,
                        int              index)
{
        pa_operation *o;

        o = pa_context_get_server_info (control->priv->pa_context,
                                        _pa_context_get_server_info_cb,
                                        control);
        if (o == NULL) {
                g_warning ("pa_context_get_server_info() failed");
                return;
        }
        pa_operation_unref (o);
}

static void
req_update_client_info (GvcMixerControl *control,
                        int              index)
{
        pa_operation *o;

        if (index < 0) {
                o = pa_context_get_client_info_list (control->priv->pa_context,
                                                     _pa_context_get_client_info_cb,
                                                     control);
        } else {
                o = pa_context_get_client_info (control->priv->pa_context,
                                                index,
                                                _pa_context_get_client_info_cb,
                                                control);
        }

        if (o == NULL) {
                g_warning ("pa_context_client_info_list() failed");
                return;
        }
        pa_operation_unref (o);
}

static void
req_update_sink_info (GvcMixerControl *control,
                      int              index)
{
        pa_operation *o;

        if (index < 0) {
                o = pa_context_get_sink_info_list (control->priv->pa_context,
                                                   _pa_context_get_sink_info_cb,
                                                   control);
        } else {
                o = pa_context_get_sink_info_by_index (control->priv->pa_context,
                                                       index,
                                                       _pa_context_get_sink_info_cb,
                                                       control);
        }

        if (o == NULL) {
                g_warning ("pa_context_get_sink_info_list() failed");
                return;
        }
        pa_operation_unref (o);
}

static void
req_update_source_info (GvcMixerControl *control,
                        int              index)
{
        pa_operation *o;

        if (index < 0) {
                o = pa_context_get_source_info_list (control->priv->pa_context,
                                                     _pa_context_get_source_info_cb,
                                                     control);
        } else {
                o = pa_context_get_source_info_by_index(control->priv->pa_context,
                                                        index,
                                                        _pa_context_get_source_info_cb,
                                                        control);
        }

        if (o == NULL) {
                g_warning ("pa_context_get_source_info_list() failed");
                return;
        }
        pa_operation_unref (o);
}

static void
req_update_sink_input_info (GvcMixerControl *control,
                            int              index)
{
        pa_operation *o;

        if (index < 0) {
                o = pa_context_get_sink_input_info_list (control->priv->pa_context,
                                                         _pa_context_get_sink_input_info_cb,
                                                         control);
        } else {
                o = pa_context_get_sink_input_info (control->priv->pa_context,
                                                    index,
                                                    _pa_context_get_sink_input_info_cb,
                                                    control);
        }

        if (o == NULL) {
                g_warning ("pa_context_get_sink_input_info_list() failed");
                return;
        }
        pa_operation_unref (o);
}

static void
req_update_source_output_info (GvcMixerControl *control,
                               int              index)
{
        pa_operation *o;

        if (index < 0) {
                o = pa_context_get_source_output_info_list (control->priv->pa_context,
                                                            _pa_context_get_source_output_info_cb,
                                                            control);
        } else {
                o = pa_context_get_source_output_info (control->priv->pa_context,
                                                       index,
                                                       _pa_context_get_source_output_info_cb,
                                                       control);
        }

        if (o == NULL) {
                g_warning ("pa_context_get_source_output_info_list() failed");
                return;
        }
        pa_operation_unref (o);
}

static void
remove_client (GvcMixerControl *control,
               guint            index)
{
        g_hash_table_remove (control->priv->clients,
                             GUINT_TO_POINTER (index));
}

static void
remove_sink (GvcMixerControl *control,
             guint            index)
{
        GvcMixerStream *stream;

#if 0
        g_debug ("Removing sink: index=%u", index);
#endif

        stream = g_hash_table_lookup (control->priv->sinks,
                                      GUINT_TO_POINTER (index));
        if (stream == NULL) {
                return;
        }
        g_hash_table_remove (control->priv->sinks,
                             GUINT_TO_POINTER (index));

        remove_stream (control, stream);
}

static void
remove_source (GvcMixerControl *control,
               guint            index)
{
        GvcMixerStream *stream;

#if 0
        g_debug ("Removing source: index=%u", index);
#endif

        stream = g_hash_table_lookup (control->priv->sources,
                                      GUINT_TO_POINTER (index));
        if (stream == NULL) {
                return;
        }
        g_hash_table_remove (control->priv->sources,
                             GUINT_TO_POINTER (index));

        remove_stream (control, stream);
}

static void
remove_sink_input (GvcMixerControl *control,
                   guint            index)
{
        GvcMixerStream *stream;

#if 0
        g_debug ("Removing sink input: index=%u", index);
#endif
        stream = g_hash_table_lookup (control->priv->sink_inputs,
                                      GUINT_TO_POINTER (index));
        if (stream == NULL) {
                return;
        }
        g_hash_table_remove (control->priv->sink_inputs,
                             GUINT_TO_POINTER (index));

        remove_stream (control, stream);
}

static void
remove_source_output (GvcMixerControl *control,
                      guint            index)
{
        GvcMixerStream *stream;

#if 0
        g_debug ("Removing source output: index=%u", index);
#endif

        stream = g_hash_table_lookup (control->priv->source_outputs,
                                      GUINT_TO_POINTER (index));
        if (stream == NULL) {
                return;
        }
        g_hash_table_remove (control->priv->source_outputs,
                             GUINT_TO_POINTER (index));

        remove_stream (control, stream);
}

static void
_pa_context_subscribe_cb (pa_context                  *context,
                          pa_subscription_event_type_t t,
                          uint32_t                     index,
                          void                        *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
        case PA_SUBSCRIPTION_EVENT_SINK:
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                        remove_sink (control, index);
                } else {
                        req_update_sink_info (control, index);
                }
                break;

        case PA_SUBSCRIPTION_EVENT_SOURCE:
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                        remove_source (control, index);
                } else {
                        req_update_source_info (control, index);
                }
                break;

        case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                        remove_sink_input (control, index);
                } else {
                        req_update_sink_input_info (control, index);
                }
                break;

        case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                        remove_source_output (control, index);
                } else {
                        req_update_source_output_info (control, index);
                }
                break;

        case PA_SUBSCRIPTION_EVENT_CLIENT:
                if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
                        remove_client (control, index);
                } else {
                        req_update_client_info (control, index);
                }
                break;

        case PA_SUBSCRIPTION_EVENT_SERVER:
                req_update_server_info (control, index);
                break;
        }
}

static void
gvc_mixer_control_ready (GvcMixerControl *control)
{
        pa_operation *o;

        pa_context_set_subscribe_callback (control->priv->pa_context,
                                           _pa_context_subscribe_cb,
                                           control);
        o = pa_context_subscribe (control->priv->pa_context,
                                  (pa_subscription_mask_t)
                                  (PA_SUBSCRIPTION_MASK_SINK|
                                   PA_SUBSCRIPTION_MASK_SOURCE|
                                   PA_SUBSCRIPTION_MASK_SINK_INPUT|
                                   PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT|
                                   PA_SUBSCRIPTION_MASK_CLIENT|
                                   PA_SUBSCRIPTION_MASK_SERVER),
                                  NULL,
                                  NULL);

        if (o == NULL) {
                g_warning ("pa_context_subscribe() failed");
                return;
        }
        pa_operation_unref (o);

        req_update_server_info (control, -1);
        req_update_client_info (control, -1);
        req_update_sink_info (control, -1);
        req_update_source_info (control, -1);
        req_update_sink_input_info (control, -1);
        req_update_source_output_info (control, -1);

        control->priv->n_outstanding = 6;

        /* This call is not always supported */
        o = pa_ext_stream_restore_read (control->priv->pa_context,
                                        _pa_ext_stream_restore_read_cb,
                                        control);
        if (o != NULL) {
                pa_operation_unref (o);
                control->priv->n_outstanding++;

                pa_ext_stream_restore_set_subscribe_cb (control->priv->pa_context,
                                                        _pa_ext_stream_restore_subscribe_cb,
                                                        control);


                o = pa_ext_stream_restore_subscribe (control->priv->pa_context,
                                                     1,
                                                     NULL,
                                                     NULL);
                if (o != NULL) {
                        pa_operation_unref (o);
                }

        } else {
                g_debug ("Failed to initialized stream_restore extension: %s",
                         pa_strerror (pa_context_errno (control->priv->pa_context)));
        }
}

static void
_pa_context_state_cb (pa_context *context,
                      void       *userdata)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (userdata);

        switch (pa_context_get_state (context)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
                break;

        case PA_CONTEXT_READY:
                gvc_mixer_control_ready (control);
                break;

        case PA_CONTEXT_FAILED:
                g_warning ("Connection failed");
                break;

        case PA_CONTEXT_TERMINATED:
        default:
                /* FIXME: */
                break;
        }
}

gboolean
gvc_mixer_control_open (GvcMixerControl *control)
{
        int res;

        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), FALSE);
        g_return_val_if_fail (control->priv->pa_context != NULL, FALSE);
        g_return_val_if_fail (pa_context_get_state (control->priv->pa_context) == PA_CONTEXT_UNCONNECTED, FALSE);

        pa_context_set_state_callback (control->priv->pa_context,
                                       _pa_context_state_cb,
                                       control);

        res = pa_context_connect (control->priv->pa_context, NULL, (pa_context_flags_t) 0, NULL);
        if (res < 0) {
                g_warning ("Failed to connect context: %s",
                           pa_strerror (pa_context_errno (control->priv->pa_context)));
        }

        return res;
}

gboolean
gvc_mixer_control_close (GvcMixerControl *control)
{
        g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), FALSE);
        g_return_val_if_fail (control->priv->pa_context != NULL, FALSE);

        pa_context_disconnect (control->priv->pa_context);
        return TRUE;
}

static void
gvc_mixer_control_dispose (GObject *object)
{
        GvcMixerControl *control = GVC_MIXER_CONTROL (object);

        if (control->priv->pa_context != NULL) {
                pa_context_unref (control->priv->pa_context);
                control->priv->pa_context = NULL;
        }

        if (control->priv->default_source_name != NULL) {
                g_free (control->priv->default_source_name);
                control->priv->default_source_name = NULL;
        }
        if (control->priv->default_sink_name != NULL) {
                g_free (control->priv->default_sink_name);
                control->priv->default_sink_name = NULL;
        }

        if (control->priv->pa_mainloop != NULL) {
                pa_glib_mainloop_free (control->priv->pa_mainloop);
                control->priv->pa_mainloop = NULL;
        }

        if (control->priv->all_streams != NULL) {
                g_hash_table_destroy (control->priv->all_streams);
                control->priv->all_streams = NULL;
        }

        if (control->priv->sinks != NULL) {
                g_hash_table_destroy (control->priv->sinks);
                control->priv->sinks = NULL;
        }
        if (control->priv->sources != NULL) {
                g_hash_table_destroy (control->priv->sources);
                control->priv->sources = NULL;
        }
        if (control->priv->sink_inputs != NULL) {
                g_hash_table_destroy (control->priv->sink_inputs);
                control->priv->sink_inputs = NULL;
        }
        if (control->priv->source_outputs != NULL) {
                g_hash_table_destroy (control->priv->source_outputs);
                control->priv->source_outputs = NULL;
        }
        if (control->priv->clients != NULL) {
                g_hash_table_destroy (control->priv->clients);
                control->priv->clients = NULL;
        }

        G_OBJECT_CLASS (gvc_mixer_control_parent_class)->dispose (object);
}

static GObject *
gvc_mixer_control_constructor (GType                  type,
                               guint                  n_construct_properties,
                               GObjectConstructParam *construct_params)
{
        GObject         *object;
        GvcMixerControl *self;
        pa_proplist     *proplist;

        object = G_OBJECT_CLASS (gvc_mixer_control_parent_class)->constructor (type, n_construct_properties, construct_params);

        self = GVC_MIXER_CONTROL (object);

        /* FIXME: read these from an object property */
        proplist = pa_proplist_new ();
        pa_proplist_sets (proplist,
                          PA_PROP_APPLICATION_NAME,
                          _("GNOME Volume Control"));
        pa_proplist_sets (proplist,
                          PA_PROP_APPLICATION_ID,
                          "org.gnome.VolumeControl");
        pa_proplist_sets (proplist,
                          PA_PROP_APPLICATION_ICON_NAME,
                          "multimedia-volume-control");
        pa_proplist_sets (proplist,
                          PA_PROP_APPLICATION_VERSION,
                          PACKAGE_VERSION);

        self->priv->pa_context = pa_context_new_with_proplist (self->priv->pa_api, NULL, proplist);
        g_assert (self->priv->pa_context);
        pa_proplist_free (proplist);

        return object;
}

static void
gvc_mixer_control_class_init (GvcMixerControlClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->constructor = gvc_mixer_control_constructor;
        object_class->dispose = gvc_mixer_control_dispose;
        object_class->finalize = gvc_mixer_control_finalize;

        signals [READY] =
                g_signal_new ("ready",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GvcMixerControlClass, ready),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE, 0);
        signals [STREAM_ADDED] =
                g_signal_new ("stream-added",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GvcMixerControlClass, stream_added),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__UINT,
                              G_TYPE_NONE, 1, G_TYPE_UINT);
        signals [STREAM_REMOVED] =
                g_signal_new ("stream-removed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GvcMixerControlClass, stream_removed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__UINT,
                              G_TYPE_NONE, 1, G_TYPE_UINT);
        signals [DEFAULT_SINK_CHANGED] =
                g_signal_new ("default-sink-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GvcMixerControlClass, default_sink_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__UINT,
                              G_TYPE_NONE, 1, G_TYPE_UINT);
        signals [DEFAULT_SOURCE_CHANGED] =
                g_signal_new ("default-source-changed",
                              G_TYPE_FROM_CLASS (klass),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GvcMixerControlClass, default_source_changed),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__UINT,
                              G_TYPE_NONE, 1, G_TYPE_UINT);

        g_type_class_add_private (klass, sizeof (GvcMixerControlPrivate));
}

static void
gvc_mixer_control_init (GvcMixerControl *control)
{
        control->priv = GVC_MIXER_CONTROL_GET_PRIVATE (control);

        control->priv->pa_mainloop = pa_glib_mainloop_new (g_main_context_default ());
        g_assert (control->priv->pa_mainloop);

        control->priv->pa_api = pa_glib_mainloop_get_api (control->priv->pa_mainloop);
        g_assert (control->priv->pa_api);

        control->priv->all_streams = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
        control->priv->sinks = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
        control->priv->sources = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
        control->priv->sink_inputs = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);
        control->priv->source_outputs = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_object_unref);

        control->priv->clients = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_free);
}

static void
gvc_mixer_control_finalize (GObject *object)
{
        GvcMixerControl *mixer_control;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GVC_IS_MIXER_CONTROL (object));

        mixer_control = GVC_MIXER_CONTROL (object);

        g_return_if_fail (mixer_control->priv != NULL);
        G_OBJECT_CLASS (gvc_mixer_control_parent_class)->finalize (object);
}

GvcMixerControl *
gvc_mixer_control_new (void)
{
        GObject *control;
        control = g_object_new (GVC_TYPE_MIXER_CONTROL, NULL);
        return GVC_MIXER_CONTROL (control);
}