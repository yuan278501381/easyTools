// ─────────────────────────────────────────────────────────────────────────────
// GestureTrailOverlay.cpp — 手势轨迹可视化覆盖层实现
//
// 核心原理:
//   1. 创建 WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST 的全屏窗口
//   2. 使用 Direct2D 绘制连续贝塞尔平滑轨迹，带双层霓虹微光流光特效
//   3. 头部绘制发光能量微粒，提升绘制动感
//   4. 手势绘制过程中及手势完成后显示按键回显风格的实时动作名称
//   5. 窗口始终 click-through（WS_EX_TRANSPARENT），不影响用户操作
//   6. 颜色支持独立自定义配置或动态联动系统主题强调色
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureTrailOverlay.h"
#include "gesture/GestureInputPolicy.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/config/ConfigManager.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <climits>

#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>

using namespace Microsoft::WRL;

namespace easy::gesture {

namespace {

int snapDown(int value, int grid) {
    if (grid <= 0) return value;
    if (value >= 0) return (value / grid) * grid;
    return -(((-value + grid - 1) / grid) * grid);
}

int snapUp(int value, int grid) {
    if (grid <= 0) return value;
    if (value >= 0) return ((value + grid - 1) / grid) * grid;
    return -((-value / grid) * grid);
}

void ensurePremultipliedAlpha(void* bits, int width, int height, int pitch) noexcept {
    if (!bits || width <= 0 || height <= 0 || pitch < width * 4) return;
    auto* row = reinterpret_cast<uint8_t*>(bits);
    for (int y = 0; y < height; ++y) {
        auto* px = reinterpret_cast<uint32_t*>(row);
        for (int x = 0; x < width; ++x) {
            const uint32_t val = px[x];
            if (val != 0) {
                const uint8_t a = static_cast<uint8_t>((val >> 24) & 0xFF);
                if (a == 0) {
                    const uint8_t r = static_cast<uint8_t>((val >> 16) & 0xFF);
                    const uint8_t g = static_cast<uint8_t>((val >> 8) & 0xFF);
                    const uint8_t b = static_cast<uint8_t>(val & 0xFF);
                    const uint8_t maxC = (std::max)({r, g, b});
                    px[x] = (static_cast<uint32_t>(maxC) << 24) | (val & 0x00FFFFFF);
                }
            }
        }
        row += pitch;
    }
}

}  // namespace

static constexpr const wchar_t* OVERLAY_CLASS = L"EasyTools_GestureOverlay";
static constexpr UINT WM_GESTURE_ACCESSIBILITY_RESULT = WM_APP + 73;

GestureTrailOverlay& GestureTrailOverlay::instance() {
    static GestureTrailOverlay inst;
    return inst;
}

// ─────────────────────────────────────────────────────────────────────────────
// 初始化 / 关闭
// ─────────────────────────────────────────────────────────────────────────────

bool GestureTrailOverlay::initialize(HINSTANCE hInstance) {
    easy::core::TraceId::Scope scope;
    m_hInstance = hInstance;

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("创建手势渲染同步事件失败");
        return false;
    }

    // 渲染线程自建并独立持有 HWND (m_hwnd 与 m_toastHwnd)，彻底根除跨线程 HWND 亲和性互锁
    m_renderThread = std::jthread([this, readyEvent](std::stop_token st) {
        renderLoop(st, readyEvent);
    });

    if (m_renderThread.native_handle()) {
        SetThreadPriority(m_renderThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    }

    // 等待渲染线程完成窗口与 DirectComposition 设备创建
    WaitForSingleObject(readyEvent, 5000);
    CloseHandle(readyEvent);

    LOG_INFO("手势轨迹覆盖层初始化成功 (专用异步渲染管线已启动)");
    return true;
}

void GestureTrailOverlay::setStyle(const TrailStyle& style) {
    m_style = style;
    m_textScale = 0.0f;
    if (m_dwriteFactory) updateTextFormat(m_dpiScale);
    reloadThemeColors();
}

void GestureTrailOverlay::reloadThemeColors() {
    m_themeDirty.store(true, std::memory_order_release);
    if (m_visible.load(std::memory_order_relaxed) ||
        m_wantVisible.load(std::memory_order_relaxed) ||
        m_fading.load(std::memory_order_relaxed)) {
        m_wakeRender.store(true, std::memory_order_release);
        m_renderCv.notify_one();
    }
}

void GestureTrailOverlay::applyThemeColorsLocked() {
    if (!m_renderTarget) return;

    auto& cfg = easy::core::ConfigManager::instance();
    const std::string colorMode = cfg.get<std::string>("/gesture/trailColorMode", "auto");
    const std::string customHex = cfg.get<std::string>("/gesture/trailColor", "#3B82F6");
    m_style.lineWidth = cfg.get<float>("/gesture/trailWidth", 2.5f);
    m_style.outlineWidth = clampTrailOutlineWidth(
        cfg.get<float>("/gesture/trailOutlineWidth", 1.5f));

    const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
    const easy::core::AccentColorRGB themeRgb = easy::core::getAccentColorRGB(accent);
    const easy::core::AccentColorRGB trailRgb =
        resolveGestureTrailRgb(colorMode, customHex, themeRgb);

    // 主流光画笔（可自定义或跟随主题）
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 1.0f),
        m_lineBrush.ReleaseAndGetAddressOf()
    );

    // 外部柔光霓虹画笔
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.40f),
        m_glowBrush.ReleaseAndGetAddressOf()
    );

    // 检测是否为亮色主题
    const std::string theme = cfg.get<std::string>("/general/theme", "system");
    bool systemAppsUseLight = false;
    if (theme == "system") {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD value = 1;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
                systemAppsUseLight = (value != 0);
            }
            RegCloseKey(hKey);
        }
    }
    const bool isLight = gestureTrailUsesLightPalette(theme, systemAppsUseLight);

    // 灰色画笔 (未匹配动作时使用)。亮色主题必须足够深，浅银灰叠在白壁纸上等于没有轨迹。
    if (isLight) {
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.28f, 0.31f, 0.38f, 0.96f),
            m_greyLineBrush.ReleaseAndGetAddressOf()
        );
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.40f),
            m_greyGlowBrush.ReleaseAndGetAddressOf()
        );
    } else {
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.60f, 0.65f, 0.75f, 0.85f),
            m_greyLineBrush.ReleaseAndGetAddressOf()
        );
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.40f, 0.45f, 0.55f, 0.30f),
            m_greyGlowBrush.ReleaseAndGetAddressOf()
        );
    }

    // 头部发光核心晶体画笔
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        m_headCoreBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        m_outlineBrush.ReleaseAndGetAddressOf()
    );

    if (!m_toastTarget) return;

    if (isLight) {
        m_toastTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.92f, 0.18f, 0.24f, 0.94f),
            m_excessiveBgBrush.ReleaseAndGetAddressOf()
        );
        m_toastTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.78f, 0.10f, 0.16f, 0.85f),
            m_excessiveBorderBrush.ReleaseAndGetAddressOf()
        );
    } else {
        m_toastTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.52f, 0.08f, 0.12f, 0.92f),
            m_excessiveBgBrush.ReleaseAndGetAddressOf()
        );
        m_toastTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.96f, 0.28f, 0.36f, 0.85f),
            m_excessiveBorderBrush.ReleaseAndGetAddressOf()
        );
    }
    m_toastTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        m_excessiveDotBrush.ReleaseAndGetAddressOf()
    );
    m_toastTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.82f),
        m_textBgBrush.ReleaseAndGetAddressOf()
    );
    m_toastTarget->CreateSolidColorBrush(
        D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.95f),
        m_themeBgBrush.ReleaseAndGetAddressOf()
    );
    m_toastTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f),
        m_textBorderBrush.ReleaseAndGetAddressOf()
    );
    m_toastTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        m_textBrush.ReleaseAndGetAddressOf()
    );
}

void GestureTrailOverlay::shutdown() {
    if (m_renderThread.joinable()) {
        m_renderThread.request_stop();
        m_renderCv.notify_all();
        LOG_DEBUG("Gesture shutdown: waiting for render worker");
        easy::core::joinWorkerWhilePumpingSentMessages(m_renderThread);
        LOG_DEBUG("Gesture shutdown: render worker stopped");
    }
    releaseD2DResources();
    m_visible.store(false);
    LOG_DEBUG("手势轨迹覆盖层已关闭");
}

void GestureTrailOverlay::clearCanvas() {
    std::lock_guard lock(m_renderMutex);
    clearCanvasLocked();
}

void GestureTrailOverlay::clearCanvasLocked() {
    if (m_memoryBits && m_height > 0 && m_memoryPitch > 0) {
        std::memset(m_memoryBits, 0, static_cast<size_t>(m_memoryPitch * m_height));
    }
    if (m_toastBits && m_toastHeight > 0 && m_toastPitch > 0) {
        std::memset(m_toastBits, 0, static_cast<size_t>(m_toastPitch * m_toastHeight));
    }
    if (m_hwnd && m_memoryDC && m_width > 0 && m_height > 0) {
        HDC hdcScreen = GetDC(nullptr);
        if (hdcScreen) {
            POINT ptSrc = { 0, 0 };
            SIZE sz = { m_width, m_height };
            POINT ptDst = { m_originX, m_originY };
            BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(m_hwnd, hdcScreen, &ptDst, &sz, m_memoryDC, &ptSrc, 0, &blend, ULW_ALPHA);
            ReleaseDC(nullptr, hdcScreen);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 异步渲染核心循环 (在专用高优先级线程运行)
// ─────────────────────────────────────────────────────────────────────────────

void GestureTrailOverlay::renderLoop(std::stop_token stopToken, HANDLE readyEvent) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoGuard { ~CoGuard() { CoUninitialize(); } } coGuard;

    D2D1_FACTORY_OPTIONS opt{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, opt, m_d2dFactory.GetAddressOf());
    if (SUCCEEDED(hr)) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
        );
        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f
        );
        m_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0, m_strokeStyle.GetAddressOf());
    }

    if (!createOverlayWindow(m_hInstance)) {
        LOG_ERROR("渲染线程创建手势覆盖层窗口失败");
        if (readyEvent) SetEvent(readyEvent);
        return;
    }

    ensureCompositorLocked();

    if (readyEvent) {
        SetEvent(readyEvent);
    }

    while (!stopToken.stop_requested()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_zOrderYieldRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        if (m_zOrderRaiseRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hwnd && IsWindow(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            if (m_toastHwnd && IsWindow(m_toastHwnd)) {
                SetWindowPos(m_toastHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        {
            std::unique_lock lock(m_renderSignalMutex);
            if (m_fading.load(std::memory_order_relaxed)) {
                m_renderCv.wait_for(lock, std::chrono::milliseconds(16), [&]() {
                    return m_hideRequested.load(std::memory_order_relaxed) ||
                           m_wakeRender.load(std::memory_order_relaxed) ||
                           m_zOrderYieldRequested.load(std::memory_order_relaxed) ||
                           m_zOrderRaiseRequested.load(std::memory_order_relaxed) ||
                           stopToken.stop_requested();
                });
            } else {
                m_renderCv.wait(lock, [&]() {
                    return m_hideRequested.load(std::memory_order_relaxed) ||
                           m_fading.load(std::memory_order_relaxed) ||
                           m_wakeRender.load(std::memory_order_relaxed) ||
                           m_zOrderYieldRequested.load(std::memory_order_relaxed) ||
                           m_zOrderRaiseRequested.load(std::memory_order_relaxed) ||
                           stopToken.stop_requested();
                });
            }

            if (stopToken.stop_requested()) break;
            m_wakeRender.store(false, std::memory_order_relaxed);
        }

        if (m_hideRequested.exchange(false, std::memory_order_acq_rel)) {
            applyHideOverlayState();
            continue;
        }

        if (m_dismissPrevious.exchange(false, std::memory_order_acq_rel)) {
            {
                std::lock_guard renderLock(m_renderMutex);
                clearCanvasLocked();
            }
            if (!m_wantVisible.load(std::memory_order_relaxed) &&
                !m_fading.load(std::memory_order_relaxed)) {
                if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
                hideToastWindow();
                m_visible.store(false, std::memory_order_relaxed);
            }
        }

        m_renderRequested.store(false, std::memory_order_relaxed);

        bool fading = m_fading.load(std::memory_order_relaxed);
        if (fading) {
            const uint64_t currentEpoch = m_trailEpoch.load(std::memory_order_acquire);
            if (currentEpoch != m_fadeEpoch) {
                // 新手势已开始并打断了上一笔淡出，绝不隐藏窗口或释放资源
                m_fading.store(false, std::memory_order_relaxed);
                m_fadeClockStarted.store(false, std::memory_order_relaxed);
                fading = false;
            } else {
                const DWORD holdMs = static_cast<DWORD>(m_style.fadeHoldMs);
                const DWORD fadeMs = static_cast<DWORD>(m_style.fadeOutMs);
                const bool clockStarted = m_fadeClockStarted.load(std::memory_order_relaxed);
                const DWORD elapsed = clockStarted ? (GetTickCount() - m_fadeStartTick) : 0;
                if (gestureFadeShouldFinish(clockStarted, elapsed, holdMs, fadeMs)) {
                    if (m_trailEpoch.load(std::memory_order_acquire) != m_fadeEpoch) {
                        m_fading.store(false, std::memory_order_relaxed);
                        m_fadeClockStarted.store(false, std::memory_order_relaxed);
                        fading = false;
                    } else {
                        m_fading.store(false, std::memory_order_relaxed);
                        m_fadeClockStarted.store(false, std::memory_order_relaxed);
                        m_fadeAlpha = 0.0f;
                        {
                            std::lock_guard trailLock(m_trailMutex);
                            m_points.clear();
                            m_resultText.clear();
                            m_smoothPathGeometry.Reset();
                        }
                        m_isRecognized.store(false, std::memory_order_relaxed);
                        m_wantVisible.store(false, std::memory_order_relaxed);
                        m_strokeSurfaceLive.store(false, std::memory_order_relaxed);
                        {
                            std::lock_guard renderLock(m_renderMutex);
                            clearCanvasLocked();
                        }
                        if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
                        hideToastWindow();
                        m_visible.store(false, std::memory_order_relaxed);
                        continue;
                    }
                } else {
                    m_fadeAlpha = gestureFadeAlpha(clockStarted, elapsed, holdMs, fadeMs);
                    const bool presented = render();
                    if (presented && !clockStarted) {
                        m_fadeStartTick = GetTickCount();
                        m_fadeClockStarted.store(true, std::memory_order_release);
                    }
                    continue;
                }
            }
        }

        if (!fading && (m_visible.load(std::memory_order_relaxed) ||
                        m_wantVisible.load(std::memory_order_relaxed))) {
            render();
        }
    }

    // 渲染线程销毁自身持有的所有 HWND 与 DirectComposition 资源
    if (m_toastHwnd) {
        DestroyWindow(m_toastHwnd);
        m_toastHwnd = nullptr;
    }
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }
    releaseCompositorLocked();
}

// ─────────────────────────────────────────────────────────────────────────────
// 轨迹操作：避免同步 I/O；实际耗时应通过 PerformanceMonitor 在目标设备上测量。
// ─────────────────────────────────────────────────────────────────────────────

void GestureTrailOverlay::beginTrail() {
    raiseZOrderForDraw();
    m_trailEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_hideRequested.store(false, std::memory_order_release);
    m_wantVisible.store(false, std::memory_order_release);
    m_strokeSurfaceLive.store(false, std::memory_order_release);
    m_fading.store(false, std::memory_order_release);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_lastWakeTick.store(0, std::memory_order_release);
    m_fadeAlpha = 1.0f;
    m_pointQueue.clear();
    {
        std::lock_guard lock(m_trailMutex);
        m_points.clear();
        m_resultText.clear();
        m_smoothPathGeometry.Reset();
    }

    m_isRecognized.store(false, std::memory_order_relaxed);
    m_themeDirty.store(true, std::memory_order_release);
    m_dismissPrevious.store(true, std::memory_order_release);
    m_wakeRender.store(true, std::memory_order_release);
    m_renderCv.notify_one();
}

void GestureTrailOverlay::addPoint(float x, float y) {
    m_pointQueue.push(TrailPoint{x, y, GetTickCount()});

    if (m_hwnd) {
        m_fading.store(false, std::memory_order_release);
        m_fadeAlpha = 1.0f;
        const bool firstVisible =
            !m_wantVisible.exchange(true, std::memory_order_acq_rel);
        m_renderRequested.store(true, std::memory_order_release);
        // 高频鼠标可能以 500/1000 Hz 上报。渲染线程只需要按 120 Hz 合并
        // 最新点；松手和状态变化仍会无条件唤醒，不会丢掉最终一帧。
        const DWORD now = GetTickCount();
        const DWORD lastWake = m_lastWakeTick.load(std::memory_order_relaxed);
        if (firstVisible || now - lastWake >= 8) {
            m_lastWakeTick.store(now, std::memory_order_relaxed);
            m_wakeRender.store(true, std::memory_order_release);
            m_renderCv.notify_one();
        }
    }
}

void GestureTrailOverlay::setLiveAction(const std::string& actionText) {
    bool changed = false;
    {
        std::lock_guard lock(m_trailMutex);
        if (m_resultText != actionText) {
            m_resultText = actionText;
            changed = true;
        }
    }
    if (changed && (m_visible.load(std::memory_order_relaxed) ||
                    m_wantVisible.load(std::memory_order_relaxed))) {
        m_wakeRender.store(true, std::memory_order_release);
        m_renderRequested.store(true, std::memory_order_release);
        m_renderCv.notify_one();
    }
}

void GestureTrailOverlay::setRecognized(bool recognized) {
    if (m_isRecognized.exchange(recognized) != recognized) {
        if (!recognized) {
            std::lock_guard lock(m_trailMutex);
            if (m_resultText != "•••") {
                m_resultText.clear();
            }
        }
        if (m_visible.load(std::memory_order_relaxed) ||
            m_wantVisible.load(std::memory_order_relaxed)) {
            m_wakeRender.store(true, std::memory_order_release);
            m_renderRequested.store(true, std::memory_order_release);
            m_renderCv.notify_one();
        }
    }
}

void GestureTrailOverlay::endTrail(const std::string& resultText) {
    bool hasPoints = false;
    size_t overlayPoints = 0;
    {
        std::lock_guard lock(m_trailMutex);
        if (!resultText.empty()) {
            m_resultText = resultText;
        }
        hasPoints = !m_points.empty();
        overlayPoints = m_points.size();
    }
    LOG_DEBUG("手势轨迹结束: overlayPoints={}, label={}", overlayPoints, resultText);
    // 松手时才命中的短手势（如 U=关闭窗口）过程中方向可能没变过，
    // live setRecognized(true) 没走到，Toast 不能因此被关掉。
    if (!resultText.empty() && resultText != "•••") {
        m_isRecognized.store(true, std::memory_order_relaxed);
        // endTrail may run on the low-level mouse-hook path. Do not synchronously
        // call a window API here; hand the accessibility update to the HWND's
        // owning thread instead.
        if (m_toastHwnd) {
            PostMessageW(m_toastHwnd, WM_GESTURE_ACCESSIBILITY_RESULT, 0, 0);
        }
    }
    if (hasPoints) {
        m_wantVisible.store(true, std::memory_order_release);
    }
    m_fadeEpoch = m_trailEpoch.load(std::memory_order_acquire);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_fadeStartTick = 0;
    m_fadeAlpha = 1.0f;
    m_fading.store(true, std::memory_order_release);
    m_wakeRender.store(true, std::memory_order_release);
    m_renderRequested.store(true, std::memory_order_release);
    m_renderCv.notify_one();
}

void GestureTrailOverlay::hide() {
    m_trailEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_fading.store(false, std::memory_order_release);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_wantVisible.store(false, std::memory_order_release);
    m_visible.store(false, std::memory_order_release);
    m_renderRequested.store(false, std::memory_order_release);
    m_hideRequested.store(true, std::memory_order_release);
    m_wakeRender.store(true, std::memory_order_release);
    m_renderCv.notify_one();
}

void GestureTrailOverlay::yieldZOrderForInput() {
    m_zOrderYieldRequested.store(true, std::memory_order_release);
    m_zOrderYielded.store(true, std::memory_order_release);
    m_wakeRender.store(true, std::memory_order_release);
    m_renderCv.notify_one();
}

void GestureTrailOverlay::raiseZOrderForDraw() {
    m_zOrderRaiseRequested.store(true, std::memory_order_release);
    m_zOrderYielded.store(false, std::memory_order_release);
    m_wakeRender.store(true, std::memory_order_release);
    m_renderCv.notify_one();
}

void GestureTrailOverlay::applyHideOverlayState() {
    {
        std::lock_guard lock{m_renderMutex};
        clearCanvasLocked();
        releaseCompositorSurfacesLocked();
    }
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    hideToastWindow();
    m_pointQueue.clear();
    {
        std::lock_guard lock{m_trailMutex};
        m_points.clear();
        m_resultText.clear();
        m_smoothPathGeometry.Reset();
    }
    m_isRecognized.store(false);
    m_fadeAlpha = 1.0f;
    m_strokeSurfaceLive.store(false, std::memory_order_relaxed);
    releaseD2DResources();
    m_width = 0;
    m_height = 0;

    // 冷路径退场修剪，主动归还物理内存
    easy::core::WinUtils::trimWorkingSet();
}

bool GestureTrailOverlay::recreateBitmapLocked(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || !m_hwnd) return false;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    if (!m_memoryDC) {
        m_memoryDC = CreateCompatibleDC(hdcScreen);
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!m_memoryDC || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return false;
    }
    std::memset(bits, 0, static_cast<size_t>(width * 4 * height));

    HBITMAP selected = static_cast<HBITMAP>(SelectObject(m_memoryDC, bmp));
    if (m_memoryBitmap && selected == m_memoryBitmap) {
        DeleteObject(m_memoryBitmap);
    } else if (selected && selected != HGDI_ERROR && !m_oldBitmap) {
        m_oldBitmap = selected;
    }
    m_memoryBitmap = bmp;
    m_memoryBits = bits;
    m_memoryPitch = width * 4;
    m_originX = x;
    m_originY = y;
    m_width = width;
    m_height = height;

    if (m_renderTarget) {
        m_renderTarget.Reset();
        m_lineBrush.Reset();
        m_glowBrush.Reset();
        m_greyLineBrush.Reset();
        m_greyGlowBrush.Reset();
        m_headCoreBrush.Reset();
        m_outlineBrush.Reset();
    }
    LOG_DEBUG("手势轨迹表面重建: {}x{} at ({},{})", width, height, x, y);
    return true;
}

bool GestureTrailOverlay::presentLayeredLocked(HWND hwnd, HDC memDC, int x, int y,
                                               int width, int height) {
    if (!hwnd || !memDC || width <= 0 || height <= 0) return false;
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    POINT ptSrc = {0, 0};
    POINT ptWin = {x, y};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL ok = UpdateLayeredWindow(
        hwnd, hdcScreen, &ptWin, &size, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    const DWORD err = ok ? 0 : GetLastError();
    ReleaseDC(nullptr, hdcScreen);
    if (!ok) {
        LOG_WARN("手势覆盖层提交失败: {}x{} error={}", width, height, err);
        return false;
    }
    // HWND_BOTTOM 沉底后 WS_EX_TOPMOST 位经常还在，旧逻辑会跳过插队，轨迹画在
    // 最大化 Electron / CEF 窗下面。沉底过就必须无条件回到 TOPMOST 组。
    const bool yielded = m_zOrderYielded.load(std::memory_order_acquire);
    if (!yielded) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    return true;
}

void GestureTrailOverlay::hideToastWindow() {
    if (m_toastHwnd && IsWindowVisible(m_toastHwnd)) {
        ShowWindow(m_toastHwnd, SW_HIDE);
    }
}

void GestureTrailOverlay::releaseToastSurfaceLocked() {
    if (m_toastDC && m_toastOldBitmap) {
        SelectObject(m_toastDC, m_toastOldBitmap);
    }
    m_toastOldBitmap = nullptr;
    if (m_toastBitmap) {
        DeleteObject(m_toastBitmap);
        m_toastBitmap = nullptr;
    }
    m_toastBits = nullptr;
    m_toastPitch = 0;
    if (m_toastDC) {
        DeleteDC(m_toastDC);
        m_toastDC = nullptr;
    }
    m_toastWidth = 0;
    m_toastHeight = 0;
}

bool GestureTrailOverlay::ensureToastSurfaceLocked(int width, int height) {
    if (width <= 0 || height <= 0 || !m_toastHwnd) return false;
    if (m_toastDC && m_toastBitmap && m_toastWidth == width && m_toastHeight == height) {
        return true;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    if (!m_toastDC) {
        m_toastDC = CreateCompatibleDC(hdcScreen);
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!m_toastDC || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return false;
    }
    std::memset(bits, 0, static_cast<size_t>(width * 4 * height));
    HBITMAP selected = static_cast<HBITMAP>(SelectObject(m_toastDC, bmp));
    if (m_toastBitmap && selected == m_toastBitmap) {
        DeleteObject(m_toastBitmap);
    } else if (selected && selected != HGDI_ERROR && !m_toastOldBitmap) {
        m_toastOldBitmap = selected;
    }
    m_toastBitmap = bmp;
    m_toastBits = bits;
    m_toastPitch = width * 4;
    m_toastWidth = width;
    m_toastHeight = height;
    return true;
}

bool GestureTrailOverlay::fitSurface(int left, int top, int right, int bottom) {
    if (m_virtualW <= 0 || m_virtualH <= 0) {
        m_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m_virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    // 只为笔迹附近分配分层位图。256px 网格和 96px 余量让普通手势
    // 通常一次分配即可完成，同时避免把 4K/超宽屏约 30MB 的整屏 DIB
    // 在每一帧重新上传给 DWM。
    constexpr int kGrid = 256;
    constexpr int kPad = 96;
    constexpr int kMin = 512;
    left = snapDown(left - kPad, kGrid);
    top = snapDown(top - kPad, kGrid);
    right = snapUp(right + kPad, kGrid);
    bottom = snapUp(bottom + kPad, kGrid);

    left = (std::max)(left, m_virtualX);
    top = (std::max)(top, m_virtualY);
    right = (std::min)(right, m_virtualX + m_virtualW);
    bottom = (std::min)(bottom, m_virtualY + m_virtualH);

    auto ensureMinimumExtent = [](int& begin, int& end, int minExtent,
                                  int boundBegin, int boundEnd) {
        if (end - begin >= minExtent || boundEnd - boundBegin <= minExtent) return;
        end = (std::min)(boundEnd, begin + minExtent);
        begin = (std::max)(boundBegin, end - minExtent);
    };
    ensureMinimumExtent(left, right, kMin, m_virtualX, m_virtualX + m_virtualW);
    ensureMinimumExtent(top, bottom, kMin, m_virtualY, m_virtualY + m_virtualH);

    const int neededW = (std::max)(1, right - left);
    const int neededH = (std::max)(1, bottom - top);
    if (m_compositorReady && m_dcompDevice && m_trailDcompVisual) {
        if (m_strokeSurfaceLive.load(std::memory_order_relaxed)) {
            if (overlaySurfaceContains(left, top, right, bottom,
                                       m_originX, m_originY, m_width, m_height) &&
                m_trailDcompSurface) {
                return true;
            }
            growOverlayRect(left, top, right, bottom,
                            m_originX, m_originY, m_width, m_height);
        } else if (overlaySurfaceContains(left, top, right, bottom,
                                          m_originX, m_originY, m_width, m_height) &&
                   overlayCanReuseSurface(neededW, neededH, m_width, m_height, 4) &&
                   m_trailDcompSurface) {
            m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
            return true;
        }

        const int targetW = (std::max)(1, right - left);
        const int targetH = (std::max)(1, bottom - top);
        m_trailDcompSurface.Reset();
        HRESULT hr = m_dcompDevice->CreateSurface(
            static_cast<UINT>(targetW), static_cast<UINT>(targetH),
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
            m_trailDcompSurface.GetAddressOf());
        if (FAILED(hr) || !m_trailDcompSurface) {
            LOG_WARN("创建 DirectComposition 轨迹表面失败: {}x{}, hr=0x{:X}", targetW, targetH, hr);
            return false;
        }
        m_trailDcompVisual->SetContent(m_trailDcompSurface.Get());
        m_trailDcompW = targetW;
        m_trailDcompH = targetH;
        m_originX = left;
        m_originY = top;
        m_width = targetW;
        m_height = targetH;
        m_strokeSurfaceLive.store(true, std::memory_order_relaxed);

        SetWindowPos(m_hwnd, nullptr, m_originX, m_originY, m_width, m_height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return true;
    }

    if (m_strokeSurfaceLive.load(std::memory_order_relaxed)) {
        if (overlaySurfaceContains(left, top, right, bottom,
                                   m_originX, m_originY, m_width, m_height) &&
            m_renderTarget && m_memoryBitmap) {
            return true;
        }
        growOverlayRect(left, top, right, bottom,
                        m_originX, m_originY, m_width, m_height);
        const bool ok = recreateBitmapLocked(
            left, top, (std::max)(1, right - left), (std::max)(1, bottom - top));
        if (ok) m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
        return ok;
    }

    if (overlaySurfaceContains(left, top, right, bottom,
                               m_originX, m_originY, m_width, m_height) &&
        overlayCanReuseSurface(neededW, neededH, m_width, m_height, 4) &&
        m_renderTarget && m_memoryBitmap) {
        m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
        return true;
    }

    const bool ok = recreateBitmapLocked(left, top, neededW, neededH);
    if (ok) m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口创建
// ─────────────────────────────────────────────────────────────────────────────

bool GestureTrailOverlay::createOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    m_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_originX = m_virtualX;
    m_originY = m_virtualY;
    m_width = 256;
    m_height = 256;

    m_helperOwnerHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC",
        L"EasyTools_GestureTrailHelperOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );

    const DWORD layeredEx = static_cast<DWORD>(normalizeGestureOverlayExStyle(0));

    m_hwnd = CreateWindowExW(
        layeredEx,
        OVERLAY_CLASS,
        L"EasyTools Gesture Trail",
        WS_POPUP,
        m_originX, m_originY, m_width, m_height,
        m_helperOwnerHwnd,
        nullptr,
        hInstance,
        this
    );

    if (!m_hwnd) {
        LOG_ERROR("创建手势轨迹窗口失败");
        return false;
    }

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
                      normalizeGestureOverlayExStyle(
                          GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)));
    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &noCorners, sizeof(noCorners));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disableTransitions, sizeof(disableTransitions));
    SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
    // HWND 需要非零尺寸才能创建；追踪表面从 0 开始，避免把虚拟屏左上角的 256×256
    // 占位框并进第一笔轨迹，把覆盖层钉死在屏幕角落。
    m_width = 0;
    m_height = 0;

    m_toastHwnd = CreateWindowExW(
        layeredEx,
        OVERLAY_CLASS,
        L"EasyTools Gesture Toast",
        WS_POPUP,
        0, 0, 64, 64,
        // Keep the result card owned by the trail surface so it cannot fall
        // behind it after another TOPMOST popup (such as a Shell menu) closes.
        m_helperOwnerHwnd,
        nullptr,
        hInstance,
        this
    );
    if (m_toastHwnd) {
        SetWindowLongPtrW(m_toastHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetWindowLongPtrW(m_toastHwnd, GWL_EXSTYLE,
                          normalizeGestureOverlayExStyle(
                              GetWindowLongPtrW(m_toastHwnd, GWL_EXSTYLE)));
        DwmSetWindowAttribute(m_toastHwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &noCorners, sizeof(noCorners));
        DwmSetWindowAttribute(m_toastHwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                              &disableTransitions, sizeof(disableTransitions));
        SetWindowDisplayAffinity(m_toastHwnd, WDA_NONE);
        ShowWindow(m_toastHwnd, SW_HIDE);
    } else {
        LOG_WARN("创建手势结果卡片窗口失败，轨迹仍可绘制");
    }

    LOG_INFO("手势覆盖层初始化完成 (原生分层窗口硬件合成加速管线已就绪)");
    return true;
}

bool GestureTrailOverlay::ensureCompositorLocked() {
    if (m_compositorReady && m_dcompDevice && m_trailDcompTarget) return true;
    releaseCompositorLocked();
    if (!m_hwnd) return false;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
    }
    if (FAILED(hr) || !d3d) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d.As(&dxgiDevice)) || !dxgiDevice) return false;

    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp;
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(dcomp.GetAddressOf()));
    if (FAILED(hr) || !dcomp) return false;

    Microsoft::WRL::ComPtr<IDCompositionTarget> trailTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> trailVisual;
    if (FAILED(dcomp->CreateTargetForHwnd(m_hwnd, TRUE, trailTarget.GetAddressOf())) ||
        FAILED(dcomp->CreateVisual(trailVisual.GetAddressOf())) ||
        FAILED(trailTarget->SetRoot(trailVisual.Get()))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDCompositionTarget> toastTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> toastVisual;
    if (m_toastHwnd) {
        if (FAILED(dcomp->CreateTargetForHwnd(m_toastHwnd, TRUE, toastTarget.GetAddressOf())) ||
            FAILED(dcomp->CreateVisual(toastVisual.GetAddressOf())) ||
            FAILED(toastTarget->SetRoot(toastVisual.Get()))) {
            return false;
        }
    }

    m_d3dDevice = std::move(d3d);
    m_dcompDevice = std::move(dcomp);
    m_trailDcompTarget = std::move(trailTarget);
    m_trailDcompVisual = std::move(trailVisual);
    m_toastDcompTarget = std::move(toastTarget);
    m_toastDcompVisual = std::move(toastVisual);
    m_compositorReady = true;
    return true;
}

void GestureTrailOverlay::releaseCompositorSurfacesLocked() {
    if (m_trailDcompVisual) m_trailDcompVisual->SetContent(nullptr);
    if (m_toastDcompVisual) m_toastDcompVisual->SetContent(nullptr);
    m_trailDcompSurface.Reset();
    m_toastDcompSurface.Reset();
    m_trailDcompW = 0;
    m_trailDcompH = 0;
    m_toastDcompW = 0;
    m_toastDcompH = 0;
}

void GestureTrailOverlay::releaseCompositorLocked() {
    releaseCompositorSurfacesLocked();
    m_trailDcompVisual.Reset();
    m_toastDcompVisual.Reset();
    m_trailDcompTarget.Reset();
    m_toastDcompTarget.Reset();
    m_dcompDevice.Reset();
    m_d3dDevice.Reset();
    m_compositorReady = false;
}



// ─────────────────────────────────────────────────────────────────────────────
// Direct2D 资源管理
// ─────────────────────────────────────────────────────────────────────────────

bool GestureTrailOverlay::createD2DResources() {
    if (m_renderTarget && m_memoryDC && m_memoryBitmap && m_lineBrush && m_textBorderBrush) return true;

    auto fail = [this]() {
        releaseD2DResourcesLocked();
        return false;
    };

    HRESULT hr = S_OK;

    // D2D 工厂
    if (!m_d2dFactory) {
        D2D1_FACTORY_OPTIONS options{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, options, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return fail();
    }

    // DirectWrite 工厂
    if (!m_dwriteFactory) {
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
        );
        if (FAILED(hr)) return fail();
    }

    if (!updateTextFormat(m_dpiScale)) return fail();

    // 渲染目标
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
    );

    hr = m_d2dFactory->CreateDCRenderTarget(&rtProps, m_renderTarget.GetAddressOf());
    if (FAILED(hr)) return fail();

    if (!m_memoryDC) {
        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen) return fail();
        m_memoryDC = CreateCompatibleDC(hdcScreen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_width;
        bmi.bmiHeader.biHeight = -m_height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        m_memoryBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hdcScreen);
        if (!m_memoryDC || !m_memoryBitmap || !pBits) return fail();
        m_oldBitmap = (HBITMAP)SelectObject(m_memoryDC, m_memoryBitmap);
    }
    
    RECT memRect = { 0, 0, m_width, m_height };
    if (FAILED(m_renderTarget->BindDC(m_memoryDC, &memRect))) return fail();

    m_renderTarget->SetDpi(96.0f, 96.0f);

    // 笔触样式 (使线段更平滑，具有圆润笔头与圆角拐弯)
    D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND,
        10.0f,
        D2D1_DASH_STYLE_SOLID,
        0.0f
    );
    hr = m_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0, m_strokeStyle.GetAddressOf());
    if (FAILED(hr)) return fail();

    applyThemeColorsLocked();
    m_themeDirty.store(false, std::memory_order_release);

    if (!m_lineBrush || !m_headCoreBrush) return fail();
    ensureToastTargetLocked();
    return true;
}

bool GestureTrailOverlay::ensureToastTargetLocked() {
    if (m_toastTarget) {
        if (!m_themeBgBrush || !m_textBgBrush || !m_textBrush) {
            applyThemeColorsLocked();
        }
        return true;
    }
    if (!m_d2dFactory || !m_toastHwnd) return false;
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
    );
    if (FAILED(m_d2dFactory->CreateDCRenderTarget(&rtProps, m_toastTarget.GetAddressOf())) ||
        !m_toastTarget) {
        m_toastTarget.Reset();
        return false;
    }
    m_toastTarget->SetDpi(96.0f, 96.0f);
    applyThemeColorsLocked();
    return m_textBrush && m_textBorderBrush;
}

bool GestureTrailOverlay::updateTextFormat(float dpiScale) {
    dpiScale = std::clamp(dpiScale, 1.0f, 5.0f);
    if (m_textFormat && std::abs(m_textScale - dpiScale) < 0.01f) return true;
    if (!m_dwriteFactory) return false;
    ComPtr<IDWriteTextFormat> format;
    const HRESULT hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        m_style.resultFontSize * dpiScale,
        L"zh-CN",
        format.GetAddressOf()
    );
    if (FAILED(hr) || !format) return false;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_textFormat = std::move(format);
    m_textScale = dpiScale;
    return true;
}

void GestureTrailOverlay::releaseD2DResources() {
    std::lock_guard lock(m_renderMutex);
    releaseD2DResourcesLocked();
}

void GestureTrailOverlay::releaseD2DResourcesLocked() {
    m_smoothPathGeometry.Reset();
    m_headCoreBrush.Reset();
    m_outlineBrush.Reset();
    m_glowBrush.Reset();
    m_greyGlowBrush.Reset();
    m_greyLineBrush.Reset();
    m_excessiveDotBrush.Reset();
    m_excessiveBorderBrush.Reset();
    m_excessiveBgBrush.Reset();
    m_textBrush.Reset();
    m_textBorderBrush.Reset();
    m_textBgBrush.Reset();
    m_themeBgBrush.Reset();
    m_lineBrush.Reset();
    m_textFormat.Reset();
    m_textScale = 0.0f;
    m_strokeStyle.Reset();
    m_toastTarget.Reset();
    m_renderTarget.Reset();
// Factories are preserved across memory trims for zero-recreation failure
    
    if (m_memoryDC && m_oldBitmap) {
        SelectObject(m_memoryDC, m_oldBitmap);
    }
    m_oldBitmap = nullptr;
    if (m_memoryBitmap) {
        DeleteObject(m_memoryBitmap);
        m_memoryBitmap = nullptr;
    }
    m_memoryBits = nullptr;
    m_memoryPitch = 0;
    if (m_memoryDC) {
        DeleteDC(m_memoryDC);
        m_memoryDC = nullptr;
    }
    releaseToastSurfaceLocked();
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染核心与硬件合成管线
// ─────────────────────────────────────────────────────────────────────────────

void GestureTrailOverlay::drawTrailGeometry(ID2D1RenderTarget* rt,
                                           const std::vector<TrailPoint>& points,
                                           bool isRecognized,
                                           float fadeAlpha) {
    if (!rt || points.empty()) return;

    auto& cfg = easy::core::ConfigManager::instance();
    const std::string colorMode = cfg.get<std::string>("/gesture/trailColorMode", "auto");
    const std::string customHex = cfg.get<std::string>("/gesture/trailColor", "#3B82F6");
    const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
    const easy::core::AccentColorRGB themeRgb = easy::core::getAccentColorRGB(accent);
    const easy::core::AccentColorRGB trailRgb = resolveGestureTrailRgb(colorMode, customHex, themeRgb);

    const std::string theme = cfg.get<std::string>("/general/theme", "system");
    bool systemAppsUseLight = false;
    if (theme == "system") {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD value = 1;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
                systemAppsUseLight = (value != 0);
            }
            RegCloseKey(hKey);
        }
    }
    const bool isLight = gestureTrailUsesLightPalette(theme, systemAppsUseLight);

    D2D1_COLOR_F lineColor = D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.96f * fadeAlpha);
    D2D1_COLOR_F glowColor = D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.28f * fadeAlpha);
    if (!isRecognized && points.size() >= 4) {
        if (isLight) {
            lineColor = D2D1::ColorF(0.28f, 0.31f, 0.38f, 0.96f * fadeAlpha);
            glowColor = D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.28f * fadeAlpha);
        } else {
            lineColor = D2D1::ColorF(0.60f, 0.65f, 0.75f, 0.85f * fadeAlpha);
            glowColor = D2D1::ColorF(0.40f, 0.45f, 0.55f, 0.28f * fadeAlpha);
        }
    }

    ComPtr<ID2D1SolidColorBrush> lineBrush;
    ComPtr<ID2D1SolidColorBrush> glowBrush;
    ComPtr<ID2D1SolidColorBrush> outlineBrush;
    ComPtr<ID2D1SolidColorBrush> headCoreBrush;

    rt->CreateSolidColorBrush(lineColor, lineBrush.GetAddressOf());
    rt->CreateSolidColorBrush(glowColor, glowBrush.GetAddressOf());
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f * fadeAlpha), outlineBrush.GetAddressOf());
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, fadeAlpha), headCoreBrush.GetAddressOf());

    auto getPt = [&](size_t idx) -> D2D1_POINT_2F {
        return D2D1::Point2F(points[idx].x - m_originX, points[idx].y - m_originY);
    };

    if (points.size() >= 2 && m_d2dFactory) {
        ComPtr<ID2D1PathGeometry> linePath;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(linePath.GetAddressOf())) && linePath) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(linePath->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(getPt(0), D2D1_FIGURE_BEGIN_HOLLOW);
                if (points.size() == 2) {
                    sink->AddLine(getPt(1));
                } else {
                    // C1 连续二次贝塞尔中点平滑算法 (Smooth Quadratic Bezier Spline)
                    const D2D1_POINT_2F p0 = getPt(0);
                    const D2D1_POINT_2F p1 = getPt(1);
                    sink->AddLine(D2D1::Point2F((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f));
                    for (size_t i = 1; i < points.size() - 1; ++i) {
                        const D2D1_POINT_2F pi = getPt(i);
                        const D2D1_POINT_2F pi1 = getPt(i + 1);
                        const D2D1_POINT_2F mid = D2D1::Point2F((pi.x + pi1.x) * 0.5f, (pi.y + pi1.y) * 0.5f);
                        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pi, mid));
                    }
                    sink->AddLine(getPt(points.size() - 1));
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    const float coreW = (std::max)(m_style.lineWidth * m_dpiScale, 4.0f);
                    const float outlineW = clampTrailOutlineWidth(m_style.outlineWidth) * m_dpiScale;
                    const float whiteW = trailOutlineWidenWidth(coreW, outlineW);
                    if (whiteW > 0.0f && outlineBrush) {
                        rt->DrawGeometry(linePath.Get(), outlineBrush.Get(), whiteW, m_strokeStyle.Get());
                        if (glowBrush) {
                            glowBrush->SetOpacity(0.22f * fadeAlpha);
                            rt->DrawGeometry(linePath.Get(), glowBrush.Get(), coreW * 1.35f, m_strokeStyle.Get());
                        }
                    } else if (glowBrush) {
                        glowBrush->SetOpacity(0.28f * fadeAlpha);
                        rt->DrawGeometry(linePath.Get(), glowBrush.Get(), coreW * 2.4f, m_strokeStyle.Get());
                    }
                    if (lineBrush) {
                        rt->DrawGeometry(linePath.Get(), lineBrush.Get(), coreW, m_strokeStyle.Get());
                    }
                }
            }
        }

        ID2D1SolidColorBrush* activeHead = (isRecognized || points.size() < 4)
            ? headCoreBrush.Get()
            : lineBrush.Get();
        if (activeHead) {
            const float headR = (std::max)(m_style.lineWidth * m_dpiScale * 0.55f, 3.0f);
            const float outlineW = clampTrailOutlineWidth(m_style.outlineWidth) * m_dpiScale;
            if (outlineBrush && outlineW > 0.0f) {
                rt->FillEllipse(
                    D2D1::Ellipse(getPt(points.size() - 1), headR + outlineW, headR + outlineW),
                    outlineBrush.Get());
            }
            rt->FillEllipse(
                D2D1::Ellipse(getPt(points.size() - 1), headR, headR), activeHead);
        }
    }
}

void GestureTrailOverlay::drawToastContent(ID2D1RenderTarget* rt,
                                           const std::string& resultText,
                                           bool isRecognized,
                                           bool excessive,
                                           int toastW, int toastH,
                                           float toastScale,
                                           float fadeAlpha) {
    if (!rt) return;

    auto& cfg = easy::core::ConfigManager::instance();
    const std::string colorMode = cfg.get<std::string>("/gesture/trailColorMode", "auto");
    const std::string customHex = cfg.get<std::string>("/gesture/trailColor", "#3B82F6");
    const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
    const easy::core::AccentColorRGB themeRgb = easy::core::getAccentColorRGB(accent);
    const easy::core::AccentColorRGB trailRgb = resolveGestureTrailRgb(colorMode, customHex, themeRgb);

    const std::string theme = cfg.get<std::string>("/general/theme", "system");
    bool systemAppsUseLight = false;
    if (theme == "system") {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD value = 1;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
                systemAppsUseLight = (value != 0);
            }
            RegCloseKey(hKey);
        }
    }
    const bool isLight = gestureTrailUsesLightPalette(theme, systemAppsUseLight);

    const float resultScale = toastScale;
    const bool hasTextFormat = updateTextFormat(resultScale);
    const float centerX = static_cast<float>(toastW) * 0.5f;
    const float centerY = static_cast<float>(toastH) * 0.5f;

    if (excessive) {
        ComPtr<ID2D1SolidColorBrush> excBg;
        ComPtr<ID2D1SolidColorBrush> excBorder;
        ComPtr<ID2D1SolidColorBrush> excDot;

        if (isLight) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.18f, 0.24f, 0.94f * fadeAlpha), excBg.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.10f, 0.16f, 0.85f * fadeAlpha), excBorder.GetAddressOf());
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.52f, 0.08f, 0.12f, 0.92f * fadeAlpha), excBg.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.28f, 0.36f, 0.85f * fadeAlpha), excBorder.GetAddressOf());
        }
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, fadeAlpha), excDot.GetAddressOf());

        float boxW = 126.0f * resultScale;
        float boxH = 58.0f * resultScale;
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
            D2D1::RectF(centerX - boxW / 2.0f, centerY - boxH / 2.0f,
                        centerX + boxW / 2.0f, centerY + boxH / 2.0f),
            16.0f * resultScale, 16.0f * resultScale);
        if (excBg) rt->FillRoundedRectangle(&rrect, excBg.Get());
        if (excBorder) rt->DrawRoundedRectangle(&rrect, excBorder.Get(), 2.6f * resultScale);
        if (excDot) {
            const float dotRadius = 6.0f * resultScale;
            const float dotSpacing = 22.0f * resultScale;
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX - dotSpacing, centerY), dotRadius, dotRadius), excDot.Get());
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY), dotRadius, dotRadius), excDot.Get());
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX + dotSpacing, centerY), dotRadius, dotRadius), excDot.Get());
        }
    } else {
        const std::wstring wText = hasTextFormat
            ? easy::core::WinUtils::utf8ToWstring(resultText) : std::wstring{};
        if (hasTextFormat && !wText.empty() && m_dwriteFactory) {
            ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(
                wText.c_str(), static_cast<UINT32>(wText.length()),
                m_textFormat.Get(),
                10000.0f, 1000.0f,
                layout.GetAddressOf());

            float boxW = 140.0f * resultScale;
            float boxH = 58.0f * resultScale;
            if (layout) {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    float paddingX = 38.0f * resultScale;
                    float paddingY = 16.0f * resultScale;
                    boxW = (std::max)(metrics.width + paddingX * 2.0f, 136.0f * resultScale);
                    boxH = (std::max)(metrics.height + paddingY * 2.0f, 58.0f * resultScale);
                }
            }

            D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
                D2D1::RectF(centerX - boxW / 2.0f, centerY - boxH / 2.0f,
                            centerX + boxW / 2.0f, centerY + boxH / 2.0f),
                16.0f * resultScale, 16.0f * resultScale);

            ComPtr<ID2D1SolidColorBrush> bgBrush;
            ComPtr<ID2D1SolidColorBrush> borderBrush;
            ComPtr<ID2D1SolidColorBrush> textBrush;

            const bool isFading = m_fading.load(std::memory_order_relaxed);
            const bool isSuccess = (isRecognized || m_isRecognized.load(std::memory_order_relaxed));
            if (isFading && isSuccess) {
                rt->CreateSolidColorBrush(D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.95f * fadeAlpha), bgBrush.GetAddressOf());
            } else {
                rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.82f * fadeAlpha), bgBrush.GetAddressOf());
            }
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f * fadeAlpha), borderBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, fadeAlpha), textBrush.GetAddressOf());

            if (bgBrush) rt->FillRoundedRectangle(&rrect, bgBrush.Get());
            if (borderBrush) rt->DrawRoundedRectangle(&rrect, borderBrush.Get(), 2.6f * resultScale);
            if (textBrush && m_textFormat) {
                rt->DrawText(
                    wText.c_str(),
                    static_cast<UINT32>(wText.size()),
                    m_textFormat.Get(),
                    D2D1::RectF(centerX - boxW / 2.0f, centerY - boxH / 2.0f,
                                centerX + boxW / 2.0f, centerY + boxH / 2.0f),
                    textBrush.Get());
            }
        }
    }
}

bool GestureTrailOverlay::render() {
    std::lock_guard lock(m_renderMutex);
    if (!m_fading.load(std::memory_order_relaxed)) {
        m_fadeAlpha = 1.0f;
    }

    // 消费无锁 SPSC 环形队列中由输入线程推入的新轨迹点
    TrailPoint drainedPt;
    while (m_pointQueue.pop(drainedPt)) {
        if (!m_points.empty()) {
            float dx = drainedPt.x - m_points.back().x;
            float dy = drainedPt.y - m_points.back().y;
            constexpr float minimumDelta = 1.0f;
            if (dx * dx + dy * dy < minimumDelta * minimumDelta) continue;
        }
        m_points.push_back(drainedPt);
    }

    std::vector<TrailPoint> points;
    std::string resultText;
    const bool isRecognized = m_isRecognized.load(std::memory_order_relaxed);
    {
        std::lock_guard trailLock(m_trailMutex);
        if (m_points.empty()) return false;
        points = m_points;
        resultText = m_resultText;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    m_dpiScale = easy::core::dpi::scaleAtPoint(cursor);
    const HMONITOR toastMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    const RECT toastWork = easy::core::dpi::workArea(toastMonitor);
    const float toastScale = easy::core::dpi::scaleForMonitor(toastMonitor);
    const int toastCenterX = (toastWork.left + toastWork.right) / 2;
    const int toastCenterY = toastWork.top +
        static_cast<int>(static_cast<float>(toastWork.bottom - toastWork.top) * 0.82f);

    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (const auto& p : points) {
        left = (std::min)(left, static_cast<int>(p.x));
        top = (std::min)(top, static_cast<int>(p.y));
        right = (std::max)(right, static_cast<int>(p.x) + 1);
        bottom = (std::max)(bottom, static_cast<int>(p.y) + 1);
    }
    if (!fitSurface(left, top, right, bottom)) return false;

    {
        std::lock_guard trailLock(m_trailMutex);
        if (m_points.empty()) return false;
        points = m_points;
        resultText = m_resultText;
    }

    bool renderedOk = false;

    if (m_compositorReady && m_trailDcompSurface && m_dcompDevice) {
        POINT offset{};
        ComPtr<IDXGISurface> dxgiSurf;
        HRESULT hr = m_trailDcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurf.GetAddressOf()), &offset);
        if (SUCCEEDED(hr) && dxgiSurf) {
            D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ComPtr<ID2D1RenderTarget> gpuRt;
            if (SUCCEEDED(m_d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurf.Get(), rtProps, gpuRt.GetAddressOf())) && gpuRt) {
                gpuRt->BeginDraw();
                gpuRt->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
                gpuRt->Clear(D2D1::ColorF(0, 0, 0, 0));
                drawTrailGeometry(gpuRt.Get(), points, isRecognized, m_fadeAlpha);
                gpuRt->EndDraw();
            }
            m_trailDcompSurface->EndDraw();
            m_dcompDevice->Commit();

            if (!IsWindowVisible(m_hwnd)) {
                ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            }
            m_visible.store(true, std::memory_order_release);
            renderedOk = true;
        } else {
            releaseCompositorLocked();
        }
    }

    if (!renderedOk) {
        if (!m_renderTarget || !m_lineBrush) {
            if (!createD2DResources()) return false;
        }
        if (m_themeDirty.exchange(false, std::memory_order_acq_rel)) {
            applyThemeColorsLocked();
        }
        if (!m_renderTarget || !m_lineBrush || !m_memoryDC) return false;

        RECT memRect = {0, 0, m_width, m_height};
        if (FAILED(m_renderTarget->BindDC(m_memoryDC, &memRect))) return false;

        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
        drawTrailGeometry(m_renderTarget.Get(), points, isRecognized, m_fadeAlpha);
        if (FAILED(m_renderTarget->EndDraw())) {
            LOG_WARN("手势轨迹 Direct2D 帧提交失败");
            return false;
        }

        ensurePremultipliedAlpha(m_memoryBits, m_width, m_height, m_memoryPitch);

        if (!presentLayeredLocked(m_hwnd, m_memoryDC, m_originX, m_originY, m_width, m_height)) {
            return false;
        }
        m_visible.store(true, std::memory_order_release);
        renderedOk = true;
    }

    const bool isExcessive = (resultText == "•••");
    const bool shouldShowToast = shouldShowGestureResultToast(
        isRecognized, !resultText.empty(), isExcessive);
    bool toastOk = true;
    if (shouldShowToast) {
        toastOk = presentToastLocked(resultText, isRecognized, isExcessive,
                                     toastCenterX, toastCenterY, toastScale);
    } else {
        hideToastWindow();
    }

    const uint64_t epoch = m_trailEpoch.load(std::memory_order_relaxed);
    if (m_loggedPresentEpoch != epoch) {
        m_loggedPresentEpoch = epoch;
        LOG_INFO("手势轨迹已提交: points={}, {}x{}, dcomp={}", points.size(), m_width, m_height, m_compositorReady);
    }
    return gestureFrameReadyToFade(true, shouldShowToast, toastOk);
}

bool GestureTrailOverlay::presentToastLocked(const std::string& resultText, bool recognized,
                                             bool excessive, int toastCenterX, int toastCenterY,
                                             float toastScale) {
    if (!m_toastHwnd) return false;

    const int toastW = static_cast<int>(400.0f * toastScale);
    const int toastH = static_cast<int>(120.0f * toastScale);
    m_toastOriginX = toastCenterX - toastW / 2;
    m_toastOriginY = toastCenterY - toastH / 2;

    if (m_compositorReady && m_toastDcompVisual && m_dcompDevice) {
        if (!m_toastDcompSurface || m_toastDcompW != toastW || m_toastDcompH != toastH) {
            m_toastDcompSurface.Reset();
            HRESULT hr = m_dcompDevice->CreateSurface(
                static_cast<UINT>(toastW), static_cast<UINT>(toastH),
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                m_toastDcompSurface.GetAddressOf());
            if (SUCCEEDED(hr) && m_toastDcompSurface) {
                m_toastDcompVisual->SetContent(m_toastDcompSurface.Get());
                m_toastDcompW = toastW;
                m_toastDcompH = toastH;
            } else {
                return false;
            }
        }

        POINT offset{};
        ComPtr<IDXGISurface> dxgiSurf;
        HRESULT hr = m_toastDcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurf.GetAddressOf()), &offset);
        if (SUCCEEDED(hr) && dxgiSurf) {
            D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ComPtr<ID2D1RenderTarget> gpuRt;
            if (SUCCEEDED(m_d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurf.Get(), rtProps, gpuRt.GetAddressOf())) && gpuRt) {
                gpuRt->BeginDraw();
                gpuRt->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
                gpuRt->Clear(D2D1::ColorF(0, 0, 0, 0));
                drawToastContent(gpuRt.Get(), resultText, recognized, excessive, toastW, toastH, toastScale, m_fadeAlpha);
                gpuRt->EndDraw();
            }
            m_toastDcompSurface->EndDraw();
            m_dcompDevice->Commit();

            const bool yielded = m_zOrderYielded.load(std::memory_order_acquire);
            SetWindowPos(m_toastHwnd, yielded ? HWND_BOTTOM : HWND_TOPMOST, m_toastOriginX, m_toastOriginY, toastW, toastH,
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            if (!IsWindowVisible(m_toastHwnd)) {
                ShowWindow(m_toastHwnd, SW_SHOWNOACTIVATE);
            }
            return true;
        }
    }

    if (!ensureToastTargetLocked() || !m_toastTarget) return false;
    if (!ensureToastSurfaceLocked(toastW, toastH)) return false;

    RECT toastRect = {0, 0, toastW, toastH};
    if (FAILED(m_toastTarget->BindDC(m_toastDC, &toastRect))) return false;

    m_toastTarget->BeginDraw();
    m_toastTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
    drawToastContent(m_toastTarget.Get(), resultText, recognized, excessive, toastW, toastH, toastScale, m_fadeAlpha);
    if (FAILED(m_toastTarget->EndDraw())) {
        LOG_WARN("手势结果卡片 Direct2D 帧提交失败");
        return false;
    }

    ensurePremultipliedAlpha(m_toastBits, m_toastWidth, m_toastHeight, m_toastPitch);

    const bool yielded = m_zOrderYielded.load(std::memory_order_acquire);
    SetWindowPos(m_toastHwnd, yielded ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return presentLayeredLocked(
        m_toastHwnd, m_toastDC, m_toastOriginX, m_toastOriginY, m_toastWidth, m_toastHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK GestureTrailOverlay::overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<GestureTrailOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {

        case WM_GETOBJECT:
            if (self && hwnd == self->m_toastHwnd) {
                return easy::core::accessibility::respondToOverlayUiaGetObject(
                    hwnd, wParam, lParam,
                    {L"EasyTools.GestureResult", L"Recognized mouse gesture result",
                     easy::core::accessibility::OverlayUiaRole::Text, true});
            }
            return easy::core::accessibility::respondToOverlayUiaGetObject(
                hwnd, wParam, lParam,
                {L"EasyTools.GestureTrail", L"Mouse gesture trail",
                 easy::core::accessibility::OverlayUiaRole::Pane, false});

        case WM_GESTURE_ACCESSIBILITY_RESULT:
            if (self && hwnd == self->m_toastHwnd) {
                std::string result;
                {
                    std::lock_guard lock(self->m_trailMutex);
                    result = self->m_resultText;
                }
                if (!result.empty()) {
                    easy::core::accessibility::announceOverlay(
                        hwnd, easy::core::WinUtils::utf8ToWstring(result));
                }
            }
            return 0;

        case WM_DISPLAYCHANGE: {
            if (self) {
                self->m_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                self->m_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                self->m_virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                self->m_virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            }
            return 0;
        }

        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_NCDESTROY:
            easy::core::accessibility::disconnectOverlayUiaProvider(hwnd);
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace easy::gesture
