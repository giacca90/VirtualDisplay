#define GST_USE_UNSTABLE_API
#include <locale.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

static GMainLoop *loop;
static GstElement *pipeline, *webrtc;
static SoupWebsocketConnection *websocket = NULL;

// Convierte JsonObject a cadena JSON
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

// Callback: offer SDP generado
static void on_offer_created(GstPromise *promise, gpointer _user_data) {
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    // Aplica y envía la descripción local
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

// Callback: nuevo ICE candidate local
static void on_ice_candidate(GstElement *webrtcbin, guint mline, gchar *candidate, gpointer _user_data) {
    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "ice");
    json_object_set_int_member   (j, "sdpMLineIndex", mline);
    json_object_set_string_member(j, "candidate", candidate);
    gchar *msg = object_to_json(j);
    soup_websocket_connection_send_text(websocket, msg);
    g_free(msg);
    json_object_unref(j);
    g_print("❄️ Nuevo ICE candidate generado\n");
}

// Callback: mensajes entrantes por WebSocket
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
    g_print("📝 Tipo de mensaje: %s\n", msg_type);

    if (g_strcmp0(msg_type, "ready") == 0) {
        g_print("📩 Recibido 'ready' → creando offer\n");
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
    }

    g_object_unref(parser);
}

// Callback: conexión WebSocket abierta
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

    // Identificación ante el señalizador
    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "gstreamer");
    gchar *msg = object_to_json(j);
    soup_websocket_connection_send_text(websocket, msg);
    g_free(msg);
    json_object_unref(j);
    g_print("📡 Identificación enviada (gstreamer)\n");
}

// Callback: negociación requerida
static void on_negotiation_needed(GstElement *element, gpointer user_data) {
    g_print("📞 WebRTC: negociación requerida\n");
}

int main(int argc, char *argv[]) {
	setlocale(LC_ALL, "");
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);
    g_print("🚀 Iniciando aplicación WebRTC con GStreamer...\n");

    // 1) Construir el pipeline
    GError *err = NULL;
    pipeline = gst_parse_launch(
        "ximagesrc use-damage=0 do-timestamp=true startx=0 starty=0 endx=1919 endy=1080 "  // do-timestamp fuerza timestamps :contentReference[oaicite:1]{index=1}
        "! video/x-raw,framerate=30/1 "
        "! videorate "                                                                  // regula framerate :contentReference[oaicite:2]{index=2}
        "! videoconvert ! video/x-raw,width=1920,height=1080 ! queue "
        "! vp8enc deadline=1 ! rtpvp8pay "
        "! application/x-rtp,media=video,encoding-name=VP8,payload=96 "
        "! webrtcbin name=webrtc",
        &err);
    if (!pipeline) {
        g_printerr("Pipeline error: %s\n", err->message);
        return -1;
    }
    g_print("🎥 Pipeline construido correctamente\n");

    // 2) Asignar reloj y fijar start-time
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    gst_element_set_start_time(pipeline, gst_clock_get_time(clock));                       // fija start time
    gst_object_unref(clock);
    g_print("🕒 Reloj asignado y start-time fijado\n");

    // 3) Conectar señales de webrtcbin
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate",      G_CALLBACK(on_ice_candidate),      NULL);
    g_print("🔌 Señales WebRTC conectadas\n");

    // 4) Poner pipeline en PLAYING antes de negociar
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_print("▶️ Pipeline en PLAYING\n");

    // 5) Conectar al servidor de señalización
    SoupSession *sess = soup_session_new_with_options(NULL);
    SoupMessage *msg = soup_message_new("GET", "ws://localhost:8000/ws");
    soup_session_websocket_connect_async(
        sess, msg,
        NULL, NULL,
        G_PRIORITY_DEFAULT,
        NULL,
        (GAsyncReadyCallback)on_ws_connected,
        NULL);
    g_print("🔗 Conectando al señalizador ws://localhost:8000/ws …\n");

    // 6) Ejecutar loop
    g_main_loop_run(loop);

    // 7) Limpieza
    g_print("🧹 Limpieza final y salida\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
