#include "eui_neo.h"

#include "assets_embedded.h"
#include "ble_heart_rate.h"
#include "heart_rate_http.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace app {
namespace {

// ---------------------------------------------------------------------------
// Palette (dark dashboard with a heart-red accent)
// ---------------------------------------------------------------------------
constexpr eui::Color kBackground{0.08f, 0.09f, 0.11f, 1.0f};
constexpr eui::Color kSurface   {0.13f, 0.14f, 0.17f, 1.0f};
constexpr eui::Color kSurface2  {0.17f, 0.18f, 0.22f, 1.0f};
constexpr eui::Color kSurface3  {0.22f, 0.24f, 0.28f, 1.0f};
constexpr eui::Color kInk       {0.94f, 0.96f, 0.98f, 1.0f};
constexpr eui::Color kMuted     {0.55f, 0.60f, 0.66f, 1.0f};
constexpr eui::Color kBorder    {0.28f, 0.31f, 0.36f, 1.0f};
constexpr eui::Color kAccent    {0.92f, 0.27f, 0.36f, 1.0f};
constexpr eui::Color kGreen     {0.26f, 0.72f, 0.48f, 1.0f};
constexpr eui::Color kAmber     {0.95f, 0.64f, 0.22f, 1.0f};
constexpr eui::Color kClear     {0.0f, 0.0f, 0.0f, 0.0f};

// OBS browser source endpoint.
constexpr int kHttpPort = 3030;

// ---------------------------------------------------------------------------
// Live state (updated from the BLE worker thread)
// ---------------------------------------------------------------------------
std::atomic<int>  g_bpm{0};
std::atomic<bool> g_hasReading{false};
std::atomic<bool> g_contact{false};
std::atomic<bool> g_hasContact{false};

std::mutex g_statusMutex;
std::string g_phase = "idle";
std::string g_detail = "点击「开始」连接你的小米手环";

std::mutex g_historyMutex;
std::vector<int> g_history; // recent BPM readings, newest at the back

heart_rate::HeartRateMonitor g_monitor;
heart_rate::HeartRateHttpServer g_httpServer;
std::string g_httpError;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string number(int value) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%d", value);
    return buf;
}

eui::Color alpha(eui::Color color, float value) {
    color.a = std::clamp(value, 0.0f, 1.0f);
    return color;
}

eui::Transition transition() {
    return eui::Transition::make(0.18f, eui::Ease::OutCubic);
}

components::theme::ThemeColorTokens themeTokens() {
    return {kBackground, kAccent, kSurface, kSurface2, kSurface3, kInk, kBorder, true};
}

// ---------------------------------------------------------------------------
// Status lookups (Chinese)
// ---------------------------------------------------------------------------
eui::Color statusColor(const std::string& phase) {
    if (phase == "connected") return kGreen;
    if (phase == "scanning" || phase == "connecting") return kAmber;
    if (phase == "error") return kAccent;
    return kMuted;
}

std::string statusText(const std::string& phase) {
    if (phase == "connected") return "已连接";
    if (phase == "scanning") return "扫描中";
    if (phase == "connecting") return "连接中";
    if (phase == "error") return "错误";
    return "空闲";
}

// ---------------------------------------------------------------------------
// BLE callbacks (run on the BLE worker thread)
// ---------------------------------------------------------------------------
void onSample(const heart_rate::Sample& sample) {
    g_bpm.store(sample.bpm);
    g_hasReading.store(true);
    g_hasContact.store(sample.hasContact);
    g_contact.store(sample.contact);

    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        g_history.push_back(sample.bpm);
        if (g_history.size() > 60) {
            g_history.erase(g_history.begin());
        }
    }

    requestUpdate();
}

void onStatus(const std::string& phase, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_phase = phase;
        g_detail = detail;
    }
    requestUpdate();
}

void startMonitoring() {
    if (g_monitor.running()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_phase = "scanning";
        g_detail = "正在启动…";
    }
    g_monitor.start(onSample, onStatus);
}

void stopMonitoring() {
    g_monitor.stop();
    g_hasReading.store(false);
    g_bpm.store(0);
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_phase = "idle";
        g_detail = "已停止";
    }
    requestUpdate();
}

// ---------------------------------------------------------------------------
// OBS browser source (http://127.0.0.1:3030)
// ---------------------------------------------------------------------------
std::string heartRateJson() {
    const int bpm = g_bpm.load();
    const bool hasReading = g_hasReading.load();
    const bool hasContact = g_hasContact.load();
    const bool contact = g_contact.load();

    std::string phase;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        phase = g_phase;
    }

    const bool connected = (phase == "connected");
    char buf[320]{};
    std::snprintf(buf, sizeof(buf),
                  "{\"bpm\":%d,\"connected\":%s,\"contact\":%s,\"hasContact\":%s,\"status\":\"%s\"}",
                  hasReading ? bpm : 0,
                  connected ? "true" : "false",
                  contact ? "true" : "false",
                  hasContact ? "true" : "false",
                  statusText(phase).c_str());
    return buf;
}

void ensureHttpServer() {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;

    std::string error;
    if (!g_httpServer.start(kHttpPort, heartRateJson, &error)) {
        g_httpError = error;
    }
}

// ---------------------------------------------------------------------------
// Embedded assets (single-exe build)
// ---------------------------------------------------------------------------
struct ExtractedAssets {
    bool ok = false;
    std::string iconPng;
    std::string iconIco;
    std::string textFont;
    std::string iconFont;
};

bool writeEmbeddedFile(const std::filesystem::path& path,
                       const unsigned char* data, std::size_t len) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return out.good();
}

// Writes the embedded fonts/icons to a temp directory so the framework can load
// them from real file paths. Returns the paths to use in the app config.
ExtractedAssets extractEmbeddedAssets() {
    using namespace embedded_assets;

    ExtractedAssets assets;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "miband_heart_rate" / "assets";

    const std::filesystem::path iconPng = dir / "icon.png";
    const std::filesystem::path iconIco = dir / "icon.ico";
    const std::filesystem::path textFont = dir / "text.ttf";
    const std::filesystem::path iconFont = dir / "icons.otf";

    assets.ok = writeEmbeddedFile(iconPng, icon_png, icon_png_len) &&
                writeEmbeddedFile(iconIco, icon_ico, icon_ico_len) &&
                writeEmbeddedFile(textFont, text_font_ttf, text_font_ttf_len) &&
                writeEmbeddedFile(iconFont, font_awesome_otf, font_awesome_otf_len);

    assets.iconPng = iconPng.string();
    assets.iconIco = iconIco.string();
    assets.textFont = textFont.string();
    assets.iconFont = iconFont.string();
    return assets;
}

// ---------------------------------------------------------------------------
// UI widgets
// ---------------------------------------------------------------------------
void label(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
           const std::string& value, float fontSize, eui::Color color,
           eui::HorizontalAlign align = eui::HorizontalAlign::Left) {
    ui.text(id)
        .x(x).y(y).size(w, h)
        .text(value)
        .fontSize(fontSize)
        .lineHeight(fontSize + 4.0f)
        .color(color)
        .horizontalAlign(align)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

void panel(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
           eui::Color color = kSurface) {
    ui.rect(id)
        .x(x).y(y).size(w, h)
        .color(color)
        .radius(16.0f)
        .border(1.0f, alpha(kBorder, 0.7f))
        .build();
}

// Draw a straight line segment (a thin quad) between two points.
void drawLineSegment(eui::Ui& ui, const std::string& id,
                     float ax, float ay, float bx, float by,
                     float thickness, eui::Color color) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.001f) {
        return;
    }
    const float hw = thickness * 0.5f;
    const float nx = -dy / length * hw;
    const float ny = dx / length * hw;

    const float x0 = std::min({ax + nx, ax - nx, bx + nx, bx - nx});
    const float x1 = std::max({ax + nx, ax - nx, bx + nx, bx - nx});
    const float y0 = std::min({ay + ny, ay - ny, by + ny, by - ny});
    const float y1 = std::max({ay + ny, ay - ny, by + ny, by - ny});

    std::vector<eui::Vec2> pts = {
        {ax + nx - x0, ay + ny - y0},
        {ax - nx - x0, ay - ny - y0},
        {bx - nx - x0, by - ny - y0},
        {bx + nx - x0, by + ny - y0},
    };
    ui.polygon(id)
        .x(x0).y(y0).size(x1 - x0, y1 - y0)
        .points(std::move(pts))
        .color(color)
        .build();
}

void composeHeader(eui::Ui& ui, float x, float y, float w, float h, float s) {
    ui.text("header.icon")
        .x(x).y(y).size(h, h)
        .icon(0xF21E) // fa-heartbeat
        .fontSize(24.0f * s)
        .color(kAccent)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    label(ui, "header.title", x + h + 6.0f, y, std::max(120.0f, w * 0.4f), h,
          "小米手环心率", 21.0f * s, kInk);

    std::string phase;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        phase = g_phase;
    }

    const float pillW = 120.0f * s;
    const float pillX = x + w - pillW;
    ui.rect("header.status.pill")
        .x(pillX).y(y + h * 0.1f).size(pillW, h * 0.8f)
        .color(alpha(statusColor(phase), 0.14f))
        .radius(h * 0.4f)
        .border(1.0f, alpha(statusColor(phase), 0.40f))
        .build();
    ui.rect("header.status.dot")
        .x(pillX + 12.0f * s).y(y + h * 0.5f - 7.0f * s).size(14.0f * s, 14.0f * s)
        .color(statusColor(phase))
        .radius(7.0f * s)
        .build();
    label(ui, "header.status.text", pillX + 30.0f * s, y, pillW - 36.0f * s, h,
          statusText(phase), 14.0f * s, statusColor(phase));
}

void composeBigBpm(eui::Ui& ui, float x, float y, float w, float h, float s) {
    panel(ui, "big.bg", x, y, w, h);

    const int bpm = g_bpm.load();
    const bool hasReading = g_hasReading.load();

    // Live/idle tag
    ui.rect("big.live.dot")
        .x(x + 16.0f * s).y(y + 16.0f * s).size(11.0f * s, 11.0f * s)
        .color(hasReading ? kAccent : alpha(kMuted, 0.5f))
        .radius(5.5f * s)
        .build();
    label(ui, "big.live.text", x + 32.0f * s, y + 9.0f * s, 56.0f * s, 22.0f * s,
          hasReading ? "实时" : "待机", 12.0f * s, hasReading ? kAccent : kMuted);

    // Sensor contact indicator (top-right)
    const bool hasContact = g_hasContact.load();
    const bool contact = g_contact.load();
    std::string contactText = "--";
    eui::Color contactColor = kMuted;
    if (hasReading && hasContact) {
        contactText = contact ? "已佩戴" : "未佩戴";
        contactColor = contact ? kGreen : kAmber;
    } else if (hasReading) {
        contactText = "n/a";
    }
    const float contactW = 84.0f * s;
    const float contactX = x + w - contactW - 16.0f * s;
    ui.rect("big.contact.dot")
        .x(contactX).y(y + 20.0f * s).size(9.0f * s, 9.0f * s)
        .color(contactColor)
        .radius(4.5f * s)
        .build();
    label(ui, "big.contact.text", contactX + 14.0f * s, y + 11.0f * s,
          contactW - 14.0f * s, 24.0f * s, contactText, 12.0f * s, contactColor);

    // Big number
    const float numY = y + h * 0.18f;
    const float numH = h * 0.56f;
    ui.text("big.value")
        .x(x).y(numY).size(w, numH)
        .text(hasReading ? number(bpm) : "--")
        .fontSize(std::clamp(h * 0.52f, 46.0f, 100.0f))
        .lineHeight(numH)
        .color(hasReading ? kInk : kMuted)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    label(ui, "big.unit", x, numY + numH, w, 22.0f * s,
          "BPM · 次/分", 14.0f * s, kAccent, eui::HorizontalAlign::Center);

    // Connection detail
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        detail = g_detail;
    }
    label(ui, "big.detail", x + 20.0f, y + h - 30.0f * s, w - 40.0f, 22.0f * s,
          detail, 13.0f * s, kMuted, eui::HorizontalAlign::Center);
}

void composeStats(eui::Ui& ui, float x, float y, float w, float h, float s) {
    std::vector<int> hist;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        hist = g_history;
    }

    int minV = 0;
    int maxV = 0;
    int avg = 0;
    if (!hist.empty()) {
        minV = *std::min_element(hist.begin(), hist.end());
        maxV = *std::max_element(hist.begin(), hist.end());
        long long sum = 0;
        for (int v : hist) {
            sum += v;
        }
        avg = static_cast<int>(sum / static_cast<long long>(hist.size()));
    }

    const float gap = 10.0f * s;
    const float cardW = (w - gap * 2.0f) / 3.0f;

    auto card = [&](const std::string& id, int idx, const std::string& name,
                    const std::string& value, eui::Color color) {
        const float cx = x + static_cast<float>(idx) * (cardW + gap);
        panel(ui, id + ".bg", cx, y, cardW, h);
        label(ui, id + ".name", cx + 14.0f, y + 8.0f * s, cardW - 28.0f, 18.0f * s,
              name, 12.0f * s, kMuted);
        label(ui, id + ".value", cx + 14.0f, y + 26.0f * s, cardW - 28.0f, h - 30.0f * s,
              value, 22.0f * s, color);
    };

    card("stat.min", 0, "最低", hist.empty() ? "--" : number(minV), kMuted);
    card("stat.avg", 1, "平均", hist.empty() ? "--" : number(avg), kAccent);
    card("stat.max", 2, "最高", hist.empty() ? "--" : number(maxV), kGreen);
}

void composeHeartChart(eui::Ui& ui, float x, float y, float w, float h, float s) {
    panel(ui, "chart.bg", x, y, w, h);

    std::vector<int> hist;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        hist = g_history;
    }
    const std::size_t maxPoints = 30;
    if (hist.size() > maxPoints) {
        hist.erase(hist.begin(), hist.end() - static_cast<std::ptrdiff_t>(maxPoints));
    }

    label(ui, "chart.title", x + 16.0f, y + 10.0f * s, 160.0f, 24.0f * s,
          "心率历史", 15.0f * s, kMuted);

    if (g_hasReading.load() && !hist.empty()) {
        label(ui, "chart.current", x + w - 150.0f, y + 10.0f * s, 134.0f, 24.0f * s,
              "当前 " + number(hist.back()) + " BPM", 14.0f * s, kAccent,
              eui::HorizontalAlign::Right);
    }

    const float plotLeft = x + 46.0f;
    const float plotRight = x + w - 12.0f;
    const float plotTop = y + 42.0f * s;
    const float plotBottom = y + h - 24.0f * s;
    const float plotW = plotRight - plotLeft;
    const float plotH = plotBottom - plotTop;
    if (plotW <= 0.0f || plotH <= 0.0f) {
        return;
    }

    // Y axis: fixed 40..200 BPM, grid + labels.
    constexpr int yMin = 40;
    constexpr int yMax = 200;
    for (int i = 0; i <= 4; ++i) {
        const int bpm = yMin + i * (yMax - yMin) / 4;
        const float gy = plotBottom - static_cast<float>(bpm - yMin) /
                                     static_cast<float>(yMax - yMin) * plotH;
        ui.rect("chart.grid." + std::to_string(i))
            .x(plotLeft).y(gy).size(plotW, 1.0f)
            .color(alpha(kBorder, 0.55f))
            .build();
        label(ui, "chart.yl." + std::to_string(i), x + 2.0f, gy - 8.0f * s, 40.0f, 16.0f * s,
              number(bpm), 10.0f * s, kMuted, eui::HorizontalAlign::Right);
    }

    // X axis labels (time ago).
    label(ui, "chart.x.old", plotLeft - 6.0f, plotBottom + 6.0f * s, 60.0f, 14.0f * s,
          hist.empty() ? "" : "-" + number(static_cast<int>(hist.size()) - 1) + "s",
          10.0f * s, kMuted);
    label(ui, "chart.x.now", plotRight - 50.0f, plotBottom + 6.0f * s, 50.0f, 14.0f * s,
          "现在", 10.0f * s, kMuted, eui::HorizontalAlign::Right);

    if (hist.empty()) {
        label(ui, "chart.empty", plotLeft, plotTop, plotW, plotH,
              "暂无数据，点击「开始」连接手环", 13.0f * s, alpha(kMuted, 0.8f),
              eui::HorizontalAlign::Center);
        return;
    }

    // Build point coordinates.
    std::vector<eui::Vec2> pts;
    pts.reserve(hist.size());
    const float stepX = hist.size() > 1 ? plotW / static_cast<float>(hist.size() - 1) : 0.0f;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        const float px = hist.size() > 1 ? plotLeft + static_cast<float>(i) * stepX
                                         : plotLeft + plotW * 0.5f;
        const float norm = std::clamp(static_cast<float>(hist[i] - yMin) /
                                          static_cast<float>(yMax - yMin),
                                      0.0f, 1.0f);
        pts.push_back({px, plotBottom - norm * plotH});
    }

    // Line + dots.
    const float dotR = 3.0f * s;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        drawLineSegment(ui, "chart.seg." + std::to_string(i),
                        pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y,
                        2.4f, kAccent);
    }
    for (std::size_t i = 0; i < pts.size(); ++i) {
        ui.rect("chart.dot." + std::to_string(i))
            .x(pts[i].x - dotR).y(pts[i].y - dotR).size(dotR * 2.0f, dotR * 2.0f)
            .color(kAccent)
            .radius(dotR)
            .build();
    }

    // Current value label pinned next to the latest point.
    const eui::Vec2& last = pts.back();
    const float labelW = 52.0f;
    const float labelX = std::clamp(last.x - labelW * 0.5f, plotLeft - 6.0f, plotRight - labelW + 6.0f);
    const float labelY = last.y - 22.0f * s;
    label(ui, "chart.last.value", labelX, labelY, labelW, 16.0f * s,
          number(hist.back()), 12.0f * s, kAccent, eui::HorizontalAlign::Center);

    // Min / max markers.
    const int minIdx = static_cast<int>(std::min_element(hist.begin(), hist.end()) - hist.begin());
    const int maxIdx = static_cast<int>(std::max_element(hist.begin(), hist.end()) - hist.begin());
    auto marker = [&](const std::string& id, int idx, eui::Color color) {
        const eui::Vec2& p = pts[static_cast<std::size_t>(idx)];
        label(ui, id, std::clamp(p.x - 26.0f, plotLeft - 6.0f, plotRight - 46.0f),
              p.y - 20.0f * s, 52.0f, 15.0f * s, number(hist[static_cast<std::size_t>(idx)]),
              11.0f * s, color, eui::HorizontalAlign::Center);
    };
    marker("chart.min.value", minIdx, kAmber);
    marker("chart.max.value", maxIdx, kGreen);
}

void composeBottom(eui::Ui& ui, float x, float y, float w, float h, float s) {
    const bool running = g_monitor.running();

    const float btnW = 150.0f * s;
    components::button(ui, "bottom.toggle")
        .position(x, y)
        .size(btnW, h)
        .text(running ? "停止" : "开始")
        .icon(running ? 0xF04D : 0xF04B)
        .iconSize(14.0f * s)
        .fontSize(15.0f * s)
        .theme(themeTokens(), !running)
        .radius(11.0f)
        .transition(transition())
        .onClick([] {
            if (g_monitor.running()) {
                stopMonitoring();
            } else {
                startMonitoring();
            }
        })
        .build();

    // OBS browser source URL (or error).
    const std::string obsText = g_httpError.empty()
        ? "OBS 浏览器源  http://127.0.0.1:3030"
        : "HTTP 服务失败：" + g_httpError;
    const float obsX = x + btnW + 14.0f * s;
    const float obsW = w - btnW - 14.0f * s;
    label(ui, "bottom.obs", obsX, y, obsW, h,
          obsText, 13.0f * s, g_httpError.empty() ? kMuted : kAmber);

    if (obsW > 420.0f) {
        label(ui, "bottom.trayhint", x + w - 320.0f, y, 320.0f, h,
              "关闭或最小化窗口 → 隐藏到托盘", 12.0f * s, alpha(kMuted, 0.8f),
              eui::HorizontalAlign::Right);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DSL app config + root compose
// ---------------------------------------------------------------------------
const DslAppConfig& dslAppConfig() {
    static const ExtractedAssets assets = extractEmbeddedAssets();
    static const DslAppConfig config = DslAppConfig{}
        .title("小米手环心率")
        .pageId("heart_rate")
        .clearColor(kBackground)
        .windowSize(760, 640)
        .fps(60.0)
        .iconPath(assets.ok ? assets.iconPng : "assets/icon.png")
        .textFont(assets.ok ? assets.textFont : "")
        .iconFont(assets.ok ? assets.iconFont : "")
        .tray(true)
        .trayTitle("小米手环心率")
        .trayIcon(assets.ok ? assets.iconIco : "assets/icon.ico");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ensureHttpServer();

    const float margin = std::clamp(screen.width * 0.03f, 10.0f, 24.0f);
    const float contentW = std::max(1.0f, screen.width - margin * 2.0f);
    const float x = std::max(0.0f, (screen.width - contentW) * 0.5f);
    const float availH = std::max(1.0f, screen.height - margin * 2.0f);

    // Responsive scale: shrink fonts and paddings on small windows.
    const float s = std::clamp(std::min(screen.width / 760.0f, screen.height / 640.0f),
                               0.68f, 1.0f);

    const float gap = 10.0f * s;
    const float headerH = 42.0f * s;
    const float bigH = std::clamp(180.0f * s, 116.0f, 200.0f);
    const float statsH = 62.0f * s;
    const float bottomH = 40.0f * s;
    const float chartH = std::max(84.0f, availH - headerH - bigH - statsH - bottomH - gap * 4.0f);

    const float headerY = margin;
    const float bigY = headerY + headerH + gap;
    const float statsY = bigY + bigH + gap;
    const float chartY = statsY + statsH + gap;
    const float bottomY = chartY + chartH + gap;

    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("background")
                .size(screen.width, screen.height)
                .color(kBackground)
                .build();

            composeHeader(ui, x, headerY, contentW, headerH, s);
            composeBigBpm(ui, x, bigY, contentW, bigH, s);
            composeStats(ui, x, statsY, contentW, statsH, s);
            composeHeartChart(ui, x, chartY, contentW, chartH, s);
            composeBottom(ui, x, bottomY, contentW, bottomH, s);

            // Periodic refresh so status transitions are reflected even when
            // no BLE callback fires in between.
            ui.stack("refresh.timer")
                .size(0.0f, 0.0f)
                .onTimer(1.0f, [] { requestUpdate(); })
                .build();
        })
        .build();
}

} // namespace app
