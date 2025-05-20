#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

static GMainLoop             *loop;
static GstElement            *pipeline, *webrtcbin;
static SoupSession           *soup_sess;
static SoupWebsocketConnection *ws_conn;

// Convierte el SDP a texto y lo envía por WebSocket
static void
send_sdp_offer(GstWebRTCSessionDescription *offer) {
    gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);

    JsonObject *json = json_object_new();
    json_object_set_string_member(json, "type", "offer");
    json_object_set_string_member(json, "sdp", sdp_str);

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, json);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *msg = json_generator_to_data(gen, NULL);

    soup_websocket_connection_send_text(ws_conn, msg);

    g_free(msg);
    g_free(sdp_str);
    g_object_unref(gen);
    json_node_free(root);
}

// Se llama cuando GStreamer crea la oferta SDP
static void
on_offer_created(GstPromise *promise, gpointer unused) {
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = NULL;
    gst_structure_get(reply, "offer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    // Establece descripción local y la envía
    g_signal_emit_by_name(webrtcbin, "set-local-description", offer, NULL);
    send_sdp_offer(offer);
    gst_webrtc_session_description_free(offer);
}

// Negociación necesaria: pide crear oferta
static void
on_negotiation_needed(GstElement *element, gpointer unused) {
    GstPromise *promise =
        gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtcbin, "create-offer", NULL, promise);
}

// Se llama cuando hay un candidato ICE local
static void
on_ice_candidate(GstElement *element, guint mlineindex,
                 gchar *candidate, gpointer unused) {
    JsonObject *json = json_object_new();
    json_object_set_string_member(json, "type", "ice");
    json_object_set_int_member   (json, "sdpMLineIndex", mlineindex);
    json_object_set_string_member(json, "candidate", candidate);

    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, json);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *msg = json_generator_to_data(gen, NULL);

    soup_websocket_connection_send_text(ws_conn, msg);

    g_free(msg);
    g_object_unref(gen);
    json_node_free(root);
}

// Mensajes entrantes por WebSocket (p.ej. respuesta SDP o ICE remoto)
static void
on_ws_message(SoupWebsocketConnection *conn,
              SoupWebsocketDataType  type,
              GBytes                *message,
              gpointer               unused) {
    gsize   size;
    const gchar *data = g_bytes_get_data(message, &size);
    g_print("📩 Mensaje recibido: %.*s\n", (int)size, data);
    // Aquí parseas JSON, extraes SDP/ICE y emites:
    // - set-remote-description
    // - add-ice-candidate
}

// Se llama cuando se completa la conexión WebSocket
static void
on_ws_connected(GObject        *source,
                GAsyncResult   *res,
                gpointer        unused) {
    GError *error = NULL;
    ws_conn = soup_session_websocket_connect_finish(soup_sess, res, &error);
    if (error) {
        g_printerr("❌ Error WebSocket: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }
    g_print("✅ WebSocket conectado\n");

    // Conecta señal de mensajes
    g_signal_connect(ws_conn, "message",
                     G_CALLBACK(on_ws_message), NULL);

    // Crea pipeline: captura pantalla, codifica VP8, empaqueta RTP y WebRTC
    pipeline = gst_parse_launch(
      "ximagesrc use-damage=0 ! video/x-raw,framerate=30/1 ! "
      "videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay ! "
      "application/x-rtp,media=video,encoding-name=VP8,payload=96 ! "
      "webrtcbin name=webrtcbin stun-server=stun://stun.l.google.com:19302",
      NULL);

    webrtcbin = gst_bin_get_by_name(GST_BIN(pipeline), "webrtcbin");
    g_signal_connect(webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    // Prepara sesión y mensaje WebSocket
    soup_sess = soup_session_new();
    SoupMessage *msg =
        soup_message_new("GET", "ws://localhost:8000/ws");

    // ¡Ojo aquí! firma libsoup-3.0:
    soup_session_websocket_connect_async(
        soup_sess,
        msg,
        NULL,               // origin
        NULL,               // protocolos (char*[])
        G_PRIORITY_DEFAULT, // io_priority
        NULL,               // GCancellable
        on_ws_connected,    // callback
        NULL                // user_data
    );

    // Ejecuta loop
    g_main_loop_run(loop);

    // Limpia
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    g_main_loop_unref(loop);
    g_object_unref(soup_sess);
    g_object_unref(msg);
    return 0;
}
