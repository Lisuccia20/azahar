#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <gst/gst.h>
#include "common/settings.h"
#include "core/frontend/framebuffer_layout.h"
#include <functional>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define closesocket closesocket
  typedef int socklen_t;
  typedef SOCKET socket_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int socket_t;
  #define closesocket close
#endif

namespace Core {
    class System;
}

class ScreenStreamer {
public:

  void SetOnConnectedCallback(std::function<void()> cb) {
        on_connected_callback = std::move(cb);
    }
    void SetOnDisconnectedCallback(std::function<void()> cb) {
        on_disconnected_callback = std::move(cb);
    }
    // Costruttore e Distruttore principali
    ScreenStreamer(uint16_t port, Core::System* system);
    ~ScreenStreamer();

    // Funzione principale richiamata dall'emulatore ad ogni frame video
    void sendFrame(const void* bgraData, int width, int height);

    // --- VARIABILI E METODI PUBBLICI PER I CALLBACK STATICI DI GSTREAMER ---
    // Queste variabili devono essere pubbliche perché i thread di GStreamer
    // devono potervi accedere durante la stretta di mano WebRTC.
    socket_t    sock = -1;
    sockaddr_in clientAddr{};
    socklen_t   clientLen = sizeof(sockaddr_in);
    bool        answer_pending = false;

    void handleClientDisconnect();

    void OnGameStarted();
    void OnGameStopped();

    bool IsDirectMode() const { return directMode; }
    std::string directSourceFormat = "RGBA";

private:

  std::function<void()> on_connected_callback;
    std::function<void()> on_disconnected_callback;
    Settings::LayoutOption previous_layout = Settings::LayoutOption::Default;
    Core::System* system = nullptr;
    std::atomic<bool> running{true};

    // Socket per ricevere i comandi
    socket_t touchSock = -1;
    socket_t inputSock = -1;

    // Stato WebRTC
    bool webrtc_alive = false;
    uint64_t webrtcFrameCount = 0;

    // Metodi interni per l'inizializzazione e il signaling WebRTC
    void initGStreamer(uint16_t port);
    void handleOffer(const std::string& sdp);
    void resetWebRTCSession();

    // Metodi interni per la ricezione dell'input (mantiene la logica Citra)
    void handleTouch(uint8_t type, uint16_t x, uint16_t y);
    void handleButton(uint8_t type, uint8_t id, int8_t value);
    void handleStick(int16_t lx, int16_t ly);

    // --- Modalità RTP Diretta (Nintendo Switch Fallback) ---
    std::atomic<bool> directMode{false};
    uint64_t          directFrameCount = 0;

    void handleDirectClient(const std::string& clientIp, uint16_t rtpPort);
    void stopDirectMode();

    // Offset della bottom screen nel framebuffer (aggiornato dal renderer)
    std::atomic<uint32_t> bottom_screen_x{0};
    std::atomic<uint32_t> bottom_screen_y{0};
    std::atomic<uint32_t> bottom_screen_w{320};
    std::atomic<uint32_t> bottom_screen_h{240};

    std::atomic<bool> direct_pending{false};
    std::string pending_ip;
    uint16_t pending_port{0};
};