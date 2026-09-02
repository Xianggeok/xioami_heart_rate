#pragma once

#include <functional>
#include <memory>
#include <string>

namespace heart_rate {

// Minimal single-purpose HTTP server for the OBS browser source.
//
// It serves:
//   GET /     -> a self-refreshing overlay page (live heart rate)
//   GET /api  -> the current state as JSON: {"bpm":N,"connected":bool,
//                "contact":bool,"hasContact":bool,"status":"..."}
//
// Point an OBS "Browser" source at http://127.0.0.1:<port>/ to show the live
// heart rate during a stream.
class HeartRateHttpServer {
public:
    // Returns the JSON state document for /api. Called on the server thread.
    using JsonProvider = std::function<std::string()>;

    HeartRateHttpServer();
    ~HeartRateHttpServer();

    HeartRateHttpServer(const HeartRateHttpServer&) = delete;
    HeartRateHttpServer& operator=(const HeartRateHttpServer&) = delete;

    // Binds 127.0.0.1:<port> and starts accepting connections on a background
    // thread. Returns false and fills `errorOut` (optional) on failure.
    bool start(int port, JsonProvider provider, std::string* errorOut = nullptr);

    // Stops and joins the background thread. Thread-safe and idempotent.
    void stop();

    bool running() const;
    int port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heart_rate
