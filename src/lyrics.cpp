#include "lyrics.hpp"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace zenith {
namespace {

using namespace std::chrono_literals;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Web::Http;

constexpr int maxDelaySteps = 60;
constexpr wchar_t lyricsDelayKey[] = L"Software\\Zenith Dot\\Lyrics Delay";

std::wstring trackDelayKey(std::wstring_view title, std::wstring_view artist, std::wstring_view album, unsigned duration) {
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    for (const auto field : {title, artist, album}) {
        mix(field.size());
        for (const wchar_t character : field) mix(static_cast<uint16_t>(character));
    }
    mix(duration);
    wchar_t value[17]{};
    swprintf_s(value, L"%016llx", static_cast<unsigned long long>(hash));
    return value;
}

int loadDelay(std::wstring const& key) {
    DWORD encoded = 0;
    DWORD size = sizeof(encoded);
    if (RegGetValueW(HKEY_CURRENT_USER, lyricsDelayKey, key.c_str(), RRF_RT_REG_DWORD, nullptr, &encoded, &size)
        != ERROR_SUCCESS || encoded > 2u * maxDelaySteps) return 0;
    return static_cast<int>(encoded) - maxDelaySteps;
}

std::wstring urlEncode(std::wstring_view value) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result;
    for (const unsigned char byte : winrt::to_string(value)) {
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            result += static_cast<wchar_t>(byte);
        } else {
            result += L'%';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    return result;
}

HttpClient client() {
    const Filters::HttpBaseProtocolFilter filter;
    filter.AllowAutoRedirect(false);
    filter.CookieUsageBehavior(Filters::HttpCookieUsageBehavior::NoCookies);
    filter.CacheControl().ReadBehavior(Filters::HttpCacheReadBehavior::NoCache);
    filter.CacheControl().WriteBehavior(Filters::HttpCacheWriteBehavior::NoCache);
    return HttpClient(filter);
}

bool transientNetworkError(winrt::hresult code) {
    using namespace winrt::Windows::Web;
    switch (WebError::GetStatus(code.value)) {
    case WebErrorStatus::ServerUnreachable:
    case WebErrorStatus::Timeout:
    case WebErrorStatus::ConnectionAborted:
    case WebErrorStatus::ConnectionReset:
    case WebErrorStatus::Disconnected:
    case WebErrorStatus::CannotConnect:
    case WebErrorStatus::HostNameNotResolved:
        return true;
    default:
        return false;
    }
}

std::optional<TimeSpan> retryDelay(HttpResponseMessage const& response, TimeSpan delay) {
    switch (static_cast<int>(response.StatusCode())) {
    case 408: case 429: case 500: case 502: case 503: case 504:
        break;
    default:
        return std::nullopt;
    }
    if (const auto retry = response.Headers().RetryAfter()) {
        if (const auto delta = retry.Delta()) delay = std::max(delay, delta.Value());
        if (const auto date = retry.Date()) delay = std::max(delay, date.Value() - winrt::clock::now());
    }
    if (delay > 8s) return std::nullopt;
    return delay;
}

IAsyncOperation<HttpResponseMessage> getWithRetry(HttpClient http, Uri uri) {
    const auto cancellation = co_await winrt::get_cancellation_token();
    cancellation.enable_propagation();
    constexpr TimeSpan backoff[]{1s, 3s};
    for (size_t attempt = 0; ; ++attempt) {
        if (cancellation()) throw winrt::hresult_canceled();
        HttpResponseMessage response{nullptr};
        const bool last = attempt == std::size(backoff);
        try {
            response = co_await http.GetAsync(uri);
        } catch (winrt::hresult_error const& error) {
            if (last || !transientNetworkError(error.code())) throw;
        }
        if (last) co_return response;
        const auto delay = response ? retryDelay(response, backoff[attempt]) : backoff[attempt];
        if (!delay) co_return response;
        response = nullptr;
        co_await winrt::resume_after(*delay);
    }
}

JsonObject jsonBody(winrt::hstring const& body) {
    JsonObject result{nullptr};
    if (body.size() <= 262'144) JsonObject::TryParse(body, result);
    return result;
}

std::wstring field(JsonObject const& object, winrt::hstring const& key) {
    const auto value = object ? object.GetNamedValue(key, nullptr) : nullptr;
    return value && value.ValueType() == JsonValueType::String ? std::wstring(value.GetString()) : std::wstring{};
}

bool equalText(std::wstring_view first, std::wstring_view second) {
    if (first.empty() || second.empty()) return first.empty() && second.empty();
    return CompareStringOrdinal(first.data(), static_cast<int>(first.size()),
                                second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

std::wstring cleanMetadata(std::wstring_view value, bool artist = false) {
    std::wstring result;
    for (auto ch : value) {
        if (ch == L'\u2019') ch = L'\'';
        if (ch == L'\u2013' || ch == L'\u2014') ch = L'-';
        if (ch == L'\t' || ch == L'\u00a0') ch = L' ';
        if (ch != L' ' || (!result.empty() && result.back() != L' ')) result += ch;
    }
    if (!result.empty() && result.back() == L' ') result.pop_back();
    constexpr std::wstring_view titleSuffixes[]{L" (Official Music Video)", L" [Official Music Video]",
        L" (Official Video)", L" [Official Video]", L" (Official Audio)", L" [Official Audio]"};
    constexpr std::wstring_view artistSuffixes[]{L" - Topic"};
    const std::span suffixes = artist ? std::span<const std::wstring_view>(artistSuffixes) :
                                      std::span<const std::wstring_view>(titleSuffixes);
    for (const auto suffix : suffixes) {
        if (result.size() > suffix.size() && equalText(std::wstring_view(result).substr(result.size() - suffix.size()), suffix)) {
            result.resize(result.size() - suffix.size());
            break;
        }
    }
    return result;
}

std::wstring lowered(std::wstring_view value) {
    std::wstring result(value);
    for (auto& ch : result) ch = static_cast<wchar_t>(std::towlower(ch));
    return result;
}

// A title for matching: "Song (feat. X)", "Song - Remastered 2011" and "Song [Remastered]" all read as "Song".
std::wstring titleKey(std::wstring_view title) {
    auto result = cleanMetadata(title);
    for (size_t open = result.find_first_of(L"(["); open != std::wstring::npos; open = result.find_first_of(L"([", open)) {
        const auto close = result.find(result[open] == L'(' ? L')' : L']', open);
        if (close == std::wstring::npos) break;
        const auto inner = lowered(std::wstring_view(result).substr(open + 1, close - open - 1));
        if (inner.starts_with(L"feat") || inner.starts_with(L"ft.") || inner.starts_with(L"with ") ||
            inner.find(L"remaster") != std::wstring::npos) {
            const size_t from = open > 0 && result[open - 1] == L' ' ? open - 1 : open;
            result.erase(from, close + 1 - from);
            open = from;
        } else {
            open = close + 1;
        }
    }
    const auto lower = lowered(result);
    for (const std::wstring_view marker : {L" feat. ", L" ft. ", L" featuring "}) {
        if (const auto at = lower.find(marker); at != std::wstring::npos && at > 0) result.resize(std::min(result.size(), at));
    }
    if (const auto dash = result.rfind(L" - "); dash != std::wstring::npos && dash > 0 &&
        lowered(std::wstring_view(result).substr(dash)).find(L"remaster") != std::wstring::npos) {
        result.resize(dash);
    }
    while (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}

// The names in "A, B & C feat. D", the first one being the main artist.
std::vector<std::wstring> artistNames(std::wstring_view artist) {
    const auto text = cleanMetadata(artist, true);
    const auto lower = lowered(text);
    constexpr std::wstring_view separators[]{L", ", L" & ", L" feat. ", L" feat ", L" ft. ", L" featuring ", L" x ",
                                             L"; ", L" / ", L"、"};
    std::vector<std::wstring> names;
    for (size_t start = 0; start <= text.size();) {
        size_t next = std::wstring::npos;
        size_t skip = 0;
        for (const auto separator : separators) {
            if (const auto at = lower.find(separator, start); at < next) {
                next = at;
                skip = separator.size();
            }
        }
        const auto name = std::wstring_view(text).substr(start, next == std::wstring::npos ? std::wstring::npos : next - start);
        if (!name.empty()) names.emplace_back(name);
        if (next == std::wstring::npos) break;
        start = next + skip;
    }
    return names;
}

std::wstring mainArtist(std::wstring_view artist) {
    auto names = artistNames(artist);
    return names.empty() ? std::wstring{} : std::move(names.front());
}

bool sameArtist(std::wstring_view first, std::wstring_view second) {
    const auto ours = artistNames(first);
    const auto theirs = artistNames(second);
    if (ours.empty() || theirs.empty()) return false;
    const auto among = [](std::wstring const& name, std::vector<std::wstring> const& names) {
        return std::ranges::any_of(names, [&](std::wstring const& other) { return equalText(name, other); });
    };
    return among(ours.front(), theirs) || among(theirs.front(), ours);
}

bool latinLetter(wchar_t ch) {
    return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
           (ch >= 0xC0 && ch <= 0x24F && ch != 0xD7 && ch != 0xF7) || (ch >= 0x1E00 && ch <= 0x1EFF);
}

bool otherLetter(wchar_t ch) {
    return (ch >= 0x370 && ch <= 0x1DFF) || (ch >= 0x3040 && ch <= 0x9FFF) || (ch >= 0xAC00 && ch <= 0xD7AF) ||
           (ch >= 0xFF66 && ch <= 0xFF9F);
}

// One name written only in Latin letters and the other with none, as with あいみょん and Aimyon: the
// same artist can't be told from the letters, so the title, album and length decide.
bool otherScripts(std::wstring_view first, std::wstring_view second) {
    const auto latinOnly = [](std::wstring_view text) {
        return std::ranges::any_of(text, latinLetter) && std::ranges::none_of(text, otherLetter);
    };
    const auto withoutLatin = [](std::wstring_view text) {
        return std::ranges::any_of(text, otherLetter) && std::ranges::none_of(text, latinLetter);
    };
    return (latinOnly(first) && withoutLatin(second)) || (withoutLatin(first) && latinOnly(second));
}

std::vector<LyricLine> timedLines(std::wstring_view lrc) {
    constexpr size_t maxLines = 4096;
    constexpr size_t textLimit = 131'072;
    std::vector<LyricLine> lines;
    size_t textSize = 0;
    while (!lrc.empty() && lines.size() < maxLines) {
        const auto newline = lrc.find(L'\n');
        auto row = lrc.substr(0, newline);
        if (newline == std::wstring_view::npos) lrc = {};
        else lrc.remove_prefix(newline + 1);
        if (const auto start = row.find_first_not_of(L"\ufeff \t"); start != std::wstring_view::npos) {
            row.remove_prefix(start);
        }
        if (!row.empty() && row.back() == L'\r') row.remove_suffix(1);

        std::vector<double> stamps;
        while (row.starts_with(L"[") && stamps.size() < maxLines - lines.size()) {
            const auto close = row.find(L']');
            const auto colon = row.find(L':');
            if (close == std::wstring_view::npos || colon == 0 || colon >= close) break;
            const auto minutes = row.substr(1, colon - 1);
            const auto seconds = row.substr(colon + 1, close - colon - 1);
            if (minutes.empty() || minutes.size() > 3 || seconds.empty() || seconds.size() > 6 ||
                minutes.find_first_not_of(L"0123456789") != std::wstring_view::npos ||
                seconds.find_first_not_of(L"0123456789.") != std::wstring_view::npos) break;
            wchar_t* end = nullptr;
            const std::wstring value(seconds);
            const double second = std::wcstod(value.c_str(), &end);
            if (end != value.c_str() + value.size() || second >= 60) break;
            const double minute = std::wcstol(std::wstring(minutes).c_str(), nullptr, 10);
            stamps.push_back(60 * minute + second);
            row.remove_prefix(close + 1);
        }
        const auto start = row.find_first_not_of(L" \t");
        const auto lyric = start == std::wstring_view::npos ? std::wstring_view{} : row.substr(start);
        for (const double stamp : stamps) {
            if (lyric.size() > textLimit - textSize) break;
            lines.push_back({stamp, std::wstring(lyric)});
            textSize += lyric.size();
        }
    }
    if (std::ranges::all_of(lines, [](LyricLine const& line) { return line.text.empty(); })) return {};
    std::ranges::stable_sort(lines, {}, &LyricLine::seconds);
    return lines;
}

void findMatchingLyrics(JsonArray const& records, std::wstring_view title, std::wstring_view artist,
                        std::wstring_view album, unsigned duration, LyricsView& result) {
    struct Match {
        JsonObject record;
        bool otherArtist;
        double durationGap;
        bool otherAlbum;
    };
    const auto wantedTitle = titleKey(title);
    const auto wantedAlbum = cleanMetadata(album);
    std::vector<Match> matches;
    for (const auto& value : records) {
        if (value.ValueType() != JsonValueType::Object) continue;
        const auto record = value.GetObject();
        const auto length = record.GetNamedValue(L"duration", nullptr);
        if (!length || length.ValueType() != JsonValueType::Number) continue;
        const double gap = std::abs(length.GetNumber() - duration);
        if (!std::isfinite(gap) || gap > 10 || !equalText(titleKey(field(record, L"trackName")), wantedTitle)) continue;
        const auto recordArtist = field(record, L"artistName");
        const bool otherAlbum = wantedAlbum.empty() || !equalText(cleanMetadata(field(record, L"albumName")), wantedAlbum);
        const bool otherArtist = !sameArtist(recordArtist, artist);
        // Uploads of one recording often differ by a few seconds of silence at the end; with title,
        // artist and album all agreeing, up to 10 seconds is the same song.
        if (gap > 4 && (otherArtist || otherAlbum)) continue;
        // Another artist name only counts when it is the same name in another script or the album agrees,
        // and then the length must agree closely too, so covers and live takes stay out.
        if (otherArtist && (gap > 2 || (otherAlbum && !otherScripts(mainArtist(recordArtist), mainArtist(artist))))) continue;
        matches.push_back({record, otherArtist, gap, otherAlbum});
    }
    std::ranges::sort(matches, {}, [](Match const& match) {
        return std::tuple{match.otherArtist, match.durationGap, match.otherAlbum};
    });
    for (const auto& match : matches) {
        result.lines = timedLines(field(match.record, L"syncedLyrics"));
        if (match.durationGap > 4 && !result.lines.empty() && result.lines.back().seconds >= duration) {
            result.lines.clear();   // lyrics running past the end are timed for another version
            continue;
        }
        if (!result.lines.empty()) return;
        if (result.plain.empty()) result.plain = field(match.record, L"plainLyrics");
    }
}

}

Lyrics::Lyrics(std::function<void()> changed) : changed_(std::move(changed)) {}

void Lyrics::cancel() {
    ++revision_;
    if (request_) request_.Cancel();
    request_ = nullptr;
}

void Lyrics::stop() {
    stopped_ = true;
    changed_ = {};
    cancel();
}

void Lyrics::retry() {
    if (open_ && view_.status == LyricsStatus::error) view_.status = LyricsStatus::idle;
}

void Lyrics::adjustDelay(int steps) {
    if (view_.status != LyricsStatus::ready || delayKey_.empty()) return;
    const int updated = std::clamp(view_.delaySteps + steps, -maxDelaySteps, maxDelaySteps);
    if (updated == view_.delaySteps) return;
    view_.delaySteps = updated;
    if (!updated) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, lyricsDelayKey, delayKey_.c_str());
    } else {
        const auto encoded = static_cast<DWORD>(updated + maxDelaySteps);
        RegSetKeyValueW(HKEY_CURRENT_USER, lyricsDelayKey, delayKey_.c_str(), REG_DWORD, &encoded, sizeof(encoded));
    }
}

void Lyrics::update(MediaState const& media, bool panelOpen, bool pillActive) {
    if (stopped_) return;
    const bool open = panelOpen || pillActive;
    const bool trackChanged = appId_ != media.appId || title_ != media.title || artist_ != media.artist ||
                              album_ != media.album;
    const auto duration = std::isfinite(media.duration) ?
        static_cast<unsigned>(std::lround(std::clamp(media.duration, 0.0, 86'400.0))) : 0u;
    const bool durationChanged = duration != 0 && (duration_ == 0 ||
        std::abs(static_cast<int>(duration) - static_cast<int>(duration_)) > 2);
    if (trackChanged || durationChanged) {
        appId_ = media.appId;
        title_ = media.title;
        artist_ = media.artist;
        album_ = media.album;
        duration_ = duration;
        delayKey_ = title_.empty() || artist_.empty() || duration_ == 0
            ? std::wstring{} : trackDelayKey(title_, artist_, album_, duration_);
        view_.delaySteps = delayKey_.empty() ? 0 : loadDelay(delayKey_);
        cancel();
        view_.status = LyricsStatus::idle;
        view_.lines.clear();
        view_.plain.clear();
        retryAfter_ = {};
    }
    if (!open && open_ && (view_.status == LyricsStatus::loading || view_.status == LyricsStatus::error)) {
        cancel();
        view_.status = LyricsStatus::idle;
    }
    if (open && (!open_ || (panelOpen && !panelOpen_)) && view_.status == LyricsStatus::unavailable &&
        std::chrono::steady_clock::now() >= retryAfter_) view_.status = LyricsStatus::idle;
    if (panelOpen && !panelOpen_ && view_.status == LyricsStatus::error) view_.status = LyricsStatus::idle;
    panelOpen_ = panelOpen;
    open_ = open;
    if (open && view_.status == LyricsStatus::idle) {
        if (title_.empty() || artist_.empty() || duration_ == 0) {
            view_.status = LyricsStatus::unavailable;
        } else {
            view_.status = LyricsStatus::loading;
            request_ = lookup(revision_, title_, artist_, album_, duration_);
        }
    }
}

Lyrics::Action Lyrics::lookup(unsigned revision, std::wstring title, std::wstring artist, std::wstring album,
                              unsigned duration) {
    const auto lifetime = shared_from_this();
    const auto cancellation = co_await winrt::get_cancellation_token();
    cancellation.enable_propagation();
    const winrt::apartment_context ui;
    co_await winrt::resume_background();
    LyricsView result;
    result.status = LyricsStatus::error;
    try {
        const auto http = client();
        if (duration > 3600) {
            result.status = LyricsStatus::unavailable;
        } else {
            auto path = L"/api/get?track_name=" + urlEncode(title) + L"&artist_name=" + urlEncode(artist);
            if (!album.empty()) path += L"&album_name=" + urlEncode(album);
            path += L"&duration=" + std::to_wstring(duration);
            const auto response = co_await getWithRetry(http, Uri(L"https://lrclib.net" + path));
            if (response.StatusCode() == HttpStatusCode::NotFound) {
                result.status = LyricsStatus::unavailable;
            } else if (response.IsSuccessStatusCode()) {
                const auto body = jsonBody(co_await response.Content().ReadAsStringAsync());
                if (body) {
                    result.status = LyricsStatus::unavailable;
                    result.lines = timedLines(field(body, L"syncedLyrics"));
                    if (result.lines.empty()) result.plain = field(body, L"plainLyrics");
                }
            }
        }
        if (result.status == LyricsStatus::unavailable && result.lines.empty()) {
            auto searchTitle = titleKey(title);
            const auto searchArtist = cleanMetadata(artist, true);
            const auto artistPrefix = searchArtist + L" - ";
            if (!searchArtist.empty() && searchTitle.size() > artistPrefix.size() &&
                equalText(std::wstring_view(searchTitle).substr(0, artistPrefix.size()), artistPrefix)) {
                searchTitle.erase(0, artistPrefix.size());
            }
            // First with the main artist; when that finds no synced lyrics, by the title alone, which
            // catches the artist written another way, such as in Japanese on Spotify and romanized here.
            const std::wstring queries[]{
                L"track_name=" + urlEncode(searchTitle) + L"&artist_name=" + urlEncode(mainArtist(searchArtist)),
                L"track_name=" + urlEncode(searchTitle)};
            for (const auto& query : queries) {
                const auto search = co_await getWithRetry(http, Uri(L"https://lrclib.net/api/search?" + query));
                if (!search.IsSuccessStatusCode()) {
                    if (result.plain.empty()) result.status = LyricsStatus::error;
                    break;
                }
                const auto body = co_await search.Content().ReadAsStringAsync();
                JsonArray records{nullptr};
                if (body.size() <= 1'048'576 && JsonArray::TryParse(body, records)) {
                    findMatchingLyrics(records, searchTitle, searchArtist, album, duration, result);
                }
                if (!result.lines.empty()) break;
            }
        }
    } catch (winrt::hresult_error const&) {
        result.status = LyricsStatus::error;
    }
    co_await ui;
    if (stopped_ || revision != revision_) co_return;
    if (!result.lines.empty()) {
        result.status = LyricsStatus::ready;
        result.plain.clear();
    } else if (result.plain.find_first_not_of(L" \t\r\n\ufeff") != std::wstring::npos) {
        result.status = LyricsStatus::plain;
    }
    result.revision = view_.revision + 1;
    result.delaySteps = view_.delaySteps;
    view_ = std::move(result);
    if (view_.status == LyricsStatus::unavailable) retryAfter_ = std::chrono::steady_clock::now() + 30s;
    changed_();
}

}
