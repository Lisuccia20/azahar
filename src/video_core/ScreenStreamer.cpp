#include "video_core/ScreenStreamer.h"
#include "input_common/main.h"
#include "input_common/udp/remote_switch.h"
#include "video_core/gpu.h"
#include "core/core.h"
#include "video_core/renderer_base.h"
#include "core/frontend/emu_window.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include "common/settings.h"

#include <atomic>
#include <thread>
#include <iostream>
#include <cerrno>
#include <vector>
#include <cstring>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close(s)  closesocket(s)
  typedef SOCKET socket_t;
#else
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <unistd.h>
  typedef int socket_t;
  #define closesocket(s) close(s)
  #define INVALID_SOCKET (-1)
#endif

/* ------------------------------------------------------------------ */
/*  PACKETS                                                             */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
struct TouchPacket  { uint8_t type; uint16_t x; uint16_t y; uint32_t ts; };
struct ButtonPacket { uint8_t type; uint8_t id; int8_t value; uint8_t padding; };
struct StickPacket  { uint8_t type; int16_t lx; int16_t ly; int16_t rx; int16_t ry; };
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/*  GLOBAL GST STATE                                                    */
/* ------------------------------------------------------------------ */
static GstElement* pipeline    = nullptr;
static GstElement* appsrc      = nullptr;
static GstElement* webrtcbin   = nullptr;
static GstElement* pay         = nullptr;
static GMainLoop*  gst_loop    = nullptr;

static GstElement* directPipeline = nullptr;
static GstElement* directAppsrc   = nullptr;

/* ------------------------------------------------------------------ */
/*  DISCOVERY                                                           */
/* ------------------------------------------------------------------ */
static std::atomic<bool> discoveryRunning{false};

static std::string getLocalIP() {
    char buf[INET_ADDRSTRLEN] = "0.0.0.0";
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return buf;

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);

    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, (sockaddr*)&local, &len) == 0)
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    }
    closesocket(s);
    return buf;
}

static void startDiscovery(uint16_t /*sigPort*/) {
    discoveryRunning = true;
    std::thread([] {
        socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));

        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = htons(0);
        local.sin_addr.s_addr = INADDR_ANY;
        bind(s, (sockaddr*)&local, sizeof(local));

        sockaddr_in bcast255{};
        bcast255.sin_family = AF_INET;
        bcast255.sin_port   = htons(5001);
        inet_pton(AF_INET, "255.255.255.255", &bcast255.sin_addr);

        std::string localIp = getLocalIP();
        std::string subnetBcast = localIp.substr(0, localIp.rfind('.') + 1) + "255";
        sockaddr_in bcastSubnet{};
        bcastSubnet.sin_family = AF_INET;
        bcastSubnet.sin_port   = htons(5001);
        inet_pton(AF_INET, subnetBcast.c_str(), &bcastSubnet.sin_addr);

        std::cerr << "[Discovery] broadcasting to 255.255.255.255 and "
                  << subnetBcast << " on port 5001\n";

        while (discoveryRunning) {
            sendto(s, "WBRT_HERE", 9, 0, (sockaddr*)&bcast255,    sizeof(bcast255));
            sendto(s, "WBRT_HERE", 9, 0, (sockaddr*)&bcastSubnet, sizeof(bcastSubnet));
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        closesocket(s);
    }).detach();
}

static void stopDiscovery() { discoveryRunning = false; }

/* ------------------------------------------------------------------ */
/*  WEBRTC CALLBACKS                                                    */
/* ------------------------------------------------------------------ */
struct IceData { int mline; std::string candidate; };

static gboolean add_ice_safe(gpointer user_data) {
    auto* d = static_cast<IceData*>(user_data);
    g_signal_emit_by_name(webrtcbin, "add-ice-candidate",
                          (guint)d->mline, d->candidate.c_str());
    delete d;
    return FALSE;
}

static void on_ice_candidate(GstElement*, guint mline,
                              gchar* candidate, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);
    if (!candidate || self->sock < 0) return;

    std::string msg = "ICE|" + std::to_string(mline) + "|0|" + candidate;
    sendto(self->sock, msg.data(), (int)msg.size(), 0,
           (sockaddr*)&self->clientAddr, self->clientLen);
}

static void on_answer_created(GstPromise* promise, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);

    const GstStructure* reply = gst_promise_get_reply(promise);
    if (!reply) { gst_promise_unref(promise); return; }

    GstWebRTCSessionDescription* answer = nullptr;
    gst_structure_get(reply, "answer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
    if (!answer) { gst_promise_unref(promise); return; }

    GstPromise* lp = gst_promise_new();
    g_signal_emit_by_name(webrtcbin, "set-local-description", answer, lp);
    gst_promise_interrupt(lp);
    gst_promise_unref(lp);

    gchar* sdp = gst_sdp_message_as_text(answer->sdp);
    if (sdp) {
        std::vector<uint8_t> buf(4 + strlen(sdp));
        memcpy(buf.data(),     "RBWB", 4);
        memcpy(buf.data() + 4, sdp,   strlen(sdp));
        sendto(self->sock, (const char*)buf.data(), (int)buf.size(), 0,
               (sockaddr*)&self->clientAddr, self->clientLen);
        g_free(sdp);
    }

    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);
    self->answer_pending = false;
}

static void on_remote_desc_set(GstPromise* promise, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);
    gst_promise_unref(promise);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstPromise* ap = gst_promise_new_with_change_func(on_answer_created, self, nullptr);
    g_signal_emit_by_name(webrtcbin, "create-answer", nullptr, ap);
}

void ScreenStreamer::handleOffer(const std::string& sdp) {
    if (answer_pending) return;
    stopDiscovery();

    GstSDPMessage* msg = nullptr;
    if (gst_sdp_message_new_from_text(sdp.c_str(), &msg) != GST_SDP_OK) return;

    auto* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, msg);
    answer_pending = true;
    GstPromise* p = gst_promise_new_with_change_func(on_remote_desc_set, this, nullptr);
    g_signal_emit_by_name(webrtcbin, "set-remote-description", offer, p);
    gst_webrtc_session_description_free(offer);
}

static void on_connection_state_changed(GstElement* element, GParamSpec*, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);
    GstWebRTCPeerConnectionState state;
    g_object_get(element, "connection-state", &state, nullptr);

    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED ||
        state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED       ||
        state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
        self->handleClientDisconnect();
    }
}

void ScreenStreamer::handleClientDisconnect() {
    std::cerr << "[GST] Client disconnected -> resetting...\n";
    answer_pending = false;
    resetWebRTCSession();
    startDiscovery(5001);
}

void ScreenStreamer::resetWebRTCSession() {
    answer_pending = false;
    webrtc_alive   = false;

    g_signal_handlers_disconnect_by_data(webrtcbin, this);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_get_state(pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    gst_bin_remove(GST_BIN(pipeline), webrtcbin);

    webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");
    g_object_set(webrtcbin,
                 "stun-server",   "stun:stun.l.google.com:19302",
                 "bundle-policy", 3,
                 "latency",       50,
                 nullptr);
    gst_bin_add(GST_BIN(pipeline), webrtcbin);

    GstPad* srcpad  = gst_element_get_static_pad(pay, "src");
    GstPad* sinkpad = gst_element_request_pad_simple(webrtcbin, "sink_%u");
    gst_pad_link(srcpad, sinkpad);
    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);

    g_signal_connect(webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), this);
    g_signal_connect(webrtcbin, "notify::connection-state",
                     G_CALLBACK(on_connection_state_changed), this);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    webrtc_alive = true;
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

/* ------------------------------------------------------------------ */
/*  GST INIT                                                            */
/* ------------------------------------------------------------------ */
void ScreenStreamer::initGStreamer(uint16_t /*port*/) {
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);

    gst_loop = g_main_loop_new(nullptr, FALSE);
    std::thread([] { g_main_loop_run(gst_loop); }).detach();

    pipeline  = gst_pipeline_new("streamer");
    appsrc    = gst_element_factory_make("appsrc",       "appsrc");
    auto* conv= gst_element_factory_make("videoconvert", "conv");
    auto* enc = gst_element_factory_make("x264enc",      "enc");
    pay       = gst_element_factory_make("rtph264pay",   "pay");
    webrtcbin = gst_element_factory_make("webrtcbin",    "webrtc");

    GstCaps* src_caps = gst_caps_new_simple("video/x-raw",
        "format",    G_TYPE_STRING,     "BGRA",
        "width",     G_TYPE_INT,        400,
        "height",    G_TYPE_INT,        240,
        "framerate", GST_TYPE_FRACTION, 60, 1,
        nullptr);

    g_object_set(appsrc,
                 "caps",         src_caps,
                 "is-live",      TRUE,
                 "format",       GST_FORMAT_TIME,
                 "do-timestamp", FALSE,
                 "block",        FALSE,
                 "max-bytes",    (guint64)(400 * 240 * 4 * 2),
                 "emit-signals", FALSE,
                 nullptr);
    gst_caps_unref(src_caps);

    g_object_set(enc,
                 "tune",           0x00000004,
                 "speed-preset",   1,
                 "bitrate",        2500,
                 "key-int-max",    30,
                 "bframes",        0,
                 "threads",        4,
                 "sync-lookahead", 0,
                 "byte-stream",    TRUE,
                 "sliced-threads", TRUE,
                 nullptr);
    gst_util_set_object_arg(G_OBJECT(enc), "profile", "baseline");
    g_object_set(pay,      "config-interval", 1, "pt", 96, nullptr);
    g_object_set(webrtcbin,
                 "stun-server", "stun:stun.l.google.com:19302",
                 "latency",     50,
                 nullptr);

    gst_bin_add_many(GST_BIN(pipeline), appsrc, conv, enc, pay, webrtcbin, nullptr);
    gst_element_link_many(appsrc, conv, enc, pay, nullptr);

    GstPad* srcpad  = gst_element_get_static_pad(pay, "src");
    GstPad* sinkpad = gst_element_request_pad_simple(webrtcbin, "sink_%u");
    gst_pad_link(srcpad, sinkpad);
    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);

    g_signal_connect(webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), this);
    g_signal_connect(webrtcbin, "notify::connection-state",
                     G_CALLBACK(on_connection_state_changed), this);

    gst_element_set_state(pipeline, GST_STATE_READY);
}

/* ------------------------------------------------------------------ */
/*  DIRECT MODE                                                         */
/* ------------------------------------------------------------------ */
void ScreenStreamer::stopDirectMode() {
    if (!directMode && !directPipeline) return;

    directMode = false;

    Settings::values.layout_option.SetValue(previous_layout);
    system->GPU().Renderer().UpdateCurrentFramebufferLayout();

    if (directAppsrc)
        gst_app_src_end_of_stream(GST_APP_SRC(directAppsrc));

    if (directPipeline) {
        gst_element_set_state(directPipeline, GST_STATE_NULL);
        gst_element_get_state(directPipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
        gst_object_unref(directPipeline);
        directPipeline = nullptr;
        directAppsrc   = nullptr;
    }

    directFrameCount = 0;
}

void ScreenStreamer::handleDirectClient(const std::string& clientIp, uint16_t rtpPort) {
    std::cerr << "[Stream] handleDirectClient chiamato ip=" << clientIp << " port=" << rtpPort << "\n";

    if (!system || !system->IsPoweredOn()) {
        std::cerr << "[Stream] SKIP: sistema non attivo\n";
        return;
    }

    stopDirectMode();

    std::cerr << "[Streaming] Avvio connessione per " << clientIp << ":" << rtpPort << "\n";

    // ✅ Nessun cambio layout — Mac resta su SingleScreen (top screen)
    // Il bottom screen viene letto direttamente da screen_infos[2].texture

    const char* encoder_name = "";
#if defined(__APPLE__)
    encoder_name = "vtenc_h264"; // VideoToolbox (Hardware Mac)
#elif defined(__linux__)
    encoder_name = "vaapih264enc"; // VA-API (Hardware Linux/SteamOS)
#elif defined(_WIN32)
    // Su Windows, d3d11h264enc è solitamente il migliore per latenza e compatibilità GPU
    encoder_name = "d3d11h264enc";
#endif

    directPipeline  = gst_pipeline_new("direct");
    directAppsrc    = gst_element_factory_make("appsrc",       "d_src");
    auto* queue     = gst_element_factory_make("queue",        "d_queue");
    auto* conv      = gst_element_factory_make("videoconvert", "d_conv");
    auto* flip      = gst_element_factory_make("videoflip",    "d_flip");
    auto* filter    = gst_element_factory_make("capsfilter",   "d_filter");
    auto* enc       = gst_element_factory_make( encoder_name,      "d_enc");
    auto* d_pay     = gst_element_factory_make("rtph264pay",   "d_pay");
    auto* udpsink   = gst_element_factory_make("udpsink",      "d_udp");

    if (!directPipeline || !directAppsrc || !queue || !conv ||
        !flip || !filter || !enc || !d_pay || !udpsink) {
        std::cerr << "[Streaming] Errore creazione elementi\n";
        return;
    }

    g_object_set(directAppsrc,
                 "format",       GST_FORMAT_TIME,
                 "is-live",      TRUE,
                 "do-timestamp", TRUE,
                 nullptr);

    // Leaky queue: droppa i frame vecchi se l'encoder è in ritardo
    g_object_set(queue,
                 "max-size-buffers", 1,
                 "max-size-bytes",   0,
                 "max-size-time",    0,
                 "leaky",            2,
                 nullptr);

    // ✅ Ruota 90° CW: la texture è 240x320 (portrait), la Switch vuole 320x240 (landscape)
    g_object_set(flip, "method", 1, nullptr);

    // Sostituisci la tua parte dei caps con questa:
    GstCaps* f_caps = nullptr;
    if (encoder_name == std::string("vaapih264enc")) {
        // NV12 è il formato nativo preferito da VA-API su AMD
        f_caps = gst_caps_from_string("video/x-raw,format=NV12");
    } else {
        // I420 per x264enc e altri
        f_caps = gst_caps_from_string("video/x-raw,format=I420");
    }
    g_object_set(filter, "caps", f_caps, nullptr);
    gst_caps_unref(f_caps);

    // ... dopo aver creato 'enc' con la logica condizionale ...

    if (encoder_name == std::string("vtenc_h264")) {
        // --- Configurazione Apple (VideoToolbox) ---
        g_object_set(enc,
                    "bitrate", 1500,
                    "allow-frame-reordering", FALSE, // Fondamentale per latenza (no B-frames)
                    "realtime", TRUE,
                    "max-keyframe-interval", 60,
                    nullptr);
    }
    else if (encoder_name == std::string("vaapih264enc")) {
        // --- Configurazione Linux (VA-API / Steam Deck) ---
        g_object_set(enc,
                    "bitrate", 1500,
                    nullptr);
        gst_util_set_object_arg(G_OBJECT(enc), "rate-control", "cbr");
    }
    else if (encoder_name == std::string("d3d11h264enc")) {
        // --- Configurazione Windows (D3D11) ---
        g_object_set(enc,
                    "bitrate", 1500,
                    "gop-size", 30,
                    nullptr);
        gst_util_set_object_arg(G_OBJECT(enc), "rc-mode", "cbr");
        gst_util_set_object_arg(G_OBJECT(enc), "tune", "zerolatency");
    }
    else {
        // --- Fallback Software (x264enc o avenc_h264) ---
        // Se è x264enc, manteniamo i tuoi parametri originali
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "tune")) {
            g_object_set(enc,
                        "tune", 4, // zerolatency
                        "speed-preset", 1, // ultrafast
                        "bitrate", 1500,
                        "key-int-max", 30,
                        "bframes", 0,
                        "byte-stream", TRUE,
                        nullptr);
        }
    }
    gst_util_set_object_arg(G_OBJECT(enc), "profile", "baseline");

    g_object_set(d_pay, "config-interval", 1, "pt", 96, nullptr);

    g_object_set(udpsink,
                 "host", clientIp.c_str(),
                 "port", (int)rtpPort,
                 "sync", FALSE,
                 nullptr);

    gst_bin_add_many(GST_BIN(directPipeline),
                     directAppsrc, queue, conv, flip, filter, enc, d_pay, udpsink, nullptr);
    // Ordine logico: Sorgente -> Coda -> Ruota -> Converti in YUV -> Filtra -> Codifica
    gst_element_link_many(directAppsrc, queue, flip, conv, filter, enc, d_pay, udpsink, nullptr);
    directFrameCount = 0;
    directMode       = true;


    previous_layout = Settings::values.layout_option.GetValue();
    Settings::values.layout_option.SetValue(Settings::LayoutOption::SingleScreen);
    system->GPU().Renderer().UpdateCurrentFramebufferLayout();

    GstStateChangeReturn ret = gst_element_set_state(directPipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[Streaming] Errore critico: GStreamer non parte\n";
        directMode = false;
    } else {
        std::cerr << "[Streaming] Pipeline in PLAYING. In attesa di frame...\n";
    }
}

void ScreenStreamer::handleTouch(uint8_t type, uint16_t x, uint16_t y) {

    if (!system) {
        std::cerr << "[Touch] SKIP: system è null\n";
        return;
    }

    Frontend::EmuWindow* window = system->GetEmuWindow();
    if (!window) {
        std::cerr << "[Touch] SKIP: window è null\n";
        return;
    }

    const auto& layout = window->GetFramebufferLayout();
    const auto& bs     = layout.bottom_screen;

    if (type == 2) {
        std::cerr << "[Touch] TouchReleased\n";
        window->TouchReleased();
        return;
    }

    const float scale_x = static_cast<float>(bs.GetWidth())  / 320.0f;
    const float scale_y = static_cast<float>(bs.GetHeight()) / 240.0f;
    const unsigned fx = static_cast<unsigned>(bs.left + x * scale_x);
    const unsigned fy = static_cast<unsigned>(bs.top  + y * scale_y);

    if (type == 0) {
        std::cerr << "[Touch] TouchPressed\n";
        window->TouchPressed(fx, fy);
    } else if (type == 1) {
        std::cerr << "[Touch] TouchMoved\n";
        window->TouchMoved(fx, fy);
    }
}

void ScreenStreamer::handleButton(uint8_t type, uint8_t id, int8_t value) {
    if (type != 0) return;
    auto remote = InputCommon::GetRemoteSwitch();
    if (remote) remote->SetButtonState(id, value != 0);
}

void ScreenStreamer::handleStick(int16_t lx, int16_t ly) {
    auto remote = InputCommon::GetRemoteSwitch();
    if (remote) remote->SetStickState(lx / 32767.0f, ly / 32767.0f);
}

/* ------------------------------------------------------------------ */
/*  FRAME PUSH                                                          */
/* ------------------------------------------------------------------ */
void ScreenStreamer::sendFrame(const void* data, int w, int h) {
    if (!data || !directMode || !directAppsrc) return;

    static int last_w = 0, last_h = 0;
    if (w != last_w || h != last_h) {
        GstCaps* caps = gst_caps_new_simple("video/x-raw",
            "format",             G_TYPE_STRING,     "BGRA",
            "width",              G_TYPE_INT,        w,
            "height",             G_TYPE_INT,        h,
            "framerate",          GST_TYPE_FRACTION, 60, 1,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, 1,  1,
            nullptr);
        gst_app_src_set_caps(GST_APP_SRC(directAppsrc), caps);
        gst_caps_unref(caps);
        last_w = w;
        last_h = h;
        std::cerr << "[Streaming] Caps: " << w << "x" << h << "\n";
    }

    const size_t size = (size_t)w * h * 4;

    // Zero-copy: GStreamer punta direttamente allo staging buffer VMA
    // notify=nullptr perché la memoria VMA vive più a lungo del buffer GStreamer
    // e il double buffering garantisce che non venga sovrascritta finché
    // GStreamer non ha finito (la fence del slot successivo lo assicura)
    GstBuffer* buf = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        const_cast<void*>(data),
        size,
        0,
        size,
        nullptr,
        nullptr
    );

    const GstClockTime duration = gst_util_uint64_scale(1, GST_SECOND, 60);
    GST_BUFFER_PTS(buf)      = directFrameCount * duration;
    GST_BUFFER_DTS(buf)      = GST_BUFFER_PTS(buf);
    GST_BUFFER_DURATION(buf) = duration;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(directAppsrc), buf);
    if (ret == GST_FLOW_OK) {
        directFrameCount++;
    } else {
        std::cerr << "[Streaming] Errore push: " << gst_flow_get_name(ret) << "\n";
    }
}

/* ------------------------------------------------------------------ */
/*  CONSTRUCTOR / DESTRUCTOR                                            */
/* ------------------------------------------------------------------ */
ScreenStreamer::ScreenStreamer(uint16_t port, Core::System* system)
    : system(system)
    , running(true)
    , sock(INVALID_SOCKET)
    , touchSock(INVALID_SOCKET)
    , inputSock(INVALID_SOCKET)
    , answer_pending(false)
    , clientLen(sizeof(clientAddr))
{
    memset(&clientAddr, 0, sizeof(clientAddr));

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    auto setup_socket = [](int p) -> socket_t {
        socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;

        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#ifndef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));
#endif
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(p);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[NX] bind failed on port " << p
                      << " (errno: " << errno << ")\n";
            closesocket(s);
            return INVALID_SOCKET;
        }
        return s;
    };

    sock      = setup_socket(port);
    touchSock = setup_socket(5002);
    inputSock = setup_socket(5003);

    startDiscovery(port);
    initGStreamer(port);

    // ---- Signaling thread ------------------------------------------
    std::thread([this] {
        char buf[65536];
        while (running) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom((socket_t)sock, buf, sizeof(buf) - 1, 0,
                             (sockaddr*)&from, &fromLen);
            if (n <= 0) continue;
            buf[n] = '\0';

            std::cerr << "[NX] ricevuto " << n << " bytes: "
                      << std::string(buf, std::min(n, 20)) << "\n";

            memcpy(&clientAddr, &from, sizeof(from));
            clientLen = fromLen;

            if (n >= 3 && memcmp(buf, "BYE", 3) == 0) {
                handleClientDisconnect();

            } else if (n >= 9 && memcmp(buf, "NX_DIRECT", 9) == 0) {
                sendto((socket_t)sock, "NX_OK", 5, 0,
                       (sockaddr*)&clientAddr, clientLen);
                char clientIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, clientIp, sizeof(clientIp));
                pending_ip   = std::string(clientIp);
                pending_port = 5004;
                if (this->system && this->system->IsPoweredOn()) {
                    handleDirectClient(pending_ip, pending_port);
                } else {
                    std::cerr << "[Stream] Connessione ricevuta, in attesa del gioco...\n";
                    direct_pending = true;
                }

            } else if (n > 10 && memcmp(buf, "WBRT_OFFER", 10) == 0) {
                handleOffer(std::string(buf + 10, n - 10));

            } else if (n > 4 && memcmp(buf, "ICE|", 4) == 0) {
                std::string msg(buf, n);
                auto s1 = msg.find('|', 4);
                auto s2 = msg.find('|', s1 + 1);
                if (s1 != std::string::npos && s2 != std::string::npos) {
                    int mline        = std::stoi(msg.substr(4, s1 - 4));
                    std::string cand = msg.substr(s2 + 1);
                    g_main_context_invoke(
                        g_main_loop_get_context(gst_loop),
                        add_ice_safe,
                        new IceData{mline, cand});
                }
            }
        }
    }).detach();

    std::thread([this] {
        std::cerr << "[Input] Thread unificato avviato su porta 5003\n";
        uint8_t buf[256];
        while (running) {
            int n = recv((socket_t)inputSock, (char*)buf, sizeof(buf), 0);
            if (n <= 0) continue;

            if (n == (int)sizeof(ButtonPacket)) {
                ButtonPacket p;
                memcpy(&p, buf, sizeof(p));
                handleButton(p.type, p.id, p.value);
                std::cerr << "[Input] ButtonPacket type=" << (int)p.type
                          << " id=" << (int)p.id
                          << " value=" << (int)p.value << "\n";
            }
            else if (n == (int)sizeof(StickPacket)) {
                if (buf[0] == 0x03) {
                    StickPacket p;
                    memcpy(&p, buf, sizeof(p));
                    handleStick(p.lx, p.ly);
                } else {
                    TouchPacket p;
                    memcpy(&p, buf, sizeof(p));
                    handleTouch(p.type, p.x, p.y);
                }
            }
        }
    }).detach();
}

ScreenStreamer::~ScreenStreamer() {
    running = false;

    if (sock      != INVALID_SOCKET) closesocket((socket_t)sock);
    if (inputSock != INVALID_SOCKET) closesocket((socket_t)inputSock);

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    if (gst_loop) {
        g_main_loop_quit(gst_loop);
        g_main_loop_unref(gst_loop);
    }

    stopDirectMode();
    stopDiscovery();

#ifdef _WIN32
    WSACleanup();
#endif
}

void ScreenStreamer::OnGameStarted() {
    if (direct_pending) {
        direct_pending = false;
        std::cerr << "[Stream] Gioco avviato, avvio stream pendente\n";
        handleDirectClient(pending_ip, pending_port);
    }
}

void ScreenStreamer::OnGameStopped() {
    std::cerr << "[Stream] Gioco fermato, stop stream\n";
    stopDirectMode();
    // direct_pending rimane true se era pendente,
    // così al prossimo gioco parte automaticamente
}