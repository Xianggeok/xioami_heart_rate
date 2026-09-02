# 小米手环心率（EUI-NEO）

读取小米手环 10「运动心率广播」的桌面程序，基于 **EUI-NEO** 框架，中文界面，
自带系统托盘。

原版 `miband-heart-rate` 是 Rust 控制台程序。这里把它的低功耗蓝牙（BLE）读取逻辑
移植成了 **C++/WinRT**，做成了一个自带图形界面、托盘、以及 **OBS 浏览器源** 的
单文件可执行程序。

## 功能

- 扫描广播 Heart Rate Service（`0x180D`）的设备，连接并订阅 Heart Rate
  Measurement 特征（`0x2A37`）。
- **OBS 直播显示**：程序内置本地 HTTP 服务器，把实时心率以网页形式提供给 OBS。
  在 OBS 里添加「浏览器」源，地址填：

  ```
  http://127.0.0.1:3030
  ```

  页面每秒自动刷新心率（`/` 为覆盖页，`/api` 返回 JSON 状态）。
- 中文仪表盘：大字号心率、佩戴状态、连接状态、最低/平均/最高、带具体 BPM 数值的
  心率历史折线图。
- 开始 / 停止监测。
- **系统托盘**：关闭或最小化窗口会隐藏到托盘（BLE 继续后台运行），托盘菜单
  「Show」恢复窗口、「Exit」退出。
- 窗口可自由缩放，小窗口下自动缩小字体与留白。

## 环境要求

- Windows 10（2004 或更高）/ Windows 11。
- 一个低功耗蓝牙适配器（大多数笔记本/台式机自带）。
- 编译：Visual Studio 2022+（勾选「使用 C++ 的桌面开发」）、CMake 3.14+、
  Windows SDK ≥ 10.0.22000（BLE 用到 SDK 自带的 C++/WinRT 头文件）。

## 编译

在仓库根目录：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DEUI_BUILD_APPS=OFF -DEUI_BUILD_USER_APPS=ON -DEUI_BUILD_TEST_FIXTURES=OFF
cmake --build build --target heart_rate --config Release
```

若本机 Windows SDK 较旧（< 10.0.22000），需额外指定现代 C++/WinRT 头文件目录
（例如用 `Microsoft.Windows.CppWinRT` 工具生成）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DHEART_RATE_CPPWINRT_DIR=D:\path\to\cppwinrt\headers `
      -DEUI_BUILD_APPS=OFF -DEUI_BUILD_USER_APPS=ON
```

## 使用

1. 手环开启心率广播：**设置 → 心率广播 → 开启**（与原版 Demo 要求一致）。
2. 运行 `heart_rate.exe`。
3. 点击「开始」，程序自动扫描、连接并显示心率。
4. OBS 添加浏览器源：`http://127.0.0.1:3030`。
5. 关闭或最小化窗口 → 隐藏到托盘；托盘菜单「Show」恢复、「Exit」退出。

## 说明

- 手环需在广播 Heart Rate Service，无需先与 Windows 配对。
- **单文件发布**：图标、字体等资源已内嵌进 `heart_rate.exe`，启动时自动释放到
  系统临时目录，无需再携带 `assets` 文件夹。
- 端口 `3030` 被占用时，界面底部会显示错误信息。

## 目录结构

```
apps/heart_rate/
  app.cpp              # 中文仪表盘 + 自绘折线图 + 托盘配置 + 内嵌资源释放
  assets_embedded.h    # 内嵌的图标/字体字节数组（单 exe 打包，自动生成）
  ble_heart_rate.h     # HeartRateMonitor 接口（不暴露 WinRT 头文件）
  ble_heart_rate.cpp   # C++/WinRT 扫描 / 连接 / 通知
  heart_rate_http.h    # OBS 浏览器源 HTTP 服务器接口
  heart_rate_http.cpp  # WinSock 实现（/ 覆盖页 + /api JSON）
```
