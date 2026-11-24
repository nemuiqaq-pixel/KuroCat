#pragma once
/// ============================================================================
/// NetworkBridge — KuroCat 联网同步模块（零外部依赖）
/// ----------------------------------------------------------------------------
/// 用 WinHTTP 实现的一个简单 WebSocket 客户端，负责：
///   1. 定时上报自己的 KPM / 总按键数 / 心情状态
///   2. 接收房间内朋友的 KPM 数据
///   3. 维护 std::vector<FriendState> 列表（主线程可读）
///
/// 完全后台线程，不阻塞 60fps 渲染循环。
/// 只需在 main.cpp 里：
///     #include "NetworkBridge.hpp"
///     AppState::net   = NetworkBridge();         // 成员
///     net.connect(url, name, room);              // 初始化
///     net.pollIncoming();                        // renderFrame 每帧调一次
///     net.sendKPM(input.currentKPM(), ...);      // 每 2 秒调一次
///     net.disconnect();                          // WM_DESTROY
/// ============================================================================
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <deque>
#include <sstream>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

struct FriendState {
    std::string userId;
    std::string name;
    int         kpm       = 0;
    int         totalKeys = 0;
    std::string mood;         // sleeping / idle / typing / busy / panicking
    uint64_t    timestamp = 0;
    bool        online    = true;
};

class NetworkBridge {
public:
    struct Config {
        std::string serverUrl;    // e.g. "wss://friend-kpm-xxxx.workers.dev"
        std::string room;         // e.g. "office"
        std::string username;     // 用户昵称
        int         reportIntervalMs = 2000; // 上报间隔
    };

    NetworkBridge() = default;
    ~NetworkBridge() { disconnect(); }

    // ──────────── 生命周期 ────────────
    void connect(const Config& cfg) {
        std::lock_guard<std::mutex> lk(mtx_);
        config_ = cfg;
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this]() { run(); });
    }

    void disconnect() {
        running_ = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (socket_) {
                WinHttpCloseHandle(socket_);
                socket_ = nullptr;
            }
        }
        if (thread_.joinable()) thread_.join();
    }

    bool isConnected() const { return connected_; }

    // ──────────── 定时上报（由主线程每 2 秒调一次）────────────
    void sendKPM(int kpm, int totalKeys, const std::string& mood) {
        // 用 WinHTTP HTTP 接口发（最简单的实现，1 个 POST 搞定）
        // 走 HTTP /api/kpm 比 WebSocket 省心，而且 Durable Object 后端也能接
        if (!running_ || config_.serverUrl.empty()) return;

        std::string url = config_.serverUrl;
        // 把 wss:// 转成 https://
        if (url.rfind("wss://", 0) == 0) url.replace(0, 6, "https://");
        if (url.rfind("ws://", 0) == 0)  url.replace(0, 5, "http://");

        std::string path = "/api/kpm";
        auto slash = url.find('/', 8);
        std::string host = (slash == std::string::npos) ? url : url.substr(8, slash - 8);

        // JSON body
        std::ostringstream body;
        body << "{\"name\":\"" << escapeJson_(config_.username)
             << "\",\"room\":\"" << escapeJson_(config_.room)
             << "\",\"kpm\":" << kpm
             << ",\"totalKeys\":" << totalKeys
             << ",\"mood\":\"" << escapeJson_(mood) << "\"}";
        std::string bodyStr = body.str();

        postHttp_(host, path, bodyStr);
    }

    // ──────────── 取朋友列表（主线程只读，拿到 copy）────────────
    std::vector<FriendState> friends() {
        std::lock_guard<std::mutex> lk(dataMtx_);
        return friends_;
    }

    // 本地 KPM 窗口（Durable Object 也需要这个）
    static std::string moodFromKPM(int kpm, double idleSec) {
        if (idleSec > 8.0)             return "sleeping";
        if (kpm == 0)                  return "idle";
        if (kpm < 20)                  return "typing";
        if (kpm < 50)                  return "busy";
        return "panicking";
    }

    // 配置持久化
    static void saveToIni(const std::string& iniPath, const Config& cfg) {
        HKEY hk = nullptr;
        auto keyPath = L"Software\\KuroCat";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            auto write = [&](const wchar_t* name, const std::string& val) {
                auto wval = utf8ToWide_(val);
                RegSetValueExW(hk, name, 0, REG_SZ,
                    (const BYTE*)wval.c_str(), (DWORD)(wval.size() + 1) * sizeof(wchar_t));
            };
            write(L"FriendServer", cfg.serverUrl);
            write(L"FriendRoom",   cfg.room);
            write(L"FriendName",   cfg.username);
            RegCloseKey(hk);
        }
    }

    static Config loadFromIni() {
        Config cfg;
        HKEY hk = nullptr;
        auto keyPath = L"Software\\KuroCat";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
            auto readStr = [&](const wchar_t* name) -> std::string {
                DWORD type = 0, size = 256;
                wchar_t buf[256] = {};
                if (RegQueryValueExW(hk, name, 0, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS)
                    return wideToUtf8_(buf);
                return "";
            };
            cfg.serverUrl = readStr(L"FriendServer");
            cfg.room      = readStr(L"FriendRoom");
            cfg.username  = readStr(L"FriendName");
            RegCloseKey(hk);
        }
        return cfg;
    }

    // ── 供外部调用的 UTF 转换（main.cpp 命令行参数解析需要）──
    static std::wstring utf8ToWide_(const std::string& s) {
        if (s.empty()) return L"";
        int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring out(needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), needed);
        return out;
    }
    static std::string wideToUtf8_(const std::wstring& s) {
        if (s.empty()) return "";
        int needed = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), needed, nullptr, nullptr);
        return out;
    }

private:
    std::mutex mtx_;
    std::mutex dataMtx_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    Config config_;
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET socket_  = nullptr;
    std::vector<FriendState> friends_;

    // ──────────── 后台线程主循环 ────────────
    void run() {
        // 用简单的长轮询比 WebSocket 更稳
        // 每 2 秒发一次心跳到 /api/poll，服务器返回朋友列表
        while (running_) {
            try {
                auto friendList = pollFriends_();
                if (!friendList.empty()) {
                    std::lock_guard<std::mutex> lk(dataMtx_);
                    friends_ = friendList;
                    connected_ = true;
                }
            } catch (...) {
                connected_ = false;
            }
            // 每 2 秒轮询一次
            for (int i = 0; i < 20 && running_; i++)
                Sleep(100);
        }
    }

    // ──────────── HTTP POST 上报 ────────────
    // 同步、无锁、每次独立 session（简单可靠）
    bool postHttp_(const std::string& host, const std::string& path,
                   const std::string& jsonBody) {
        bool ok = false;
        HINTERNET session = WinHttpOpen(
            L"KuroCat-NetworkBridge/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;

        HINTERNET connect = WinHttpConnect(session, utf8ToWide_(host).c_str(),
                                           INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connect) {
            HINTERNET request = WinHttpOpenRequest(connect, L"POST",
                utf8ToWide_(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (request) {
                WinHttpAddRequestHeaders(request,
                    L"Content-Type: application/json\r\n", (DWORD)-1);
                auto wbody = utf8ToWide_(jsonBody);
                DWORD bodyLen = (DWORD)jsonBody.size();
                WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    (LPVOID)jsonBody.c_str(), bodyLen, bodyLen, 0);
                ok = WinHttpReceiveResponse(request, nullptr);
                WinHttpCloseHandle(request);
            }
            WinHttpCloseHandle(connect);
        }
        WinHttpCloseHandle(session);
        return ok;
    }

    // ──────────── HTTP GET 轮询朋友列表 ────────────
    std::vector<FriendState> pollFriends_() {
        std::vector<FriendState> result;
        std::string url = config_.serverUrl;
        if (url.rfind("wss://", 0) == 0) url.replace(0, 6, "https://");
        if (url.rfind("ws://", 0) == 0)  url.replace(0, 5, "http://");

        std::string path = "/api/room/" + urlEncode_(config_.room) +
                           "?name=" + urlEncode_(config_.username);
        auto slash = url.find('/', 8);
        std::string host = (slash == std::string::npos) ? url : url.substr(8, slash - 8);
        bool useHttps = url.rfind("https://", 0) == 0;

        HINTERNET session = WinHttpOpen(
            L"KuroCat-NetworkBridge/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return result;

        HINTERNET connect = WinHttpConnect(session, utf8ToWide_(host).c_str(),
            useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
        if (connect) {
            HINTERNET request = WinHttpOpenRequest(connect, L"GET",
                utf8ToWide_(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                useHttps ? WINHTTP_FLAG_SECURE : 0);
            if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                        && WinHttpReceiveResponse(request, nullptr)) {
                DWORD size = 8192;
                std::vector<char> buf(size + 1);
                DWORD got = 0;
                if (WinHttpReadData(request, buf.data(), size, &got)) {
                    buf[got] = 0;
                    result = parseFriendsJson_(std::string(buf.data(), got));
                }
                WinHttpCloseHandle(request);
            }
            WinHttpCloseHandle(connect);
        }
        WinHttpCloseHandle(session);
        return result;
    }

    // ──────────── 极简 JSON 解析（不用第三方库）────────────
    // 只处理我们这个特定格式的 JSON 数组
    std::vector<FriendState> parseFriendsJson_(const std::string& json) {
        std::vector<FriendState> out;
        // 找所有 { "userId": ..., "name": ..., "kpm": ..., "mood": ..., ... }
        size_t pos = 0;
        while ((pos = json.find("\"userId\"", pos)) != std::string::npos) {
            FriendState f;
            f.userId = extractString_(json, "\"userId\"");
            f.name   = extractString_(json, "\"name\"");
            f.mood   = extractString_(json, "\"mood\"");
            f.kpm       = extractInt_(json, "\"kpm\"");
            f.totalKeys = extractInt_(json, "\"totalKeys\"");
            f.online    = true;
            if (!f.userId.empty()) out.push_back(std::move(f));
            pos++;
        }
        return out;
    }

    static std::string extractString_(const std::string& s, const std::string& key) {
        size_t k = s.find(key);
        if (k == std::string::npos) return "";
        size_t colon = s.find(':', k);
        if (colon == std::string::npos) return "";
        size_t q1 = s.find('"', colon);
        if (q1 == std::string::npos) return "";
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return s.substr(q1 + 1, q2 - q1 - 1);
    }

    static int extractInt_(const std::string& s, const std::string& key) {
        size_t k = s.find(key);
        if (k == std::string::npos) return 0;
        size_t colon = s.find(':', k);
        if (colon == std::string::npos) return 0;
        size_t start = colon + 1;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) start++;
        size_t end = start;
        while (end < s.size() && (isdigit(s[end]) || s[end] == '-')) end++;
        if (end == start) return 0;
        try { return std::stoi(s.substr(start, end - start)); } catch (...) { return 0; }
    }

    // ──────────── 辅助函数 ────────────
    static std::string escapeJson_(const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c >= 32) out += c;
        }
        return out;
    }
    static std::string urlEncode_(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_') out += c;
            else { char buf[4]; sprintf(buf, "%%%02X", c); out += buf; }
        }
        return out;
    }
};
