#include "lyrics.hpp"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

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

// ---- Romaji for Japanese lyrics -------------------------------------------------------------------
// Windows' Japanese analyzer gives each word's reading in kana; the readings become Hepburn romaji.

bool isKana(wchar_t ch) { return (ch >= 0x3040 && ch <= 0x309F) || (ch >= 0x30A0 && ch <= 0x30FF); }
bool isKanji(wchar_t ch) { return ch >= 0x4E00 && ch <= 0x9FFF; }

std::wstring kanaRomaji(std::wstring_view kana) {
    static const std::pair<wchar_t, std::wstring_view> table[]{
        {L'あ', L"a"}, {L'い', L"i"}, {L'う', L"u"}, {L'え', L"e"}, {L'お', L"o"},
        {L'か', L"ka"}, {L'き', L"ki"}, {L'く', L"ku"}, {L'け', L"ke"}, {L'こ', L"ko"},
        {L'が', L"ga"}, {L'ぎ', L"gi"}, {L'ぐ', L"gu"}, {L'げ', L"ge"}, {L'ご', L"go"},
        {L'さ', L"sa"}, {L'し', L"shi"}, {L'す', L"su"}, {L'せ', L"se"}, {L'そ', L"so"},
        {L'ざ', L"za"}, {L'じ', L"ji"}, {L'ず', L"zu"}, {L'ぜ', L"ze"}, {L'ぞ', L"zo"},
        {L'た', L"ta"}, {L'ち', L"chi"}, {L'つ', L"tsu"}, {L'て', L"te"}, {L'と', L"to"},
        {L'だ', L"da"}, {L'ぢ', L"ji"}, {L'づ', L"zu"}, {L'で', L"de"}, {L'ど', L"do"},
        {L'な', L"na"}, {L'に', L"ni"}, {L'ぬ', L"nu"}, {L'ね', L"ne"}, {L'の', L"no"},
        {L'は', L"ha"}, {L'ひ', L"hi"}, {L'ふ', L"fu"}, {L'へ', L"he"}, {L'ほ', L"ho"},
        {L'ば', L"ba"}, {L'び', L"bi"}, {L'ぶ', L"bu"}, {L'べ', L"be"}, {L'ぼ', L"bo"},
        {L'ぱ', L"pa"}, {L'ぴ', L"pi"}, {L'ぷ', L"pu"}, {L'ぺ', L"pe"}, {L'ぽ', L"po"},
        {L'ま', L"ma"}, {L'み', L"mi"}, {L'む', L"mu"}, {L'め', L"me"}, {L'も', L"mo"},
        {L'や', L"ya"}, {L'ゆ', L"yu"}, {L'よ', L"yo"},
        {L'ら', L"ra"}, {L'り', L"ri"}, {L'る', L"ru"}, {L'れ', L"re"}, {L'ろ', L"ro"},
        {L'わ', L"wa"}, {L'ゐ', L"i"}, {L'ゑ', L"e"}, {L'を', L"o"}, {L'ん', L"n"}, {L'ゔ', L"vu"},
        {L'ぁ', L"a"}, {L'ぃ', L"i"}, {L'ぅ', L"u"}, {L'ぇ', L"e"}, {L'ぉ', L"o"},
        {L'ゃ', L"ya"}, {L'ゅ', L"yu"}, {L'ょ', L"yo"}, {L'ゎ', L"wa"},
    };
    const auto lookup = [](wchar_t ch) -> std::wstring_view {
        for (const auto& [kana, romaji] : table) if (kana == ch) return romaji;
        return {};
    };
    std::wstring out;
    bool doubleNext = false;
    for (size_t i = 0; i < kana.size(); ++i) {
        wchar_t ch = kana[i];
        if (ch >= 0x30A1 && ch <= 0x30F6) ch = static_cast<wchar_t>(ch - 0x60);   // katakana to hiragana
        if (ch == L'っ' || ch == L'ッ') { doubleNext = true; continue; }
        if (ch == L'ー') {
            if (!out.empty()) out += out.back();
            continue;
        }
        auto syllable = std::wstring(lookup(ch));
        if (syllable.empty()) { out += ch; doubleNext = false; continue; }
        // A small ya/yu/yo or small vowel joins the syllable before it: ki + ya = kya, shi + ya = sha, fu + a = fa.
        if (i + 1 < kana.size()) {
            wchar_t next = kana[i + 1];
            if (next >= 0x30A1 && next <= 0x30F6) next = static_cast<wchar_t>(next - 0x60);
            if (next == L'ゃ' || next == L'ゅ' || next == L'ょ') {
                const auto tail = lookup(next);
                const bool palatal = syllable == L"shi" || syllable == L"chi" || syllable == L"ji";
                syllable = palatal ? syllable.substr(0, syllable.size() - 1) + std::wstring(tail.substr(1))
                                   : syllable.substr(0, syllable.size() - 1) + std::wstring(tail);
                ++i;
            } else if (next == L'ぁ' || next == L'ぃ' || next == L'ぅ' || next == L'ぇ' || next == L'ぉ') {
                syllable = syllable.substr(0, syllable.size() - 1) + std::wstring(lookup(next));
                ++i;
            }
        }
        if (doubleNext) {
            out += syllable.starts_with(L"ch") ? L't' : syllable.front();
            doubleNext = false;
        }
        out += syllable;
    }
    return out;
}

std::wstring romajiFor(std::wstring const& line) {
    using winrt::Windows::Globalization::JapanesePhoneticAnalyzer;
    static constexpr std::wstring_view particles[]{L"は", L"が", L"を", L"に", L"へ", L"で", L"と", L"の", L"も", L"や",
                                                    L"から", L"まで", L"より", L"ね", L"よ", L"か", L"な"};
    std::wstring out;
    // The analyzer reads at most 100 characters at a time.
    for (size_t start = 0; start < line.size(); start += 100) {
        const auto words = JapanesePhoneticAnalyzer::GetWords(winrt::hstring(std::wstring_view(line).substr(start, 100)));
        for (const auto& word : words) {
            const std::wstring display{word.DisplayText()};
            const std::wstring yomi{word.YomiText()};
            const bool particle = std::ranges::find(particles, std::wstring_view(display)) != std::end(particles);
            std::wstring piece;
            if (display == L"は" && particle) piece = L"wa";
            else if (display == L"へ" && particle) piece = L"e";
            else if (display == L"を") piece = L"o";
            else piece = kanaRomaji(yomi.empty() ? display : yomi);
            const bool spaced = !out.empty() && out.back() != L' ' && (word.IsPhraseStart() || particle) &&
                                !piece.empty() && iswalnum(piece.front());
            if (spaced) out += L' ';
            out += piece;
        }
    }
    return out;
}

// ---- The romaji add-on: a sorted "word<TAB>reading" list built from JMdict (tools/build_romaji_dict.py).

constexpr wchar_t romajiAddonUrl[] = L"https://github.com/alifihsan7/Zenula/releases/download/romaji-ja-v1/romaji-ja.tsv";
constexpr char romajiAddonHeader[] = "# Zenula romaji dictionary";

std::filesystem::path romajiAddonPath() {
    wchar_t folder[MAX_PATH]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(folder) / L"Zenith Dot" / L"addons" / L"romaji-ja.tsv";
}

std::string utf8(std::wstring_view text) { return winrt::to_string(text); }

// Loaded for one song and dropped after, so the dictionary costs no memory between songs.
class RomajiDictionary {
public:
    bool load(std::filesystem::path const& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (size <= 0 || size > 40'000'000) return false;
        data_.resize(static_cast<size_t>(size));
        if (!input.seekg(0).read(data_.data(), size) || !data_.starts_with(romajiAddonHeader)) return false;
        for (size_t start = 0; start < data_.size();) {
            const auto end = data_.find('\n', start);
            const std::string_view line(data_.data() + start, (end == std::string::npos ? data_.size() : end) - start);
            start = end == std::string::npos ? data_.size() : end + 1;
            const auto tab = line.find('\t');
            if (line.empty() || line.front() == '#' || tab == std::string_view::npos) continue;
            entries_.push_back({line.substr(0, tab), line.substr(tab + 1)});
        }
        return !entries_.empty();
    }
    std::optional<std::string_view> find(std::string_view word) const {
        const auto found = std::ranges::lower_bound(entries_, word, {}, &Entry::word);
        if (found == entries_.end() || found->word != word) return std::nullopt;
        return found->reading;
    }

private:
    struct Entry {
        std::string_view word;
        std::string_view reading;
    };
    std::string data_;
    std::vector<Entry> entries_;
};

std::wstring particleRomaji(std::wstring_view kana) {
    if (kana == L"は") return L"wa";
    if (kana == L"へ") return L"e";
    if (kana == L"を") return L"o";
    return kanaRomaji(kana);
}

// Particles, longest first. Case particles split a run whenever a word came before them; the
// sentence-final ones only at the end of a run of kana.
constexpr std::wstring_view longParticles[]{L"から", L"まで", L"より", L"だけ", L"でも", L"しか", L"けど", L"ので", L"のに", L"って"};
constexpr std::wstring_view caseParticles[]{L"は", L"が", L"を", L"に", L"へ", L"で", L"と", L"の", L"も", L"や"};
constexpr std::wstring_view finalParticles[]{L"ね", L"よ", L"か", L"な", L"さ"};

// Splits a line into words: kanji words by the longest match in the dictionary, verb endings stay
// on their verb, common kana words come from the dictionary too, and particles stand alone
// (ima wa mada tsutaerarenai kedo). Japanese punctuation becomes its Latin twin on the word before.
std::wstring dictionaryRomaji(std::wstring_view line, RomajiDictionary const* dictionary) {
    struct Word {
        std::wstring text;
        bool open = false;   // a verb or adjective form: the kana after it are its ending
    };
    std::vector<Word> words;
    const auto longest = [&](size_t at, size_t from, size_t to) -> std::pair<size_t, std::optional<std::string_view>> {
        if (!dictionary) return {0, std::nullopt};
        for (size_t length = std::min(from, line.size() - at); length >= to; --length) {
            if (const auto reading = dictionary->find(utf8(line.substr(at, length)))) return {length, reading};
            if (length == to) break;
        }
        return {0, std::nullopt};
    };
    const auto particleAt = [&](size_t at) -> size_t {
        const auto rest = line.substr(at);
        for (const auto particle : longParticles) if (rest.starts_with(particle)) return particle.size();
        if (!words.empty()) for (const auto particle : caseParticles) if (rest.starts_with(particle)) return 1;
        for (const auto particle : finalParticles) {
            if (rest.starts_with(particle) && (rest.size() == 1 || !isKana(rest[1]))) return 1;
        }
        return 0;
    };
    for (size_t i = 0; i < line.size();) {
        const wchar_t ch = line[i];
        if (isKanji(ch) || ch == L'々') {
            const auto [length, reading] = longest(i, 12, 1);
            if (!reading) return {};   // an unknown kanji: no romaji rather than a half-read line
            const bool open = isKana(line[i + length - 1]);
            words.push_back({kanaRomaji(winrt::to_hstring(*reading)), open});
            i += length;
        } else if (isKana(ch)) {
            if (!words.empty() && words.back().open) {
                // The ending of a verb or adjective runs until a long particle or the end of the kana.
                size_t to = i;
                while (to < line.size() && isKana(line[to])) {
                    const auto rest = line.substr(to);
                    if (to > i && std::ranges::any_of(longParticles, [&](auto p) { return rest.starts_with(p); })) break;
                    ++to;
                }
                words.back().text += kanaRomaji(line.substr(i, to - i));
                words.back().open = false;
                i = to;
            } else if (const size_t particle = particleAt(i)) {
                words.push_back({particleRomaji(line.substr(i, particle))});
                i += particle;
            } else if (const auto [length, reading] = longest(i, 8, 2); reading) {
                words.push_back({kanaRomaji(winrt::to_hstring(*reading))});
                i += length;
            } else {
                // A kana the dictionary does not start a word with: it belongs to the word before.
                const auto read = kanaRomaji(line.substr(i, 1));
                if (!words.empty()) words.back().text += read;
                else words.push_back({read});
                ++i;
            }
        } else {
            static constexpr std::pair<wchar_t, wchar_t> marks[]{{L'、', L','}, {L'。', L'.'}, {L'！', L'!'}, {L'？', L'?'},
                                                                  {L'，', L','}, {L'．', L'.'}, {L'～', L'~'}};
            if (const auto mark = std::ranges::find(marks, ch, &std::pair<wchar_t, wchar_t>::first); mark != std::end(marks)) {
                if (!words.empty()) words.back().text += mark->second;
                ++i;
                continue;
            }
            size_t to = i;
            while (to < line.size() && !isKana(line[to]) && !isKanji(line[to]) &&
                   std::ranges::find(marks, line[to], &std::pair<wchar_t, wchar_t>::first) == std::end(marks)) ++to;
            std::wstring other(line.substr(i, to - i));
            std::erase_if(other, [](wchar_t c) { return c == L'　' || c == L'「' || c == L'」' || c == L'・'; });
            const auto first = other.find_first_not_of(L' ');
            if (first != std::wstring::npos) words.push_back({other.substr(first, other.find_last_not_of(L' ') - first + 1)});
            i = to;
        }
    }
    std::wstring out;
    for (const auto& word : words) {
        if (word.text.empty()) continue;
        if (!out.empty()) out += L' ';
        out += word.text;
    }
    return out;
}

bool imeReadsJapanese() {
    try {
        return winrt::Windows::Globalization::JapanesePhoneticAnalyzer::GetWords(L"日本").Size() > 0;
    } catch (winrt::hresult_error const&) {
        return false;
    }
}

// Adds romaji to lyrics that are Japanese: any kana in the song, then every line with kana or kanji.
// The Windows Japanese keyboard reads best; without it the add-on dictionary does, and kana-only lines
// need neither.
void addRomaji(std::vector<LyricLine>& lines, bool addon) {
    const bool japanese = std::ranges::any_of(lines, [](LyricLine const& line) {
        return std::ranges::any_of(line.text, isKana);
    });
    if (!japanese) return;
    const bool ime = imeReadsJapanese();
    std::optional<RomajiDictionary> dictionary;
    if (!ime && addon) {
        dictionary.emplace();
        if (!dictionary->load(romajiAddonPath())) dictionary.reset();
    }
    for (auto& line : lines) {
        if (!std::ranges::any_of(line.text, [](wchar_t ch) { return isKana(ch) || isKanji(ch); })) continue;
        try {
            line.romaji = ime ? romajiFor(line.text) : dictionaryRomaji(line.text, dictionary ? &*dictionary : nullptr);
        } catch (winrt::hresult_error const&) {
            line.romaji.clear();
        }
    }
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
        double durationGap;
        bool otherAlbum;
    };
    std::vector<Match> matches;
    for (const auto& value : records) {
        if (value.ValueType() != JsonValueType::Object) continue;
        const auto record = value.GetObject();
        const auto length = record.GetNamedValue(L"duration", nullptr);
        if (!length || length.ValueType() != JsonValueType::Number) continue;
        const double gap = std::abs(length.GetNumber() - duration);
        if (!std::isfinite(gap) || gap > 4 || !equalText(cleanMetadata(field(record, L"trackName")), title) ||
            !equalText(cleanMetadata(field(record, L"artistName"), true), artist)) continue;
        matches.push_back({record, gap, !equalText(field(record, L"albumName"), album)});
    }
    std::ranges::sort(matches, {}, [](Match const& match) {
        return std::pair{match.durationGap, match.otherAlbum};
    });
    for (const auto& match : matches) {
        result.lines = timedLines(field(match.record, L"syncedLyrics"));
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

void Lyrics::setRomaji(bool enabled) {
    romaji_ = enabled;
    const auto path = romajiAddonPath();
    if (path.empty()) return;
    std::error_code error;
    if (!enabled) {
        std::filesystem::remove(path, error);
        return;
    }
    if (!std::filesystem::exists(path, error) && !downloadingRomaji_) downloadRomaji();
}

Lyrics::Action Lyrics::downloadRomaji() {
    const auto lifetime = shared_from_this();
    downloadingRomaji_ = true;
    const winrt::apartment_context ui;
    co_await winrt::resume_background();
    bool saved = false;
    try {
        const HttpClient http;   // GitHub release downloads redirect to their storage host
        const auto response = co_await http.GetAsync(Uri(romajiAddonUrl));
        if (response.IsSuccessStatusCode()) {
            const auto buffer = co_await response.Content().ReadAsBufferAsync();
            const std::string_view bytes(reinterpret_cast<char const*>(buffer.data()), buffer.Length());
            if (bytes.size() < 40'000'000 && bytes.starts_with(romajiAddonHeader)) {
                const auto path = romajiAddonPath();
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                auto pending = path;
                pending += L".tmp";
                {
                    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
                    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
                std::filesystem::rename(pending, path, error);
                saved = !error;
            }
        }
    } catch (winrt::hresult_error const&) {
        OutputDebugStringW(L"Zenula could not download the romaji dictionary.\n");
    }
    co_await ui;
    downloadingRomaji_ = false;
    if (saved && !stopped_ && romaji_) refreshRomaji();
}

// Gives the lyrics already on screen their romaji once the dictionary arrives.
Lyrics::Action Lyrics::refreshRomaji() {
    const auto lifetime = shared_from_this();
    if (view_.status != LyricsStatus::ready) co_return;
    const auto revision = view_.revision;
    auto lines = view_.lines;
    const winrt::apartment_context ui;
    co_await winrt::resume_background();
    addRomaji(lines, true);
    co_await ui;
    if (stopped_ || view_.revision != revision) co_return;
    view_.lines = std::move(lines);
    ++view_.revision;
    if (changed_) changed_();
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
    const bool romajiAddon = romaji_;
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
            auto searchTitle = cleanMetadata(title);
            const auto searchArtist = cleanMetadata(artist, true);
            const auto artistPrefix = searchArtist + L" - ";
            if (!searchArtist.empty() && searchTitle.size() > artistPrefix.size() &&
                equalText(std::wstring_view(searchTitle).substr(0, artistPrefix.size()), artistPrefix)) {
                searchTitle.erase(0, artistPrefix.size());
            }
            const auto search = co_await getWithRetry(http, Uri(L"https://lrclib.net/api/search?track_name=" +
                urlEncode(searchTitle) + L"&artist_name=" + urlEncode(searchArtist)));
            result.status = LyricsStatus::error;
            if (search.IsSuccessStatusCode()) {
                const auto body = co_await search.Content().ReadAsStringAsync();
                JsonArray records{nullptr};
                if (body.size() <= 1'048'576 && JsonArray::TryParse(body, records)) {
                    findMatchingLyrics(records, searchTitle, searchArtist, album, duration, result);
                    result.status = LyricsStatus::unavailable;
                }
            }
        }
    } catch (winrt::hresult_error const&) {
        result.status = LyricsStatus::error;
    }
    addRomaji(result.lines, romajiAddon);   // still on the background thread
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
