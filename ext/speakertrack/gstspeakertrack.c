/*
 * GStreamer
 * Copyright (C) 2012,2013 Duzy Chan <code@duzy.info>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-speakertrack
 *
 * Performs face detection on videos and images.
 *
 * The image is scaled down multiple times using the GstSpeakerTrack::scale-factor
 * until the size is &lt;= GstSpeakerTrack::min-size-width or 
 * GstSpeakerTrack::min-size-height. 
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-0.10 autovideosrc ! decodebin2 ! colorspace ! facedetect ! speakertrack ! videoconvert ! xvimagesink
 * ]| Detect and show faces
 * |[
 * gst-launch-0.10 autovideosrc ! video/x-raw,width=320,height=240 ! videoconvert ! colorspace ! speakertrack min-size-width=60 min-size-height=60 ! xvimagesink
 * ]| Detect large faces on a smaller image 
 *
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <glib/gprintf.h>
#include <stdlib.h>

#include "gstspeakertrack.h"

GST_DEBUG_CATEGORY_STATIC (gst_speaker_track_debug);
#define GST_CAT_DEFAULT gst_speaker_track_debug

#define GST_SPEAKER_TRACK_LOCK_FACES(st) (g_mutex_lock(&((st)->faces_lock)))
#define GST_SPEAKER_TRACK_UNLOCK_FACES(st) (g_mutex_unlock(&((st)->faces_lock)))

#if 0
#define FACE_MOTION_SCALE 0.65
#define FACE_MOTION_SHIFT 0.25
#else
#define FACE_MOTION_SCALE 0.65
#define FACE_MOTION_SHIFT 0.25
#endif

#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
};

/*
 * GstSpeakerTrackFlags:
 *
 * Flags parameter to OpenCV's cvHaarDetectObjects function.
 */
typedef enum
{
  GST_SPEAKER_TRACK_HAAR_DO_CANNY_PRUNING = (1 << 0)
} GstSpeakerTrackFlags;

#define GST_TYPE_SPEAKER_TRACK_FLAGS (gst_speaker_track_flags_get_type())

struct _TrackingFace
{
  gint number;
  guint x, y, w, h;
};

/*
static void
gst_speaker_track_register_flags (GType * id)
{
  static const GFlagsValue values[] = {
    {(guint) GST_SPEAKER_TRACK_HAAR_DO_CANNY_PRUNING,
        "Do Canny edge detection to discard some regions", "do-canny-pruning"},
    {0, NULL, NULL}
  };
  *id = g_flags_register_static ("GstSpeakerTrackFlags", values);
}

static GType
gst_speaker_track_flags_get_type (void)
{
  static GType id;
  static GOnce once = G_ONCE_INIT;

  g_once (&once, (GThreadFunc) gst_speaker_track_register_flags, &id);
  return id;
}
*/

/* the capabilities of the inputs and outputs.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

G_DEFINE_TYPE (GstSpeakerTrack, gst_speaker_track, GST_TYPE_BASE_TRANSFORM);

static void gst_speaker_track_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_speaker_track_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

/* initialize the new element
 * initialize instance structure
 */
static void
gst_speaker_track_init (GstSpeakerTrack * track)
{
  track->tracking_face = g_new0 (TrackingFace, 1);
  track->faces = NULL;
  g_mutex_init (&track->faces_lock);
}

static void
gst_speaker_track_finalize (GObject * obj)
{
  GstSpeakerTrack *track = GST_SPEAKER_TRACK (obj);

  g_list_free_full (track->faces, (GDestroyNotify) gst_structure_free);
  g_free (track->tracking_face);
  track->tracking_face = NULL;

  g_mutex_clear (&track->faces_lock);

  G_OBJECT_CLASS (gst_speaker_track_parent_class)->finalize (obj);
}

static void
gst_speaker_track_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpeakerTrack *filter = GST_SPEAKER_TRACK (object);

  (void) filter;

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_speaker_track_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSpeakerTrack *filter = GST_SPEAKER_TRACK (object);

  (void) filter;

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_speaker_track_get_face_rect (GstStructure * face,
    guint * x, guint * y, guint * w, guint * h, const gchar * prefix)
{
  gchar name[24];

  g_sprintf (name, "%sx", prefix);
  if (gst_structure_has_field (face, name)) {
    gst_structure_get_uint (face, name, x);

    g_sprintf (name, "%sy", prefix);
    gst_structure_get_uint (face, name, y);

    g_sprintf (name, "%swidth", prefix);
    gst_structure_get_uint (face, name, w);

    g_sprintf (name, "%sheight", prefix);
    gst_structure_get_uint (face, name, h);
    return TRUE;
  }

  return FALSE;
}

/*
static GstMessage *
gst_speaker_track_message_new (GstSpeakerTrack * filter, GstBuffer * buf)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM_CAST (filter);
  GstStructure *s;
  GstClockTime running_time, stream_time;

  running_time = gst_segment_to_running_time (&trans->segment, GST_FORMAT_TIME,
      GST_BUFFER_TIMESTAMP (buf));
  stream_time = gst_segment_to_stream_time (&trans->segment, GST_FORMAT_TIME,
      GST_BUFFER_TIMESTAMP (buf));

  s = gst_structure_new ("speakertrack",
      "timestamp", G_TYPE_UINT64, GST_BUFFER_TIMESTAMP (buf),
      "stream-time", G_TYPE_UINT64, stream_time,
      "running-time", G_TYPE_UINT64, running_time,
      "duration", G_TYPE_UINT64, GST_BUFFER_DURATION (buf), NULL);

  return gst_message_new_element (GST_OBJECT (filter), s);
}

static gboolean
gst_speaker_track_lock (GstSpeakerTrack * st, IplImage * img, GstBuffer * buf,
    GstStructure * face, gint i)
{
  GstMessage *msg = gst_face_detect_message_new (st, buf);
  GValue face_value = { 0 };

  if (st->focus_face) {
    guint dx, dy;
    CvRect r1, r2;
    gst_structure_get_uint (st->focus_face, "x", (guint *) & r1.x);
    gst_structure_get_uint (st->focus_face, "y", (guint *) & r1.y);
    gst_structure_get_uint (st->focus_face, "width", (guint *) & r1.width);
    gst_structure_get_uint (st->focus_face, "height", (guint *) & r1.height);
    gst_structure_get_uint (face, "x", (guint *) & r2.x);
    gst_structure_get_uint (face, "y", (guint *) & r2.y);
    gst_structure_get_uint (face, "width", (guint *) & r2.width);
    gst_structure_get_uint (face, "height", (guint *) & r2.height);

    dx = r1.width / 2;
    dy = r1.height / 2;
    if (dx <= abs (r2.x - r1.x)
        || dy <= abs (r2.y - r1.y)
        || dx <= abs (r2.width - r1.width)
        || dy <= abs (r2.height - r1.height)) {
      //if (st->lock_stamp <= 6000)
      return FALSE;
    }

    gst_structure_free (st->focus_face);
  }

  st->focus_face = gst_structure_copy (face);
  st->lock_stamp = gst_util_get_timestamp ();

  g_value_init (&face_value, GST_TYPE_STRUCTURE);
  g_value_take_boxed (&face_value, face);
  gst_structure_set_value ((GstStructure *) gst_message_get_structure (msg),
      "face", &face_value);
  g_value_unset (&face_value);

  gst_element_post_message (GST_ELEMENT (st), msg);
  return TRUE;
}
*/

/*
static gboolean
gst_speaker_track_compare_face (GstSpeakerTrack * filter,
    GstStructure * face1, GstStructure * face2)
{
  guint face1_x, face1_y, face1_w, face1_h;
  guint face2_x, face2_y, face2_w, face2_h;
  guint face_x, face_y, face_w, face_h;
  float a = FACE_MOTION_SCALE, ix, iy, iw, ih;
  float off = FACE_MOTION_SHIFT;

  gst_speaker_track_get_face_rect (face1,
      &face1_x, &face1_y, &face1_w, &face1_h, "");
  gst_speaker_track_get_face_rect (face2,
      &face2_x, &face2_y, &face2_w, &face2_h, "");

#if 0
  face_x = max (face1_x, face2_x);
  face_y = max (face1_y, face2_y);
  face_w = min (face1_x + face1_w, face2_x + face2_w) - face1_x;
  face_h = min (face1_y + face1_h, face2_y + face2_h) - face2_y;
  if (face_w < 0 || face_h < 0) {
    face_w = 0, face_h = 0;
  }

  (void) face_x, (void) face_y;

  ix = (float) abs (face2_x - face1_x) / (float) face1_w;
  iy = (float) abs (face2_y - face1_y) / (float) face1_h;
  iw = (float) face_w / (float) face1_w;
  ih = (float) face_h / (float) face1_h;
  if (ix <= off && iy <= off && a <= iw && a <= ih) {
    return TRUE;
  }
#else
  ix = max (face1_x, face2_x);
  iy = max (face1_y, face2_y);
  iw = min (face1_x + face1_w, face2_x + face2_w) - ix;
  ih = min (face1_y + face1_h, face2_y + face2_h) - iy;
  g_printf ("intersect: [(%f, %f), %fx%f]\n", ix, iy, iw, ih);

  if (iw <= 0 || ih <= 0) {
    return FALSE;
  }

  (void) face_x;
  (void) face_y;
  (void) face_w;
  (void) face_h;
  (void) off;
  (void) a;
#endif

  return TRUE;
}
*/

static gboolean
gst_speaker_track_select_face (GstSpeakerTrack * track, gint x, gint y)
{
  gint selected = 0;
  GList *face;

  GST_SPEAKER_TRACK_LOCK_FACES (track);

  //g_print ("select: (%d, %d)\n", x, y);

  if (!track->faces) {
    g_print ("select: no faces, (%d, %d)\n", x, y);
    return FALSE;
  }

  for (face = track->faces; face; face = g_list_next (face)) {
    guint fx, fy, fw, fh;
    if (gst_speaker_track_get_face_rect (GST_STRUCTURE (face->data),
            &fx, &fy, &fw, &fh, "")) {
      if (fx <= x && fy <= y && x <= (fx + fw) && y <= (fy + fh)) {
        track->tracking_face->x = fx;
        track->tracking_face->y = fy;
        track->tracking_face->w = fw;
        track->tracking_face->h = fh;
        g_print ("selected: [(%d, %d), %d, %d]\n", fx, fy, fw, fh);
        ++selected;
      }
    }
  }

  GST_SPEAKER_TRACK_UNLOCK_FACES (track);
  return 0 < selected;
}

/*
static void
gst_speaker_track_update_face (GstSpeakerTrack * track,
    const GstStructure * s)
{
  GstStructure *face = gst_structure_copy (s);
  GList *item = NULL;
  guint x, y, w, h;

  gst_speaker_track_get_face_rect (face, &x, &y, &w, &h, "");

  g_print ("update: face[(%d, %d), %d, %d] (of %d faces)\n",
      g_list_length (track->faces), x, y, w, h);

  for (item = track->faces; item; item = g_list_next (item)) {
    GstStructure *f = GST_STRUCTURE (item->data);
    if (gst_speaker_track_compare_face (track, f, face)) {
      gst_structure_free (f);
      item->data = face;
      goto tracking_face;
    }
  }

  track->faces = g_list_append (track->faces, face);

tracking_face:
  if (track->tracking_face) {
    for (item = track->faces; item; item = g_list_next (item)) {
      GstStructure *f = GST_STRUCTURE (item->data);
      if (gst_speaker_track_compare_face (track, track->tracking_face, f)) {
        gst_structure_free (track->tracking_face);
        track->tracking_face = gst_structure_copy (f);
      }
    }
  }
}
*/

static void
gst_speaker_track_report_tracking (GstSpeakerTrack * track, GstStructure * face)
{
  GstMessage *msg = NULL;
  GstStructure *s = NULL;
  GstIterator *iter = NULL;
  gboolean done = FALSE;
  GValue item = { 0 };

  //g_print ("track: %d, %d, %d, %d\n", fx, fy, fw, fh);

  s = gst_structure_copy (face);
  gst_structure_set_name (s, "facetrack");
  msg = gst_message_new_element (GST_OBJECT (track), s);
  gst_element_post_message (GST_ELEMENT (track), msg);


  done = FALSE;
  iter = gst_element_iterate_src_pads (GST_ELEMENT (track));
  while (!done) {
    switch (gst_iterator_next (iter, &item)) {
      case GST_ITERATOR_OK:
      {
        GstPad *srcpad = NULL;
        GstEvent *ev = NULL;
        GstPad *pad = NULL;
        s = gst_structure_copy (face);
        gst_structure_set_name (s, "facetrack");
        srcpad = (GstPad *) g_value_get_object (&item);
        ev = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, s);
        pad = gst_pad_get_peer (srcpad);
        if (!gst_pad_send_event (pad, ev)) {
          // GST_WARNING_OBJECT (detect, "face ignored");
        }
        gst_object_unref (pad);
      }
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
        GST_ERROR_OBJECT (track, "Error looping sink pads");
        done = TRUE;
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (iter);
}

static void
gst_speaker_track_update_tracking (GstSpeakerTrack * track)
{
  guint ix, iy, iw, ih;         // intersect rectangle
  guint fx, fy, fw, fh;
  GList *item;

  /*
     g_print ("track: %d, %d, %d, %d\n",
     track->tracking_face->x, track->tracking_face->y,
     track->tracking_face->w, track->tracking_face->h);
   */

  for (item = track->faces; item; item = g_list_next (item)) {
    GstStructure *face = GST_STRUCTURE (item->data);
    gst_structure_get_uint (face, "x", &fx);
    gst_structure_get_uint (face, "y", &fy);
    gst_structure_get_uint (face, "width", &fw);
    gst_structure_get_uint (face, "height", &fh);

    ix = max (track->tracking_face->x, fx);
    iy = max (track->tracking_face->y, fy);
    iw = min (ix + track->tracking_face->w, ix + fw) - ix;
    ih = min (iy + track->tracking_face->h, iy + fh) - iy;

    //g_print ("track: %d, %d, %d, %d\n", ix, iy, iw, ih);

    if (0.1 * fw < iw && 0.1 * fh < ih) {
      track->tracking_face->x = fx;
      track->tracking_face->y = fy;
      track->tracking_face->w = fw;
      track->tracking_face->h = fh;
      //g_print ("track: %d, %d, %d, %d\n", fx, fy, fw, fh);

      gst_speaker_track_report_tracking (track, face);
      break;
    }
  }
}

static void
gst_speaker_track_update_faces (GstSpeakerTrack * track, const GstStructure * s)
{
  const GValue *faces = gst_structure_get_value (s, "faces");
  gsize size = faces ? gst_value_list_get_size (faces) : 0, n;

  GST_SPEAKER_TRACK_LOCK_FACES (track);

  g_list_free_full (track->faces, (GDestroyNotify) gst_structure_free);
  track->faces = NULL;

  for (n = 0; n < size; ++n) {
    const GValue *facevalue = gst_value_list_get_value (faces, n);
    GstStructure *face = (GstStructure *) g_value_get_boxed (facevalue);
    if (GST_IS_STRUCTURE (face)) {
      guint x, y, w, h;
      gst_structure_get_uint (face, "x", &x);
      gst_structure_get_uint (face, "y", &y);
      gst_structure_get_uint (face, "width", &w);
      gst_structure_get_uint (face, "height", &h);
      //g_printf ("face: [%d] [(%d, %d), %dx%d]\n", (gint) n, x, y, w, h);
      track->faces = g_list_append (track->faces, gst_structure_copy (face));
    }
  }

  gst_speaker_track_update_tracking (track);
  GST_SPEAKER_TRACK_UNLOCK_FACES (track);
}

static gboolean
gst_speaker_track_send_event (GstElement * element, GstEvent * event)
{
  GstElementClass *element_class =
      GST_ELEMENT_CLASS (gst_speaker_track_parent_class);
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_DOWNSTREAM:{
      const GstStructure *s = gst_event_get_structure (event);
      if (gst_structure_has_name (s, "select")) {
        gboolean selected = FALSE;
        gint x, y;
        if (gst_structure_get_int (s, "x", &x) &&
            gst_structure_get_int (s, "y", &y)) {
          selected =
              gst_speaker_track_select_face (GST_SPEAKER_TRACK (element), x, y);
        }
        return selected;
      }
    }
      break;

    default:
      break;
  }
  return element_class->send_event (element, event);
}

static gboolean
gst_speaker_track_sink_eventfunc (GstBaseTransform * trans, GstEvent * event)
{
  GstSpeakerTrack *track = GST_SPEAKER_TRACK (trans);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_DOWNSTREAM:{
      const GstStructure *s = gst_event_get_structure (event);
      if (gst_structure_has_name (s, "facedetect")) {
        gst_speaker_track_update_faces (track, s);
        return TRUE;
      }
    }
      break;

    default:
      break;
  }

  return gst_pad_push_event (trans->srcpad, event);
}

static gboolean
gst_speaker_track_src_eventfunc (GstBaseTransform * trans, GstEvent * event)
{
  GstSpeakerTrack *track = GST_SPEAKER_TRACK (trans);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:{
      const GstStructure *s = gst_event_get_structure (event);
      if (gst_structure_has_name (s, "facedetect")) {
        gst_speaker_track_update_faces (track, s);
        return TRUE;
      }
    }
      break;

    default:
      break;
  }

  return gst_pad_push_event (trans->sinkpad, event);
}

/* initialize the speakertrack's class */
static void
gst_speaker_track_class_init (GstSpeakerTrackClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_speaker_track_finalize);
  gobject_class->set_property = gst_speaker_track_set_property;
  gobject_class->get_property = gst_speaker_track_get_property;

  element_class->send_event = GST_DEBUG_FUNCPTR (gst_speaker_track_send_event);

  transform_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_speaker_track_sink_eventfunc);
  transform_class->src_event =
      GST_DEBUG_FUNCPTR (gst_speaker_track_src_eventfunc);

  gst_element_class_set_static_metadata (element_class,
      "speakertrack",
      "Filter/Effect/Video",
      "Tracks speaker on videos, report speaker positions to bus",
      "Duzy Chan <code@duzy.info>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));

  GST_DEBUG_CATEGORY_INIT (gst_speaker_track_debug, "speakertrack",
      0, "Tracking selected face, report tracking to message bus.");
}

#include "gstfacedetect2.h"
#include "gstcamcontrol.h"

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "facedetect2", GST_RANK_NONE,
          GST_TYPE_FACE_DETECT2)) {
    return FALSE;
  }

  if (!gst_element_register (plugin, "speakertrack", GST_RANK_NONE,
          GST_TYPE_SPEAKER_TRACK)) {
    return FALSE;
  }

  if (!gst_element_register (plugin, "camcontrol", GST_RANK_NONE,
          GST_TYPE_CAM_CONTROL)) {
    return FALSE;
  }

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    skeakertrack,
    "GStreamer Speaker Track Plugin",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
