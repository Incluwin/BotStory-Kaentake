#include "pch.h"
#include "patcher.h"
#include "debug.h"
#include "constants.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <strsafe.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr int  MANIFEST_TIMEOUT_MS = 10 * 1000;
constexpr int  DOWNLOAD_TIMEOUT_MS = 10 * 60 * 1000;
constexpr DWORD READ_CHUNK_BYTES   = 64 * 1024;
constexpr const char* CACHE_FILE   = ".\\.patch_cache";

struct ManifestEntry {
    uint64_t    size   = 0;
    std::string sha256;
    std::string path;
    std::string source;   // server folder the file downloads from ("" = patch_url itself)
};

// Local side-cache so we don't re-SHA256 files that haven't been touched
// since the last run. Keyed by the same relative path the manifest uses.
// If (size, mtime) still match, the stored hash is trusted without reading
// the file. Any stat mismatch or a missing/corrupt cache forces a real hash.
struct CacheEntry {
    uint64_t    size  = 0;
    uint64_t    mtime = 0;    // FILETIME as one uint64
    std::string hash;
};
using CacheMap = std::unordered_map<std::string, CacheEntry>;

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
    bool https = false;
};

bool g_console_attached = false;

void EnsureConsole() {
    if (g_console_attached) return;
    if (!AllocConsole()) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    SetConsoleTitleA("BotStory Patcher");
    g_console_attached = true;
}

// FreeConsole alone sometimes leaves the conhost window on screen when the
// console was AllocConsole'd mid-process: freopen_s opens new handles on
// CONOUT$ that the CRT holds, and conhost won't exit while anything still
// references it. Flushing + fclose'ing the CRT streams, then WM_CLOSE'ing
// the window, makes the close reliable.
void DetachConsole() {
    if (!g_console_attached) return;
    fflush(stdout);
    fflush(stderr);
    HWND hwnd = GetConsoleWindow();
    fclose(stdout);
    fclose(stderr);
    FreeConsole();
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    g_console_attached = false;
}

void Log(const char* fmt, ...) {
    EnsureConsole();
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    StringCbVPrintfA(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    OutputDebugStringA(buf);
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

// --- config ---

bool ReadPatchUrlFromConfig(std::string& out) {
    char buf[1024];
    DWORD n = GetPrivateProfileStringA(
        "config", "patch_url", "",
        buf, sizeof(buf), ".\\" CONSTANTS_CONFIG_NAME);
    if (n == 0) return false;
    while (n > 0 && (buf[n-1] == '/' || buf[n-1] == '\\')) buf[--n] = '\0';
    out.assign(buf, n);
    return !out.empty();
}

// [config] base=<name> names the WZ set this client is built on. Unset or
// "repack" is the Community Repack set the main manifest describes. Any other
// name also applies the patch server's bases/<name>/manifest.txt (see
// ApplyOverlay), so a client built on the clean Cosmic WZ set keeps its own WZ
// files. Returns false if the value is not a plain name.
bool ReadBaseFromConfig(std::string& out) {
    char buf[64];
    DWORD n = GetPrivateProfileStringA(
        "config", "base", "",
        buf, sizeof(buf), ".\\" CONSTANTS_CONFIG_NAME);
    out.clear();
    for (DWORD i = 0; i < n; ++i) {
        char c = buf[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
        out.push_back(c);
    }
    if (out == "repack") out.clear();
    return true;
}

// --- url parsing ---

bool ParseUrl(const std::string& url, ParsedUrl& out) {
    size_t p = 0;
    if (url.compare(0, 7, "http://") == 0) {
        out.https = false;
        p = 7;
    } else if (url.compare(0, 8, "https://") == 0) {
        out.https = true;
        p = 8;
    } else {
        return false;
    }
    size_t slash = url.find('/', p);
    std::string authority = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);
    size_t colon = authority.find(':');
    std::string host;
    INTERNET_PORT port = out.https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (colon == std::string::npos) {
        host = authority;
    } else {
        host = authority.substr(0, colon);
        port = (INTERNET_PORT)std::atoi(authority.c_str() + colon + 1);
    }
    if (host.empty()) return false;
    out.host = Widen(host);
    out.port = port;
    out.path = Widen(path);
    return true;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& rel) {
    std::wstring r = base;
    if (r.empty() || r.back() != L'/') r.push_back(L'/');
    size_t start = 0;
    while (start < rel.size() && rel[start] == L'/') ++start;
    r.append(rel, start, std::wstring::npos);
    return r;
}

// URL-encode reserved characters in a path component. Leaves '/' alone so
// subdirectory paths round-trip.
std::wstring UrlEncodePath(const std::string& rel) {
    static const char* hex = "0123456789ABCDEF";
    std::string enc;
    enc.reserve(rel.size());
    for (unsigned char c : rel) {
        bool safe =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            enc.push_back((char)c);
        } else {
            enc.push_back('%');
            enc.push_back(hex[c >> 4]);
            enc.push_back(hex[c & 0xF]);
        }
    }
    return Widen(enc);
}

// --- winhttp ---

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : h_(h) {}
    ~WinHttpHandle() { if (h_) WinHttpCloseHandle(h_); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) { if (h_) WinHttpCloseHandle(h_); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
private:
    HINTERNET h_ = nullptr;
};

bool HttpOpenSession(WinHttpHandle& session, int timeout_ms) {
    HINTERNET h = WinHttpOpen(
        L"Kaentake-Patcher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) return false;
    WinHttpSetTimeouts(h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    session = WinHttpHandle(h);
    return true;
}

bool HttpOpenRequest(const ParsedUrl& base, const std::wstring& full_path,
                     WinHttpHandle& session, WinHttpHandle& connect,
                     WinHttpHandle& request) {
    HINTERNET c = WinHttpConnect(session.get(), base.host.c_str(), base.port, 0);
    if (!c) return false;
    connect = WinHttpHandle(c);

    DWORD flags = base.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = WinHttpOpenRequest(
        connect.get(), L"GET", full_path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!r) return false;
    request = WinHttpHandle(r);
    return true;
}

bool HttpSendAndRecv(HINTERNET req, DWORD& out_status, int64_t& out_content_length) {
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(req, nullptr)) return false;

    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) return false;
    out_status = status;

    wchar_t clen[64] = {};
    DWORD clen_sz = sizeof(clen);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX, clen, &clen_sz, WINHTTP_NO_HEADER_INDEX)) {
        out_content_length = _wtoi64(clen);
    } else {
        out_content_length = -1;
    }
    return true;
}

bool HttpGetBytes(const ParsedUrl& base, const std::wstring& full_path,
                  std::vector<uint8_t>& out, int timeout_ms) {
    WinHttpHandle session, connect, request;
    if (!HttpOpenSession(session, timeout_ms)) return false;
    if (!HttpOpenRequest(base, full_path, session, connect, request)) return false;

    DWORD status = 0;
    int64_t content_length = -1;
    if (!HttpSendAndRecv(request.get(), status, content_length)) return false;
    if (status != 200) {
        Log("  http %lu for %ls", status, full_path.c_str());
        return false;
    }

    out.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &avail)) return false;
        if (avail == 0) break;
        size_t off = out.size();
        out.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), out.data() + off, avail, &read)) return false;
        out.resize(off + read);
        if (read == 0) break;
    }
    return true;
}

// Download to `dest_path + ".new"`, then the caller is responsible for
// hashing and atomically renaming. Prints a simple progress line.
bool HttpDownloadToFile(const ParsedUrl& base, const std::wstring& full_path,
                        const std::string& dest_path, uint64_t expected_size) {
    WinHttpHandle session, connect, request;
    if (!HttpOpenSession(session, DOWNLOAD_TIMEOUT_MS)) return false;
    if (!HttpOpenRequest(base, full_path, session, connect, request)) return false;

    DWORD status = 0;
    int64_t content_length = -1;
    if (!HttpSendAndRecv(request.get(), status, content_length)) return false;
    if (status != 200) {
        Log("  http %lu downloading %s", status, dest_path.c_str());
        return false;
    }

    std::string tmp = dest_path + ".new";
    HANDLE fh = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        Log("  could not create %s (err %lu)", tmp.c_str(), GetLastError());
        return false;
    }

    std::vector<uint8_t> buf(READ_CHUNK_BYTES);
    uint64_t received = 0;
    DWORD last_tick = GetTickCount();
    bool ok = true;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &avail)) { ok = false; break; }
        if (avail == 0) break;
        if (buf.size() < avail) buf.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buf.data(), avail, &read)) { ok = false; break; }
        if (read == 0) break;
        DWORD written = 0;
        if (!WriteFile(fh, buf.data(), read, &written, nullptr) || written != read) {
            Log("  write failed on %s (err %lu)", tmp.c_str(), GetLastError());
            ok = false;
            break;
        }
        received += read;

        DWORD now = GetTickCount();
        if (now - last_tick > 500) {
            last_tick = now;
            if (expected_size > 0) {
                double pct = 100.0 * (double)received / (double)expected_size;
                double mib = received / (1024.0 * 1024.0);
                double total_mib = expected_size / (1024.0 * 1024.0);
                fprintf(stdout, "\r    %.1f / %.1f MiB  (%.0f%%)      ",
                        mib, total_mib, pct);
                fflush(stdout);
            }
        }
    }
    if (expected_size > 0) fputc('\n', stdout);
    CloseHandle(fh);
    if (!ok) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    if (expected_size > 0 && received != expected_size) {
        Log("  size mismatch: got %llu expected %llu",
            (unsigned long long)received, (unsigned long long)expected_size);
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

// --- sha256 ---

void HexEncode(const uint8_t* bytes, size_t n, std::string& out) {
    static const char* hex = "0123456789abcdef";
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out[2*i]   = hex[bytes[i] >> 4];
        out[2*i+1] = hex[bytes[i] & 0xF];
    }
}

bool Sha256File(const std::string& path, std::string& out_hex) {
    HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    std::vector<uint8_t> buf(1 << 20);

    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (st != 0) goto cleanup;
    st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st != 0) goto cleanup;

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(fh, buf.data(), (DWORD)buf.size(), &read, nullptr)) goto cleanup;
        if (read == 0) break;
        st = BCryptHashData(hash, buf.data(), read, 0);
        if (st != 0) goto cleanup;
    }
    {
        uint8_t digest[32];
        st = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (st != 0) goto cleanup;
        HexEncode(digest, sizeof(digest), out_hex);
        ok = true;
    }
cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(fh);
    return ok;
}

// --- manifest ---

bool ParseManifest(const std::vector<uint8_t>& body, std::vector<ManifestEntry>& out) {
    out.clear();
    const char* p = (const char*)body.data();
    const char* end = p + body.size();
    while (p < end) {
        const char* eol = (const char*)memchr(p, '\n', end - p);
        const char* line_end = eol ? eol : end;
        std::string line(p, line_end);
        p = eol ? eol + 1 : end;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        size_t s1 = line.find(' ');
        if (s1 == std::string::npos) continue;
        size_t s2 = line.find(' ', s1 + 1);
        if (s2 == std::string::npos) continue;

        ManifestEntry e;
        e.size   = _strtoui64(line.c_str(), nullptr, 10);
        e.sha256 = line.substr(s1 + 1, s2 - s1 - 1);
        e.path   = line.substr(s2 + 1);
        if (e.path.empty() || e.sha256.size() != 64) continue;

        // Sanitize: reject traversal, absolute paths, and backslashes.
        bool bad = false;
        if (e.path[0] == '/' || e.path[0] == '\\') bad = true;
        if (e.path.size() >= 2 && e.path[1] == ':') bad = true;
        if (e.path.find("..") != std::string::npos) bad = true;
        if (bad) {
            Log("  skipping unsafe manifest path: %s", e.path.c_str());
            continue;
        }
        out.push_back(std::move(e));
    }
    return true;
}

// A base's overlay entry replaces the main entry with the same path (Windows
// paths, so case-insensitive) or adds the file, and downloads from `source`.
void ApplyOverlay(std::vector<ManifestEntry>& entries,
                  std::vector<ManifestEntry>& overlay, const std::string& source) {
    for (auto& o : overlay) {
        o.source = source;
        bool replaced = false;
        for (auto& e : entries) {
            if (_stricmp(e.path.c_str(), o.path.c_str()) == 0) {
                e = o;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.push_back(o);
    }
}

// --- local file ops ---

bool FileStat(const std::string& path, uint64_t& size, uint64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    LARGE_INTEGER li;
    li.LowPart  = fad.nFileSizeLow;
    li.HighPart = fad.nFileSizeHigh;
    size  = (uint64_t)li.QuadPart;
    mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32)
          | (uint64_t)fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

void EnsureDirForFile(const std::string& file_path) {
    size_t p = 0;
    while (p < file_path.size()) {
        size_t slash = file_path.find_first_of("/\\", p + 1);
        if (slash == std::string::npos) break;
        std::string dir = file_path.substr(0, slash);
        CreateDirectoryA(dir.c_str(), nullptr);
        p = slash;
    }
}

// --- cache ---

void LoadCache(CacheMap& out) {
    out.clear();
    HANDLE fh = CreateFileA(CACHE_FILE, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(fh, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (1 << 20)) {
        CloseHandle(fh);
        return;
    }
    std::string body;
    body.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(fh, body.data(), (DWORD)body.size(), &read, nullptr);
    CloseHandle(fh);
    if (!ok || read != body.size()) return;

    size_t p = 0;
    while (p < body.size()) {
        size_t eol = body.find('\n', p);
        if (eol == std::string::npos) eol = body.size();
        std::string line(body, p, eol - p);
        p = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        size_t s1 = line.find(' ');
        if (s1 == std::string::npos) continue;
        size_t s2 = line.find(' ', s1 + 1);
        if (s2 == std::string::npos) continue;
        size_t s3 = line.find(' ', s2 + 1);
        if (s3 == std::string::npos) continue;

        CacheEntry e;
        e.size  = _strtoui64(line.c_str(), nullptr, 10);
        e.mtime = _strtoui64(line.c_str() + s1 + 1, nullptr, 10);
        e.hash  = line.substr(s2 + 1, s3 - s2 - 1);
        std::string path = line.substr(s3 + 1);
        if (e.hash.size() != 64 || path.empty()) continue;
        out[std::move(path)] = std::move(e);
    }
}

bool SaveCache(const CacheMap& cache) {
    std::string tmp = std::string(CACHE_FILE) + ".tmp";

    // Clear hidden/system attrs before overwriting; CreateFile otherwise fails
    // with ERROR_ACCESS_DENIED if we previously hid the cache file.
    SetFileAttributesA(CACHE_FILE, FILE_ATTRIBUTE_NORMAL);

    HANDLE fh = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;

    std::string body = "# BotStory patcher cache v1\n"
                       "# <size> <mtime_u64> <sha256> <path>\n";
    char line[512];
    for (const auto& kv : cache) {
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%llu %llu %s %s\n",
                            (unsigned long long)kv.second.size,
                            (unsigned long long)kv.second.mtime,
                            kv.second.hash.c_str(),
                            kv.first.c_str());
        if (n > 0) body.append(line, n);
    }

    DWORD written = 0;
    BOOL wok = WriteFile(fh, body.data(), (DWORD)body.size(), &written, nullptr);
    CloseHandle(fh);
    if (!wok || written != body.size()) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    if (!MoveFileExA(tmp.c_str(), CACHE_FILE, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    SetFileAttributesA(CACHE_FILE, FILE_ATTRIBUTE_HIDDEN);
    return true;
}

// --- orchestration ---

bool ProcessEntry(const ParsedUrl& base, const ManifestEntry& e,
                  CacheMap& cache, int& updated) {
    std::string local = ".\\" + e.path;
    for (char& c : local) if (c == '/') c = '\\';

    uint64_t cur_size = 0, cur_mtime = 0;
    bool exists = FileStat(local, cur_size, cur_mtime);

    // Decide whether to skip the download. We accept the file as up-to-date if:
    //   1. it exists with the manifest's size, AND
    //   2. we can prove its hash matches the manifest, either by trusting the
    //      cache (size + mtime unchanged since last known-good hash) or by
    //      hashing now and finding a match.
    bool up_to_date = false;
    if (exists && cur_size == e.size) {
        auto it = cache.find(e.path);
        if (it != cache.end()
                && it->second.size == cur_size
                && it->second.mtime == cur_mtime) {
            // Trust cached hash: mtime+size haven't changed since we last
            // verified, so the stored hash still reflects disk contents.
            up_to_date = (it->second.hash == e.sha256);
        } else {
            std::string local_hash;
            if (Sha256File(local, local_hash)) {
                up_to_date = (local_hash == e.sha256);
                if (up_to_date) {
                    cache[e.path] = {cur_size, cur_mtime, e.sha256};
                }
            } else {
                Log("  could not hash %s, re-downloading", local.c_str());
            }
        }
    }

    if (up_to_date) return true;

    Log("updating %s (%.1f MiB)", e.path.c_str(), e.size / (1024.0 * 1024.0));

    std::wstring url_path = JoinPath(base.path, UrlEncodePath(e.source + e.path));
    EnsureDirForFile(local);

    for (int attempt = 1; attempt <= 2; ++attempt) {
        if (!HttpDownloadToFile(base, url_path, local, e.size)) {
            Log("  download failed (attempt %d)", attempt);
            if (attempt == 2) return false;
            continue;
        }
        std::string tmp = local + ".new";
        std::string got;
        if (!Sha256File(tmp, got)) {
            Log("  hash failed on %s", tmp.c_str());
            DeleteFileA(tmp.c_str());
            if (attempt == 2) return false;
            continue;
        }
        if (got != e.sha256) {
            Log("  hash mismatch (attempt %d): got %s", attempt, got.c_str());
            DeleteFileA(tmp.c_str());
            if (attempt == 2) return false;
            continue;
        }
        if (!MoveFileExA(tmp.c_str(), local.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Log("  rename failed: %s -> %s (err %lu)",
                tmp.c_str(), local.c_str(), GetLastError());
            DeleteFileA(tmp.c_str());
            return false;
        }
        uint64_t ns = 0, nm = 0;
        if (FileStat(local, ns, nm)) {
            cache[e.path] = {ns, nm, e.sha256};
        } else {
            cache.erase(e.path);
        }
        ++updated;
        return true;
    }
    return false;
}

bool AskContinue(const char* title, const char* msg) {
    int r = MessageBoxA(nullptr, msg, title, MB_YESNO | MB_ICONWARNING);
    return r == IDYES;
}

} // namespace


bool PatcherRun() {
    std::string patch_url;
    if (!ReadPatchUrlFromConfig(patch_url)) {
        // No patch URL configured -> skip patching, proceed to launch.
        return true;
    }

    ParsedUrl base;
    if (!ParseUrl(patch_url, base)) {
        ErrorMessage("Invalid patch_url in config.ini: %s", patch_url.c_str());
        return false;
    }

    std::string wz_base;
    if (!ReadBaseFromConfig(wz_base)) {
        ErrorMessage("Invalid base in config.ini (letters, digits, - and _ only).");
        return false;
    }

    // Deliberately silent on the happy-path fast case: when every file is
    // cache-valid and matches the manifest, we never open a console at all.
    // EnsureConsole() is called lazily via Log() only when we hit real work
    // (a download, an error, or a user-facing retry prompt).

    std::wstring manifest_path = JoinPath(base.path, L"manifest.txt");
    std::vector<uint8_t> manifest_body;
    if (!HttpGetBytes(base, manifest_path, manifest_body, MANIFEST_TIMEOUT_MS)) {
        Log("could not reach patch server (%s)", patch_url.c_str());
        bool cont = AskContinue(
            "BotStory",
            "Could not reach the patch server.\n\n"
            "Play with your current files?\n"
            "(Click No to quit and try again later.)");
        DetachConsole();
        return cont;
    }

    std::vector<ManifestEntry> entries;
    if (!ParseManifest(manifest_body, entries) || entries.empty()) {
        Log("manifest was empty or invalid");
        bool cont = AskContinue(
            "BotStory",
            "Patch server returned an invalid manifest.\n\n"
            "Play with your current files?");
        DetachConsole();
        return cont;
    }

    if (!wz_base.empty()) {
        std::string source = "bases/" + wz_base + "/";
        std::vector<uint8_t> overlay_body;
        std::vector<ManifestEntry> overlay;
        if (!HttpGetBytes(base, JoinPath(base.path, Widen(source + "manifest.txt")),
                          overlay_body, MANIFEST_TIMEOUT_MS)
                || !ParseManifest(overlay_body, overlay) || overlay.empty()) {
            // Never patch with the main manifest alone: it would replace this
            // client's WZ files with the Community Repack set.
            Log("patch server has no manifest for base=%s", wz_base.c_str());
            char msg[512];
            StringCbPrintfA(msg, sizeof(msg),
                "The patch server has no files for this client's WZ set "
                "(config.ini base=%s).\n\n"
                "Play with your current files?", wz_base.c_str());
            bool cont = AskContinue("BotStory", msg);
            DetachConsole();
            return cont;
        }
        ApplyOverlay(entries, overlay, source);
    }

    CacheMap cache;
    LoadCache(cache);

    int updated = 0;
    int checked = 0;
    for (const auto& e : entries) {
        ++checked;
        if (!ProcessEntry(base, e, cache, updated)) {
            SaveCache(cache);
            ErrorMessage(
                "Patcher failed on %s.\n\n"
                "Could not sync client files. Try again or contact the host.",
                e.path.c_str());
            DetachConsole();
            return false;
        }
    }

    SaveCache(cache);

    // Only log a final summary if we actually opened a console during this
    // run (i.e. there was real work). No-op if the fast-path kept us silent.
    if (g_console_attached) {
        if (updated == 0) {
            Log("up to date (%d files checked)", checked);
        } else {
            Log("patched %d / %d files, launching game...", updated, checked);
        }
    }

    DetachConsole();
    return true;
}
