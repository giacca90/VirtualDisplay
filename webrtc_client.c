#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

static GstElement *pipeline = NULL, *webrtc = NULL;
static SoupWebsocketConnection *ws_conn = NULL;

/* Enviar mensaje JSON por WebSocket */
static void send_sdp_to_server(const gchar *type, const gchar *sdp)
{
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, type);
    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, sdp);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *str = json_generator_to_data(gen, NULL);

    soup_websocket_connection_send_text(ws_conn, str);

    g_free(str);
    g_object_unref(gen);
    g_object_unref(builder);
    json_node_free(root);
}

/* Recibe mensajes del servidor WebSocket */
static void on_ws_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                          GBytes *message, gpointer user_data)
{
    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);
    JsonParser *parser = json_parser_new();

    if (!json_parser_load_from_data(parser, data, size, NULL)) {
        g_printerr("❌ Error al parsear JSON\n");
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = json_node_get_object(root);

    const gchar *msg_type = json_object_get_string_member(obj, "type");

    if (g_strcmp0(msg_type, "answer") == 0) {
        const gchar *sdp = json_object_get_string_member(obj, "sdp");
        GstSDPMessage *sdp_msg;
        gst_sdp_message_new(&sdp_msg);
        gst_sdp_message_parse_buffer((guint8 *)sdp, strlen(sdp), sdp_msg);

        GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, NULL);
        gst_webrtc_session_description_free(answer);
    }

    g_object_unref(parser);
}

/* Callback cuando WebSocket está conectado */
static void on_ws_connected(GObject *session_obj, GAsyncResult *res, gpointer user_data)
{
    GError *err = NULL;
    SoupSession *session = SOUP_SESSION(session_obj);

    ws_conn = soup_session_websocket_connect_finish(session, res, &err);

    if (err) {
        g_printerr("❌ WebSocket error: %s\n", err->message);
        g_error_free(err);
        return;
    }

    g_print("✅ WebSocket conectado\n");

    soup_websocket_connection_send_text(ws_conn, "{\"type\":\"gstreamer\"}");
    g_signal_connect(ws_conn, "message", G_CALLBACK(on_ws_message), NULL);
}

/* Callback cuando el WebRTC genera una oferta */
static void on_offer_created(GstPromise *promise, gpointer user_data)
{
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);

    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    g_signal_emit_by_name(webrtc, "set-local-description", offer, NULL);

    gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);
    send_sdp_to_server("offer", sdp_str);
    g_free(sdp_str);
    gst_webrtc_session_description_free(offer);
}

/* Arranca la pipeline de GStreamer */
static void start_pipeline()
{
    GstStateChangeReturn ret;

    pipeline = gst_parse_launch(
        "ximagesrc use-damage=0 ! videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay ! "
        "application/x-rtp,media=video,encoding-name=VP8,payload=96 ! "
        "webrtcbin name=webrtc", NULL);

    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");

    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK([](GstElement *webrtc, gpointer user_data) {
        GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
        g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
    }), NULL);

    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK([](GstElement *webrtc, guint mline, gchar *candidate, gpointer user_data) {
        // Aquí puedes enviar candidatos ICE si implementas ICE en tu servidor
    }), NULL);

    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("❌ Error al arrancar pipeline\n");
        exit(-1);
    }
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    if (argc < 2) {
        g_printerr("Uso: %s ws://host:puerto/endpoint\n", argv[0]);
        return -1;
    }

    start_pipeline();

    SoupSession *session = soup_session_new();
    g_autoptr(SoupURI) uri = soup_uri_new(argv[1]);

    soup_session_websocket_connect_async(
        session,
        uri,
        NULL,  // protocols
        NULL,  // origin
        NULL,  // io_priority
        NULL,  // cancellable
        on_ws_connected,
        NULL); // user_data

    g_print("⏳ Esperando WebSocket...\n");
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
