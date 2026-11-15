#pragma once

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Sockets.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zenith {

// A device running LocalSend (localsend.org) on the same network.
struct LocalSendDevice {
    std::wstring fingerprint;
    std::wstring alias;
    std::wstring model;
    std::wstring type;       // mobile, desktop, web, headless, server
    std::wstring host;
    uint16_t port = 53317;
    bool https = true;
};

struct LocalSendStatus {
    enum class Stage { idle, waiting, sending, sent, declined, failed };
    Stage stage = Stage::idle;
    std::wstring device;          // the receiver's name
    std::wstring fingerprint;
    uint64_t sent = 0;
    uint64_t total = 0;
    std::wstring message;         // why it failed

    bool busy() const { return stage == Stage::waiting || stage == Stage::sending; }
};

// Sends one Drop Zone item to a LocalSend device with the LocalSend v2 protocol: a multicast
// announcement finds devices, then prepare-upload and upload go over HTTP(S). Zenula only sends,
// so it runs no server; devices answer the announcement over multicast. The socket is open only
// while the send view asks for devices, and closes by itself a minute later.
class LocalSend {
public:
    using Devices = std::function<void(std::vector<LocalSendDevice> const&)>;
    using Status = std::function<void(LocalSendStatus const&)>;

    LocalSend();
    ~LocalSend();
    LocalSend(LocalSend const&) = delete;
    LocalSend& operator=(LocalSend const&) = delete;

    // Callbacks run on the thread that calls attach (the UI thread).
    void attach(Devices devices, Status status);
    void scan();
    void stopScan();
    void sendFile(std::wstring const& fingerprint, std::wstring path);
    void sendText(std::wstring const& fingerprint, std::wstring text);
    void cancel();

private:
    winrt::fire_and_forget openSocket(unsigned generation);
    winrt::fire_and_forget announce(unsigned generation);
    winrt::fire_and_forget expire(unsigned generation);
    winrt::fire_and_forget found(LocalSendDevice device);
    winrt::fire_and_forget progressed();
    winrt::fire_and_forget cancelSession(LocalSendDevice device, std::wstring session);
    winrt::Windows::Foundation::IAsyncAction transfer(LocalSendDevice device, std::wstring path, std::wstring text,
                                                       unsigned generation);
    void start(std::wstring const& fingerprint, std::wstring path, std::wstring text);
    void report(LocalSendStatus status);
    void received(winrt::Windows::Networking::Sockets::DatagramSocket const& socket,
                  winrt::Windows::Networking::Sockets::DatagramSocketMessageReceivedEventArgs const& args);

    std::shared_ptr<bool> const alive_ = std::make_shared<bool>(true);
    std::optional<winrt::apartment_context> ui_;
    Devices devices_;
    Status status_;
    std::wstring fingerprint_;
    std::wstring alias_;
    std::vector<LocalSendDevice> found_;
    winrt::Windows::Networking::Sockets::DatagramSocket socket_{nullptr};
    winrt::event_token receivedToken_{};
    bool socketReady_ = false;
    unsigned scanGeneration_ = 0;
    unsigned transferGeneration_ = 0;
    winrt::Windows::Foundation::IAsyncAction transfer_{nullptr};
    LocalSendDevice receiver_;
    std::wstring session_;
    LocalSendStatus shown_;
    std::chrono::steady_clock::time_point progressShownAt_{};
    std::atomic<uint64_t> progress_{0};
    std::atomic<bool> progressQueued_{false};
};

}
