/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * Copyright © 2001 Jonathan Blandford <jrb@gnome.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Red Hat not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  Red Hat makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * Authors:  Jonathan Blandford
 */

#include <gtk/gtk.h>

#include "gsd-locate-pointer.h"

#define LARGE_SIZE 101
#define SMALL_SIZE 51

typedef enum {
  STAGE_ONE,
  STAGE_TWO,
  STAGE_THREE,
  STAGE_FOUR,
  STAGE_DONE
} LocatePointerStage;

static LocatePointerStage stage;
static GdkWindow *window = NULL;
static gint cursor_x, cursor_y;
static guint locate_pointer_id = 0;

static gint
locate_pointer_expose (GtkWidget      *widget,
                       GdkEventExpose *event,
                       gpointer        data)
{
  gint size;
  GdkPoint points[4];

  if (event->window != window)
    return FALSE;

  switch (stage)
    {
    case STAGE_ONE:
    case STAGE_TWO:
      size = LARGE_SIZE;
      break;
    case STAGE_THREE:
    case STAGE_FOUR:
      size = SMALL_SIZE;
      break;
    default:
      return FALSE;
    }

  gdk_draw_rectangle (event->window,
                      widget->style->black_gc,
                      TRUE,
                      0, 0, size, size);
  switch (stage)
    {
    case STAGE_ONE:
    case STAGE_THREE:
      gdk_draw_rectangle (event->window,
                          widget->style->white_gc,
                          FALSE,
                          1, 1, size - 3, size - 3);
      break;
    case STAGE_TWO:
    case STAGE_FOUR:
      points[0].x = size/2;
      points[0].y = 0 + 1;
      points[1].x = size - 2;
      points[1].y = size/2;
      points[2].x = size/2;
      points[2].y = size - 2;
      points[3].x = 0 + 1;
      points[3].y = size/2;
      gdk_draw_polygon (event->window,
                        widget->style->white_gc,
                        FALSE, points, 4);
      break;
    default:
      g_assert_not_reached ();
    }

  return TRUE;
}

static void
setup_window (void)
{
  gint size;
  GdkBitmap *mask;
  GdkGC *gc;
  GdkColor col;
  GdkPoint points[4];

  gdk_window_hide (window);
  switch (stage)
    {
    case STAGE_ONE:
    case STAGE_TWO:
      size = LARGE_SIZE;
      break;
    case STAGE_THREE:
    case STAGE_FOUR:
      size = SMALL_SIZE;
      break;
    default:
      return;
    }

  gdk_window_move_resize (window,
                          cursor_x - size/2,
                          cursor_y - size/2,
                          size, size);
  mask = gdk_pixmap_new (window, size, size, 1);
  gc = gdk_gc_new (mask);
  switch (stage)
    {
    case STAGE_ONE:
    case STAGE_THREE:
      col.pixel = 1;
      gdk_gc_set_foreground (gc, &col);
      gdk_draw_rectangle (mask, gc, TRUE, 0, 0, size, size);
      col.pixel = 0;
      gdk_gc_set_foreground (gc, &col);
      gdk_draw_rectangle (mask, gc, TRUE, 3, 3, size - 6, size - 6);
      break;
    case STAGE_TWO:
    case STAGE_FOUR:
      col.pixel = 0;
      gdk_gc_set_foreground (gc, &col);
      gdk_draw_rectangle (mask, gc, TRUE, 0, 0, size, size);
      col.pixel = 1;
      gdk_gc_set_foreground (gc, &col);
      points[0].x = size/2;
      points[0].y = 0;
      points[1].x = size - 1;
      points[1].y = size/2;
      points[2].x = size/2;
      points[2].y = size - 1;
      points[3].x = 0;
      points[3].y = size/2;
      gdk_draw_polygon (mask, gc, FALSE, points, 4);
      points[0].x = size/2;
      points[0].y = 0 + 1;
      points[1].x = size - 2;
      points[1].y = size/2;
      points[2].x = size/2;
      points[2].y = size - 2;
      points[3].x = 0 + 1;
      points[3].y = size/2;
      gdk_draw_polygon (mask, gc, FALSE, points, 4);
      points[0].x = size/2;
      points[0].y = 0 + 2;
      points[1].x = size - 3;
      points[1].y = size/2;
      points[2].x = size/2;
      points[2].y = size - 3;
      points[3].x = 0 + 2;
      points[3].y = size/2;
      gdk_draw_polygon (mask, gc, FALSE, points, 4);
      break;
    default:
      g_assert_not_reached ();
    }

  gdk_window_shape_combine_mask (window, mask, 0, 0);
  g_object_unref (G_OBJECT (gc));
  g_object_unref (G_OBJECT (mask));
  gdk_window_show (window);
}

static void
create_window (GdkScreen *screen)
{
  GdkWindowAttr attributes;
  GtkWidget *invisible;

  invisible = gtk_invisible_new_for_screen (screen);

  attributes.window_type = GDK_WINDOW_TEMP;
  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.visual = gtk_widget_get_visual (invisible);
  attributes.colormap = gtk_widget_get_colormap (invisible);
  attributes.event_mask = GDK_VISIBILITY_NOTIFY_MASK | GDK_EXPOSURE_MASK;
  attributes.width = 1;
  attributes.height = 1;
  window = gdk_window_new (gdk_screen_get_root_window (screen),
                           &attributes,
                           GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP);
  gdk_window_set_user_data (window, invisible);
  g_signal_connect (G_OBJECT (invisible),
                    "expose_event",
                    (GCallback) locate_pointer_expose,
                    NULL);
}

static gboolean
locate_pointer_timeout (gpointer data)
{
  stage++;
  if (stage == STAGE_DONE)
    {
      gdk_window_hide (window);
      locate_pointer_id = 0;
      return FALSE;
    }
  setup_window ();
  return TRUE;
}

void
gsd_locate_pointer (GdkScreen *screen)
{
  gdk_window_get_pointer (gdk_screen_get_root_window (screen), &cursor_x, &cursor_y, NULL);

  if (locate_pointer_id)
    gtk_timeout_remove (locate_pointer_id);

  /* Create the window if it is not created OR if it is not for the
   * current screen.
   */

  if (window == NULL)
    create_window (screen);
  else if( gdk_screen_get_number (screen) != gdk_screen_get_number (gdk_drawable_get_screen (window)))
    create_window (screen);

  stage = STAGE_ONE;
  setup_window ();
  gdk_window_show (window);
  locate_pointer_id = gtk_timeout_add (100, locate_pointer_timeout, NULL);
}