#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

#include "config/config_loader.h"
#include "capture/dxgi_capturer.h"
#include "encode/nvenc_encoder.h"
#include "encode/encoder_interface.h"
#include "network/udp_transport.h"
#include "input/input_handler.h"

namespace penstream {

static std::atomic<bool> g_running{true};
static HWND g_hwnd_tray = nullptr;
static UINT g_wm_trayicon = WM_USER + 1;
static NOTIFYICONDATAA g_nid = {};

// IDs de menú
#define ID_TRAY_START 1001
#define ID_TRAY_STOP  1002
#define ID_TRAY_EXIT  1003

void signal_handler(int /*signal*/) {
    g_running.store(false);
}

void show_tray_icon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = g_wm_trayicon;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strncpy_s(g_nid.szTip, "PenStream Server - Starting...", _TRUNCATE);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

void update_tray_tip(const char* tip) {
    strncpy_s(g_nid.szTip, tip, _TRUNCATE);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

void remove_tray_icon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hmenu = CreatePopupMenu();
    AppendMenuA(hmenu, MF_STRING, ID_TRAY_START, "Start Streaming");
    AppendMenuA(hmenu, MF_STRING, ID_TRAY_STOP, "Stop Streaming");
    AppendMenuA(hmenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hmenu, MF_STRING, ID_TRAY_EXIT, "Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hmenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hmenu);
}

LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_wm_trayicon) {
        if (lParam == WM_RBUTTONUP) {
            show_tray_menu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            // Doble click: mostrar/ocultar consola
            HWND hconsole = GetConsoleWindow();
            if (hconsole) {
                ShowWindow(hconsole, IsWindowVisible(hconsole) ? SW_HIDE : SW_SHOW);
            }
        }
        return 0;
    }

    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_TRAY_EXIT:
                    g_running.store(false);
                    PostQuitMessage(0);
                    return 0;
            }
            break;
        case WM_DESTROY:
            remove_tray_icon();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void create_tray_window() {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = tray_wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "PenStreamTray";

    RegisterClassExA(&wc);

    g_hwnd_tray = CreateWindowExA(
        0, "PenStreamTray", "PenStream Server",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 200,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );
}

int run() {
    auto config = config::load_config();

    std::cout << "========================================" << std::endl;
    std::cout << "  PenStream Server v1.0.0" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Port: " << config.port << std::endl;
    std::cout << "Target: " << config.width << "x" << config.height << "@" << config.fps << "fps" << std::endl;
    std::cout << "Bitrate: " << config.bitrate_kbps << " kbps" << std::endl;
    std::cout << "Encoder: " << config.encoder << std::endl;
    std::cout << "========================================" << std::endl;
    update_tray_tip("PenStream Server - Initializing...");

    // Inicializar captura DXGI
    capture::DXGICapturer capturer;
    std::cout << "[1/4] Initializing screen capture..." << std::flush;
    if (!capturer.initialize()) {
        std::cerr << " FAILED" << std::endl;
        std::cerr << "Error: Failed to initialize DXGI capturer" << std::endl;
        std::cerr << "Possible causes:" << std::endl;
        std::cerr << "  - No DirectX 11 compatible GPU detected" << std::endl;
        std::cerr << "  - Desktop Duplication API not available (Windows 8+ required)" << std::endl;
        std::cerr << "  - Another application is using exclusive fullscreen" << std::endl;
        update_tray_tip("PenStream Server - Error: Capture failed");
        MessageBoxA(NULL,
            "Failed to initialize screen capture.\n\n"
            "Make sure you have:\n"
            "  - A DirectX 11 compatible GPU\n"
            "  - Windows 8 or later\n"
            "  - No exclusive fullscreen applications running",
            "PenStream Server Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    std::cout << " OK (" << capturer.get_width() << "x" << capturer.get_height() << ")" << std::endl;

    // Inicializar encoder
    encode::NVEncoder encoder;
    encoder.set_d3d_device(capturer.get_device());

    encode::EncodeConfig encode_config;
    encode_config.width      = config.width;
    encode_config.height     = config.height;
    encode_config.bitrate_bps = config.bitrate_kbps * 1000;
    encode_config.fps        = config.fps;
    encode_config.low_latency = config.low_latency;

    std::cout << "[2/4] Initializing NVENC encoder..." << std::flush;
    if (!encoder.initialize(encode_config)) {
        std::cerr << " FAILED" << std::endl;
        std::cerr << "Error: Failed to initialize NVENC encoder" << std::endl;
        std::cerr << "Possible causes:" << std::endl;
        std::cerr << "  - No NVIDIA GPU detected" << std::endl;
        std::cerr << "  - NVIDIA drivers not installed or outdated" << std::endl;
        std::cerr << "  - NVENC SDK not installed" << std::endl;
        std::cerr << "Note: Fallback to AMF (AMD) or QSV (Intel) not yet implemented." << std::endl;
        update_tray_tip("PenStream Server - Error: Encoder failed");
        MessageBoxA(NULL,
            "Failed to initialize NVENC encoder.\n\n"
            "Make sure you have:\n"
            "  - An NVIDIA GPU (GTX 600 series or later)\n"
            "  - Latest NVIDIA drivers installed\n"
            "  - NVIDIA Video Codec SDK installed",
            "PenStream Server Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    std::cout << " OK" << std::endl;

    // Inicializar red UDP
    network::UDPTransport transport(static_cast<uint16_t>(config.port));
    std::cout << "[3/4] Initializing UDP transport..." << std::flush;
    if (!transport.initialize()) {
        std::cerr << " FAILED" << std::endl;
        std::cerr << "Error: Failed to initialize UDP transport on port " << config.port << std::endl;
        std::cerr << "Possible causes:" << std::endl;
        std::cerr << "  - Another application is using port " << config.port << std::endl;
        std::cerr << "  - Firewall blocking UDP connections" << std::endl;
        update_tray_tip("PenStream Server - Error: Network failed");
        MessageBoxA(NULL,
            ("Failed to initialize UDP transport on port " + std::to_string(config.port) + ".\n\n"
            "Make sure:\n"
            "  - No other application is using port " + std::to_string(config.port) + "\n"
            "  - Windows Firewall allows UDP connections").c_str(),
            "PenStream Server Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    std::cout << " OK" << std::endl;

    // Inicializar input handler
    input::InputHandler input_handler;
    std::cout << "[4/4] Initializing input handler..." << std::flush;
    if (!input_handler.initialize()) {
        std::cerr << " FAILED" << std::endl;
        std::cerr << "Error: Failed to initialize input handler" << std::endl;
        update_tray_tip("PenStream Server - Error: Input failed");
        MessageBoxA(NULL,
            "Failed to initialize input handler.",
            "PenStream Server Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    std::cout << " OK" << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Server started. Waiting for client..." << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
    std::cout << "Double-click tray icon to show/hide console." << std::endl;
    std::cout << "========================================" << std::endl;
    update_tray_tip("PenStream Server - Waiting for client...");

    uint32_t frame_id = 0;
    auto frame_interval   = std::chrono::microseconds(1000000 / config.fps);
    auto last_frame_time  = std::chrono::steady_clock::now();
    uint32_t frames_sent  = 0;
    auto stats_time       = std::chrono::steady_clock::now();
    bool was_connected = false;

    while (g_running.load()) {
        // Process Windows messages
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!transport.has_client()) {
            std::vector<uint8_t> recv_buffer;
            sockaddr_in client_addr{};

            if (transport.receive(recv_buffer, client_addr, 100)) {
                if (recv_buffer.size() >= 12) {
                    if (recv_buffer[0] == 0x50 && recv_buffer[1] == 0x53) {
                        uint8_t type = recv_buffer[2];
                        if (type == 0x10) { // CONNECT_REQ
                            std::cout << "Client connection request received" << std::endl;
                            transport.send_connect_response(
                                client_addr, true,
                                static_cast<uint16_t>(config.width),
                                static_cast<uint16_t>(config.height),
                                static_cast<uint16_t>(config.bitrate_kbps));
                            std::cout << "Client connected!" << std::endl;
                            update_tray_tip("PenStream Server - Client connected!");
                        }
                    }
                }
            }
        }

        if (!transport.has_client()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Actualizar tip cuando hay conexión
        if (!was_connected && transport.has_client()) {
            update_tray_tip("PenStream Server - Streaming...");
            was_connected = true;
        }

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = now - last_frame_time;

        if (elapsed >= frame_interval) {
            capture::Frame frame;
            if (capturer.capture_frame(frame)) {
                std::vector<uint8_t> encoded_data;
                if (encoder.encode(frame, encoded_data) && !encoded_data.empty()) {
                    if (transport.send_video_frame(encoded_data, frame_id, frame.timestamp_us,
                                                   frame.width, frame.height)) {
                        frames_sent++;
                    }
                }
            }
            frame_id++;
            last_frame_time = now;
        }

        input_handler.process_pending_inputs();

        {
            std::vector<uint8_t> recv_buffer;
            sockaddr_in dummy_addr{};
            if (transport.receive(recv_buffer, dummy_addr, 0)) {
                if (recv_buffer.size() >= sizeof(network::InputPacket)) {
                    const auto* input =
                        reinterpret_cast<const network::InputPacket*>(recv_buffer.data());

                    if (input->header.type == 0x02) { // TOUCH_INPUT
                        input::InputEvent event;
                        event.x         = input->x;
                        event.y         = input->y;
                        event.pressure  = input->pressure;
                        event.tilt_x    = input->tilt_x;
                        event.tilt_y    = input->tilt_y;
                        event.buttons   = input->buttons;
                        event.timestamp = input->header.timestamp;
                        input_handler.queue_input(event);
                    }
                }
            }
        }

        // Stats cada 5 segundos
        auto stats_elapsed = now - stats_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(stats_elapsed).count() >= 5) {
            auto fps   = static_cast<float>(frames_sent) /
                         static_cast<float>(
                             std::chrono::duration_cast<std::chrono::seconds>(stats_elapsed).count());
            auto stats = transport.get_stats();
            std::cout << "Stats: " << fps << " fps | "
                      << stats.bytes_sent / 1024 << " KB sent | "
                      << stats.packets_received << " inputs received" << std::endl;

            // Actualizar tray tip con stats
            char tip[128];
            _snprintf_s(tip, sizeof(tip), _TRUNCATE, "PenStream - %.1f fps | %d inputs", fps, stats.packets_received);
            update_tray_tip(tip);

            frames_sent = 0;
            stats_time  = std::chrono::steady_clock::now();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "Shutting down..." << std::endl;
    update_tray_tip("PenStream Server - Shutting down...");
    remove_tray_icon();
    return 0;
}

} // namespace penstream

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    // Alloc console for debug output
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    std::signal(SIGINT,  penstream::signal_handler);
    std::signal(SIGTERM, penstream::signal_handler);
    SetConsoleOutputCP(CP_UTF8);

    // Create tray window
    penstream::create_tray_window();
    penstream::show_tray_icon(penstream::g_hwnd_tray);

    int result = penstream::run();

    // Cleanup
    penstream::remove_tray_icon();
    fclose(stdout);
    fclose(stderr);
    FreeConsole();

    return result;
}

// Fallback main para modo consola
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    // Si se ejecuta desde consola, mostrar mensaje y mantener ventana abierta
    std::cout << "PenStream Server should run as a Windows application." << std::endl;
    std::cout << "If you see this, the GUI failed to initialize." << std::endl;
    std::cout << std::endl;

    // Llamar a WinMain manualmente
    return penstream::WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOW);
}
