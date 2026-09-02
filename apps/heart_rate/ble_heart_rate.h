#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace heart_rate {

// One parsed heart-rate measurement from a BLE notification.
struct Sample {
    int bpm = 0;            // heart rate in beats per minute
    bool hasContact = false; // whether the sensor-contact bit is present
    bool contact = false;    // sensor contact detected (band is being worn)
};

// Invoked on the BLE worker thread whenever a new measurement arrives.
using SampleCallback = std::function<void(const Sample&)>;

// Invoked on the BLE worker thread when the connection phase changes.
// `phase` is one of: "idle", "scanning", "connecting", "connected", "error".
using StatusCallback = std::function<void(const std::string& phase, const std::string& detail)>;

// Scans for a device advertising the Bluetooth Heart Rate Service (0x180D),
// connects to it, subscribes to the Heart Rate Measurement characteristic
// (0x2A37) and delivers parsed samples. Mirrors the reference Rust demo
// (miband-heart-rate) but implemented with C++/WinRT so the whole app is a
// single self-contained executable.
class HeartRateMonitor {
public:
    HeartRateMonitor();
    ~HeartRateMonitor();

    HeartRateMonitor(const HeartRateMonitor&) = delete;
    HeartRateMonitor& operator=(const HeartRateMonitor&) = delete;

    // Starts scanning/connecting on a background thread. Thread-safe.
    void start(SampleCallback onSample, StatusCallback onStatus);

    // Stops and joins the background thread. Thread-safe and idempotent.
    void stop();

    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heart_rate
