#define GST_USE_UNSTABLE_API
#include <locale.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <glib.h>

static GMainLoop *loop;
static GstElement *pipeline, *ximagesrc, *queue_elem, *postproc, *encoder, *parser_elem, *payloader, *webrtc;
static SoupWebsocketConnection *websocket = NULL;

typedef struct { int x,y,w,h; } Geometry;
static Geometry prev_geom = {0,0,0,0};

static gchar *object_to_json(JsonObject *obj) {
    JsonNode *root = json_node_alloc();
    json_node_init_object(root, obj);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *str = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    return str;
}

static void on_offer_created(GstPromise *promise, gpointer _user_data) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    g_signal_emit_by_name(webrtc, "set-local-description", offer, NULL);
    gchar *sdp_txt = gst_sdp_message_as_text(offer->sdp);

    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "offer");
    json_object_set_string_member(j, "sdp", sdp_txt);
    gchar *msg = object_to_json(j);
    soup_websocket_connection_send_text(websocket, msg);

    g_free(sdp_txt);
    g_free(msg);
    json_object_unref(j);
    gst_webrtc_session_description_free(offer);

    g_print("📡 Enviando offer SDP al servidor de señalización\n");
}

static void on_ice_candidate(GstElement *webrtcbin, guint mline, gchar *candidate, gpointer _user_data) {
    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "ice");
    json_object_set_int_member(j, "sdpMLineIndex", mline);
    json_object_set_string_member(j, "candidate", candidate);
    gchar *msg = object_to_json(j);
    soup_websocket_connection_send_text(websocket, msg);
    g_free(msg);
    json_object_unref(j);
    g_print("❄️ Nuevo ICE candidate generado\n");
}

static gboolean check_monitor(gpointer _){
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return TRUE;
    Window root = DefaultRootWindow(dpy);
    int nmon;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &nmon);
    if (mons && nmon > 1) {
        XRRMonitorInfo *m = &mons[1];
        int nx = m->x, ny = m->y, nw = m->width, nh = m->height;
        if (nx!=prev_geom.x || ny!=prev_geom.y || nw!=prev_geom.w || nh!=prev_geom.h) {
            prev_geom = (Geometry){nx,ny,nw,nh};
            g_print("🔄 Región cambiada: %d,%d  %dx%d\n", nx, ny, nw, nh);
            g_object_set(ximagesrc,
                         "startx", nx,
                         "starty", ny,
                         "endx",   nx + nw - 1,
                         "endy",   ny + nh - 1,
                         NULL);
            GstPad *srcpad = gst_element_get_static_pad(ximagesrc, "src");
            gst_pad_send_event(srcpad, gst_event_new_reconfigure());
            gst_object_unref(srcpad);
            GstPromise *p = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
            g_signal_emit_by_name(webrtc, "create-offer", NULL, p);
        }
    }
    if (mons) XRRFreeMonitors(mons);
    XCloseDisplay(dpy);
    return TRUE;
}

static void on_ws_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type, GBytes *message, gpointer user_data) {
    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);
    g_print("📨 Mensaje WS recibido: %s\n", data);

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, data, size, &error)) {
        g_printerr("❌ Error al parsear JSON: %s\n", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }
    JsonObject *msg = json_node_get_object(json_parser_get_root(parser));
    const gchar *msg_type = json_object_get_string_member(msg, "type");

    if (g_strcmp0(msg_type, "ready") == 0) {
        g_print("📩 Recibido 'ready' → arrancando timer de monitor…\n");
        g_timeout_add_seconds(1, check_monitor, NULL);
        GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
        g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);

    } else if (g_strcmp0(msg_type, "answer") == 0) {
        const gchar *sdp = json_object_get_string_member(msg, "sdp");
        GstSDPMessage *sdp_msg;
        gst_sdp_message_new(&sdp_msg);
        gst_sdp_message_parse_buffer((guint8 *)sdp, strlen(sdp), sdp_msg);
        GstWebRTCSessionDescription *answer =
            gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, NULL);
        gst_webrtc_session_description_free(answer);
        g_print("✅ SDP answer aplicada\n");

    } else if (g_strcmp0(msg_type, "ice") == 0) {
        const gchar *candidate = json_object_get_string_member(msg, "candidate");
        int sdpMLineIndex = json_object_get_int_member(msg, "sdpMLineIndex");
        g_signal_emit_by_name(webrtc, "add-ice-candidate", sdpMLineIndex, candidate);
        g_print("🧊 ICE candidate añadido\n");
    } else if (g_strcmp0(msg_type, "quality") == 0) {
        const gchar *action = json_object_get_string_member(msg, "action");
        if (g_strcmp0(action, "lower") == 0) {
            g_print("⚠️ Cliente pide bajar calidad\n");
            g_object_set(ximagesrc,
                "endx", 1279, "endy", 719, // 1280x720
                NULL);
            g_object_set(encoder,
                "bitrate", 1000000, // 1 Mbps
                NULL);
            g_object_set(encoder, "force-keyframe", TRUE, NULL);
        } else if (g_strcmp0(action, "raise") == 0) {
            g_print("⬆️ Cliente pide subir calidad\n");
            g_object_set(ximagesrc,
                "endx", 1919, "endy", 1079, // 1920x1080
                NULL);
            g_object_set(encoder,
                "bitrate", 2000000, // 2 Mbps (ajusta según tu red)
                NULL);
            g_object_set(encoder, "force-keyframe", TRUE, NULL);
        }
    }
    g_object_unref(parser);
}

static void on_ws_connected(SoupSession *session, GAsyncResult *res, gpointer _user_data) {
    GError *error = NULL;
    websocket = soup_session_websocket_connect_finish(session, res, &error);
    if (!websocket) {
        g_printerr("Error WS: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("✅ WebSocket conectado\n");
    g_signal_connect(websocket, "message", G_CALLBACK(on_ws_message), NULL);

    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "gstreamer");
    gchar *msg = object_to_json(j);
    soup_websocket_connection_send_text(websocket, msg);
    g_free(msg); json_object_unref(j);
    g_print("📡 Identificación enviada\n");
}

static void on_negotiation_needed(GstElement *element, gpointer user_data) {
    g_print("📞 Negociación requerida\n");
}

static void on_element_added(GstBin *bin, GstElement *child, gpointer user_data) {
    const gchar *stream_id = user_data;
    gst_element_decorate_stream_id(child, stream_id);
    g_print("   ▶ Sub-elemento %s etiquetado con %s\n",
            GST_ELEMENT_NAME(child), stream_id);
}

static void print_h264_caps_info(GstElement *parser) {
    GstPad *srcpad = gst_element_get_static_pad(parser, "src");
    if (!srcpad) {
        g_printerr("❌ No se pudo obtener el pad src de h264parse\n");
        return;
    }

    GstCaps *current_caps = gst_pad_get_current_caps(srcpad);
    if (!current_caps) {
        g_printerr("❌ No hay caps actuales disponibles\n");
        gst_object_unref(srcpad);
        return;
    }

    gchar *caps_str = gst_caps_to_string(current_caps);
    g_print("📺 Caps H264 actuales: %s\n", caps_str);

    g_free(caps_str);
    gst_caps_unref(current_caps);
    gst_object_unref(srcpad);
}


int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    // 0) Generar stream-ID único
    gchar *stream_id = g_uuid_string_random();
    g_print("🔖 Stream ID: %s\n", stream_id);

    // 1) Crear pipeline y elementos
    pipeline   = gst_pipeline_new("webrtc-pipeline");
    gst_element_decorate_stream_id(pipeline, stream_id);
    gst_pipeline_set_delay(GST_PIPELINE(pipeline), 0);

    ximagesrc  = gst_element_factory_make("ximagesrc",     "src");
    gst_element_decorate_stream_id(ximagesrc,  stream_id);
    g_object_set(ximagesrc,
             "use-damage", FALSE,
             "do-timestamp", TRUE,
             "startx", 0, "starty", 0,
             "endx", 1919, "endy", 1079,
			//"framerate", 30,
             "sync", FALSE,
             "show-pointer", TRUE,                 
             NULL);

    queue_elem = gst_element_factory_make("queue", "queue");
    gst_element_decorate_stream_id(queue_elem, stream_id);
	g_object_set(queue_elem,
	    "leaky", 2,
	    "max-size-buffers", 1,
	    "max-size-time", 0,
	    "max-size-bytes", 0,
	    NULL);

    GstElement *videoconvert = gst_element_factory_make("videoconvert", "videoconvert");
    gst_element_decorate_stream_id(videoconvert, stream_id);

    encoder    = gst_element_factory_make("vaapih264enc",   "encoder");
    gst_element_decorate_stream_id(encoder,    stream_id);
    g_object_set(encoder,
    "bitrate", 0, // 0 = auto
    "keyframe-period", 0, // 0 = auto
    "low-latency", TRUE,
	"tune", 3, // lowpower
    NULL); 

    /* encoder = gst_element_factory_make("x264enc", "encoder");
gst_element_decorate_stream_id(encoder, stream_id);
g_object_set(encoder,
    "bitrate", 8000,            // en kbit/s
    "key-int-max", 5,           // keyframe cada 5 frames
    "tune", 0x00000004,         // zerolatency
    "speed-preset", 3,          // ultrafast (1)
    "threads", 4,               // ajusta según tu CPU
    NULL); */

    parser_elem= gst_element_factory_make("h264parse",      "parser");
    gst_element_decorate_stream_id(parser_elem,stream_id);

    payloader  = gst_element_factory_make("rtph264pay",     "payloader");
    gst_element_decorate_stream_id(payloader,  stream_id);
    g_object_set(payloader,
             "config-interval", 1,
             "pt", 96,
             NULL);

    webrtc     = gst_element_factory_make("webrtcbin",      "webrtc");
    gst_element_decorate_stream_id(webrtc,     stream_id);
    g_object_set(webrtc,
             "latency", 0,
             "stun-server", "stun://stun.l.google.com:19302",
             NULL);


    if (!pipeline || !ximagesrc || !queue_elem || 
        !encoder || !parser_elem || !payloader || !webrtc) {
        g_printerr("❌ Error al crear elementos\n");
        return -1;
    }

    // 3) Señal para etiquetar futuros sub-elementos
    g_signal_connect(pipeline, "element-added",
                     G_CALLBACK(on_element_added), stream_id);

    // 5) Montar el pipeline
    gst_bin_add_many(GST_BIN(pipeline),
                 ximagesrc, queue_elem, videoconvert,
                 encoder, parser_elem, payloader, webrtc,
                 NULL);
    if (!gst_element_link(ximagesrc, queue_elem) ||
        !gst_element_link(queue_elem, videoconvert) ||
        !gst_element_link(videoconvert, encoder) ||
        !gst_element_link(encoder, parser_elem) ||
        !gst_element_link(parser_elem, payloader)) {
        g_printerr("❌ Error al enlazar elementos\n");
        return -1;
    }
    // Enlazar payloader -> webrtcbin
        GstPad *srcpad  = gst_element_get_static_pad(payloader,  "src");
        GstPad *sinkpad = gst_element_request_pad_simple(webrtc, "sink_%u");
        if (!srcpad || !sinkpad || gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
            g_printerr("❌ No se pudo enlazar payloader -> webrtcbin\n");
            return -1;
        }
        gst_object_unref(srcpad);
        gst_object_unref(sinkpad);

    // 6) Configurar reloj y start-time
    
    GstClock *clock = gst_system_clock_obtain();
	gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
	gst_element_set_start_time(pipeline, gst_clock_get_time(clock));
	gst_object_unref(clock);

    // 7) Señales WebRTC
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate",      G_CALLBACK(on_ice_candidate),      NULL);

    // 8) Poner pipeline en PLAYING
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_print("▶️ Pipeline en PLAYING\n");

	// Espera breve para que se negocien caps
	g_usleep(200 * 1000); // 200 ms

	// Imprimir profile y level actuales
	print_h264_caps_info(parser_elem);

    // 9) Conexión al servidor de señalización
    {
        SoupSession *sess = soup_session_new_with_options(NULL);
        SoupMessage *msg = soup_message_new("GET", "ws://localhost:8000/ws");
        soup_session_websocket_connect_async(
            sess, msg, NULL, NULL, G_PRIORITY_DEFAULT,
            NULL, (GAsyncReadyCallback)on_ws_connected, NULL);
    }

    // 10) Bucle principal
    g_main_loop_run(loop);

    // Limpieza
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    g_free(stream_id);
    return 0;
}
