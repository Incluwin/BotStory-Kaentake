// cosmicSso.cpp — drop a signed MapleEdit SSO attestation on game login.
//
// Non-custodial SSO bridge: the Cosmic server holds an ed25519 key and
// signs a short-lived identity payload; this file is the courier. We do
// NOT hold any secret — we just POST the user's credentials to Cosmic's
// loopback admin control port (which they already gave the game client
// for login) and drop the signed envelope where MapleEdit will find it
// at %APPDATA%\Cosmic\sso.json.
//
// Flow (background thread, never blocks the UI):
//   1. POST /actions/cosmic-login {account, password}  -> character list
//   2. Pick the top-level character (matches MapleEdit's UI default)
//   3. POST /actions/cosmic-sso-attest {account, password, character}
//   4. Write the response envelope to %APPDATA%\Cosmic\sso.json
//
// All failures are silent — the normal game login is unaffected, and the
// user can still sign into MapleEdit the long way (password on the web
// page) if the bridge is unavailable.

#include "pch.h"
#include "debug.h"

#include <winhttp.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kCosmicHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kCosmicPort = 8480;
constexpr wchar_t kLoginPath[]  = L"/actions/cosmic-login";
constexpr wchar_t kAttestPath[] = L"/actions/cosmic-sso-attest";

// -------- small helpers ---------------------------------------------------

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    StringCchPrintfA(buf, sizeof(buf), "\\u%04x",
                                     static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Very narrow JSON helpers that match the shape AdminJson produces on the
// server side (flat objects, numeric IDs, no nested escapes we care about).
// Not a general parser — we only look for specific keys.

bool FindNumAfterKey(const std::string& body, const std::string& key, int& out) {
    size_t i = body.find("\"" + key + "\"");
    if (i == std::string::npos) return false;
    i += key.size() + 2;
    while (i < body.size() && (body[i] == ' ' || body[i] == ':')) i++;
    int sign = 1;
    if (i < body.size() && body[i] == '-') { sign = -1; i++; }
    long long v = 0;
    bool any = false;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        v = v * 10 + (body[i] - '0');
        any = true;
        i++;
    }
    if (!any) return false;
    out = static_cast<int>(sign * v);
    return true;
}

bool FindStrAfterKey(const std::string& body, size_t from, const std::string& key,
                     std::string& out, size_t& endOut) {
    size_t i = body.find("\"" + key + "\"", from);
    if (i == std::string::npos) return false;
    i += key.size() + 2;
    while (i < body.size() && (body[i] == ' ' || body[i] == ':')) i++;
    if (i >= body.size() || body[i] != '"') return false;
    i++;
    std::string v;
    while (i < body.size()) {
        char c = body[i];
        if (c == '"') { endOut = i + 1; out = v; return true; }
        if (c == '\\' && i + 1 < body.size()) {
            char esc = body[i + 1];
            switch (esc) {
                case '"':  v += '"';  break;
                case '\\': v += '\\'; break;
                case '/':  v += '/';  break;
                case 'n':  v += '\n'; break;
                case 'r':  v += '\r'; break;
                case 't':  v += '\t'; break;
                default:   v += esc;
            }
            i += 2;
        } else {
            v += c;
            i++;
        }
    }
    return false;
}

// Walk the "characters":[...] array and return the name of the top-level char.
std::string PickTopCharacter(const std::string& body) {
    size_t arrStart = body.find("\"characters\"");
    if (arrStart == std::string::npos) return {};
    arrStart = body.find('[', arrStart);
    if (arrStart == std::string::npos) return {};
    size_t arrEnd = body.find(']', arrStart);
    if (arrEnd == std::string::npos) return {};
    std::string arr = body.substr(arrStart, arrEnd - arrStart);

    std::string bestName;
    int bestLevel = -1;
    size_t cur = 0;
    while (cur < arr.size()) {
        size_t objStart = arr.find('{', cur);
        if (objStart == std::string::npos) break;
        size_t objEnd = arr.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string obj = arr.substr(objStart, objEnd - objStart + 1);
        std::string name;
        size_t nameEnd = 0;
        int level = 0;
        if (FindStrAfterKey(obj, 0, "name", name, nameEnd) &&
            FindNumAfterKey(obj, "level", level)) {
            if (level > bestLevel) {
                bestLevel = level;
                bestName = name;
            }
        }
        cur = objEnd + 1;
    }
    return bestName;
}

// -------- HTTP ------------------------------------------------------------

bool HttpPostJson(const wchar_t* path, const std::string& body, std::string& out) {
    HINTERNET hSession = WinHttpOpen(L"Kaentake-CosmicSSO/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // Short timeouts — this is a sidecar, we never want to stall the UI.
    WinHttpSetTimeouts(hSession, 2000, 2000, 4000, 4000);

    HINTERNET hConnect = WinHttpConnect(hSession, kCosmicHost, kCosmicPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path,
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false;
    }

    const wchar_t kHeaders[] = L"Content-Type: application/json\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, kHeaders, (DWORD)-1L,
                                   const_cast<char*>(body.data()), (DWORD)body.size(),
                                   (DWORD)body.size(), 0);
    BOOL got = sent && WinHttpReceiveResponse(hRequest, nullptr);

    if (got) {
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        out.clear();
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
            out.append(buf.data(), read);
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return status == 200;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return false;
}

// -------- file writing ----------------------------------------------------

std::wstring AppDataCosmicDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(buf);
    dir += L"\\Cosmic";
    CreateDirectoryW(dir.c_str(), nullptr);  // ignore "already exists"
    return dir;
}

bool WriteEnvelopeAtomic(const std::wstring& dir, const std::string& body) {
    std::wstring tmp = dir + L"\\sso.json.tmp";
    std::wstring dst = dir + L"\\sso.json";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != body.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// -------- main worker -----------------------------------------------------

void Worker(std::string account, std::string password) {
    if (account.empty() || password.empty()) return;

    std::string loginBody = "{\"account\":\"" + JsonEscape(account)
                          + "\",\"password\":\"" + JsonEscape(password) + "\"}";

    std::string loginResp;
    if (!HttpPostJson(kLoginPath, loginBody, loginResp)) return;
    if (loginResp.find("\"ok\":true") == std::string::npos) return;

    std::string character = PickTopCharacter(loginResp);
    if (character.empty()) return;

    std::string attestBody = "{\"account\":\"" + JsonEscape(account)
                           + "\",\"password\":\"" + JsonEscape(password)
                           + "\",\"character\":\"" + JsonEscape(character) + "\"}";

    std::string envelope;
    if (!HttpPostJson(kAttestPath, attestBody, envelope)) return;
    if (envelope.find("\"ok\":true") == std::string::npos) return;

    std::wstring dir = AppDataCosmicDir();
    if (dir.empty()) return;
    WriteEnvelopeAtomic(dir, envelope);
}

}  // namespace

// Public entry point — rememberpw.cpp calls this from the login hook.
// Fire-and-forget: spawns a detached worker and returns immediately so the
// MapleStory UI thread keeps running.
void DispatchCosmicSsoWrite(const std::string& account, const std::string& password) {
    if (account.empty() || password.empty()) return;
    try {
        std::thread(Worker, account, password).detach();
    } catch (...) {
        // std::thread can throw if the system is out of resources — silent.
    }
}
