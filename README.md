# 小米手环心率（OBS 直播）

[English](README_EN.md)

读取小米手环 10「运动心率广播」的桌面程序，中文界面，支持 **OBS 直播实时显示心率**、
系统托盘、单文件运行。界面基于 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) 框架。

## 功能

- 通过低功耗蓝牙（BLE）读取心率：扫描广播 Heart Rate Service（`0x180D`）的设备，
  连接并订阅 Heart Rate Measurement 特征（`0x2A37`）。
- **OBS 浏览器源**：内置本地 HTTP 服务器，实时心率以网页形式提供给 OBS。
  OBS 添加「浏览器」源，地址填 `http://127.0.0.1:3030`（`/` 为覆盖页，`/api` 返回 JSON）。
- 中文仪表盘：大字号心率、佩戴状态、连接状态、最低/平均/最高、带具体 BPM 数值的
  心率历史折线图。
- 系统托盘：关闭或最小化窗口隐藏到托盘，后台继续运行；托盘菜单「Show」恢复、
  「Exit」退出。
- **单 exe 发布**：图标、字体等资源内嵌进 exe，启动时自动释放到临时目录，无需
  携带 `assets` 文件夹。
- 窗口可自由缩放，小窗口自动缩小字体与留白。

## 环境要求

- Windows 10（2004 或更高）/ Windows 11。
- 低功耗蓝牙适配器（大多数笔记本/台式机自带）。
- 编译：Visual Studio 2022+（勾选「使用 C++ 的桌面开发」）、CMake 3.14+、
  Windows SDK ≥ 10.0.22000（BLE 用到 SDK 自带的 C++/WinRT 头文件）。

## 编译

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DEUI_BUILD_APPS=OFF -DEUI_BUILD_USER_APPS=ON -DEUI_BUILD_TEST_FIXTURES=OFF
cmake --build build --target heart_rate --config Release
```

可执行文件输出在 `build\heart_rate.exe`（单文件，无需 `assets` 目录）。

> 若本机 Windows SDK 较旧（< 10.0.22000），需额外指定现代 C++/WinRT 头文件目录
> （可用 `Microsoft.Windows.CppWinRT` 工具生成，目录需包含 `winrt/*.h`）：
> `-DHEART_RATE_CPPWINRT_DIR=D:\path\to\cppwinrt\headers`。

## 使用

1. 手环开启心率广播：**设置 → 心率广播 → 开启**。
2. 运行 `heart_rate.exe`。
3. 点击「开始」，程序自动扫描、连接并显示心率。
4. OBS 添加浏览器源：`http://127.0.0.1:3030`。
5. 关闭或最小化窗口 → 隐藏到托盘。

## 目录结构

```
apps/heart_rate/
  app.cpp              # 中文仪表盘 + 自绘折线图 + 托盘 + 内嵌资源释放
  assets_embedded.h    # 内嵌的图标/字体字节数组（单 exe 打包，自动生成）
  ble_heart_rate.h/.cpp# C++/WinRT 扫描 / 连接 / 通知
  heart_rate_http.h/.cpp # OBS 浏览器源 HTTP 服务器（/ 覆盖页 + /api JSON）

core/ include/ components/ modules/  # EUI-NEO 框架
3rd/                                # 依赖（GLFW / FreeType / zlib / libpng …）
assets/                             # 图标与字体源文件（生成内嵌头文件的来源）
```

## 许可与致谢

- UI 框架：[EUI-NEO](https://github.com/sudoevolve/EUI-NEO)（Apache-2.0，见 `LICENSE`）。
- 心率读取逻辑移植自 [miband-heart-rate](https://github.com/Tnze/miband-heart-rate)
  （MIT，© 2023 Tnze），见 `THIRD_PARTY_NOTICES.md`。
