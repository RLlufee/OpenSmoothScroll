/**
 * ============================================================================
 * SmoothScroll 核心原理复刻原型 (Proof of Concept)
 * ============================================================================
 * 基于 REA MCP 与 Ghidra 逆向工程提取出的核心技术实现：
 * 1. WH_MOUSE_LL 全局底层鼠标钩子 + dwExtraInfo=99 防重入白名单
 * 2. explorer.exe (资源管理器) 动态嗅探 + SPI_SETWHEELSCROLLLINES (0x69) 热篡改
 * 3. gdi32.dll D3DKMTWaitForVerticalBlankEvent 显示器硬件级 VSYNC 垂直消隐对齐
 * 4. 自然底数 e (0x4005bf0a8b145769) 粘性流体分段指数衰减缓动公式
 * 5. Ctrl 键缩放避让 + Shift 键水平滚轮自动变异
 * 6. 拨动时间差分动量加速度累加 (Kinetic Momentum)
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <cmath>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <iostream>

#pragma comment(lib, "winmm.lib")

// ----------------------------------------------------------------------------
// 1. D3DKMT 硬件垂直同步 (VSYNC) 结构体与函数指针定义
// ----------------------------------------------------------------------------
typedef LONG NTSTATUS;
typedef UINT D3DKMT_HANDLE;

typedef struct _D3DKMT_OPENADAPTERFROMHDC {
    HDC           hDc;            // [in]
    D3DKMT_HANDLE hAdapter;       // [out]
    LUID          AdapterLuid;    // [out]
    UINT          VidPnSourceId;  // [out]
} D3DKMT_OPENADAPTERFROMHDC;

typedef struct _D3DKMT_WAITFORVERTICALBLANKEVENT {
    D3DKMT_HANDLE hAdapter;       // [in]
    D3DKMT_HANDLE hDevice;        // [in]
    UINT          VidPnSourceId;  // [in]
} D3DKMT_WAITFORVERTICALBLANKEVENT;

typedef NTSTATUS (APIENTRY *PFND3DKMTOpenAdapterFromHdc)(D3DKMT_OPENADAPTERFROMHDC*);
typedef NTSTATUS (APIENTRY *PFND3DKMTWaitForVerticalBlankEvent)(D3DKMT_WAITFORVERTICALBLANKEVENT*);

// ----------------------------------------------------------------------------
// 2. 全局状态与配置参数
// ----------------------------------------------------------------------------
constexpr ULONG_PTR MAGIC_EXTRA_INFO = 99;      // 防重入标记
constexpr double E_CONST = 2.718281828459045;   // 自然底数 e
constexpr double DEFAULT_ANIMATION_MS = 380.0;  // 单次滚动动画时长 (毫秒)
constexpr double ACCEL_THRESHOLD_MS = 120.0;    // 连续拨动判定阈值 (毫秒)
constexpr double MAX_ACCELERATION = 3.5;         // 最大加速度倍率

struct ScrollTask {
    double totalDelta;         // 本次物理滚动目标总距离
    double dispatchedDelta;    // 已注入给系统的位移量
    DWORD  startTime;          // 开始动画的毫秒时间戳
    double duration;           // 动画持续时间
    DWORD  mouseFlag;          // MOUSEEVENTF_WHEEL 或 MOUSEEVENTF_HWHEEL
};

static HHOOK g_hHook = NULL;
static std::mutex g_TaskMutex;
static std::vector<ScrollTask> g_TaskList;
static std::atomic<bool> g_Running{ true };
static std::thread g_VsyncThread;

// 资源管理器动态劫持状态
static UINT g_OriginalScrollLines = 3;
static bool g_InExplorer = false;

// 动量加速状态追踪
static DWORD g_LastPhysicalWheelTime = 0;

// ----------------------------------------------------------------------------
// 3. 缓动函数：逆向自 FUN_14005d210 (粘性流体阻尼指数衰减)
// ----------------------------------------------------------------------------
static double EvaluateEasing(double t) {
    // t 为归一化进度 [0.0, 1.0]
    double k = 1.0; 
    double t_prime = t * k;
    if (t_prime >= 1.0) {
        double d1 = std::exp(-1.0);
        double d2 = std::exp(-(t_prime - 1.0));
        return (1.0 - d2) * (1.0 - d1) + d1;
    } else {
        double d1 = std::exp(-t_prime);
        return t_prime - (1.0 - d1);
    }
}

// ----------------------------------------------------------------------------
// 4. 辅助函数：判断窗口是否属于 explorer.exe (逆向自 FUN_140069de0)
// ----------------------------------------------------------------------------
static bool IsExplorerWindow(HWND hWnd) {
    if (!hWnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t exePath[MAX_PATH] = { 0 };
    DWORD size = MAX_PATH;
    bool isExplorer = false;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
        if (wcsstr(exePath, L"\\explorer.exe") != nullptr) {
            isExplorer = true;
        }
    }
    CloseHandle(hProc);
    return isExplorer;
}

// ----------------------------------------------------------------------------
// 5. 帧渲染工作线程：逆向自 FUN_14005cdd0 与 FUN_14005cb20
// ----------------------------------------------------------------------------
static void VsyncWorker() {
    // 提升工作线程优先级
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 动态挂载 GDI32 内核 D3DKMT 接口
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

    std::cout << "[VsyncWorker] 渲染模式: " << (hasVsync ? "DirectX D3DKMT 硬件 VSYNC 对齐" : "1ms 多媒体时钟回退") << std::endl;

    timeBeginPeriod(1);

    while (g_Running) {
        // 等待硬件垂直消隐或高精度休眠
        if (hasVsync) {
            D3DKMT_WAITFORVERTICALBLANKEVENT waitStruct = { 0 };
            waitStruct.hAdapter = hAdapter;
            waitStruct.VidPnSourceId = vidPnSourceId;
            if (pfnWaitVBlank(&waitStruct) < 0) {
                Sleep(8);
            }
        } else {
            Sleep(8); // 约 120Hz 帧率轮询
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

        // 当本帧存在需滚动的微步时，注入合成事件
        if (totalFrameDelta != 0) {
            INPUT input = { 0 };
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = mouseFlag;
            input.mi.mouseData = totalFrameDelta;
            input.mi.dwExtraInfo = MAGIC_EXTRA_INFO; // 贴上 99 号白名单标记，防重入！

            SendInput(1, &input, sizeof(INPUT));
        }
    }

    timeEndPeriod(1);
}

// ----------------------------------------------------------------------------
// 6. 全局底层鼠标钩子回调：逆向自 FUN_14005b780 与 FUN_14005bc60
// ----------------------------------------------------------------------------
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // A. 关键防重入拦截：如果是自身发出的合成平滑微步，直接放行给窗口！
        if (pMouse->dwExtraInfo == MAGIC_EXTRA_INFO) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // B. Ctrl 键缩放紧急避让：如果用户按着 Ctrl，直接放行，避免网页缩放打架
        if (GetKeyState(VK_CONTROL) < 0) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // C. Shift 键动态转换：按住 Shift 时自动变成水平滚轮
        DWORD mouseFlag = MOUSEEVENTF_WHEEL;
        short rawDelta = static_cast<short>(HIWORD(pMouse->mouseData));
        double delta = static_cast<double>(rawDelta);

        if (GetKeyState(VK_SHIFT) < 0) {
            mouseFlag = MOUSEEVENTF_HWHEEL;
            delta = -delta; // 反转方向贴合人类水平直觉
        }

        // D. 触控板防二次平滑：如果单次步长很小 (非标准 120 跳变)，认为是触控板，放行
        int absDelta = std::abs((int)rawDelta);
        if (absDelta < 50 || (absDelta % 120 != 0 && absDelta % 100 != 0 && absDelta < 121)) {
            return CallNextHookEx(g_hHook, nCode, wParam, lParam);
        }

        // E. 动量加速度计算 (Kinetic Momentum)
        DWORD now = timeGetTime();
        DWORD dt = now - g_LastPhysicalWheelTime;
        g_LastPhysicalWheelTime = now;

        if (dt < ACCEL_THRESHOLD_MS && dt > 0) {
            double accel = (ACCEL_THRESHOLD_MS / (double)dt + 1.0) * 0.5;
            if (accel > MAX_ACCELERATION) accel = MAX_ACCELERATION;
            delta *= accel;
        }

        // F. 资源管理器 (explorer.exe) 专属动态热篡改技巧！
        HWND hWndTarget = WindowFromPoint(pMouse->pt);
        bool isExplorer = IsExplorerWindow(hWndTarget);

        if (isExplorer) {
            if (!g_InExplorer) {
                // 进入资源管理器：强行把系统滚轮行数从 3 改为 1
                SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, 1, NULL, SPIF_SENDCHANGE);
                g_InExplorer = true;
                std::cout << "[ExplorerHack] 进入文件资源管理器：SPI_SETWHEELSCROLLLINES -> 1" << std::endl;
            }
            // 步长放大 3 倍以维持整体滚动位移
            delta *= 3.0;
        } else {
            if (g_InExplorer) {
                // 离开资源管理器：还原系统原本行数
                SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, g_OriginalScrollLines, NULL, SPIF_SENDCHANGE);
                g_InExplorer = false;
                std::cout << "[ExplorerHack] 离开文件资源管理器：还原全局行数 -> " << g_OriginalScrollLines << std::endl;
            }
        }

        // G. 将物理输入压入动画物理队列
        {
            std::lock_guard<std::mutex> lock(g_TaskMutex);
            ScrollTask task;
            task.totalDelta = delta;
            task.dispatchedDelta = 0.0;
            task.startTime = now;
            task.duration = DEFAULT_ANIMATION_MS;
            task.mouseFlag = mouseFlag;
            g_TaskList.push_back(task);
        }

        // H. 彻底吞噬丢弃原始硬件突兀滚轮事件！
        return -1;
    }

    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ----------------------------------------------------------------------------
// 7. 控制台关闭处理与优雅清理
// ----------------------------------------------------------------------------
static BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (g_InExplorer) {
        SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, g_OriginalScrollLines, NULL, SPIF_SENDCHANGE);
    }
    return FALSE;
}

// ----------------------------------------------------------------------------
// 8. 入口主函数
// ----------------------------------------------------------------------------
int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "==========================================================" << std::endl;
    std::cout << " SmoothScroll 核心原理复刻原型 (PoC)" << std::endl;
    std::cout << " 基于 REA MCP 与 Ghidra 逆向分析实现" << std::endl;
    std::cout << "==========================================================" << std::endl;

    // 记录原始系统滚轮行数
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &g_OriginalScrollLines, 0);
    std::cout << "[Init] 当前系统默认滚轮行数: " << g_OriginalScrollLines << std::endl;

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // 提升进程优先级为 HIGH 或 REALTIME
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // 启动 VSYNC 帧同步后台工作线程
    g_VsyncThread = std::thread(VsyncWorker);

    // 安装 WH_MOUSE_LL 全局低级别钩子
    g_hHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
    if (!g_hHook) {
        std::cerr << "[Error] 安装 WH_MOUSE_LL 钩子失败，错误码: " << GetLastError() << std::endl;
        g_Running = false;
        if (g_VsyncThread.joinable()) g_VsyncThread.join();
        return 1;
    }

    std::cout << "[Init] 钩子安装成功！平滑滚动已激活。" << std::endl;
    std::cout << "[Tip] 请尝试在文件资源管理器 (Explorer)、浏览器、记事本中滚动滚轮。" << std::endl;
    std::cout << "[Tip] 尝试按住 Shift 进行横向平滑滚动，或按住 Ctrl 进行原生缩放。" << std::endl;
    std::cout << "[Tip] 按 Ctrl + C 退出程序并自动恢复系统设置。" << std::endl;

    // 消息泵保持钩子存活
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
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

    return 0;
}
