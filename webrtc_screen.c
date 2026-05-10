#define GST_USE_UNSTABLE_API
#include <locale.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <glib.h>

// --- ESTRUCTURAS ---
typedef struct { int x,y,w,h; } Geometry;

typedef struct {
    gchar *peer_id;
    GstElement *queue;
    GstElement *webrtcbin;
    GstPad *tee_pad;
} WebRTCClient;

// --- GLOBALES ---
static GMainLoop *loop;
static GstElement *pipeline, *ximagesrc, *capsfilter, *queue_elem, *postproc, *encoder, *parser_elem, *payloader, *tee;
static SoupWebsocketConnection *websocket = NULL;
static GHashTable *clients; 
static Geometry prev_geom = {0,0,0,0};

// --- PROTOTIPOS (Para evitar errores de orden) ---
static void on_offer_created(GstPromise *promise, gpointer user_data);
static void on_ice_candidate(GstElement *webrtcbin, guint mline, gchar *candidate, gpointer user_data);
static WebRTCClient* create_client(const gchar *peer_id);
static void remove_client(const gchar *peer_id);

// --- UTILIDADES ---
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

// --- LÓGICA DE PANTALLA VIRTUAL ---
static void trigger_renegotiation(gpointer key, gpointer value, gpointer user_data) {
    WebRTCClient *client = (WebRTCClient *)value;
    GstPromise *p = gst_promise_new_with_change_func(on_offer_created, client, NULL);
    g_signal_emit_by_name(client->webrtcbin, "create-offer", NULL, p);
}

static gboolean check_monitor(gpointer _data){
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return TRUE;

    Window root = DefaultRootWindow(dpy);
    int nmon;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &nmon);
    
    if (mons && nmon > 1) {
        XRRMonitorInfo *m = &mons[1]; // Pantalla virtual
        if (m->x != prev_geom.x || m->y != prev_geom.y || m->width != prev_geom.w || m->height != prev_geom.h) {
            prev_geom = (Geometry){m->x, m->y, m->width, m->height};
            
            g_object_set(ximagesrc, 
                "startx", m->x, "starty", m->y, 
                "endx", m->x + m->width - 1, "endy", m->y + m->height - 1, NULL);

            GstPad *spad = gst_element_get_static_pad(ximagesrc, "src");
            if (spad) {
                gst_pad_send_event(spad, gst_event_new_reconfigure());
                gst_object_unref(spad);
            }
            g_hash_table_foreach(clients, trigger_renegotiation, NULL);
        }
    }
    if (mons) XRRFreeMonitors(mons);
    XCloseDisplay(dpy);
    return TRUE;
}

// --- CALLBACKS WEBRTC ---
static void on_offer_created(GstPromise *promise, gpointer user_data) {
    WebRTCClient *client = (WebRTCClient *)user_data;
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    
    if (!gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL)) {
        gst_promise_unref(promise);
        return;
    }
    gst_promise_unref(promise);

    g_signal_emit_by_name(client->webrtcbin, "set-local-description", offer, NULL);
    gchar *sdp_txt = gst_sdp_message_as_text(offer->sdp);

    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "offer");
    json_object_set_string_member(j, "sdp", sdp_txt);
    json_object_set_string_member(j, "peer_id", client->peer_id);
    
    gchar *msg = object_to_json(j);
    if (websocket) soup_websocket_connection_send_text(websocket, msg);

    g_free(sdp_txt); g_free(msg);
    json_object_unref(j);
    gst_webrtc_session_description_free(offer);
}

static void on_ice_candidate(GstElement *webrtcbin, guint mline, gchar *candidate, gpointer user_data) {
    WebRTCClient *client = (WebRTCClient *)user_data;
    JsonObject *j = json_object_new();
    json_object_set_string_member(j, "type", "ice");
    json_object_set_int_member(j, "sdpMLineIndex", mline);
    json_object_set_string_member(j, "candidate", candidate);
    json_object_set_string_member(j, "peer_id", client->peer_id);
    gchar *msg = object_to_json(j);
    if (websocket) soup_websocket_connection_send_text(websocket, msg);
    g_free(msg); json_object_unref(j);
}

// --- GESTIÓN DE CLIENTES ---
static WebRTCClient* create_client(const gchar *peer_id) {
    WebRTCClient *client = g_new0(WebRTCClient, 1);
    client->peer_id = g_strdup(peer_id);

    client->queue = gst_element_factory_make("queue", NULL);
    client->webrtcbin = gst_element_factory_make("webrtcbin", NULL);
    g_object_set(client->webrtcbin, "bundle-policy", 3, "latency", 0, NULL);

    gst_bin_add_many(GST_BIN(pipeline), client->queue, client->webrtcbin, NULL);

    GstPad *q_src = gst_element_get_static_pad(client->queue, "src");
    GstPad *w_sink = gst_element_request_pad_simple(client->webrtcbin, "sink_%u");
    gst_pad_link(q_src, w_sink);
    gst_object_unref(q_src); gst_object_unref(w_sink);

    GstWebRTCRTPTransceiver *trans = NULL;
    g_signal_emit_by_name(client->webrtcbin, "add-transceiver", 
                          GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL, &trans);
    if (trans) gst_object_unref(trans);

    client->tee_pad = gst_element_request_pad_simple(tee, "src_%u");
    GstPad *q_sink = gst_element_get_static_pad(client->queue, "sink");
    gst_pad_link(client->tee_pad, q_sink);
    gst_object_unref(q_sink);

    g_signal_connect(client->webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), client);

    gst_element_sync_state_with_parent(client->queue);
    gst_element_sync_state_with_parent(client->webrtcbin);

    g_hash_table_insert(clients, g_strdup(peer_id), client);
    return client;
}

static void remove_client(const gchar *peer_id) {
    gpointer key, value;
    if (g_hash_table_lookup_extended(clients, peer_id, &key, &value)) {
        WebRTCClient *client = (WebRTCClient *)value;
        gst_element_set_state(client->webrtcbin, GST_STATE_NULL);
        gst_element_set_state(client->queue, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(pipeline), client->webrtcbin);
        gst_bin_remove(GST_BIN(pipeline), client->queue);
        gst_element_release_request_pad(tee, client->tee_pad);
        g_hash_table_remove(clients, peer_id);
    }
}

// --- SEÑALIZACIÓN ---
static void on_ws_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type, GBytes *message, gpointer user_data) {
    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, size, NULL)) { g_object_unref(parser); return; }
    
    JsonObject *msg = json_node_get_object(json_parser_get_root(parser));
    const gchar *m_type = json_object_get_string_member(msg, "type");
    const gchar *peer_id = json_object_has_member(msg, "peer_id") ? json_object_get_string_member(msg, "peer_id") : "default";

    if (g_strcmp0(m_type, "ready") == 0) {
        remove_client(peer_id);
        WebRTCClient *c = create_client(peer_id);
        g_usleep(100000); 
        GstPromise *p = gst_promise_new_with_change_func(on_offer_created, c, NULL);
        g_signal_emit_by_name(c->webrtcbin, "create-offer", NULL, p);
    } else if (g_strcmp0(m_type, "answer") == 0) {
        WebRTCClient *c = g_hash_table_lookup(clients, peer_id);
        if (c) {
            GstSDPMessage *sdp; gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer((guint8 *)json_object_get_string_member(msg, "sdp"), -1, sdp);
            GstWebRTCSessionDescription *ans = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
            g_signal_emit_by_name(c->webrtcbin, "set-remote-description", ans, NULL);
            gst_webrtc_session_description_free(ans);
        }
    } else if (g_strcmp0(m_type, "ice") == 0) {
        WebRTCClient *c = g_hash_table_lookup(clients, peer_id);
        if (c) g_signal_emit_by_name(c->webrtcbin, "add-ice-candidate", 
                                     json_object_get_int_member(msg, "sdpMLineIndex"), 
                                     json_object_get_string_member(msg, "candidate"));
    }
    g_object_unref(parser);
}

static void on_ws_connected(SoupSession *s, GAsyncResult *res, gpointer user_data) {
    websocket = soup_session_websocket_connect_finish(s, res, NULL);
    if (websocket) {
        g_signal_connect(websocket, "message", G_CALLBACK(on_ws_message), NULL);
        JsonObject *j = json_object_new(); json_object_set_string_member(j, "type", "gstreamer");
        gchar *m = object_to_json(j); soup_websocket_connection_send_text(websocket, m);
        g_free(m); json_object_unref(j);
    }
}

// --- MAIN ---
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);
    
    // Hash table con destructores automáticos
    clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    pipeline = gst_pipeline_new("pipeline");
    ximagesrc = gst_element_factory_make("ximagesrc", "src");
    g_object_set(ximagesrc, "use-damage", FALSE, "do-timestamp", TRUE, "show-pointer", TRUE, NULL);

    capsfilter = gst_element_factory_make("capsfilter", "fps");
    GstCaps *caps = gst_caps_from_string("video/x-raw,framerate=30/1");
    g_object_set(capsfilter, "caps", caps, NULL); gst_caps_unref(caps);

    queue_elem = gst_element_factory_make("queue", "q_main");
    postproc = gst_element_factory_make("vaapipostproc", "pp");
    encoder = gst_element_factory_make("vaapih264enc", "enc");
    if (encoder) {
        g_object_set(encoder, "max-bframes", 0, "bitrate", 4000, "rate-control", 2, NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", "enc");
    }

    parser_elem = gst_element_factory_make("h264parse", "parse");
    payloader = gst_element_factory_make("rtph264pay", "pay");
    g_object_set(payloader, "config-interval", 1, "pt", 96, "ssrc", 1337, NULL);

    GstElement *rtpcaps = gst_element_factory_make("capsfilter", "rtpcaps");
    GstCaps *v_caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96");
    g_object_set(rtpcaps, "caps", v_caps, NULL); gst_caps_unref(v_caps);

    tee = gst_element_factory_make("tee", "tee");
    g_object_set(tee, "allow-not-linked", TRUE, NULL); 

    gst_bin_add_many(GST_BIN(pipeline), ximagesrc, capsfilter, queue_elem, postproc, encoder, parser_elem, payloader, rtpcaps, tee, NULL);
    gst_element_link_many(ximagesrc, capsfilter, queue_elem, postproc, encoder, parser_elem, payloader, rtpcaps, tee, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_timeout_add_seconds(1, check_monitor, NULL);

    SoupSession *sess = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", "ws://localhost:8000/ws");
    soup_session_websocket_connect_async(sess, msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL, (GAsyncReadyCallback)on_ws_connected, NULL);

    g_main_loop_run(loop);
    return 0;
}