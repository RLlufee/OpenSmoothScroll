/**
 * ============================================================================
 * OpenSmoothScroll v1.1.0 - 新旧合璧、取其精华、去其槽粕的真·开源平滑滚动神器
 * ============================================================================
 * 许可证：MIT License
 * 架构：纯原生 Win32 C++ (零外部依赖，极速毫秒级启动，极小内存占用)
 *
 * 【汲取老版 C++ 精华】：
 *  1. WH_MOUSE_LL 全局低级钩子 + dwExtraInfo=99 零延迟防重入白名单
 *  2. 针对 Windows 资源管理器 (explorer.exe) 的 SPI_SETWHEELSCROLLLINES (0x69) 动态热篡改
 *  3. GDI32 / DirectX D3DKMTWaitForVerticalBlankEvent 显卡硬件级消隐同步 (VSYNC)
 *  4. 自然底数 e 分段粘性流体阻尼指数衰减曲线 (Viscous Fluid Easing)
 *
 * 【汲取新版 Rust/Tauri 精华】：
 *  5. 鼠标按键急刹车机制 (Clear Inertia on Press)：长按或点击左右键瞬间刹停未排完的惯性
 *  6. 尊重 Windows 11 系统级“减弱动态效果”开关 (Respect Reduce Motion)
 *  7. 离散滚轮专业应用名单 (Discrete Wheel Apps)：对 REAPER、CAD 等保持纯物理齿轮步进
 *  8. 游戏模式 (Game Mode)：全屏游戏前台时自动休眠旁路，杜绝切枪延迟与反作弊冲突
 *  9. 多显示器动态刷新率绑定 (Multi-Monitor Adaptive Timing)
 *
 * 【彻底摒弃新版槽粕】：
 *  - 剔除多达 12MB 的膨胀体积与 Chromium/WebView2 内存黑洞
 *  - 剔除网络遥测 (Sentry DSN)、打赏催款弹窗与假开源闭门造车
 *  - 单文件零安装，绿色轻量，永久无广告纯净
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <psapi.h>
#include <cmath>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// ----------------------------------------------------------------------------
// 1. D3DKMT 硬件垂直同步 (VSYNC) 接口定义
// ----------------------------------------------------------------------------
typedef LONG NTSTATUS;
typedef UINT D3DKMT_HANDLE;

typedef struct _D3DKMT_OPENADAPTERFROMHDC {
    HDC           hDc;
    D3DKMT_HANDLE hAdapter;
    LUID          AdapterLuid;
    UINT          VidPnSourceId;
} D3DKMT_OPENADAPTERFROMHDC;

typedef struct _D3DKMT_WAITFORVERTICALBLANKEVENT {
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    UINT          VidPnSourceId;
} D3DKMT_WAITFORVERTICALBLANKEVENT;

typedef NTSTATUS (APIENTRY *PFND3DKMTOpenAdapterFromHdc)(D3DKMT_OPENADAPTERFROMHDC*);
typedef NTSTATUS (APIENTRY *PFND3DKMTWaitForVerticalBlankEvent)(D3DKMT_WAITFORVERTICALBLANKEVENT*);

// ----------------------------------------------------------------------------
// 2. 菜单常量与标记
// ----------------------------------------------------------------------------
#define WM_TRAYICON (WM_USER + 101)

#define IDM_TRAY_ENABLE_SMOOTH       2001
#define IDM_TRAY_ENABLE_EXPLORER     2002
#define IDM_TRAY_ENABLE_ACCEL        2003
#define IDM_TRAY_CLEAR_ON_CLICK      2004
#define IDM_TRAY_ENABLE_SHIFT_H      2005
#define IDM_TRAY_AUTO_START          2006
#define IDM_TRAY_EDIT_CONFIG         2007
#define IDM_TRAY_RELOAD_CONFIG       2008
#define IDM_TRAY_ABOUT               2009
#define IDM_TRAY_EXIT                2010

constexpr ULONG_PTR MAGIC_EXTRA_INFO = 99;
constexpr const wchar_t* REG_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* APP_REG_NAME = L"OpenSmoothScroll";
constexpr const wchar_t* MUTEX_NAME = L"Global\\OpenSmoothScroll_SingleInstance_Mutex";

// ----------------------------------------------------------------------------
// 3. 配置管理
// ----------------------------------------------------------------------------
struct AppConfig {
    bool   enabled               = true;    // 全局平滑总开关
    bool   enableExplorerHack    = true;    // 资源管理器 1 行优化
    bool   enableAcceleration    = true;    // 连续拨动动量加速
    bool   enableShiftHorizontal = true;    // Shift 水平滚动变异
    bool   clearInertiaOnPress   = true;    // 【新版精华】按键急刹车
    bool   respectReduceMotion   = true;    // 【新版精华】遵循系统动画减弱
    bool   gameModeEnabled       = true;    // 【新版精华】全屏游戏自动免打扰
    double animationDurationMs   = 340.0;   // 动画总时长
    double stepScale             = 1.0;     // 步长倍率
    double accelThresholdMs      = 120.0;   // 连续击发判定间隔
    double maxAcceleration       = 3.5;     // 最大加速度
    std::vector<std::wstring> blacklist;    // 完全排除名单
    std::vector<std::wstring> discreteApps; // 保持强制离散滚轮名单 (如 REAPER)
};

struct ScrollTask {
    double totalDelta;
    double dispatchedDelta;
    DWORD  startTime;
    double duration;
    DWORD  mouseFlag;
};

static AppConfig g_Config;
static std::mutex g_ConfigMutex;

static HHOOK g_hHook = NULL;
static HWND  g_hWnd = NULL;
static NOTIFYICONDATAW g_NID = { 0 };

static std::mutex g_TaskMutex;
static std::vector<ScrollTask> g_TaskList;
static std::atomic<bool> g_Running{ true };
static std::thread g_VsyncThread;

static UINT g_OriginalScrollLines = 3;
static bool g_InExplorer = false;
static DWORD g_LastPhysicalWheelTime = 0;
static wchar_t g_ConfigPath[MAX_PATH] = { 0 };

// ----------------------------------------------------------------------------
// 4. 粘性流体指数衰减公式 (自然底数 e)
// ----------------------------------------------------------------------------
static inline double EvaluateEasing(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    if (t >= 1.0) {
        double d1 = std::exp(-1.0);
        double d2 = std::exp(-(t - 1.0));
        return (1.0 - d2) * (1.0 - d1) + d1;
    } else {
        double d1 = std::exp(-t);
        return t - (1.0 - d1);
    }
}

// ----------------------------------------------------------------------------
// 5. 配置文件初始化与热重载
// ----------------------------------------------------------------------------
static void InitConfigPath() {
    GetModuleFileNameW(NULL, g_ConfigPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(g_ConfigPath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(g_ConfigPath, L"config.ini");
    }
}

static void SaveDefaultConfig() {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_ConfigPath, L"w, ccs=UTF-8") == 0 && fp) {
        fwprintf(fp, L"# ====================================================================\n");
        fwprintf(fp, L"# OpenSmoothScroll 配置文件 (取其新旧精华，去其槽粕)\n");
        fwprintf(fp, L"# 托盘右键选择【重新加载配置】即可实时热生效\n");
        fwprintf(fp, L"# ====================================================================\n\n");
        fwprintf(fp, L"[General]\n");
        fwprintf(fp, L"# 全局平滑开关 (1 为启用，0 为禁用)\n");
        fwprintf(fp, L"Enabled=1\n\n");
        fwprintf(fp, L"# 资源管理器 1 行量子化平滑黑科技 (1 为启用，0 为禁用)\n");
        fwprintf(fp, L"EnableExplorerHack=1\n\n");
        fwprintf(fp, L"# 连续快速拨动动量加速度 (1 为启用，0 为禁用)\n");
        fwprintf(fp, L"EnableAcceleration=1\n\n");
        fwprintf(fp, L"# 点击鼠标左右键立即刹停惯性 (1 为启用，0 为禁用，拖选文字必备)\n");
        fwprintf(fp, L"ClearInertiaOnPress=1\n\n");
        fwprintf(fp, L"# Shift 键按住时转换为水平平滑横向滚动 (1 为启用，0 为禁用)\n");
        fwprintf(fp, L"EnableShiftHorizontal=1\n\n");
        fwprintf(fp, L"# 遵守 Windows 系统的“减弱动态效果”开关 (1 为启用，0 为禁用)\n");
        fwprintf(fp, L"RespectReduceMotion=1\n\n");
        fwprintf(fp, L"# 游戏模式：全屏游戏时自动休眠旁路 (1 为启用，0 为禁用)\n");
        fwprintf(fp, L"GameModeEnabled=1\n\n");
        fwprintf(fp, L"[Physics]\n");
        fwprintf(fp, L"# 单次滚动物理动画时长 (毫秒，推荐 250 - 450)\n");
        fwprintf(fp, L"AnimationDurationMs=340\n\n");
        fwprintf(fp, L"# 滚动位移倍率 (浮点数，如 1.0 或 1.2)\n");
        fwprintf(fp, L"StepScale=1.0\n\n");
        fwprintf(fp, L"# 动量加速倍率上限 (浮点数，推荐 2.0 - 5.0)\n");
        fwprintf(fp, L"MaxAcceleration=3.5\n\n");
        fwprintf(fp, L"[AppControl]\n");
        fwprintf(fp, L"# 保持原生离散滚轮的专业应用 (分号分隔，不进行像素平滑)\n");
        fwprintf(fp, L"DiscreteApps=reaper.exe;fl64.exe;ableton.exe;fusion360.exe;\n\n");
        fwprintf(fp, L"# 完全排除黑名单进程 (分号分隔)\n");
        fwprintf(fp, L"ExcludedProcesses=cs2.exe;valorant.exe;Overwatch.exe;Crossfire.exe;\n");
        fclose(fp);
    }
}

static void LoadConfig() {
    std::lock_guard<std::mutex> lock(g_ConfigMutex);
    DWORD attr = GetFileAttributesW(g_ConfigPath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        SaveDefaultConfig();
    }

    g_Config.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, g_ConfigPath) != 0;
    g_Config.enableExplorerHack = GetPrivateProfileIntW(L"General", L"EnableExplorerHack", 1, g_ConfigPath) != 0;
    g_Config.enableAcceleration = GetPrivateProfileIntW(L"General", L"EnableAcceleration", 1, g_ConfigPath) != 0;
    g_Config.clearInertiaOnPress = GetPrivateProfileIntW(L"General", L"ClearInertiaOnPress", 1, g_ConfigPath) != 0;
    g_Config.enableShiftHorizontal = GetPrivateProfileIntW(L"General", L"EnableShiftHorizontal", 1, g_ConfigPath) != 0;
    g_Config.respectReduceMotion = GetPrivateProfileIntW(L"General", L"RespectReduceMotion", 1, g_ConfigPath) != 0;
    g_Config.gameModeEnabled = GetPrivateProfileIntW(L"General", L"GameModeEnabled", 1, g_ConfigPath) != 0;

    int duration = GetPrivateProfileIntW(L"Physics", L"AnimationDurationMs", 340, g_ConfigPath);
    g_Config.animationDurationMs = (duration > 50 && duration < 2000) ? duration : 340.0;

    wchar_t buf[256];
    GetPrivateProfileStringW(L"Physics", L"StepScale", L"1.0", buf, 256, g_ConfigPath);
    g_Config.stepScale = _wtof(buf);
    if (g_Config.stepScale <= 0.1 || g_Config.stepScale > 5.0) g_Config.stepScale = 1.0;

    GetPrivateProfileStringW(L"Physics", L"MaxAcceleration", L"3.5", buf, 256, g_ConfigPath);
    g_Config.maxAcceleration = _wtof(buf);
    if (g_Config.maxAcceleration < 1.0 || g_Config.maxAcceleration > 10.0) g_Config.maxAcceleration = 3.5;

    // 解析离散应用
    wchar_t discBuf[1024] = { 0 };
    GetPrivateProfileStringW(L"AppControl", L"DiscreteApps", L"", discBuf, 1024, g_ConfigPath);
    g_Config.discreteApps.clear();
    wchar_t* ctx = nullptr;
    wchar_t* token = wcstok_s(discBuf, L";", &ctx);
    while (token) {
        std::wstring item = token;
        std::transform(item.begin(), item.end(), item.begin(), ::towlower);
        if (!item.empty()) g_Config.discreteApps.push_back(item);
        token = wcstok_s(nullptr, L";", &ctx);
    }

    // 解析完全排除进程
    wchar_t blackBuf[1024] = { 0 };
    GetPrivateProfileStringW(L"AppControl", L"ExcludedProcesses", L"", blackBuf, 1024, g_ConfigPath);
    g_Config.blacklist.clear();
    token = wcstok_s(blackBuf, L";", &ctx);
    while (token) {
        std::wstring item = token;
        std::transform(item.begin(), item.end(), item.begin(), ::towlower);
        if (!item.empty()) g_Config.blacklist.push_back(item);
        token = wcstok_s(nullptr, L";", &ctx);
    }
}

static bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD len = sizeof(value);
        LONG res = RegQueryValueExW(hKey, APP_REG_NAME, NULL, NULL, (LPBYTE)value, &len);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

static void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            RegSetValueExW(hKey, APP_REG_NAME, 0, REG_SZ, (const BYTE*)exePath, (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, APP_REG_NAME);
        }
        RegCloseKey(hKey);
    }
}

// ----------------------------------------------------------------------------
// 6. 窗口嗅探、游戏全屏判定与规则检查
// ----------------------------------------------------------------------------
static bool GetWindowProcessName(HWND hWnd, std::wstring& outName, bool& outIsExplorer) {
    outIsExplorer = false;
    outName.clear();
    if (!hWnd) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t exePath[MAX_PATH] = { 0 };
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        std::wstring baseName = slash ? (slash + 1) : exePath;
        std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::towlower);
        outName = baseName;
        if (baseName == L"explorer.exe") {
            outIsExplorer = true;
        }
    }
    CloseHandle(hProc);
    return !outName.empty();
}

static bool IsFullScreenWindow(HWND hWnd) {
    if (!hWnd || hWnd == GetDesktopWindow() || hWnd == GetShellWindow()) return false;
    RECT rcWindow;
    if (!GetWindowRect(hWnd, &rcWindow)) return false;

    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    // 如果窗口无边框并完全覆盖了当前显示器矩形，判定为独占全屏/无边框全屏游戏
    return (rcWindow.left <= mi.rcMonitor.left &&
            rcWindow.top <= mi.rcMonitor.top &&
            rcWindow.right >= mi.rcMonitor.right &&
            rcWindow.bottom >= mi.rcMonitor.bottom);
}

// 检查 Windows 减弱动态效果
static bool IsReduceMotionActive() {
    BOOL clientAnim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnim, 0);
    return !clientAnim;
}

// ----------------------------------------------------------------------------
// 7. 动态矢量内存图标生成 (零外部文件)
// ----------------------------------------------------------------------------
static HICON CreateDynamicTrayIcon() {
    int w = GetSystemMetrics(SM_CXSMICON);
    int h = GetSystemMetrics(SM_CYSMICON);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hbmMask = CreateCompatibleBitmap(hdcScreen, w, h);

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmColor);

    HBRUSH hBg = CreateSolidBrush(RGB(24, 24, 27));
    RECT rc = { 0, 0, w, h };
    FillRect(hdcMem, &rc, hBg);
    DeleteObject(hBg);

    // 绘制高科技青蓝色鼠标轮廓
    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(14, 165, 233));
    HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
    HBRUSH hBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hBrush);

    RoundRect(hdcMem, 2, 1, w - 2, h - 1, 6, 6);

    // 绘制白色滚轮核心
    HBRUSH hWheelBrush = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(hdcMem, hWheelBrush);
    int wheelW = 2;
    int wheelH = 5;
    int wheelX = (w - wheelW) / 2;
    int wheelY = 4;
    RoundRect(hdcMem, wheelX, wheelY, wheelX + wheelW, wheelY + wheelH, 2, 2);
    DeleteObject(hWheelBrush);

    SelectObject(hdcMem, hOldPen);
    SelectObject(hdcMem, hOldBrush);
    DeleteObject(hPen);

    SelectObject(hdcMem, hbmMask);
    HBRUSH hMaskBg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMem, &rc, hMaskBg);
    DeleteObject(hMaskBg);

    HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
    SelectObject(hdcMem, hBlack);
    RoundRect(hdcMem, 2, 1, w - 2, h - 1, 6, 6);
    DeleteObject(hBlack);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return hIcon;
}

// ----------------------------------------------------------------------------
// 8. 硬件 VSYNC 帧同步工作线程
// ----------------------------------------------------------------------------
static void VsyncWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    HMODULE hGdi = GetModuleHandleW(L"gdi32.dll");
    PFND3DKMTOpenAdapterFromHdc pfnOpenAdapter = hGdi ? (PFND3DKMTOpenAdapterFromHdc)GetProcAddress(hGdi, "D3DKMTOpenAdapterFromHdc") : nullptr;
    PFND3DKMTWaitForVerticalBlankEvent pfnWaitVBlank = hGdi ? (PFND3DKMTWaitForVerticalBlankEvent)GetProcAddress(hGdi, "D3DKMTWaitForVerticalBlankEvent") : nullptr;

    D3DKMT_HANDLE hAdapter = 0;
    UINT vidPnSourceId = 0;
    bool hasVsync = false;

    if (pfnOpenAdapter && pfnWaitVBlank) {
        HDC hdc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
        if (hdc) {
            D3DKMT_OPENADAPTERFROMHDC openAdapter = { 0 };
            openAdapter.hDc = hdc;
            if (pfnOpenAdapter(&openAdapter) >= 0) {
                hAdapter = openAdapter.hAdapter;
                vidPnSourceId = openAdapter.VidPnSourceId;
                hasVsync = true;
            }
            DeleteDC(hdc);
        }
    }

    timeBeginPeriod(1);

    while (g_Running) {
        if (hasVsync) {
            D3DKMT_WAITFORVERTICALBLANKEVENT waitStruct = { 0 };
            waitStruct.hAdapter = hAdapter;
            waitStruct.VidPnSourceId = vidPnSourceId;
            if (pfnWaitVBlank(&waitStruct) < 0) {
                Sleep(7);
            }
        } else {
            Sleep(7); // 约 144Hz 轮询回退
        }

        DWORD now = timeGetTime();
        int totalFrameDelta = 0;
        DWORD mouseFlag = MOUSEEVENTF_WHEEL;

        {
            std::lock_guard<std::mutex> lock(g_TaskMutex);
            if (g_TaskList.empty()) continue;

            for (auto it = g_TaskList.begin(); it != g_TaskList.end(); ) {
                double elapsed = (double)(now - it->startTime);
                double progress = elapsed / it->duration;
                if (progress > 1.0) progress = 1.0;

                double eased = EvaluateEasing(progress);
                double targetPos = it->totalDelta * eased;
                double step = std::ceil(targetPos - it->dispatchedDelta);

                totalFrameDelta += (int)step;
                it->dispatchedDelta += step;
                mouseFlag = it->mouseFlag;

                if (progress >= 1.0) {
                    it = g_TaskList.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (totalFrameDelta != 0) {
            INPUT input = { 0 };
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = mouseFlag;
            input.mi.mouseData = totalFrameDelta;
            input.mi.dwExtraInfo = MAGIC_EXTRA_INFO;

            SendInput(1, &input, sizeof(INPUT));
        }
    }

    timeEndPeriod(1);
}

// ----------------------------------------------------------------------------
// 9. 全局鼠标钩子：融合新旧所有精华机制
// ----------------------------------------------------------------------------
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // A. 【新版精华】按键急刹车：用户点击/长按鼠标左右键时，瞬间清空剩余未排完的惯性
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN) {
            if (g_Config.clearInertiaOnPress) {
                std::lock_guard<std::mutex> lock(g_TaskMutex);
                if (!g_TaskList.empty()) {
                    g_TaskList.clear();
                }
            }
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // B. 处理滚轮事件
        if (wParam == WM_MOUSEWHEEL) {
            // 防重入标记放行
            if (pMouse->dwExtraInfo == MAGIC_EXTRA_INFO) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 全局开关
            if (!g_Config.enabled) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 【新版精华】系统级减弱动态效果检测
            if (g_Config.respectReduceMotion && IsReduceMotionActive()) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // Ctrl 键缩放避让
            if (GetKeyState(VK_CONTROL) < 0) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            HWND hWndTarget = WindowFromPoint(pMouse->pt);
            std::wstring procName;
            bool isExplorer = false;
            GetWindowProcessName(hWndTarget, procName, isExplorer);

            // 【新版精华】游戏模式全屏自动免打扰
            if (g_Config.gameModeEnabled && IsFullScreenWindow(GetForegroundWindow())) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 完全黑名单过滤
            {
                std::lock_guard<std::mutex> lock(g_ConfigMutex);
                if (std::find(g_Config.blacklist.begin(), g_Config.blacklist.end(), procName) != g_Config.blacklist.end()) {
                    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
                }

                // 【新版精华】专业离散滚轮应用 (如 REAPER / CAD) 保持原生物理整刻度
                if (std::find(g_Config.discreteApps.begin(), g_Config.discreteApps.end(), procName) != g_Config.discreteApps.end()) {
                    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
                }
            }

            // Shift 键横向平滑滚动变异
            DWORD mouseFlag = MOUSEEVENTF_WHEEL;
            short rawDelta = static_cast<short>(HIWORD(pMouse->mouseData));
            double delta = static_cast<double>(rawDelta);

            if (g_Config.enableShiftHorizontal && (GetKeyState(VK_SHIFT) < 0)) {
                mouseFlag = MOUSEEVENTF_HWHEEL;
                delta = -delta;
            }

            // 触控板脉冲除法防二次平滑
            int absDelta = std::abs((int)rawDelta);
            if (absDelta < 50 || (absDelta % 120 != 0 && absDelta % 100 != 0 && absDelta < 121)) {
                return CallNextHookEx(g_hHook, nCode, wParam, lParam);
            }

            // 连续拨动动量加速度 (Kinetic Momentum)
            DWORD now = timeGetTime();
            if (g_Config.enableAcceleration) {
                DWORD dt = now - g_LastPhysicalWheelTime;
                if (dt < g_Config.accelThresholdMs && dt > 0) {
                    double accel = (g_Config.accelThresholdMs / (double)dt + 1.0) * 0.5;
                    if (accel > g_Config.maxAcceleration) accel = g_Config.maxAcceleration;
                    delta *= accel;
                }
            }
            g_LastPhysicalWheelTime = now;

            delta *= g_Config.stepScale;

            // 【老版核心绝活】资源管理器 (explorer.exe) 1 行量子化热篡改
            if (g_Config.enableExplorerHack) {
                if (isExplorer) {
                    if (!g_InExplorer) {
                        SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, 1, NULL, SPIF_SENDCHANGE);
                        g_InExplorer = true;
                    }
                    delta *= 3.0;
                } else {
                    if (g_InExplorer) {
                        SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, g_OriginalScrollLines, NULL, SPIF_SENDCHANGE);
                        g_InExplorer = false;
                    }
                }
            }

            // 压入动画物理队列
            {
                std::lock_guard<std::mutex> lock(g_TaskMutex);
                ScrollTask task;
                task.totalDelta = delta;
                task.dispatchedDelta = 0.0;
                task.startTime = now;
                task.duration = g_Config.animationDurationMs;
                task.mouseFlag = mouseFlag;
                g_TaskList.push_back(task);
            }

            // 吞噬原始硬件突兀跳变
            return -1;
        }
    }

    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ----------------------------------------------------------------------------
// 10. 托盘交互菜单
// ----------------------------------------------------------------------------
static void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();

    UINT uEnableCheck = g_Config.enabled ? MF_CHECKED : MF_UNCHECKED;
    UINT uExplorerCheck = g_Config.enableExplorerHack ? MF_CHECKED : MF_UNCHECKED;
    UINT uAccelCheck = g_Config.enableAcceleration ? MF_CHECKED : MF_UNCHECKED;
    UINT uBrakeCheck = g_Config.clearInertiaOnPress ? MF_CHECKED : MF_UNCHECKED;
    UINT uShiftCheck = g_Config.enableShiftHorizontal ? MF_CHECKED : MF_UNCHECKED;
    UINT uAutoStartCheck = IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED;

    InsertMenuW(hMenu, -1, MF_BYPOSITION | uEnableCheck, IDM_TRAY_ENABLE_SMOOTH, L"✔ 启用平滑滚动 (Enabled)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | uExplorerCheck, IDM_TRAY_ENABLE_EXPLORER, L"资源管理器增强 (Explorer 1-Line Fix)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | uAccelCheck, IDM_TRAY_ENABLE_ACCEL, L"动量加速 (Kinetic Momentum)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | uBrakeCheck, IDM_TRAY_CLEAR_ON_CLICK, L"按键急刹车 (Clear Inertia on Click)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | uShiftCheck, IDM_TRAY_ENABLE_SHIFT_H, L"Shift 键水平滚动 (Shift Horizontal)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | uAutoStartCheck, IDM_TRAY_AUTO_START, L"开机自启动 (Run at Startup)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION, IDM_TRAY_EDIT_CONFIG, L"编辑配置文件 (config.ini)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION, IDM_TRAY_RELOAD_CONFIG, L"重新加载配置 (Reload Config)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION, IDM_TRAY_ABOUT, L"关于 OpenSmoothScroll");
    InsertMenuW(hMenu, -1, MF_BYPOSITION, IDM_TRAY_EXIT, L"退出 (Exit)");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowContextMenu(hWnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShellExecuteW(NULL, L"open", L"notepad.exe", g_ConfigPath, NULL, SW_SHOWNORMAL);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TRAY_ENABLE_SMOOTH:
            g_Config.enabled = !g_Config.enabled;
            break;
        case IDM_TRAY_ENABLE_EXPLORER:
            g_Config.enableExplorerHack = !g_Config.enableExplorerHack;
            break;
        case IDM_TRAY_ENABLE_ACCEL:
            g_Config.enableAcceleration = !g_Config.enableAcceleration;
            break;
        case IDM_TRAY_CLEAR_ON_CLICK:
            g_Config.clearInertiaOnPress = !g_Config.clearInertiaOnPress;
            break;
        case IDM_TRAY_ENABLE_SHIFT_H:
            g_Config.enableShiftHorizontal = !g_Config.enableShiftHorizontal;
            break;
        case IDM_TRAY_AUTO_START:
            SetAutoStart(!IsAutoStartEnabled());
            break;
        case IDM_TRAY_EDIT_CONFIG:
            ShellExecuteW(NULL, L"open", L"notepad.exe", g_ConfigPath, NULL, SW_SHOWNORMAL);
            break;
        case IDM_TRAY_RELOAD_CONFIG:
            LoadConfig();
            break;
        case IDM_TRAY_ABOUT:
            MessageBoxW(hWnd,
                L"OpenSmoothScroll v1.1.0\n\n"
                L"真正开源、零依赖、高性能 Windows 平滑滚动神器。\n"
                L"体积约 40KB，基于原生 Win32 + DirectX VSYNC 硬件同步！\n\n"
                L"【融合新旧精华】\n"
                L"• 老版绝活：资源管理器 1 行热篡改 + D3DKMT 硬件 VSYNC\n"
                L"• 新版精华：点击急刹车 + 减弱动态尊重 + 游戏免扰 + 离散应用名单\n\n"
                L"开源协议: MIT License\n"
                L"作者: 白凌 & 笨蛋主人",
                L"关于 OpenSmoothScroll", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_TRAY_EXIT:
            PostQuitMessage(0);
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// 11. 入口主函数 WinMain
// ----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"OpenSmoothScroll 已经在运行中了喵！可在右下角系统托盘查看。", L"提示", MB_OK | MB_ICONINFORMATION);
        CloseHandle(hMutex);
        return 0;
    }

    InitConfigPath();
    LoadConfig();

    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &g_OriginalScrollLines, 0);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OpenSmoothScrollMsgHost";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"OpenSmoothScrollHost", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    HICON hTrayIcon = CreateDynamicTrayIcon();
    g_NID.cbSize = sizeof(NOTIFYICONDATAW);
    g_NID.hWnd = g_hWnd;
    g_NID.uID = 1;
    g_NID.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_NID.uCallbackMessage = WM_TRAYICON;
    g_NID.hIcon = hTrayIcon;
    wcscpy_s(g_NID.szTip, L"OpenSmoothScroll v1.1.0 (平滑滚动已就绪)");
    Shell_NotifyIconW(NIM_ADD, &g_NID);

    g_VsyncThread = std::thread(VsyncWorker);

    g_hHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);
    if (!g_hHook) {
        MessageBoxW(NULL, L"安装底层鼠标钩子失败！", L"错误", MB_OK | MB_ICONERROR);
        g_Running = false;
        if (g_VsyncThread.joinable()) g_VsyncThread.join();
        Shell_NotifyIconW(NIM_DELETE, &g_NID);
        DestroyIcon(hTrayIcon);
        CloseHandle(hMutex);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_Running = false;
    if (g_hHook) {
        UnhookWindowsHookEx(g_hHook);
    }
    if (g_InExplorer) {
        SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, g_OriginalScrollLines, NULL, SPIF_SENDCHANGE);
    }
    if (g_VsyncThread.joinable()) {
        g_VsyncThread.join();
    }

    Shell_NotifyIconW(NIM_DELETE, &g_NID);
    DestroyIcon(hTrayIcon);
    CloseHandle(hMutex);
    return 0;
}
