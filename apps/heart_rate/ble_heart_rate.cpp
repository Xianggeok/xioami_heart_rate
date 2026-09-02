#include "ble_heart_rate.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace heart_rate {

namespace {

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;

// Bluetooth SIG assigned 16-bit UUIDs.
// Heart Rate Service     : 0x180D
// Heart Rate Measurement : 0x2A37
constexpr guid kHeartRateService{0x0000180D, 0x0000, 0x1000,
                                 {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
constexpr guid kHeartRateMeasurement{0x00002A37, 0x0000, 0x1000,
                                     {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

// Parse a Heart Rate Measurement characteristic value (per Bluetooth GATT
// Heart Rate Service spec). Layout:
//   byte 0: flags
//     bit 0: heart rate value is 16-bit (otherwise 8-bit)
//     bit 1: sensor contact supported + contact detected
//     bit 2: sensor contact supported + contact NOT detected
//   byte 1: heart rate (low byte)
//   byte 2: heart rate (high byte, only when 16-bit)
Sample parseSample(const IBuffer& buffer) {
    Sample sample;
    if (!buffer || buffer.Length() < 2) {
        return sample;
    }

    DataReader reader = DataReader::FromBuffer(buffer);
    const uint8_t flags = reader.ReadByte();

    int bpm = reader.ReadByte();
    if ((flags & 0x01) != 0 && reader.UnconsumedBufferLength() >= 1) {
        bpm |= static_cast<int>(reader.ReadByte()) << 8;
    }
    sample.bpm = bpm;

    if ((flags & 0x04) != 0) {
        sample.hasContact = true;
        sample.contact = (flags & 0x02) != 0;
    }
    return sample;
}

// Sleep in small slices so a stop request is honoured promptly.
void interruptibleSleep(std::chrono::milliseconds total,
                        const std::atomic<bool>& stop) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + total;
    while (!stop && clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// Initialise this thread's WinRT apartment (MTA) and keep it alive for the
// whole thread lifetime. Recent C++/WinRT headers return an RAII
// apartment_context whose destructor tears the apartment down; the older
// SDK-bundled headers return void and rely on thread exit for cleanup. Handle
// both so the guard object is safe to keep in scope either way.
auto initThreadApartment() {
    using result_t = decltype(init_apartment(apartment_type::multi_threaded));
    if constexpr (std::is_void_v<result_t>) {
        struct NoOp {
            ~NoOp() noexcept {}
        };
        init_apartment(apartment_type::multi_threaded);
        return NoOp{};
    } else {
        return init_apartment(apartment_type::multi_threaded);
    }
}

} // namespace

struct HeartRateMonitor::Impl {
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};

    // Owned by the Impl so they outlive the worker thread (and any in-flight
    // event handler invocation) until the next start() or destruction.
    SampleCallback onSample;
    StatusCallback onStatus;

    void run() {
        // Initialise this thread's WinRT apartment (MTA) and keep it alive for
        // the whole thread lifetime (RAII).
        auto apartment = initThreadApartment();

        running.store(true);

        while (!stopRequested) {
            onStatus("scanning", "正在搜索心率广播…");

            // ---- 1. Scan for a device advertising the Heart Rate service ----
            uint64_t deviceAddress = 0;
            std::string deviceName;
            {
                std::atomic<bool> found{false};
                std::atomic<uint64_t> foundAddress{0};
                std::string foundName;

                BluetoothLEAdvertisementWatcher watcher;

                BluetoothLEAdvertisement advertisement;
                advertisement.ServiceUuids().Append(kHeartRateService);
                BluetoothLEAdvertisementFilter filter;
                filter.Advertisement(advertisement);
                watcher.AdvertisementFilter(filter);

                event_token token = watcher.Received(
                    [&](BluetoothLEAdvertisementWatcher const&,
                        BluetoothLEAdvertisementReceivedEventArgs const& args) {
                        if (stopRequested || found.load()) {
                            return;
                        }
                        foundAddress.store(args.BluetoothAddress());
                        foundName = winrt::to_string(args.Advertisement().LocalName());
                        found.store(true);
                    });

                watcher.Start();

                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(12);
                while (!found.load() && !stopRequested &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                watcher.Stop();
                watcher.Received(token);

                if (stopRequested) {
                    break;
                }
                if (!found.load()) {
                    onStatus("idle",
                             "未找到设备，请在手环上开启「心率广播」功能。");
                    interruptibleSleep(std::chrono::seconds(2), stopRequested);
                    continue;
                }

                deviceAddress = foundAddress.load();
                deviceName = std::move(foundName);
            }

            // ---- 2. Open the device by its Bluetooth address ----
            const std::string target =
                deviceName.empty() ? std::string("device") : deviceName;
            onStatus("connecting", "正在连接 " + target + "…");

            BluetoothLEDevice device = nullptr;
            try {
                device = BluetoothLEDevice::FromBluetoothAddressAsync(deviceAddress).get();
            } catch (const hresult_error&) {
                device = nullptr;
            }
            if (!device) {
                onStatus("error", "无法打开设备。");
                interruptibleSleep(std::chrono::seconds(2), stopRequested);
                continue;
            }

            if (deviceName.empty()) {
                deviceName = winrt::to_string(device.Name());
            }

            // ---- 3. Resolve the service and measurement characteristic ----
            GattDeviceService service = nullptr;
            GattCharacteristic characteristic = nullptr;
            try {
                GattDeviceServicesResult services =
                    device.GetGattServicesForUuidAsync(kHeartRateService).get();
                if (services &&
                    services.Status() == GattCommunicationStatus::Success &&
                    services.Services().Size() > 0) {
                    service = services.Services().GetAt(0);
                }
                if (service) {
                    GattCharacteristicsResult chars =
                        service.GetCharacteristicsForUuidAsync(kHeartRateMeasurement).get();
                    if (chars &&
                        chars.Status() == GattCommunicationStatus::Success &&
                        chars.Characteristics().Size() > 0) {
                        characteristic = chars.Characteristics().GetAt(0);
                    }
                }
            } catch (const hresult_error&) {
                characteristic = nullptr;
            }

            if (!characteristic) {
                onStatus("error", "设备上未找到心率服务。");
                if (service) {
                    service.Close();
                }
                device.Close();
                interruptibleSleep(std::chrono::seconds(2), stopRequested);
                continue;
            }

            // ---- 4. Subscribe to notifications ----
            try {
                GattCommunicationStatus status =
                    characteristic
                        .WriteClientCharacteristicConfigurationDescriptorAsync(
                            GattClientCharacteristicConfigurationDescriptorValue::Notify)
                        .get();
                if (status != GattCommunicationStatus::Success) {
                    onStatus("error", "启用心率通知失败。");
                    service.Close();
                    device.Close();
                    interruptibleSleep(std::chrono::seconds(2), stopRequested);
                    continue;
                }
            } catch (const hresult_error&) {
                onStatus("error", "启用心率通知失败。");
                service.Close();
                device.Close();
                interruptibleSleep(std::chrono::seconds(2), stopRequested);
                continue;
            }

            const std::string connectedText =
                deviceName.empty() ? "已连接" : ("已连接 " + deviceName);
            onStatus("connected", connectedText);

            // ---- 5. Receive notifications until disconnect / stop ----
            std::atomic<bool> disconnected{false};

            event_token valueToken = characteristic.ValueChanged(
                [&](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
                    if (stopRequested) {
                        return;
                    }
                    const Sample sample = parseSample(args.CharacteristicValue());
                    if (sample.bpm > 0) {
                        onSample(sample);
                    }
                });

            event_token statusToken = device.ConnectionStatusChanged(
                [&](auto const& dev, auto const&) {
                    if (dev.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
                        disconnected.store(true);
                    }
                });

            while (!disconnected.load() && !stopRequested) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            characteristic.ValueChanged(valueToken);
            device.ConnectionStatusChanged(statusToken);
            if (service) {
                service.Close();
            }
            device.Close();

            if (stopRequested) {
                break;
            }
            onStatus("scanning", "连接断开，正在重新扫描…");
        }

        running.store(false);
    }
};

HeartRateMonitor::HeartRateMonitor() : impl_(std::make_unique<Impl>()) {}

HeartRateMonitor::~HeartRateMonitor() {
    stop();
}

void HeartRateMonitor::start(SampleCallback onSample, StatusCallback onStatus) {
    if (!impl_ || impl_->running.load()) {
        return;
    }
    impl_->onSample = std::move(onSample);
    impl_->onStatus = std::move(onStatus);
    impl_->stopRequested.store(false);
    impl_->worker = std::thread([this] {
        impl_->run();
    });
}

void HeartRateMonitor::stop() {
    if (!impl_) {
        return;
    }
    impl_->stopRequested.store(true);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

bool HeartRateMonitor::running() const {
    return impl_ && impl_->running.load();
}

} // namespace heart_rate
