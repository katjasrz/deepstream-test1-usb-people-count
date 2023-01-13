/*
 * Copyright (c) 2018-2022, NVIDIA CORPORATION. All rights reserved.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <cuda_runtime_api.h>
#include "gstnvdsmeta.h"
#include "nvds_yml_parser.h"

#define MAX_DISPLAY_LEN 64

#define PGIE_CLASS_ID_PERSON 0

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

#define MUXER_OUTPUT_WIDTH_CAMERA 1280
#define MUXER_OUTPUT_HEIGHT_CAMERA 720

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 40000

gint frame_number = 0;
gchar pgie_classes_str[3][32] = { "Person", "Bag", "Face"
};

typedef struct _perf_measure{
  GstClockTime pre_time;
  GstClockTime total_time;
  guint count;
}perf_measure;

/* osd_sink_pad_buffer_probe  will extract metadata received on OSD sink pad
 * and update params for drawing rectangle, object information etc. */

static GstPadProbeReturn
osd_sink_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *) info->data;
    guint num_rects = 0; 
    NvDsObjectMeta *obj_meta = NULL;
    guint person_count = 0;
    NvDsMetaList * l_frame = NULL;
    NvDsMetaList * l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;

    GstClockTime now;
    perf_measure * perf = (perf_measure *)(u_data);

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    now = g_get_monotonic_time();

    if (perf->pre_time == GST_CLOCK_TIME_NONE) {
      perf->pre_time = now;
      perf->total_time = GST_CLOCK_TIME_NONE;
    } else {
      if (perf->total_time == GST_CLOCK_TIME_NONE) {
        perf->total_time = (now - perf->pre_time);
      } else {
        perf->total_time += (now - perf->pre_time);
      }
      perf->pre_time = now;
      perf->count++;
    }

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
      l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);
        int offset = 0;
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL;
                l_obj = l_obj->next) {
            obj_meta = (NvDsObjectMeta *) (l_obj->data);
            if (obj_meta->class_id == PGIE_CLASS_ID_PERSON) {
                person_count++;
                num_rects++;
            }
        }
        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        NvOSD_TextParams *txt_params  = &display_meta->text_params[0];
        display_meta->num_labels = 1;
        txt_params->display_text = g_malloc0 (MAX_DISPLAY_LEN);
        offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Person = %d ", person_count);

        // Now set the offsets where the string should appear 
        txt_params->x_offset = 10;
        txt_params->y_offset = 12;

        // Font , font-color and font-size 
        txt_params->font_params.font_name = "Serif";
        txt_params->font_params.font_size = 10;
        txt_params->font_params.font_color.red = 1.0;
        txt_params->font_params.font_color.green = 1.0;
        txt_params->font_params.font_color.blue = 1.0;
        txt_params->font_params.font_color.alpha = 1.0;

        // Text background color 
        txt_params->set_bg_clr = 1;
        txt_params->text_bg_clr.red = 0.0;
        txt_params->text_bg_clr.green = 0.0;
        txt_params->text_bg_clr.blue = 0.0;
        txt_params->text_bg_clr.alpha = 1.0;

        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }

    g_print ("Frame Number = %d Number of objects = %d "
            "Person Count = %d\n",
            frame_number, num_rects, person_count);
    frame_number++;
    return GST_PAD_PROBE_OK;
}

static gboolean
bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
  GMainLoop *loop = (GMainLoop *) data;
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_ERROR:{
      gchar *debug;
      GError *error;
      gst_message_parse_error (msg, &error, &debug);
      g_printerr ("ERROR from element %s: %s\n",
          GST_OBJECT_NAME (msg->src), error->message);
      if (debug)
        g_printerr ("Error details: %s\n", debug);
      g_free (debug);
      g_error_free (error);
      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}


int
main (int argc, char *argv[])
{
  GMainLoop *loop = NULL;
  GstElement *pipeline = NULL, *source = NULL, *h264parser = NULL,
      *decoder = NULL, *streammux = NULL, *sink = NULL, *pgie = NULL, *nvvidconv = NULL,
      *nvosd = NULL;

  GstElement *cap_filter = NULL;
  GstElement *transform = NULL;
  GstBus *bus = NULL;
  guint bus_watch_id;
  GstPad *osd_sink_pad = NULL;

  perf_measure perf_measure;

  int current_device = -1;
  cudaGetDevice(&current_device);
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  /* Check input arguments */
  if (argc != 2) {
    g_printerr ("Usage: %s <yml file>\n", argv[0]);
    g_printerr ("OR: %s <H264 filename>\n", argv[0]);
    g_printerr ("OR: %s camera\n", argv[0]);
    return -1;
  }
  bool USE_CAMERA_INPUT = !strcmp(argv[1], "camera"); 

  /* Standard GStreamer initialization */
  gst_init (&argc, &argv);
  loop = g_main_loop_new (NULL, FALSE);

  perf_measure.pre_time = GST_CLOCK_TIME_NONE;
  perf_measure.total_time = GST_CLOCK_TIME_NONE;
  perf_measure.count = 0;  

  /* Create gstreamer elements */
  // Create Pipeline element that will form a connection of other elements 
  pipeline = gst_pipeline_new ("dstest1-usb-cam-pipeline");

  // Create nvstreammux instance to form batches from one or more sources. 
  streammux = gst_element_factory_make ("nvstreammux", "stream-muxer");

  if (!pipeline || !streammux) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

  if (g_str_has_suffix (argv[1], ".yml") || g_str_has_suffix (argv[1], ".yaml")) {
      nvds_parse_streammux(streammux, argv[1],"streammux");
  }

  if (USE_CAMERA_INPUT) {

    GstElement *cap_filter1 = NULL;
    GstCaps *caps = NULL, *caps1 = NULL, *convertCaps = NULL;
    GstCapsFeatures *feature = NULL;
    GstElement *nvvidconv1 = NULL;
    GstElement *nvvidconv2 = NULL;

    g_object_set (G_OBJECT (streammux), "batch-size", 1, NULL);

    g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH_CAMERA, "height",
          MUXER_OUTPUT_HEIGHT_CAMERA,
          "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, "live-source", TRUE, \
          NULL);

    /* Source element for reading from camera */
    source = gst_element_factory_make ("v4l2src", "src_elem");
    if (!source  ) {
        g_printerr ("Could not create 'src_elem'.\n");
        return -1;
    }
    //Source setting 
    g_object_set (G_OBJECT (source), "device", "/dev/video0", NULL);
    
    /* capsfilter for v4l2src */
    cap_filter1 = gst_element_factory_make("capsfilter", "src_cap_filter1");
    if (!cap_filter1) {
        g_printerr ("Could not create 'src_cap_filter1'.\n");
        return -1;
    }

    caps1 = gst_caps_from_string ("video/x-raw");

    cap_filter = gst_element_factory_make ("capsfilter", "src_cap_filter");
    if (!cap_filter) {
      g_printerr ("Could not create 'src_cap_filter'.\n");
      return -1;
    }

    caps = gst_caps_from_string ("video/x-raw(memory:NVMM), format=NV12, framerate=30/1");

    if(!prop.integrated) {
      nvvidconv1 = gst_element_factory_make ("videoconvert", "nvvidconv1");
      if (!nvvidconv1) {
        g_printerr ("Failed to create 'nvvidconv1'.\n");
        return -1;
      }
    }

    feature = gst_caps_features_new ("memory:NVMM", NULL);
    gst_caps_set_features (caps, 0, feature);
    g_object_set (G_OBJECT (cap_filter), "caps", caps, NULL);
    g_object_set (G_OBJECT (cap_filter1), "caps", caps1, NULL);

    nvvidconv2 = gst_element_factory_make ("nvvideoconvert", "nvvidconv2");
    if (!nvvidconv2) {
      g_printerr ("Failed to create 'nvvidconv2'.\n");
      return -1;
    }

    g_object_set (G_OBJECT (nvvidconv2), "nvbuf-memory-type", 0, NULL);

    if(!prop.integrated) {
      gst_bin_add_many (GST_BIN (pipeline), source, cap_filter1,
          nvvidconv1, nvvidconv2, cap_filter, NULL);
      if (!gst_element_link_many (source, cap_filter1,
          nvvidconv1, nvvidconv2, cap_filter, NULL)) {
        g_printerr ("Elements could not be linked: 1. Exiting.\n");
        return -1;
      }
    } else {
      gst_bin_add_many (GST_BIN (pipeline), source, cap_filter1,
          nvvidconv2, cap_filter, NULL);
      if (!gst_element_link_many (source, cap_filter1,
          nvvidconv2, cap_filter, NULL)) {
        g_printerr ("Elements could not be linked: 1. Exiting.\n");
        return -1;
      }
    }

  } else {

    /* Source element for reading from the file */
    source = gst_element_factory_make ("filesrc", "file-source");

    /* If filename has been passed as a parameter */
    if (g_str_has_suffix (argv[1], ".h264")) {
      /* we set the input filename to the source element */
      g_object_set (G_OBJECT (source), "location", argv[1], NULL);

      g_object_set (G_OBJECT (streammux), "batch-size", 1, NULL);

      g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height",
          MUXER_OUTPUT_HEIGHT,
          "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, "live-source", FALSE, NULL);
    }

    /* If yaml has been passed as a parameter */
    if (g_str_has_suffix (argv[1], ".yml") || g_str_has_suffix (argv[1], ".yaml")) {
      nvds_parse_file_source(source, argv[1],"source");
    }

    /* Since the data format in the input file is elementary h264 stream,
    * we need a h264parser */
    h264parser = gst_element_factory_make ("h264parse", "h264-parser");

    /* Use nvdec_h264 for hardware accelerated decode on GPU */
    decoder = gst_element_factory_make ("nvv4l2decoder", "nvv4l2-decoder");

    if (!source || !h264parser || !decoder ) {
      g_printerr ("One element could not be created. Exiting.\n");
      return -1;
    }

    gst_bin_add_many (GST_BIN (pipeline),
        source, h264parser, decoder, NULL);
    
    /* file-source -> h264-parser -> nvh264-decoder */
    if (!gst_element_link_many (source, h264parser, decoder, NULL)) {
      g_printerr ("Elements could not be linked: 1. Exiting.\n");
      return -1;
    }

  }

  gst_bin_add (GST_BIN(pipeline), streammux);

  // Use nvinfer to run inferencing on decoder's output,
  // behaviour of inferencing is set through config file 
  pgie = gst_element_factory_make ("nvinfer", "primary-nvinference-engine");

  // Set all the necessary properties of the nvinfer element,
  //   the necessary ones are : 
  g_object_set (G_OBJECT (pgie),
        "config-file-path", "dstest1_usb_pgie_config.yml", NULL);

  // Use convertor to convert from NV12 to RGBA as required by nvosd 
  nvvidconv = gst_element_factory_make ("nvvideoconvert", "osd_conv");
  if (!nvvidconv) {
    g_printerr ("Failed to create 'osd_conv'.\n");
    return -1;
  }
  g_object_set (G_OBJECT (nvvidconv), "gpu-id", 0, NULL);
  g_object_set (G_OBJECT (nvvidconv), "nvbuf-memory-type", 0, NULL);

  // Create OSD to draw on the converted RGBA buffer 
  nvosd = gst_element_factory_make ("nvdsosd", "nv-onscreendisplay");
  if (!nvosd) {
    g_printerr ("Failed to create 'nv-onscreendisplay'.\n");
    return -1;
  }
  g_object_set (G_OBJECT (nvosd), "gpu-id", 0, NULL);

  // Finally render the osd output 
  if(prop.integrated) {
    transform = gst_element_factory_make ("nvegltransform", "nvegl-transform");
  }
  sink = gst_element_factory_make ("nveglglessink", "nvvideo-renderer");

  if (!pgie || !sink) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

  if(!transform && prop.integrated) {
    g_printerr ("One tegra element could not be created. Exiting.\n");
    return -1;
  }

  GstPad *sinkpad, *srcpad;
  gchar pad_name_sink[16] = "sink_0";
  gchar pad_name_src[16] = "src";

  sinkpad = gst_element_get_request_pad (streammux, pad_name_sink);
  if (!sinkpad) {
    g_printerr ("Streammux request sink pad failed. Exiting.\n");
    return -1;
  }

  if (USE_CAMERA_INPUT) {
    srcpad = gst_element_get_static_pad (cap_filter, pad_name_src);
  } else {
    srcpad = gst_element_get_static_pad (decoder, pad_name_src);
  }
  if (!srcpad) {
    g_printerr ("Decoder request src pad failed. Exiting.\n");
    return -1;
  }

  if (gst_pad_link (srcpad, sinkpad) != GST_PAD_LINK_OK) {
      g_printerr ("Failed to link decoder to stream muxer. Exiting.\n");
      return -1;
  }
  gst_object_unref (sinkpad);
  gst_object_unref (srcpad);

  // Set up the pipeline 
  // we add all elements into the pipeline 
  if(prop.integrated) {
    gst_bin_add_many (GST_BIN (pipeline),
      pgie, nvvidconv, nvosd, transform, sink, NULL);

    if (!gst_element_link_many (streammux, pgie,
        nvvidconv, nvosd, transform, sink, NULL)) {
      g_printerr ("Elements could not be linked: 2. Exiting.\n");
      return -1;
    }
  }
  else {
    gst_bin_add_many (GST_BIN (pipeline),
       pgie, nvvidconv, nvosd, sink, NULL);

    if (!gst_element_link_many (streammux, pgie,
        nvvidconv, nvosd, sink, NULL)) {
      g_printerr ("Elements could not be linked: 2. Exiting.\n");
      return -1;
    }
  }

  // Lets add probe to get informed of the meta data generated, we add probe to
  // the sink pad of the osd element, since by that time, the buffer would have
  // had got all the metadata. 
  osd_sink_pad = gst_element_get_static_pad (nvosd, "sink");
  if (!osd_sink_pad)
    g_print ("Unable to get sink pad\n");
  //else
    gst_pad_add_probe (osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        osd_sink_pad_buffer_probe, &perf_measure, NULL);
  gst_object_unref (osd_sink_pad);


// we add a message handler 
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  // Set the pipeline to "playing" state 
  if (USE_CAMERA_INPUT ) {
    g_print ("Using USB camera input\n");
  } else {
    g_print ("Using file: %s\n", argv[1]);
  }

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  // Wait till pipeline encounters an error or EOS 
  g_print ("Running...\n");
  g_main_loop_run (loop);


  // Out of the main loop, clean up 
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_print ("Deleting pipeline\n");

  if(perf_measure.total_time)
  {
    g_print ("Average fps %f\n",
      ((perf_measure.count-1)*1000000.0)/perf_measure.total_time);
  }
  
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
  return 0;

  
}
