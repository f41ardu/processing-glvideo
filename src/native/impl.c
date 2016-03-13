/*
This implementation is largely based on testegl.c from gst-omx
with modifications: Copyright (c) The Processing Foundation 2016
Developed by Gottfried Haider

Copyright (c) 2012, Broadcom Europe Ltd
Copyright (c) 2012, OtherCrashOverride
Copyright (C) 2013, Fluendo S.A.
   @author: Josep Torra <josep@fluendo.com>
Copyright (C) 2013, Video Experts Group LLC.
   @author: Ilya Smelykh <ilya@videoexpertsgroup.com>
Copyright (C) 2014 Julien Isorce <julien.isorce@collabora.co.uk>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <stdbool.h>
#include <stdlib.h>
#include "impl.h"
#include "iface.h"

#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

typedef enum
{
  GST_PLAY_FLAG_VIDEO = (1 << 0),
  GST_PLAY_FLAG_AUDIO = (1 << 1),
  GST_PLAY_FLAG_TEXT = (1 << 2),
  GST_PLAY_FLAG_VIS = (1 << 3),
  GST_PLAY_FLAG_SOFT_VOLUME = (1 << 4),
  GST_PLAY_FLAG_NATIVE_AUDIO = (1 << 5),
  GST_PLAY_FLAG_NATIVE_VIDEO = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD = (1 << 7),
  GST_PLAY_FLAG_BUFFERING = (1 << 8),
  GST_PLAY_FLAG_DEINTERLACE = (1 << 9),
  GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10)
} GstPlayFlags;

static GThread *thread;
static GMainLoop *mainloop;
static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

static void
handle_buffer (GLVIDEO_STATE_T * state, GstBuffer * buffer)
{
  g_mutex_lock (&state->buffer_lock);
  if (state->next_buffer) {
    gst_buffer_unref (state->next_buffer);
    state->next_buffer = NULL;
  }
  state->next_tex = 0;

  GstMemory *mem = gst_buffer_peek_memory (buffer, 0);

  if (unlikely (!gst_is_gl_memory (mem))) {
    g_printerr ("GLVideo: Not using GPU memory, unsupported\n");
    g_mutex_unlock (&state->buffer_lock);
    return;
  }

  state->next_buffer = gst_buffer_ref (buffer);
  state->next_tex = ((GstGLMemory *) mem)->tex_id;
  g_mutex_unlock (&state->buffer_lock);
}

static void
preroll_cb (GstElement * fakesink, GstBuffer * buffer, GstPad * pad,
    gpointer user_data)
{
  GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *) user_data;
  handle_buffer (state, buffer);
}

static void
buffers_cb (GstElement * fakesink, GstBuffer * buffer, GstPad * pad,
    gpointer user_data)
{
  GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *) user_data;
  handle_buffer (state, buffer);
}

static GstPadProbeReturn
events_cb (GstPad * pad, GstPadProbeInfo * probe_info, gpointer user_data)
{
  GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *) user_data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (probe_info);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      if (state->caps) {
        gst_caps_unref (state->caps);
        state->caps = NULL;
      }
      gst_event_parse_caps (event, &state->caps);
      if (state->caps)
        gst_caps_ref (state->caps);
      break;
    }
    // this is handled in eos_cb
    //case GST_EVENT_EOS:
    //  break;
    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
query_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *) user_data;
  GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
    {
      if (gst_gl_handle_context_query (state->pipeline, query,
              (GstGLDisplay **) & state->gst_display,
              (GstGLContext **) & state->gl_context))
        return GST_PAD_PROBE_HANDLED;
      break;
    }
    default:
      break;
  }

  return GST_PAD_PROBE_OK;
}

static GstBusSyncReply
bus_sync_handler (GstBus * bus, GstMessage * message, GstPipeline * data)
{
  return GST_BUS_PASS;
}

static void
error_cb (GstBus * bus, GstMessage * msg, GLVIDEO_STATE_T * state)
{
  GError *err;
  gchar *debug_info;

  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("GLVideo: %s: %s\n",
    GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);
}

static void
buffering_cb (GstBus * bus, GstMessage * msg, GLVIDEO_STATE_T * state)
{
  gint percent;

  gst_message_parse_buffering (msg, &percent);
  if (percent < 100) {
    gst_element_set_state (state->pipeline, GST_STATE_PAUSED);
    state->buffering = true;
  } else {
    gst_element_set_state (state->pipeline, GST_STATE_PLAYING);
    state->buffering = false;
  }
}

static void
eos_cb (GstBus * bus, GstMessage * msg, GLVIDEO_STATE_T * state)
{
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (state->pipeline)) {
    if (state->looping) {
      GstEvent *event;
      event = gst_event_new_seek (state->rate,
        GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
        GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE);
      if (!gst_element_send_event (state->vsink, event)) {
        g_printerr ("GLVideo: Error rewinding video\n");
      }
    } else {
      gst_element_set_state (state->pipeline, GST_STATE_PAUSED);
    }
  }
}

static gboolean
init_playbin_player (GLVIDEO_STATE_T * state, const gchar * uri)
{
  GstPad *pad = NULL;
  GstPad *ghostpad = NULL;
  GstElement *vbin = gst_bin_new ("vbin");

  /* insert a gl filter so that the GstGLBufferPool
   * is managed automatically */
  GstElement *glfilter = gst_element_factory_make ("glupload", "glfilter");
  GstElement *capsfilter = gst_element_factory_make ("capsfilter", NULL);
  GstElement *vsink = gst_element_factory_make ("fakesink", "vsink");

  g_object_set (capsfilter, "caps",
      gst_caps_from_string ("video/x-raw(memory:GLMemory)"), NULL);
  g_object_set (vsink, "sync", TRUE, "silent", TRUE, "qos", TRUE,
      "enable-last-sample", FALSE, "max-lateness", 20 * GST_MSECOND,
      "signal-handoffs", TRUE, NULL);

  g_signal_connect (vsink, "preroll-handoff", G_CALLBACK (preroll_cb), state);
  g_signal_connect (vsink, "handoff", G_CALLBACK (buffers_cb), state);

  gst_bin_add_many (GST_BIN (vbin), glfilter, capsfilter, vsink, NULL);

  pad = gst_element_get_static_pad (glfilter, "sink");
  ghostpad = gst_ghost_pad_new ("sink", pad);
  gst_object_unref (pad);
  gst_element_add_pad (vbin, ghostpad);

  pad = gst_element_get_static_pad (vsink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, events_cb, state,
      NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, query_cb, state,
      NULL);
  gst_object_unref (pad);

  gst_element_link (glfilter, capsfilter);
  gst_element_link (capsfilter, vsink);

  /* Instantiate and configure playbin */
  state->pipeline = gst_element_factory_make ("playbin", "player");
  g_object_set (state->pipeline, "uri", uri,
      "video-sink", vbin, "flags",
      GST_PLAY_FLAG_NATIVE_VIDEO | GST_PLAY_FLAG_AUDIO, NULL);

  state->vsink = gst_object_ref (vsink);
  return TRUE;
}

static void *
glvideo_mainloop (void * data) {
  mainloop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (mainloop);
  return NULL;
}

JNIEXPORT void JNICALL Java_processing_glvideo_GLVideo_gstreamer_1setEnvVar
  (JNIEnv * env, jclass cls, jstring _name, jstring _val) {
    const char *name = (*env)->GetStringUTFChars (env, _name, JNI_FALSE);
    const char *val = (*env)->GetStringUTFChars (env, _val, JNI_FALSE);
    setenv (name, val, 1);
    (*env)->ReleaseStringUTFChars (env, _val, val);
    (*env)->ReleaseStringUTFChars (env, _name, name);
  }

JNIEXPORT jboolean JNICALL Java_processing_glvideo_GLVideo_gstreamer_1init
  (JNIEnv * env, jclass cls) {
    GError *error = NULL;

    // initialize GStreamer
    gst_init_check (NULL, NULL, &error);
    if (error) {
      g_printerr ("Could not initialize library: %s\n", error->message);
      g_error_free (error);
      return JNI_FALSE;
    }

    // save the current EGL context
    display = eglGetCurrentDisplay ();
    surface = eglGetCurrentSurface (0);
    context = eglGetCurrentContext ();
    if (!context) {
      g_printerr ("GLVideo requires the P3D renderer.\n");
      g_error_free (error);
      return JNI_FALSE;
    }
    fprintf (stderr, "GLVideo: display %p, surface %p, context %p at init\n",
      (void *) display, (void *) surface, (void *) context);

    // start GLib main loop in a separate thread
    thread = g_thread_new ("glvideo-mainloop", glvideo_mainloop, NULL);

    // TODO: add note re gpu_mem

    return JNI_TRUE;
  }

JNIEXPORT jlong JNICALL Java_processing_glvideo_GLVideo_gstreamer_1open
  (JNIEnv * env, jobject obj, jstring _fn_or_uri) {
    GLVIDEO_STATE_T *state = malloc (sizeof (GLVIDEO_STATE_T));
    if (!state) {
      return 0L;
    }
    memset (state, 0, sizeof (*state));
    state->rate = 1.0f;

    // setup EGL context sharing
    state->gst_display = gst_gl_display_egl_new_with_egl_display (display);
    state->gl_context =
      gst_gl_context_new_wrapped (GST_GL_DISPLAY (state->gst_display),
      (guintptr) context, GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);

    // setup mutex to protect double buffering scheme
    g_mutex_init (&state->buffer_lock);

    // make sure we have a valid uri
    const char *fn_or_uri = (*env)->GetStringUTFChars (env, _fn_or_uri, JNI_FALSE);
    gchar *uri;
    if (gst_uri_is_valid (fn_or_uri)) {
      uri = g_strdup (fn_or_uri);
    } else {
      uri = gst_filename_to_uri (fn_or_uri, NULL);
    }
    (*env)->ReleaseStringUTFChars (env, _fn_or_uri, fn_or_uri);

    // instantiate pipeline
    if (!init_playbin_player (state, uri)) {
      free (state);
      return 0L;
    }

    // connect the bus handlers
    GstBus *bus = gst_element_get_bus (state->pipeline);

    gst_bus_set_sync_handler (bus, (GstBusSyncHandler) bus_sync_handler, state,
      NULL);
    gst_bus_add_signal_watch_full (bus, G_PRIORITY_HIGH);
    gst_bus_enable_sync_message_emission (bus);

    g_signal_connect (G_OBJECT (bus), "message::error", (GCallback) error_cb,
      state);
    g_signal_connect (G_OBJECT (bus), "message::buffering",
      (GCallback) buffering_cb, state);
    g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback) eos_cb, state);
    gst_object_unref (bus);

    // start paused
    gst_element_set_state (state->pipeline, GST_STATE_PAUSED);

    return (intptr_t) state;
  }

JNIEXPORT jboolean JNICALL Java_processing_glvideo_GLVideo_gstreamer_1isAvailable
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    return (state->next_tex);
  }

JNIEXPORT jint JNICALL Java_processing_glvideo_GLVideo_gstreamer_1getFrame
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    g_mutex_lock (&state->buffer_lock);
    if (likely(state->current_buffer != NULL)) {
      gst_buffer_unref (state->current_buffer);
    }
    state->current_buffer = state->next_buffer;
    state->current_tex = state->next_tex;
    state->next_buffer = NULL;
    state->next_tex = 0;
    g_mutex_unlock (&state->buffer_lock);
    return state->current_tex;
  }

JNIEXPORT void JNICALL Java_processing_glvideo_GLVideo_gstreamer_1startPlayback
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;

    gst_element_set_state (state->pipeline, GST_STATE_PLAYING);
  }

JNIEXPORT jboolean JNICALL Java_processing_glvideo_GLVideo_gstreamer_1isPlaying
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    GstState s;

    gst_element_get_state (state->pipeline, &s, NULL, 0);
    return (s == GST_STATE_PLAYING || (s == GST_STATE_PAUSED && state->buffering));
  }

JNIEXPORT void JNICALL Java_processing_glvideo_GLVideo_gstreamer_1stopPlayback
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;

    gst_element_set_state (state->pipeline, GST_STATE_PAUSED);
  }

JNIEXPORT void JNICALL Java_processing_glvideo_GLVideo_gstreamer_1setLooping
  (JNIEnv * env, jobject obj, jlong handle, jboolean looping) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    state->looping = looping;
  }

JNIEXPORT jboolean JNICALL Java_processing_glvideo_GLVideo_gstreamer_1seek
  (JNIEnv * env, jobject obj, jlong handle, jfloat sec) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    GstEvent *event;

    event = gst_event_new_seek (state->rate,
      GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
      GST_SEEK_TYPE_SET, (gint64)(sec * 1000000000), GST_SEEK_TYPE_SET,
      GST_CLOCK_TIME_NONE);
    return gst_element_send_event (state->vsink, event);
  }

JNIEXPORT jboolean JNICALL Java_processing_glvideo_GLVideo_gstreamer_1setSpeed
  (JNIEnv * env, jobject obj, jlong handle, jfloat rate) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    GstEvent *event;
    gint64 start = 0;
    gint64 stop = 0;

    if (rate == state->rate) {
      return true;
    }
    if (0 < rate) {
      gst_element_query_position (state->vsink, GST_FORMAT_TIME, &start);
      stop = GST_CLOCK_TIME_NONE;
    } else {
      /*
      start = 0;
      gst_element_query_position (state->vsink, GST_FORMAT_TIME, &stop);
      */
      // this currently freezes the application
      return false;
    }
    state->rate = rate;
    event = gst_event_new_seek (state->rate,
      GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
      GST_SEEK_TYPE_SET, start, GST_SEEK_TYPE_SET,
      stop);
    return gst_element_send_event (state->vsink, event);
  }

JNIEXPORT jboolean JNICALL Java_processing_glvideo_GLVideo_gstreamer_1setVolume
  (JNIEnv * env, jobject obj, jlong handle, jfloat vol) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    // TODO: doesn' work
    g_object_set (state->pipeline, "volume", (double)vol, NULL);
    return true;
  }

JNIEXPORT jfloat JNICALL Java_processing_glvideo_GLVideo_gstreamer_1getDuration
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    gint64 duration = 0;

    gst_element_query_duration (state->pipeline, GST_FORMAT_TIME, &duration);
    return duration/1000000000.0f;
  }

JNIEXPORT jfloat JNICALL Java_processing_glvideo_GLVideo_gstreamer_1getPosition
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    gint64 position = 0;

    gst_element_query_position (state->vsink, GST_FORMAT_TIME, &position);
    return position/1000000000.0f;
  }

JNIEXPORT jint JNICALL Java_processing_glvideo_GLVideo_gstreamer_1getWidth
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    const GstStructure *str;
    int width = 0;

    if (!state->caps || !gst_caps_is_fixed (state->caps)) {
      return 0;
    }
    str = gst_caps_get_structure (state->caps, 0);
    gst_structure_get_int (str, "width", &width);
    return width;
  }

JNIEXPORT jint JNICALL Java_processing_glvideo_GLVideo_gstreamer_1getHeight
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    const GstStructure *str;
    int height = 0;

    if (!state->caps || !gst_caps_is_fixed (state->caps)) {
      return 0;
    }
    str = gst_caps_get_structure (state->caps, 0);
    gst_structure_get_int (str, "height", &height);
    return height;
  }

JNIEXPORT jfloat JNICALL Java_processing_glvideo_GLVideo_gstreamer_1getFramerate
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;
    const GstStructure *str;
    double fps = 0.0;

    if (!state->caps || !gst_caps_is_fixed (state->caps)) {
      return 0.0f;
    }
    str = gst_caps_get_structure (state->caps, 0);
    // TODO: doesn't work
    gst_structure_get_double (str, "framerate", &fps);
    return (float)fps;
  }

JNIEXPORT void JNICALL Java_processing_glvideo_GLVideo_gstreamer_1close
  (JNIEnv * env, jobject obj, jlong handle) {
    GLVIDEO_STATE_T *state = (GLVIDEO_STATE_T *)(intptr_t) handle;

    // stop pipeline
    gst_element_set_state (state->pipeline, GST_STATE_NULL);

    // free both buffers
    g_mutex_lock (&state->buffer_lock);
    if (state->current_buffer) {
      gst_buffer_unref (state->current_buffer);
      state->current_buffer = NULL;
    }
    if (state->next_buffer) {
      gst_buffer_unref (state->next_buffer);
      state->next_buffer = NULL;
    }
    g_mutex_unlock (&state->buffer_lock);

    gst_object_unref (state->gl_context);
    gst_object_unref (state->gst_display);

    gst_object_unref (state->vsink);
    gst_object_unref (state->pipeline);

    if (state->caps) {
      gst_caps_unref (state->caps);
    }

    g_mutex_clear (&state->buffer_lock);

    free(state);
  }
