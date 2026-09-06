#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TrayWindow.h — WebView2 托盘无边框悬浮菜单
// ─────────────────────────────────────────────────────────────────────────────

#ifndef EASYTOOLS_UI_TRAYWINDOW_H
#define EASYTOOLS_UI_TRAYWINDOW_H

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <wrl/client.h>
#include <WebView2.h>

namespace easy::ui {

class TrayWindow {
public:
    static TrayWindow& instance();

    /// 预热托盘菜单 WebView2 渲染环境（后台静默就绪，使用户右键时 0 毫秒瞬间呼出）
    void preload(HINSTANCE hInstance);

    /// 显示托盘菜单，并将其定位到指定的坐标附近 (通常是鼠标点击系统托盘的位置)
    void show(HINSTANCE hInstance, int x, int y);

    /// 动态设置托盘菜单内容实际尺寸
    void setContentSize(int width, int height);

    /// 隐藏
    void hide();

    /// 是否可见
    bool isVisible() const;

    /// 销毁
    void destroy();

    /// 查询 WebView2 渲染环境是否已就绪
    bool isWebViewReady() const { return m_webViewReady.load(); }

    /// 设置 WebView2 就绪状态（供生命周期及测试桩注入使用）
    void setWebViewReady(bool ready) { m_webViewReady.store(ready); }

    /// 判断指定窗口是否属于任务栏或托盘溢出窗口白名单 (Shell_TrayWnd, Shell_SecondaryTrayWnd, NotifyIconOverflowWindow, TopLevelWindowForOverflowXamlIsland)
    static inline bool isTaskbarOrOverflowWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        auto matchClass = [](HWND h) {
            wchar_t cls[128] = {};
            if (GetClassNameW(h, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) == 0) return false;
            return _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
                   _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                   _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0 ||
                   _wcsicmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0;
        };
        if (matchClass(hwnd)) return true;
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && root != hwnd && matchClass(root)) return true;
        return false;
    }

private:
    TrayWindow() = default;
    ~TrayWindow() { destroy(); }
    TrayWindow(const TrayWindow&) = delete;
    TrayWindow& operator=(const TrayWindow&) = delete;

    bool createWindow(HINSTANCE hInstance, int x, int y);
    void initializeWebView2();
    void updatePlacement();
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_webViewReady{false};
    std::atomic<uint64_t> m_generation{0};
    uint64_t m_showTimeTick{0};
    POINT m_anchor{};
    bool m_updatingPlacement = false;
    int m_contentWidth = 0;
    int m_contentHeight = 0;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
};

} // namespace easy::ui

#endif // EASYTOOLS_UI_TRAYWINDOW_H
