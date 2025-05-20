#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup-3.0/libsoup/soup.h>
#include <json-glib/json-glib.h>

static GMainLoop               *loop;
static GstElement              *pipeline, *webrtcbin;
static SoupSession             *soup_sess;
static SoupWebsocketConnection *ws_conn;

// 1) Registro inicial (“gstreamer”)
static void
send_registration (void)
{
    JsonObject *j = json_object_new ();
    json_object_set_string_member (j, "type", "gstreamer");
    JsonNode *root = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (root, j);

    JsonGenerator *gen = json_generator_new ();
    json_generator_set_root (gen, root);
    gchar *txt = json_generator_to_data (gen, NULL);

    soup_websocket_connection_send_text (ws_conn, txt);

    g_free (txt);
    g_object_unref (gen);
    json_node_free (root);
}

// 2) Cuando GStreamer crea la oferta SDP
static void
on_offer_created (GstPromise *promise, gpointer _)
{
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply (promise);

    gst_structure_get (reply, "offer",
                       GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref (promise);

    // 2.1) Aplicar local description
    g_signal_emit_by_name (webrtcbin, "set-local-description", offer, NULL);

    // 2.2) Enviar offer via WS
    gchar *sdp_txt = gst_sdp_message_as_text (offer->sdp);
    JsonObject *j = json_object_new ();
    json_object_set_string_member (j, "type", "offer");
    json_object_set_string_member (j, "sdp", sdp_txt);

    JsonNode *root = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (root, j);
    JsonGenerator *gen = json_generator_new ();
    json_generator_set_root (gen, root);
    gchar *msg = json_generator_to_data (gen, NULL);

    soup_websocket_connection_send_text (ws_conn, msg);

    g_free (msg);
    g_free (sdp_txt);
    g_object_unref (gen);
    json_node_free (root);

    gst_webrtc_session_description_free (offer);
}

// 3) Al dispararse necesidad de negociación
static void
on_negotiation_needed (GstElement *, gpointer _)
{
    GstPromise *promise =
        gst_promise_new_with_change_func (on_offer_created, NULL, NULL);
    g_signal_emit_by_name (webrtcbin, "create-offer", NULL, promise);
}

// 4) Cuando hay un ICE local
static void
on_ice_candidate (GstElement *, guint mline, gchar *candidate, gpointer _)
{
    JsonObject *j = json_object_new ();
    json_object_set_string_member (j, "type", "ice");
    json_object_set_int_member    (j, "sdpMLineIndex", mline);
    json_object_set_string_member (j, "candidate", candidate);

    JsonNode *root = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (root, j);
    JsonGenerator *gen = json_generator_new ();
    json_generator_set_root (gen, root);
    gchar *msg = json_generator_to_data (gen, NULL);

    soup_websocket_connection_send_text (ws_conn, msg);

    g_free (msg);
    g_object_unref (gen);
    json_node_free (root);
}

// 5) Manejo de mensajes entrantes (answer / ice)
static void
on_ws_message (SoupWebsocketConnection *, SoupWebsocketDataType, GBytes *bytes, gpointer _)
{
    gsize size;
    const gchar *data = g_bytes_get_data (bytes, &size);
    gchar *txt = g_strndup (data, size);
    g_print ("📩 Recibido: %s\n", txt);

    JsonParser *parser = json_parser_new ();
    GError *error = NULL;
    if (!json_parser_load_from_data (parser, txt, -1, &error)) {
        g_printerr ("❌ JSON inválido: %s\n", error->message);
        g_error_free (error);
        g_object_unref (parser);
        g_free (txt);
        return;
    }

    JsonObject *obj = json_node_get_object (json_parser_get_root (parser));
    const gchar *t = json_object_get_string_member (obj, "type");

    if (g_strcmp0 (t, "answer") == 0) {
        // Aplicar answer
        const gchar *sdp_txt = json_object_get_string_member (obj, "sdp");
        GstSDPMessage *sdp;
        gst_sdp_message_new (&sdp);
        gst_sdp_message_parse_buffer ((guint8*) sdp_txt,
                                      strlen (sdp_txt), sdp);
        GstWebRTCSessionDescription *answer =
            gst_webrtc_session_description_new (
                GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        g_signal_emit_by_name (webrtcbin,
                               "set-remote-description", answer, NULL);
        gst_webrtc_session_description_free (answer);
    }
    else if (g_strcmp0 (t, "ice") == 0) {
        // Añadir ICE candidate
        guint idx = json_object_get_int_member (obj, "sdpMLineIndex");
        const gchar *cand = json_object_get_string_member (obj, "candidate");
        g_print ("▶️ Añadiendo ICE: %s\n", cand);
        g_signal_emit_by_name (webrtcbin,
                               "add-ice-candidate", idx, cand);
    }

    g_object_unref (parser);
    g_free (txt);
}

// 6) Callback tras conectar WebSocket
static void
on_ws_connected (GObject *source, GAsyncResult *res, gpointer _)
{
    GError *err = NULL;
    ws_conn = soup_session_websocket_connect_finish (soup_sess, res, &err);
    if (err) {
        g_printerr ("❌ WS error: %s\n", err->message);
        g_error_free (err);
        g_main_loop_quit (loop);
        return;
    }
    g_print ("✅ WS conectado\n");

    g_signal_connect (ws_conn, "message",
                      G_CALLBACK (on_ws_message), NULL);

    // 6.1) Registrarse
    send_registration ();

    // 6.2) Montar pipeline
    pipeline = gst_parse_launch (
      "ximagesrc use-damage=0 startx=0 starty=0 endx=1920 endy=1080 ! "
      "video/x-raw,framerate=30/1 ! videoconvert ! queue ! "
      "vp8enc deadline=1 ! rtpvp8pay ! "
      "application/x-rtp,media=video,encoding-name=VP8,payload=96 ! "
      "webrtcbin name=webrtcbin "
      "stun-server=stun://stun.l.google.com:19302",
      NULL);

    webrtcbin = gst_bin_get_by_name (GST_BIN (pipeline), "webrtcbin");
    g_signal_connect (webrtcbin, "on-negotiation-needed",
                      G_CALLBACK (on_negotiation_needed), NULL);
    g_signal_connect (webrtcbin, "on-ice-candidate",
                      G_CALLBACK (on_ice_candidate), NULL);

    gst_element_set_state (pipeline, GST_STATE_PLAYING);
}

int
main (int argc, char *argv[])
{
    gst_init (&argc, &argv);
    loop = g_main_loop_new (NULL, FALSE);

    // Crear sesión libsoup-3
    soup_sess = soup_session_new ();

    // Conectar WS (client) usando URI directa
    soup_session_websocket_connect_async (
        soup_sess,
        "ws://localhost:8000/ws",   /* URI */
        NULL,                        /* origin */
        NULL,                        /* protocols */
        SOUP_SESSION_WEBSOCKET_CLIENT, /* flags = 0 */
        G_PRIORITY_DEFAULT,
        NULL,                        /* cancellable */
        on_ws_connected,
        NULL
    );

    g_main_loop_run (loop);

    // Teardown
    if (pipeline) {
        gst_element_set_state (pipeline, GST_STATE_NULL);
        gst_object_unref (pipeline);
    }
    g_main_loop_unref (loop);
    g_object_unref (soup_sess);
    return 0;
}
