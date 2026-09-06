#include "core/hotkey/MouseHook.h"
#include "core/events/EventBus.h"
#include "core/logger/Logger.h"

namespace easy::core {

MouseHook& MouseHook::instance() {
    static MouseHook inst;
    return inst;
}

MouseHook::~MouseHook() {
    uninstall();
}

bool MouseHook::isPaused() const {
    return m_paused.load(std::memory_order_relaxed);
}

bool MouseHook::isInstalled() const {
    return m_hookHandle.load(std::memory_order_relaxed) != nullptr;
}

bool MouseHook::install() {
    if (isInstalled()) return true;

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("创建输入线程就绪事件失败, error={}", GetLastError());
        return false;
    }

    m_inputThread = std::jthread([this, readyEvent]() {
        inputThreadWorker(readyEvent);
    });

    WaitForSingleObject(readyEvent, 3000);
    CloseHandle(readyEvent);

    if (!isInstalled()) {
        LOG_ERROR("安装全局独立高优先级鼠标钩子失败");
        uninstall();
        return false;
    }

    LOG_INFO("全局独立高优先级输入线程已启动，鼠标钩子安装成功 (THREAD_PRIORITY_HIGHEST)");
    return true;
}

void MouseHook::inputThreadWorker(HANDLE readyEvent) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

    HHOOK hook = SetWindowsHookExW(
        WH_MOUSE_LL,
        lowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0
    );

    if (hook) {
        m_hookHandle.store(hook, std::memory_order_release);
    } else {
        LOG_ERROR("SetWindowsHookExW(WH_MOUSE_LL) 失败, error={}", GetLastError());
    }

    SetEvent(readyEvent);

    if (!hook) {
        m_threadId.store(0, std::memory_order_release);
        return;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (HHOOK h = m_hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
        UnhookWindowsHookEx(h);
    }
    m_threadId.store(0, std::memory_order_release);
}

void MouseHook::uninstall() {
    DWORD tid = m_threadId.exchange(0, std::memory_order_acq_rel);
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        if (m_inputThread.joinable()) {
            m_inputThread.join();
        }
    }

    if (HHOOK h = m_hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
        UnhookWindowsHookEx(h);
        LOG_INFO("全局独立输入线程鼠标钩子已卸载");
    }
}

void MouseHook::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
}

void MouseHook::setMouseActivityCallback(std::function<void(int, long, long)> cb) {
    std::lock_guard lock(m_callbackMutex);
    m_activityCallback = std::move(cb);
}

void MouseHook::setInterceptor(MouseHookRawCallback interceptor) {
    std::lock_guard lock(m_callbackMutex);
    m_interceptor = std::move(interceptor);
}

LRESULT CALLBACK MouseHook::lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    auto& self = MouseHook::instance();

    if (nCode == HC_ACTION && !self.m_paused.load(std::memory_order_relaxed)) {
        auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (data) {
            // 1. 优先调用上层拦截器 (如鼠标手势引擎识别管线)
            MouseHookRawCallback interceptor;
            {
                std::lock_guard lock(self.m_callbackMutex);
                interceptor = self.m_interceptor;
            }
            if (interceptor) {
                try {
                    if (interceptor(nCode, wParam, *data)) {
                        return 1; // 彻底拦截，不传递给下层
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("MouseHook 拦截器异常: {}", e.what());
                } catch (...) {
                    LOG_ERROR("MouseHook 拦截器未知异常");
                }
            }

            // 2. 仅在按键按下时分发活动通知 (杜绝 1000Hz 移动时无脑广播竞争互斥锁)
            int button = -1;
            switch (wParam) {
                case WM_LBUTTONDOWN: button = 0; break;
                case WM_RBUTTONDOWN: button = 1; break;
                case WM_MBUTTONDOWN: button = 2; break;
                default: break;
            }

            if (button != -1) {
                try {
                    std::function<void(int, long, long)> cb;
                    {
                        std::lock_guard lock(self.m_callbackMutex);
                        cb = self.m_activityCallback;
                    }
                    if (cb) {
                        cb(button, data->pt.x, data->pt.y);
                    }
                    EventBus::instance().publish(MouseActivityEvent{button, data->pt.x, data->pt.y});
                } catch (const std::exception& e) {
                    LOG_ERROR("MouseHook 活动事件分发异常: {}", e.what());
                } catch (...) {
                    LOG_ERROR("MouseHook 活动事件分发未知异常");
                }
            }
        }
    }

    HHOOK h = self.m_hookHandle.load(std::memory_order_relaxed);
    return CallNextHookEx(h, nCode, wParam, lParam);
}

bool MouseHook::injectRawEventForTesting(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data) {
    return lowLevelMouseProc(nCode, wParam, reinterpret_cast<LPARAM>(&data)) != 0;
}

} // namespace easy::core
