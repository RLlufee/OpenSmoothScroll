# 更新日志 (CHANGELOG)

本文档记录工作区内的所有主要修改与新增内容。

## [2026-11-08]

### 新增与逆向突破 (Reverse Engineering & Innovation)
- **深度逆向新版 `smoothscroll-app.exe` (v1.36.0)**：
  - 确认其为 Rust 编译的 Tauri/WebView2 桌面程序。
  - 提取完整模块结构（`hook_wiring.rs`, `engine_thread.rs`, `game_mode.rs`, `webview_memory.rs` 等）与完整配置架构。
  - 提炼出其核心亮点（按键急刹车 `clear_inertia_on_press`、系统减弱动态效果遵循 `respect_reduce_motion`、离散专业应用名单 `discrete_wheel_apps`、全屏游戏免扰 `game_mode`）。
  - 识别并抛弃其严重槽粕（12MB 膨胀体积、WebView2 内存黑洞与压缩补丁、Sentry 遥测网络上报、假开源闭门造车）。

### 产物与架构构建 (Architecture & Artifacts)
- **开源项目正式发布：[OpenSmoothScroll.exe](file:///s:/workhome/smooth/OpenSmoothScroll.exe) (v1.1.0)**：
  - 完整源码位于 `s:\workhome\smooth\src\main.cpp`。
  - 采用 `-mwindows` 原生 Win32 编译，无控制台黑框，体积仅 **69 KB**（相比新版 12MB 缩小约 170 倍，0 秒冷启动，内存占用微乎其微）。
  - 具备全功能系统托盘菜单（支持右键热切换各项黑科技、双击打开 `config.ini`、开机自启注入）。
  - 动态内存绘制矢量鼠标滚轮托盘图标，完全独立绿色免安装，零外部资源依赖。
- **工程构建体系完善 (Build & Tooling)**：
  - 新增 `build.bat`：一键批处理编译脚本（支持 MinGW-w64）。
  - 新增 `CMakeLists.txt`：跨编译器标准 CMake 支持（兼容 MSVC 与 MinGW-w64）。
  - 新增 `LICENSE`：正式采用宽松的 MIT 许可证。
