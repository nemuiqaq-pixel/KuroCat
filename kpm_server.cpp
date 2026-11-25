// KuroCat Team Backend — 单文件 C++ 版
// 零依赖，编译成 exe，双击就能跑
// 用法: KpmServer.exe [port]

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <mutex>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

struct Friend {
    std::string userId;
    std::string name;
    int kpm, totalKeys;
    std::string mood;
    long long timestamp;
    double _ts;  // 内部过期时间戳
};

std::vector<Friend> g_friends;
std::mutex g_lock;
const double CUTOFF = 120.0;

double nowSec() {
    return (double)time(nullptr);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

void cleanup() {
    double now = nowSec();
    std::lock_guard<std::mutex> lk(g_lock);
    g_friends.erase(
        std::remove_if(g_friends.begin(), g_friends.end(),
            [now](const Friend& f) { return now - f._ts > CUTOFF; }),
        g_friends.end());
}

std::string buildJsonResponse(const std::string& body) {
    char header[512];
    int bodyLen = (int)body.size();
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n", bodyLen);
    return std::string(header) + body;
}

void handleClient(SOCKET clientSock) {
    char buf[8192];
    memset(buf, 0, sizeof(buf));
    
    // 收请求
    int total = 0, n;
    while (true) {
        n = recv(clientSock, buf + total, sizeof(buf) - total - 1, 0);
        if (n <= 0) break;
        total += n;
        if (strstr(buf, "\r\n\r\n")) break;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    
    if (total == 0) { closesocket(clientSock); return; }
    buf[total] = '\0';
    
    std::string req(buf);
    std::string method = req.substr(0, req.find(' '));
    size_t pathStart = req.find(' ') + 1;
    size_t pathEnd = req.find(' ', pathStart);
    std::string path = req.substr(pathStart, pathEnd - pathStart);
    
    // OPTIONS preflight
    if (method == "OPTIONS") {
        std::string resp = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n";
        send(clientSock, resp.c_str(), (int)resp.size(), 0);
        closesocket(clientSock);
        return;
    }
    
    cleanup();
    
    // GET /health 或 GET /
    if ((path == "/" || path == "/health") && method == "GET") {
        std::string body = "{\"ok\":true,\"count\":";
        int count = 0;
        double now = nowSec();
        {
            std::lock_guard<std::mutex> lk(g_lock);
            for (auto& f : g_friends) if (now - f._ts <= CUTOFF) count++;
        }
        body += std::to_string(count) + "}";
        std::string resp = buildJsonResponse(body);
        send(clientSock, resp.c_str(), (int)resp.size(), 0);
        closesocket(clientSock);
        return;
    }
    
    // GET /api/kpm —— 列出所有朋友
    if (path == "/api/kpm" && method == "GET") {
        std::string body = "[";
        double now = nowSec();
        {
            std::lock_guard<std::mutex> lk(g_lock);
            bool first = true;
            for (auto& f : g_friends) {
                if (now - f._ts > CUTOFF) continue;
                if (!first) body += ",";
                first = false;
                char tmp[256];
                snprintf(tmp, sizeof(tmp),
                    "{\"userId\":\"%s\",\"name\":\"%s\",\"kpm\":%d,\"totalKeys\":%d,\"mood\":\"%s\",\"timestamp\":%lld}",
                    jsonEscape(f.userId).c_str(),
                    jsonEscape(f.name).c_str(),
                    f.kpm, f.totalKeys,
                    jsonEscape(f.mood).c_str(),
                    f.timestamp);
                body += tmp;
            }
        }
        body += "]";
        std::string resp = buildJsonResponse(body);
        send(clientSock, resp.c_str(), (int)resp.size(), 0);
        closesocket(clientSock);
        return;
    }
    
    // POST /api/kpm —— 上报 KPM
    if (path == "/api/kpm" && method == "POST") {
        // 找 Content-Length
        int contentLen = 0;
        size_t clPos = req.find("Content-Length:");
        if (clPos != std::string::npos) {
            clPos += 15;
            while (clPos < req.size() && (req[clPos] == ' ' || req[clPos] == '\t')) clPos++;
            while (clPos < req.size() && isdigit(req[clPos])) {
                contentLen = contentLen * 10 + (req[clPos] - '0');
                clPos++;
            }
        }
        
        // 找 body
        size_t bodyStart = req.find("\r\n\r\n");
        if (bodyStart != std::string::npos) bodyStart += 4;
        else bodyStart = total;
        std::string bodyJson = req.substr(bodyStart, contentLen > 0 ? contentLen : total - bodyStart);
        
        // 简单解析 JSON（key:value）
        auto parseStr = [](const std::string& j, const std::string& key) -> std::string {
            size_t p = j.find("\"" + key + "\"");
            if (p == std::string::npos) return "";
            p = j.find(':', p) + 1;
            while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
            if (p >= j.size()) return "";
            if (j[p] == '"') {
                size_t end = j.find('"', p + 1);
                if (end == std::string::npos) return "";
                return j.substr(p + 1, end - p - 1);
            }
            size_t end = p;
            while (end < j.size() && j[end] != ',' && j[end] != '}' && j[end] != ' ') end++;
            return j.substr(p, end - p);
        };
        
        std::string name = parseStr(bodyJson, "name");
        if (name.empty()) name = "anon";
        if (name.size() > 16) name = name.substr(0, 16);
        
        std::string kpmStr = parseStr(bodyJson, "kpm");
        std::string totalStr = parseStr(bodyJson, "totalKeys");
        std::string mood = parseStr(bodyJson, "mood");
        if (mood.empty()) mood = "idle";
        if (mood.size() > 16) mood = mood.substr(0, 16);
        
        int kpm = atoi(kpmStr.c_str());
        int totalKeys = atoi(totalStr.c_str());
        if (kpm < 0) kpm = 0;
        if (kpm > 999) kpm = 999;
        
        Friend f;
        char uid[32];
        snprintf(uid, sizeof(uid), "%s_%06d", name.c_str(), (int)(time(nullptr) % 1000000));
        f.userId = uid;
        f.name = name;
        f.kpm = kpm;
        f.totalKeys = totalKeys;
        f.mood = mood;
        f.timestamp = (long long)(time(nullptr) * 1000);
        f._ts = nowSec();
        
        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_friends.push_back(f);
        }
        
        printf("  🐱 %-10s | KPM=%3d | %s\n", f.name.c_str(), f.kpm, f.mood.c_str());
        
        char respBody[256];
        snprintf(respBody, sizeof(respBody), "{\"ok\":true,\"userId\":\"%s\"}", f.userId.c_str());
        std::string resp = buildJsonResponse(respBody);
        send(clientSock, resp.c_str(), (int)resp.size(), 0);
        closesocket(clientSock);
        return;
    }
    
    // 404
    std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(clientSock, resp.c_str(), (int)resp.size(), 0);
    closesocket(clientSock);
}

int main(int argc, char* argv[]) {
    int port = 8765;
    if (argc > 1) port = atoi(argv[1]);
    
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("❌ 端口 %d 被占用！换一个: KpmServer.exe 8766\n", port);
        WSACleanup();
        return 1;
    }
    
    listen(server, 64);
    
    printf("=" * 50);
    printf("\n🐱 KuroCat Team Backend 启动!\n");
    printf("   本地: http://localhost:%d\n", port);
    printf("=" * 50);
    printf("\n按 Ctrl+C 停止\n\n");
    
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server, &fds);
        timeval tv{1, 0};
        int ret = select(0, &fds, nullptr, nullptr, &tv);
        if (ret == SOCKET_ERROR) break;
        if (ret == 0) continue;  // 超时，循环
        
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(server, (sockaddr*)&clientAddr, &clientLen);
        if (clientSock == INVALID_SOCKET) continue;
        handleClient(clientSock);
    }
    
    closesocket(server);
    WSACleanup();
    printf("\n👋 已停止\n");
    return 0;
}
