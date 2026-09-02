#include "heart_rate_http.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

namespace heart_rate {

namespace {

// Overlay page served to the OBS browser source. It polls /api once a second
// and keeps the number updated without a full page reload (no flicker).
constexpr const char* kOverlayHtml = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Heart Rate</title>
<style>
  html, body { margin:0; padding:0; height:100%; background:transparent; overflow:hidden; }
  body { font-family:"Segoe UI", "Microsoft YaHei", -apple-system, sans-serif;
         color:#fff; display:flex; align-items:center; justify-content:center;
         text-align:center; user-select:none; }
  .row { display:flex; align-items:center; justify-content:center; gap:18px; }
  .heart { width:132px; height:132px; transform-origin:50% 50%;
           animation:pulse 1s ease-in-out infinite;
           filter: drop-shadow(0 0 22px rgba(255,77,94,.6)); }
  @keyframes pulse { 0%,100%{transform:scale(1);} 50%{transform:scale(1.16);} }
  .bpm { font-size:150px; font-weight:800; line-height:1.05; color:#ff4d5e;
         text-shadow:0 0 28px rgba(255,77,94,.65); font-variant-numeric:tabular-nums; }
  .unit { font-size:28px; letter-spacing:8px; color:#ffb3ba; margin-top:6px; }
  .meta { margin-top:18px; font-size:20px; color:rgba(255,255,255,.85); }
</style>
</head>
<body>
  <div class="wrap">
    <div class="row">
      <svg class="heart" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
        <path fill="#ff4d5e" d="M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z"/>
      </svg>
      <div class="bpm" id="bpm">--</div>
    </div>
    <div class="unit">BPM</div>
    <div class="meta" id="meta">正在等待数据…</div>
  </div>
<script>
  var bpmEl = document.getElementById('bpm');
  var metaEl = document.getElementById('meta');
  function tick() {
    fetch('/api', {cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
      bpmEl.textContent = d.connected ? d.bpm : '--';
      if (d.connected) {
        metaEl.textContent = d.hasContact ? (d.contact ? '已佩戴' : '未佩戴') : '心率广播';
      } else {
        metaEl.textContent = d.status || '等待连接…';
      }
    }).catch(function(){});
  }
  tick();
  setInterval(tick, 1000);
</script>
</body>
</html>
)HTML";

std::string httpResponse(const char* contentType, const std::string& body) {
    std::string out;
    out.reserve(body.size() + 160);
    out += "HTTP/1.1 200 OK\r\n";
    out += "Content-Type: ";
    out += contentType;
    out += "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "Connection: close\r\n";
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    return out;
}

// Extract the request path (second token of the request line).
std::string requestPath(const std::string& request) {
    const std::size_t firstSpace = request.find(' ');
    if (firstSpace == std::string::npos) {
        return "/";
    }
    const std::size_t secondSpace = request.find(' ', firstSpace + 1);
    if (secondSpace == std::string::npos) {
        return request.substr(firstSpace + 1);
    }
    return request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
}

} // namespace

struct HeartRateHttpServer::Impl {
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    SOCKET listenSocket = INVALID_SOCKET;
    int port_ = 0;
    JsonProvider provider;

    void run() {
        running.store(true);

        while (!stopRequested) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);
            timeval timeout{1, 0};

            const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
            if (selected == SOCKET_ERROR) {
                break;
            }
            if (selected == 0) {
                continue; // timeout: re-check stopRequested
            }

            SOCKET client = accept(listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            handleClient(client);
            closesocket(client);
        }

        running.store(false);
    }

    void handleClient(SOCKET client) {
        char buffer[4096];
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
        if (received <= 0) {
            return;
        }
        buffer[received] = '\0';

        const std::string request(buffer, received);
        const std::string path = requestPath(request);

        std::string body;
        const char* contentType = "text/plain; charset=utf-8";

        if (path == "/api" || path.rfind("/api?", 0) == 0) {
            body = provider ? provider() : "{}";
            contentType = "application/json; charset=utf-8";
        } else {
            body = kOverlayHtml;
            contentType = "text/html; charset=utf-8";
        }

        const std::string response = httpResponse(contentType, body);
        send(client, response.data(), static_cast<int>(response.size()), 0);
    }
};

HeartRateHttpServer::HeartRateHttpServer() : impl_(std::make_unique<Impl>()) {}

HeartRateHttpServer::~HeartRateHttpServer() {
    stop();
}

bool HeartRateHttpServer::start(int port, JsonProvider provider, std::string* errorOut) {
    if (!impl_ || impl_->running.load()) {
        if (errorOut) {
            *errorOut = "already running";
        }
        return false;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        if (errorOut) {
            *errorOut = "WSAStartup failed";
        }
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        if (errorOut) {
            *errorOut = "socket() failed";
        }
        WSACleanup();
        return false;
    }

    const BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        if (errorOut) {
            char buf[128]{};
            std::snprintf(buf, sizeof(buf), "bind() failed (port %d already in use?)", port);
            *errorOut = buf;
        }
        closesocket(sock);
        WSACleanup();
        return false;
    }

    if (listen(sock, SOMAXCONN) == SOCKET_ERROR) {
        if (errorOut) {
            *errorOut = "listen() failed";
        }
        closesocket(sock);
        WSACleanup();
        return false;
    }

    impl_->listenSocket = sock;
    impl_->port_ = port;
    impl_->provider = std::move(provider);
    impl_->stopRequested.store(false);
    impl_->worker = std::thread([this] { impl_->run(); });
    return true;
}

void HeartRateHttpServer::stop() {
    if (!impl_) {
        return;
    }
    impl_->stopRequested.store(true);
    if (impl_->listenSocket != INVALID_SOCKET) {
        // Wake the accept loop so it observes stopRequested promptly.
        closesocket(impl_->listenSocket);
        impl_->listenSocket = INVALID_SOCKET;
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    WSACleanup();
}

bool HeartRateHttpServer::running() const {
    return impl_ && impl_->running.load();
}

int HeartRateHttpServer::port() const {
    return impl_ ? impl_->port_ : 0;
}

} // namespace heart_rate
