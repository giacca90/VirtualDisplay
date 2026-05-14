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
// Estructura para almacenar la geometría (posición y tamaño) de la pantalla.
typedef struct { int x,y,w,h; } Geometry;

// Estructura para mantener el estado de cada cliente WebRTC conectado.
typedef struct {
    gchar *peer_id;        // ID único del cliente (peer).
    GstElement *queue;     // Elemento de cola GStreamer para el cliente.
    GstElement *webrtcbin; // Elemento WebRTCBin para la comunicación con el cliente.
    GstPad *tee_pad;       // Pad del elemento 'tee' para enviar datos a este cliente.
} WebRTCClient;

// --- GLOBALES ---
// Bucle principal de GLib para manejar eventos.
static GMainLoop *loop;
// Elementos GStreamer globales de la pipeline.
static GstElement *pipeline, *ximagesrc, *capsfilter, *queue_elem, *postproc, *encoder, *parser_elem, *payloader, *tee;
// Conexión WebSocket para la señalización.
static SoupWebsocketConnection *websocket = NULL;
// Tabla hash para almacenar los clientes WebRTC conectados.
static GHashTable *clients; 
// Geometría de la pantalla virtual capturada previamente.
static Geometry prev_geom = {0,0,0,0};
// Frecuencia de cuadros actual (FPS).
static gint current_fps = 60; // FPS iniciales

// --- PROTOTIPOS (Para evitar errores de orden) ---
// Callback que se ejecuta cuando se crea una oferta SDP.
static void on_offer_created(GstPromise *promise, gpointer user_data);
static void on_ice_candidate(GstElement *webrtcbin, guint mline, gchar *candidate, gpointer user_data);
// Crea un nuevo cliente WebRTC y lo añade a la tabla de clientes.
static WebRTCClient* create_client(const gchar *peer_id);
// Elimina un cliente WebRTC de la tabla de clientes.
static void remove_client(const gchar *peer_id);
// Ajusta la velocidad de cuadros (framerate) de la captura de video.
static void adjust_framerate(gint delta);

// --- UTILIDADES ---
// Convierte un objeto JsonObject a una cadena JSON.
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
// Función para disparar la renegociación de SDP para un cliente específico.
static void trigger_renegotiation(gpointer key, gpointer value, gpointer user_data) {
    WebRTCClient *client = (WebRTCClient *)value;
    GstPromise *p = gst_promise_new_with_change_func(on_offer_created, client, NULL);
    g_signal_emit_by_name(client->webrtcbin, "create-offer", NULL, p);
}

// Comprueba si la geometría de la pantalla virtual ha cambiado y ajusta ximagesrc si es necesario.
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
// Se llama cuando GStreamer ha creado una oferta SDP.
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

// Se llama cuando GStreamer ha encontrado un candidato ICE.
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
// Crea un nuevo cliente WebRTC, inicializa sus elementos GStreamer y lo añade a la pipeline.
static WebRTCClient* create_client(const gchar *peer_id) {
    WebRTCClient *client = g_new0(WebRTCClient, 1);
    client->peer_id = g_strdup(peer_id);

    client->queue = gst_element_factory_make("queue", NULL);
    g_object_set(client->queue,
        "max-size-buffers", 1,
        "max-size-bytes", 0,
        "max-size-time", 20000000,
        "flush-on-eos", TRUE,
        NULL);
    client->webrtcbin = gst_element_factory_make("webrtcbin", NULL);
    g_object_set(client->webrtcbin, "bundle-policy", 3, "latency", 0, NULL);

    gst_bin_add_many(GST_BIN(pipeline), client->queue, client->webrtcbin, NULL);

    GstPad *q_src = gst_element_get_static_pad(client->queue, "src");
    GstPad *w_sink = gst_element_request_pad_simple(client->webrtcbin, "sink_%u");
    gst_pad_link(q_src, w_sink);
    gst_object_unref(q_src); gst_object_unref(w_sink);

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

// Elimina un cliente WebRTC existente, liberando sus recursos GStreamer.
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

// Ajusta la tasa de bits (bitrate) del codificador de video.
static void adjust_bitrate(gint delta) {
    if (!encoder) return;
    gint current_bitrate;
    g_object_get(encoder, "bitrate", &current_bitrate, NULL);
    // Aumentamos el mínimo a 4000kbps (4Mbps) para 60fps, máximo 20Mbps
    gint new_bitrate = CLAMP(current_bitrate + delta, 4000, 20000);
    g_object_set(encoder, "bitrate", new_bitrate, NULL);
    g_print("Bitrate ajustado a: %d kbps\n", new_bitrate);
}

// Ajusta la velocidad de cuadros (framerate) del filtro de caps.
static void adjust_framerate(gint delta) {
    if (!capsfilter) return;
    gint new_fps = CLAMP(current_fps + delta, 30, 60);
    if (new_fps == current_fps) return;

    current_fps = new_fps;
    gchar *caps_str = g_strdup_printf("video/x-raw,framerate=%d/1,format=BGRx", current_fps);
    GstCaps *new_caps = gst_caps_from_string(caps_str);
    g_free(caps_str);
    g_object_set(capsfilter, "caps", new_caps, NULL);
    gst_caps_unref(new_caps);
    g_print("🎬 Framerate adaptativo: %d fps\n", current_fps);
}

// Maneja los mensajes recibidos a través de la conexión WebSocket.
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
    } else if (g_strcmp0(m_type, "remove") == 0) {
        remove_client(peer_id);
        g_print("🗑️ Cliente eliminado por orden del servidor: %s\n", peer_id);
    } else if (g_strcmp0(m_type, "answer") == 0) {
        WebRTCClient *c = g_hash_table_lookup(clients, peer_id);
        if (c) {
            GstSDPMessage *sdp; gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer((guint8 *)json_object_get_string_member(msg, "sdp"), -1, sdp);
            GstWebRTCSessionDescription *ans = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
            g_signal_emit_by_name(c->webrtcbin, "set-remote-description", ans, NULL);
            gst_webrtc_session_description_free(ans);
        }
    } else if (g_strcmp0(m_type, "quality") == 0) {
        const gchar *action = json_object_get_string_member(msg, "action");
        const gchar *reason = json_object_has_member(msg, "reason") ? json_object_get_string_member(msg, "reason") : "unknown";
        
        gint current_bitrate;
        g_object_get(encoder, "bitrate", &current_bitrate, NULL);

        if (g_strcmp0(action, "raise") == 0) {
            // Si hay más de 200kb por frame, subimos fluidez (FPS) en lugar de bitrate
            if ((current_bitrate / current_fps) >= 200) {
                adjust_framerate(10);
            } else {
                adjust_bitrate(1000);
            }
        } 
        else if (g_strcmp0(action, "lower") == 0) {
            // Si ya estamos al mínimo de bitrate (4000kbps), bajamos FPS para reducir carga CPU en cliente
            if (current_bitrate <= 4000) {
                adjust_framerate(-10);
            } else {
                if (g_strcmp0(reason, "CPU/Decodificación") == 0) adjust_bitrate(-2000);
                else adjust_bitrate(-1000);
            }
        }
        
        JsonObject *resp = json_object_new();
        json_object_set_string_member(resp, "type", "quality_ack");
        g_object_get(encoder, "bitrate", &current_bitrate, NULL);
        json_object_set_int_member(resp, "bitrate", current_bitrate);
        gchar *m = object_to_json(resp);
        soup_websocket_connection_send_text(conn, m);
        g_free(m); json_object_unref(resp);
    } else if (g_strcmp0(m_type, "ice") == 0) {
        WebRTCClient *c = g_hash_table_lookup(clients, peer_id);
        if (c) g_signal_emit_by_name(c->webrtcbin, "add-ice-candidate", 
                                     json_object_get_int_member(msg, "sdpMLineIndex"), 
                                     json_object_get_string_member(msg, "candidate"));
    }
    g_object_unref(parser);
}

// Se llama cuando la conexión WebSocket se ha establecido.
static void on_ws_connected(SoupSession *s, GAsyncResult *res, gpointer user_data) {
    websocket = soup_session_websocket_connect_finish(s, res, NULL);
    if (websocket) {
        g_signal_connect(websocket, "message", G_CALLBACK(on_ws_message), NULL);
        JsonObject *j = json_object_new(); json_object_set_string_member(j, "type", "gstreamer");
        gchar *m = object_to_json(j); soup_websocket_connection_send_text(websocket, m);
        g_free(m); json_object_unref(j);
    }
}

// --- FUNCIÓN PRINCIPAL ---
int main(int argc, char *argv[]) {
    // Configura la localización para manejo de caracteres.
    setlocale(LC_ALL, "");
    // Inicializa GStreamer.
    gst_init(&argc, &argv);
    // Crea un nuevo bucle principal de GLib.
    loop = g_main_loop_new(NULL, FALSE);
    
    // Inicializa la tabla hash para almacenar los clientes, con destructores automáticos.
    clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    // Crea la pipeline de GStreamer.
    pipeline = gst_pipeline_new("pipeline");
    // Elemento para capturar la pantalla X11.
    ximagesrc = gst_element_factory_make("ximagesrc", "src");
    // Configura ximagesrc para no usar el mecanismo de daño (damage), capturar remotamente,
    // añadir timestamps y mostrar el puntero del ratón.
    g_object_set(ximagesrc, "use-damage", FALSE, "remote", TRUE, "do-timestamp", TRUE, "show-pointer", TRUE, NULL);

    // Filtro de capacidades para establecer el framerate y el formato de video.
    capsfilter = gst_element_factory_make("capsfilter", "fps");
    // Crea las capacidades para video raw a 60 FPS en formato BGRx.
    GstCaps *caps = gst_caps_from_string("video/x-raw,framerate=60/1,format=BGRx,colorimetry=sRGB");
    g_object_set(capsfilter, "caps", caps, NULL); gst_caps_unref(caps);

    queue_elem = gst_element_factory_make("queue", "q_main");
    g_object_set(queue_elem, 
        "max-size-buffers", 1, 
        "max-size-bytes", 0,
        "max-size-time", 20000000,
        "leaky", 2, 
        "flush-on-eos", TRUE, 
        NULL);

    postproc = gst_element_factory_make("vaapipostproc", "pp");

    encoder = gst_element_factory_make("vaapih264enc", "enc");
    if (encoder) {
        // VAAPI: Forzamos baseline quitando frames B y reduciendo el intervalo entre I-frames
        g_object_set(encoder, 
            "max-bframes", 0, 
            "bitrate", 9000, 
            "rate-control", 2, 
            "keyframe-period", 30,
            "refs", 1,
             NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", "enc");
        if (encoder) {
            // x264enc: tune=zerolatency(4), speed-preset=ultrafast(0)
            g_object_set(encoder, 
                "tune", 4, 
                "speed-preset", 0, 
                "bitrate", 8000, 
                "threads", 1,
                "key-int-max", 30,
                "bframes", 0,
                NULL);
        }
    }

    parser_elem = gst_element_factory_make("h264parse", "parse");
    // El parser es el encargado de extraer los SPS/PPS y meterlos en las CAPS como 'codec_data'
    // Para eso necesitamos forzar el formato 'avc' y alineación 'au' justo después.

    GstElement *h264caps = gst_element_factory_make("capsfilter", "h264caps");
    GstCaps *h_caps = gst_caps_from_string("video/x-h264,stream-format=avc,profile=constrained-baseline");
    g_object_set(h264caps, "caps", h_caps, NULL);
    gst_caps_unref(h_caps);

    payloader = gst_element_factory_make("rtph264pay", "pay");
    // config-interval=-1 para que rtph264pay use el codec_data de las caps
    g_object_set(payloader, 
        "config-interval", 1, 
        "pt", 96, 
        "aggregate-mode", 0,
        NULL);

    tee = gst_element_factory_make("tee", "tee");
    g_object_set(tee, "allow-not-linked", TRUE, NULL); 

    gst_bin_add_many(GST_BIN(pipeline), ximagesrc, capsfilter, queue_elem, postproc, encoder, parser_elem, h264caps, payloader, tee, NULL);
    gst_element_link_many(ximagesrc, capsfilter, queue_elem, postproc, encoder, parser_elem, h264caps, payloader, tee, NULL);

    // Inicializar geometría antes de empezar
    check_monitor(NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_timeout_add_seconds(1, check_monitor, NULL);

    SoupSession *sess = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", "ws://localhost:8000/ws");
    soup_session_websocket_connect_async(sess, msg, NULL, NULL, G_PRIORITY_DEFAULT, NULL, (GAsyncReadyCallback)on_ws_connected, NULL);

    g_main_loop_run(loop);
    return 0;
}