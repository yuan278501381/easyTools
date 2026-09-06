#ifndef EASYTOOLS_CORE_HOTKEY_MOUSEHOOK_H
#define EASYTOOLS_CORE_HOTKEY_MOUSEHOOK_H

#include "core/utils/Export.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace easy::core {

/// 鼠标底层拦截回调，返回 true 表示彻底拦截该事件，不向下层分发
using MouseHookRawCallback = std::function<bool(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data)>;

class EASYCORE_API MouseHook {
public:
    static MouseHook& instance();

    bool install();
    void uninstall();

    void setPaused(bool paused);
    bool isPaused() const;

    void setMouseActivityCallback(std::function<void(int button, long x, long y)> cb);
    void setInterceptor(MouseHookRawCallback interceptor);

    bool isInstalled() const;

    /// 单元测试注入接口，用于验证底层拦截器和钩子回调管线
    bool injectRawEventForTesting(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data);

private:
    MouseHook() = default;
    ~MouseHook();

    static LRESULT CALLBACK lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void inputThreadWorker(HANDLE readyEvent);

    std::atomic<HHOOK> m_hookHandle{nullptr};
    std::atomic<DWORD> m_threadId{0};
    std::jthread m_inputThread;
    std::atomic<bool> m_paused{false};
    std::function<void(int button, long x, long y)> m_activityCallback;
    MouseHookRawCallback m_interceptor;
    mutable std::mutex m_callbackMutex;
};

} // namespace easy::core

#endif // EASYTOOLS_CORE_HOTKEY_MOUSEHOOK_H
