#include "local_send.hpp"

#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Security.Cryptography.Certificates.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.h>

#include <algorithm>
#include <cwchar>
#include <utility>

namespace zenith {
namespace {

using namespace std::chrono_literals;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Networking;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Web::Http;

constexpr wchar_t multicastGroup[] = L"224.0.0.167";
constexpr wchar_t servicePort[] = L"53317";
constexpr auto scanTime = 60s;
constexpr auto progressInterval = 100ms;
constexpr uint32_t maxAnnouncement = 16 * 1024;

std::wstring computerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 64]{};
    DWORD length = static_cast<DWORD>(std::size(name));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &length) && length) return {name, length};
    return L"Zenula";
}

JsonObject deviceInfo(std::wstring const& alias, std::wstring const& fingerprint) {
    JsonObject info;
    info.Insert(L"alias", JsonValue::CreateStringValue(alias));
    info.Insert(L"version", JsonValue::CreateStringValue(L"2.1"));
    info.Insert(L"deviceModel", JsonValue::CreateStringValue(L"Windows"));
    info.Insert(L"deviceType", JsonValue::CreateStringValue(L"desktop"));
    info.Insert(L"fingerprint", JsonValue::CreateStringValue(fingerprint));
    info.Insert(L"port", JsonValue::CreateNumberValue(53317));
    info.Insert(L"protocol", JsonValue::CreateStringValue(L"https"));
    info.Insert(L"download", JsonValue::CreateBooleanValue(false));
    return info;
}

std::wstring fileName(std::wstring const& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring mediaType(std::wstring const& name) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"application/octet-stream";
    const auto* extension = name.c_str() + dot + 1;
    constexpr std::pair<wchar_t const*, wchar_t const*> types[]{
        {L"png", L"image/png"}, {L"jpg", L"image/jpeg"}, {L"jpeg", L"image/jpeg"}, {L"gif", L"image/gif"},
        {L"webp", L"image/webp"}, {L"bmp", L"image/bmp"}, {L"heic", L"image/heic"}, {L"svg", L"image/svg+xml"},
        {L"mp4", L"video/mp4"}, {L"mov", L"video/quicktime"}, {L"mkv", L"video/x-matroska"}, {L"webm", L"video/webm"},
        {L"mp3", L"audio/mpeg"}, {L"m4a", L"audio/mp4"}, {L"wav", L"audio/wav"}, {L"flac", L"audio/flac"},
        {L"pdf", L"application/pdf"}, {L"zip", L"application/zip"}, {L"apk", L"application/vnd.android.package-archive"},
        {L"txt", L"text/plain"}, {L"md", L"text/markdown"}, {L"html", L"text/html"}, {L"json", L"application/json"},
    };
    for (const auto& [suffix, type] : types) if (_wcsicmp(extension, suffix) == 0) return type;
    return L"application/octet-stream";
}

std::wstring randomHex(uint32_t bytes) {
    return CryptographicBuffer::EncodeToHexString(CryptographicBuffer::GenerateRandom(bytes)).c_str();
}

std::wstring baseUri(LocalSendDevice const& device) {
    auto host = device.host;
    if (host.find(L':') != std::wstring::npos) host = L"[" + host + L"]";
    return (device.https ? L"https://" : L"http://") + host + L":" + std::to_wstring(device.port) + L"/api/localsend/v2/";
}

std::wstring query(std::wstring_view value) {
    std::wstring result;
    for (const wchar_t ch : value) {
        if (iswalnum(ch) || ch == L'-' || ch == L'_' || ch == L'.') result += ch;
    }
    return result;
}

// LocalSend devices use self-signed certificates, so trust comes from the fingerprint the device
// announced: the SHA-256 of its certificate.
HttpClient client(LocalSendDevice const& device, std::shared_ptr<std::atomic<bool>> rejected = {}) {
    const Filters::HttpBaseProtocolFilter filter;
    filter.AllowUI(false);
    filter.AllowAutoRedirect(false);
    filter.CookieUsageBehavior(Filters::HttpCookieUsageBehavior::NoCookies);
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
    if (device.https) {
        using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;
        filter.IgnorableServerCertificateErrors().Append(ChainValidationResult::Untrusted);
        filter.IgnorableServerCertificateErrors().Append(ChainValidationResult::InvalidName);
        filter.IgnorableServerCertificateErrors().Append(ChainValidationResult::Expired);
        filter.ServerCustomValidationRequested([expected = device.fingerprint, rejected](auto const&,
            Filters::HttpServerCustomValidationRequestedEventArgs const& args) {
            const auto sha256 = Core::HashAlgorithmProvider::OpenAlgorithm(Core::HashAlgorithmNames::Sha256());
            const auto hash = CryptographicBuffer::EncodeToHexString(
                sha256.HashData(args.ServerCertificate().GetCertificateBlob()));
            if (_wcsicmp(hash.c_str(), expected.c_str()) == 0) return;
            if (rejected) *rejected = true;
            args.Reject();
        });
    }
    return HttpClient(filter);
}

std::wstring networkError(winrt::hresult code, std::wstring const& device) {
    using winrt::Windows::Web::WebError;
    using winrt::Windows::Web::WebErrorStatus;
    switch (WebError::GetStatus(code.value)) {
    case WebErrorStatus::CertificateCommonNameIsIncorrect:
    case WebErrorStatus::CertificateExpired:
    case WebErrorStatus::CertificateContainsErrors:
    case WebErrorStatus::CertificateRevoked:
    case WebErrorStatus::CertificateIsInvalid:
        return L"Couldn't verify " + device;
    case WebErrorStatus::Unknown:
        break;
    default:
        return L"Couldn't reach " + device;
    }
    if (code == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || code == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return L"The file is gone";
    if (code == E_ACCESSDENIED) return L"Windows won't let Zenula read the file";
    return L"Couldn't reach " + device;
}

}

LocalSend::LocalSend() : fingerprint_(randomHex(32)), alias_(computerName()) {}

LocalSend::~LocalSend() {
    *alive_ = false;
    if (transfer_) transfer_.Cancel();
    if (socket_) {
        socket_.MessageReceived(receivedToken_);
        socket_.Close();
    }
}

void LocalSend::attach(Devices devices, Status status) {
    ui_.emplace();
    devices_ = std::move(devices);
    status_ = std::move(status);
}

void LocalSend::scan() {
    const auto generation = ++scanGeneration_;
    if (!socket_) openSocket(generation);
    else if (socketReady_) announce(generation);
    expire(generation);
}

void LocalSend::stopScan() {
    ++scanGeneration_;
    if (socket_) {
        socket_.MessageReceived(receivedToken_);
        socket_.Close();
        socket_ = nullptr;
    }
    socketReady_ = false;
    found_.clear();
}

winrt::fire_and_forget LocalSend::openSocket(unsigned generation) {
    const auto alive = alive_;
    const auto ui = *ui_;
    DatagramSocket socket;
    // Shares the port with a LocalSend app on this PC instead of taking it.
    socket.Control().MulticastOnly(true);
    receivedToken_ = socket.MessageReceived({this, &LocalSend::received});
    socket_ = socket;
    bool ready = false;
    try {
        co_await socket.BindServiceNameAsync(servicePort);
        socket.JoinMulticastGroup(HostName(multicastGroup));
        ready = true;
    } catch (...) {
    }
    co_await ui;
    if (!*alive || socket_ != socket) co_return;
    if (!ready) {
        stopScan();
        co_return;
    }
    socketReady_ = true;
    announce(std::max(generation, scanGeneration_));
}

// LocalSend repeats its own announcement a few times, since multicast can drop a packet.
winrt::fire_and_forget LocalSend::announce(unsigned generation) {
    const auto alive = alive_;
    const auto ui = *ui_;
    auto json = deviceInfo(alias_, fingerprint_);
    json.Insert(L"announce", JsonValue::CreateBooleanValue(true));
    const auto message = json.Stringify();
    for (const auto delay : {0ms, 500ms, 2000ms}) {
        if (delay > 0ms) co_await winrt::resume_after(delay);
        co_await ui;
        if (!*alive || generation != scanGeneration_ || !socket_) co_return;
        const auto socket = socket_;
        try {
            const auto stream = co_await socket.GetOutputStreamAsync(HostName(multicastGroup), servicePort);
            winrt::Windows::Storage::Streams::DataWriter writer(stream);
            writer.WriteString(message);
            co_await writer.StoreAsync();
            writer.DetachStream();
        } catch (...) {
        }
    }
}

winrt::fire_and_forget LocalSend::expire(unsigned generation) {
    const auto alive = alive_;
    const auto ui = *ui_;
    co_await winrt::resume_after(scanTime);
    co_await ui;
    if (!*alive || generation != scanGeneration_ || !socket_) co_return;
    // Devices found so far stay listed; only the socket closes.
    socket_.MessageReceived(receivedToken_);
    socket_.Close();
    socket_ = nullptr;
    socketReady_ = false;
}

void LocalSend::received(DatagramSocket const&, DatagramSocketMessageReceivedEventArgs const& args) {
    try {
        const auto reader = args.GetDataReader();
        const auto length = reader.UnconsumedBufferLength();
        if (length == 0 || length > maxAnnouncement) return;
        JsonObject json{nullptr};
        if (!JsonObject::TryParse(reader.ReadString(length), json)) return;
        LocalSendDevice device;
        device.fingerprint = json.GetNamedString(L"fingerprint", L"").c_str();
        device.alias = json.GetNamedString(L"alias", L"").c_str();
        if (device.fingerprint.empty() || device.fingerprint == fingerprint_ || device.alias.empty()) return;
        device.model = json.GetNamedString(L"deviceModel", L"").c_str();
        device.type = json.GetNamedString(L"deviceType", L"").c_str();
        const auto port = json.GetNamedNumber(L"port", 53317);
        device.port = port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 53317;
        device.https = json.GetNamedString(L"protocol", L"https") != L"http";
        device.host = args.RemoteAddress().CanonicalName().c_str();
        found(std::move(device));
    } catch (...) {
    }
}

winrt::fire_and_forget LocalSend::found(LocalSendDevice device) {
    const auto alive = alive_;
    co_await *ui_;
    if (!*alive || !socket_) co_return;
    // The same address with a new fingerprint is the same device after a restart.
    const auto known = std::ranges::find_if(found_, [&device](LocalSendDevice const& other) {
        return other.fingerprint == device.fingerprint || (other.host == device.host && other.port == device.port);
    });
    if (known == found_.end()) {
        found_.push_back(std::move(device));
    } else {
        if (known->alias == device.alias && known->model == device.model && known->host == device.host &&
            known->port == device.port && known->https == device.https) co_return;
        *known = std::move(device);
    }
    if (devices_) devices_(found_);
}

void LocalSend::report(LocalSendStatus status) {
    shown_ = std::move(status);
    progressShownAt_ = std::chrono::steady_clock::now();
    if (status_) status_(shown_);
}

void LocalSend::sendFile(std::wstring const& fingerprint, std::wstring path) { start(fingerprint, std::move(path), {}); }
void LocalSend::sendText(std::wstring const& fingerprint, std::wstring text) { start(fingerprint, {}, std::move(text)); }

void LocalSend::start(std::wstring const& fingerprint, std::wstring path, std::wstring text) {
    if (shown_.busy()) return;
    const auto device = std::ranges::find(found_, fingerprint, &LocalSendDevice::fingerprint);
    if (device == found_.end()) return;
    receiver_ = *device;
    session_.clear();
    report({LocalSendStatus::Stage::waiting, receiver_.alias, receiver_.fingerprint});
    transfer_ = transfer(receiver_, std::move(path), std::move(text), ++transferGeneration_);
}

void LocalSend::cancel() {
    if (!shown_.busy()) {
        shown_ = {};
        return;
    }
    ++transferGeneration_;
    if (transfer_) transfer_.Cancel();
    transfer_ = nullptr;
    if (!session_.empty()) cancelSession(receiver_, std::exchange(session_, {}));
    report({});
}

winrt::fire_and_forget LocalSend::cancelSession(LocalSendDevice device, std::wstring session) {
    try {
        const auto http = client(device);
        co_await http.PostAsync(Uri(baseUri(device) + L"cancel?sessionId=" + query(session)), nullptr);
    } catch (...) {
    }
}

winrt::fire_and_forget LocalSend::progressed() {
    const auto alive = alive_;
    co_await *ui_;
    progressQueued_ = false;
    if (!*alive || shown_.stage != LocalSendStatus::Stage::sending) co_return;
    const auto now = std::chrono::steady_clock::now();
    if (now - progressShownAt_ < progressInterval) co_return;
    auto status = shown_;
    status.sent = std::min(progress_.load(), status.total);
    report(std::move(status));
}

IAsyncAction LocalSend::transfer(LocalSendDevice device, std::wstring path, std::wstring text, unsigned generation) {
    auto cancellation = co_await winrt::get_cancellation_token();
    cancellation.enable_propagation();
    const auto alive = alive_;
    const auto ui = *ui_;
    LocalSendStatus status{LocalSendStatus::Stage::waiting, device.alias, device.fingerprint};
    std::wstring failure;
    bool canceled = false;
    const auto rejected = std::make_shared<std::atomic<bool>>(false);
    try {
        std::wstring name;
        uint64_t size = 0;
        if (!path.empty()) {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
                failure = L"The file is gone";
            } else if (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                failure = L"Folders can't be sent yet";
            }
            size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
            name = fileName(path);
        } else {
            size = winrt::to_string(text).size();
            name = L"Text.txt";
        }
        if (failure.empty()) {
            const auto fileId = randomHex(8);
            JsonObject file;
            file.Insert(L"id", JsonValue::CreateStringValue(fileId));
            file.Insert(L"fileName", JsonValue::CreateStringValue(name));
            file.Insert(L"size", JsonValue::CreateNumberValue(static_cast<double>(size)));
            file.Insert(L"fileType", JsonValue::CreateStringValue(path.empty() ? L"text/plain" : mediaType(name)));
            // With a preview, LocalSend shows the text as a message and needs no upload.
            if (path.empty()) file.Insert(L"preview", JsonValue::CreateStringValue(text));
            JsonObject files;
            files.Insert(fileId, file);
            JsonObject body;
            body.Insert(L"info", deviceInfo(alias_, fingerprint_));
            body.Insert(L"files", files);

            const auto http = client(device, rejected);
            const auto base = baseUri(device);
            // This waits while the person on the other device accepts or declines.
            const auto prepared = co_await http.PostAsync(Uri(base + L"prepare-upload"),
                HttpStringContent(body.Stringify(), winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                  L"application/json"));
            const int code = static_cast<int>(prepared.StatusCode());
            std::wstring session, token;
            if (code == 200) {
                JsonObject reply{nullptr};
                const auto content = co_await prepared.Content().ReadAsStringAsync();
                if (content.size() <= 65536 && JsonObject::TryParse(content, reply)) {
                    session = reply.GetNamedString(L"sessionId", L"").c_str();
                    if (const auto tokens = reply.GetNamedObject(L"files", nullptr)) {
                        token = tokens.GetNamedString(fileId, L"").c_str();
                    }
                }
            }
            co_await ui;
            if (!*alive || generation != transferGeneration_) co_return;
            switch (code) {
            case 200: break;
            case 204: status.stage = LocalSendStatus::Stage::sent; break;
            case 401: failure = device.alias + L" asks for a PIN"; break;
            case 403: status.stage = LocalSendStatus::Stage::declined; break;
            case 409: failure = device.alias + L" is busy with another transfer"; break;
            case 429: failure = L"Too many tries, wait a moment"; break;
            default: failure = device.alias + L" answered with error " + std::to_wstring(code); break;
            }
            if (code == 200 && (session.empty() || token.empty())) {
                status.stage = LocalSendStatus::Stage::sent;   // nothing left to upload
            } else if (code == 200) {
                session_ = session;
                status.stage = LocalSendStatus::Stage::sending;
                status.total = size;
                report(status);
                IHttpContent content{nullptr};
                if (!path.empty()) {
                    const auto source = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path);
                    content = HttpStreamContent(co_await source.OpenSequentialReadAsync());
                } else {
                    content = HttpBufferContent(CryptographicBuffer::ConvertStringToBinary(
                        text, BinaryStringEncoding::Utf8));
                }
                content.Headers().ContentType(Headers::HttpMediaTypeHeaderValue(L"application/octet-stream"));
                content.Headers().ContentLength(size);
                progress_ = 0;
                auto upload = http.PostAsync(Uri(base + L"upload?sessionId=" + query(session) + L"&fileId=" +
                                                 query(fileId) + L"&token=" + query(token)), content);
                upload.Progress([this, alive](auto const&, HttpProgress const& progress) {
                    if (!*alive) return;
                    progress_ = progress.BytesSent;
                    if (!progressQueued_.exchange(true)) progressed();
                });
                const auto uploaded = co_await upload;
                co_await ui;
                if (!*alive || generation != transferGeneration_) co_return;
                session_.clear();
                if (uploaded.IsSuccessStatusCode()) {
                    status.stage = LocalSendStatus::Stage::sent;
                    status.sent = size;
                } else if (uploaded.StatusCode() == HttpStatusCode::Forbidden) {
                    status.stage = LocalSendStatus::Stage::declined;
                } else {
                    failure = device.alias + L" stopped the transfer";
                }
            }
        }
    } catch (winrt::hresult_canceled const&) {
        canceled = true;
    } catch (winrt::hresult_error const& error) {
        failure = *rejected ? L"Couldn't verify " + device.alias : networkError(error.code(), device.alias);
    } catch (...) {
        failure = L"Couldn't send to " + device.alias;
    }
    co_await ui;
    if (!*alive || canceled || generation != transferGeneration_) co_return;
    transfer_ = nullptr;
    if (!failure.empty()) {
        status.stage = LocalSendStatus::Stage::failed;
        status.message = std::move(failure);
    }
    report(std::move(status));
}

}
