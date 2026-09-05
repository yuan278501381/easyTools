#include "core/remote/RemoteMasterEngine.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/events/EventBus.h"
#include "core/utils/WinUtils.h"

#include <imm.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>

#pragma comment(lib, "imm32.lib")

namespace easy::core {

namespace {

std::string toLowerAscii(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

std::wstring toLowerWide(std::wstring_view s) {
    std::wstring result;
    result.reserve(s.size());
    for (wchar_t c : s) {
        result.push_back(static_cast<wchar_t>(::towlower(c)));
    }
    return result;
}

std::string wstringToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

} // namespace

nlohmann::json RemoteMasterSettings::toJson() const {
    return {
        {"enabled", enabled},
        {"hotkeyTunnelEnabled", hotkeyTunnelEnabled},
        {"emergencyFlushEnabled", emergencyFlushEnabled},
        {"doubleRightCtrlTrigger", doubleRightCtrlTrigger},
        {"imeSanitizerEnabled", imeSanitizerEnabled},
        {"emergencyShortcut", emergencyShortcut},
        {"targetProcesses", targetProcesses},
        {"targetClasses", targetClasses}
    };
}

RemoteMasterSettings RemoteMasterSettings::fromJson(const nlohmann::json& j) {
    RemoteMasterSettings s;
    if (j.contains("enabled") && j["enabled"].is_boolean()) s.enabled = j["enabled"].get<bool>();
    if (j.contains("hotkeyTunnelEnabled") && j["hotkeyTunnelEnabled"].is_boolean()) s.hotkeyTunnelEnabled = j["hotkeyTunnelEnabled"].get<bool>();
    if (j.contains("emergencyFlushEnabled") && j["emergencyFlushEnabled"].is_boolean()) s.emergencyFlushEnabled = j["emergencyFlushEnabled"].get<bool>();
    if (j.contains("doubleRightCtrlTrigger") && j["doubleRightCtrlTrigger"].is_boolean()) s.doubleRightCtrlTrigger = j["doubleRightCtrlTrigger"].get<bool>();
    if (j.contains("imeSanitizerEnabled") && j["imeSanitizerEnabled"].is_boolean()) s.imeSanitizerEnabled = j["imeSanitizerEnabled"].get<bool>();
    if (j.contains("emergencyShortcut") && j["emergencyShortcut"].is_string()) s.emergencyShortcut = j["emergencyShortcut"].get<std::string>();
    if (j.contains("targetProcesses") && j["targetProcesses"].is_array()) {
        s.targetProcesses.clear();
        for (const auto& item : j["targetProcesses"]) {
            if (item.is_string()) s.targetProcesses.push_back(item.get<std::string>());
        }
    }
    if (j.contains("targetClasses") && j["targetClasses"].is_array()) {
        s.targetClasses.clear();
        for (const auto& item : j["targetClasses"]) {
            if (item.is_string()) s.targetClasses.push_back(item.get<std::string>());
        }
    }
    return s;
}

RemoteMasterEngine& RemoteMasterEngine::instance() {
    static RemoteMasterEngine inst;
    return inst;
}

bool RemoteMasterEngine::initialize() {
    std::lock_guard lock(m_mutex);
    loadSettings();

    // 注册 Windows 前台窗口变更通知钩子 (异步解耦，不阻塞用户消息)
    if (!m_foregroundHook) {
        m_foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr,
            &RemoteMasterEngine::winEventProc,
            0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );
        if (!m_foregroundHook) {
            LOG_WARN("[RemoteMaster] Failed to install EVENT_SYSTEM_FOREGROUND hook, fallback to polling");
        }
    }

    // 立即侦测当前前台窗口状态
    HWND curFg = GetForegroundWindow();
    if (curFg) {
        std::string procName;
        bool isRemote = checkWindowIsRemote(curFg, &procName);
        m_isRemoteForeground.store(isRemote, std::memory_order_relaxed);
        m_activeRemoteHwnd.store(isRemote ? curFg : nullptr, std::memory_order_relaxed);
        m_activeRemoteProcess = isRemote ? procName : "";
        if (isRemote) {
            onRemoteForegroundGained(curFg);
        }
    }

    LOG_INFO("[RemoteMaster] Initialized successfully");
    return true;
}

void RemoteMasterEngine::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_foregroundHook) {
            UnhookWinEvent(m_foregroundHook);
            m_foregroundHook = nullptr;
        }
        if (m_imeSanitized.load(std::memory_order_relaxed)) {
            onRemoteForegroundLost(m_activeRemoteHwnd.load(std::memory_order_relaxed));
        }
        m_isRemoteForeground.store(false, std::memory_order_relaxed);
        m_activeRemoteHwnd.store(nullptr, std::memory_order_relaxed);
        m_activeRemoteProcess.clear();
        m_cachedForegroundHwnd = nullptr;
        m_cachedIsRemote = false;
        m_cachedProcessName.clear();
    }
    // 冷路径退场释放内存
    easy::core::WinUtils::trimWorkingSet();
    LOG_INFO("[RemoteMaster] Shutdown completed and working set trimmed");
}

RemoteMasterSettings RemoteMasterEngine::getSettings() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void RemoteMasterEngine::updateSettings(const RemoteMasterSettings& settings) {
    {
        std::lock_guard lock(m_mutex);
        m_settings = settings;
        updateParsedEmergencyShortcut();
        saveSettings();
    }
    LOG_INFO("[RemoteMaster] Settings updated");
}

void RemoteMasterEngine::resetDefaults() {
    {
        std::lock_guard lock(m_mutex);
        m_settings = RemoteMasterSettings{};
        updateParsedEmergencyShortcut();
        saveSettings();
    }
    if (m_imeSanitized.load(std::memory_order_relaxed)) {
        onRemoteForegroundLost(m_activeRemoteHwnd.load(std::memory_order_relaxed));
    }
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);
    LOG_INFO("[RemoteMaster] Settings reset to defaults");
}

void RemoteMasterEngine::loadSettings() {
    auto j = ConfigManager::instance().get<nlohmann::json>("/remote_boost", nlohmann::json::object());
    if (j.is_object() && !j.empty()) {
        m_settings = RemoteMasterSettings::fromJson(j);
    }
    updateParsedEmergencyShortcut();
}

void RemoteMasterEngine::saveSettings() {
    ConfigManager::instance().set<nlohmann::json>("/remote_boost", m_settings.toJson());
}

void RemoteMasterEngine::updateParsedEmergencyShortcut() {
    m_parsedEmergencyShortcut = parseEmergencyShortcut(m_settings.emergencyShortcut);
}

ParsedEmergencyShortcut RemoteMasterEngine::getParsedEmergencyShortcut() const {
    std::lock_guard lock(m_mutex);
    return m_parsedEmergencyShortcut;
}

bool RemoteMasterEngine::isRemoteForeground() const {
    if (m_mockActive) return m_mockIsRemote;
    return m_isRemoteForeground.load(std::memory_order_relaxed);
}

HWND RemoteMasterEngine::getActiveRemoteHwnd() const {
    if (m_mockActive) return m_mockHwnd;
    return m_activeRemoteHwnd.load(std::memory_order_relaxed);
}

std::string RemoteMasterEngine::getActiveRemoteProcess() const {
    if (m_mockActive) return m_mockProcessName;
    std::lock_guard lock(m_mutex);
    return m_activeRemoteProcess;
}

bool RemoteMasterEngine::isImeSanitized() const {
    return m_imeSanitized.load(std::memory_order_relaxed);
}

HKL RemoteMasterEngine::getSavedHkl() const {
    std::lock_guard lock(m_mutex);
    return m_savedHkl;
}

void RemoteMasterEngine::setMockForeground(HWND hwnd, bool isRemote, const std::string& processName) {
    m_mockActive = true;
    m_mockHwnd = hwnd;
    m_mockIsRemote = isRemote;
    m_mockProcessName = processName;
    if (isRemote) {
        onRemoteForegroundGained(hwnd);
    } else {
        onRemoteForegroundLost(hwnd);
    }
}

void RemoteMasterEngine::clearMockForeground() {
    if (m_mockActive && m_mockIsRemote) {
        onRemoteForegroundLost(m_mockHwnd);
    }
    m_mockActive = false;
    m_mockHwnd = nullptr;
    m_mockIsRemote = false;
    m_mockProcessName.clear();
}

bool RemoteMasterEngine::isRemoteProcessOrClass(
    const std::wstring& processName,
    const std::wstring& className,
    const std::vector<std::string>& targetProcesses,
    const std::vector<std::string>& targetClasses
) {
    if (processName.empty() && className.empty()) return false;

    // 提取纯文件名（去除路径）并小写化
    std::wstring pLower = toLowerWide(processName);
    size_t lastSlash = pLower.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        pLower = pLower.substr(lastSlash + 1);
    }

    std::wstring cLower = toLowerWide(className);

    // 匹配进程名
    for (const auto& tp : targetProcesses) {
        if (tp.empty()) continue;
        std::string tpLower = toLowerAscii(tp);
        std::wstring tpWLower(tpLower.begin(), tpLower.end());
        if (pLower == tpWLower || (tpWLower.find(L'.') == std::wstring::npos && pLower == tpWLower + L".exe")) {
            return true;
        }
    }

    // 匹配类名 (支持子串包含与精确匹配)
    for (const auto& tc : targetClasses) {
        if (tc.empty()) continue;
        std::string tcLower = toLowerAscii(tc);
        std::wstring tcWLower(tcLower.begin(), tcLower.end());
        if (cLower == tcWLower || cLower.find(tcWLower) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

std::vector<INPUT> RemoteMasterEngine::buildEmergencyFlushInputs() {
    struct ModDef {
        WORD vk;
        WORD scanCode;
        bool isExtended;
    };

    static const ModDef kModifiers[8] = {
        { VK_LCONTROL, 0x1D, false },
        { VK_RCONTROL, 0x1D, true  },
        { VK_LMENU,    0x38, false },
        { VK_RMENU,    0x38, true  },
        { VK_LSHIFT,   0x2A, false },
        { VK_RSHIFT,   0x36, false },
        { VK_LWIN,     0x5B, true  },
        { VK_RWIN,     0x5C, true  }
    };

    std::vector<INPUT> inputs;
    inputs.reserve(11);

    // 8 组修饰键 KEYUP
    for (const auto& mod : kModifiers) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = mod.vk;
        inp.ki.wScan = mod.scanCode;
        inp.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE | (mod.isExtended ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(inp);
    }

    // 3 组鼠标键释放
    {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        inputs.push_back(inp);
    }
    {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        inputs.push_back(inp);
    }
    {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        inputs.push_back(inp);
    }

    return inputs;
}

ParsedEmergencyShortcut RemoteMasterEngine::parseEmergencyShortcut(const std::string& str) {
    ParsedEmergencyShortcut res;
    if (str.empty()) return res;

    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= str.size()) {
        const size_t end = str.find('+', start);
        tokens.push_back(str.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (tokens.empty() || tokens.back().empty()) return res;

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        std::string token = toLowerAscii(tokens[i]);
        if (token == "ctrl" || token == "control") res.ctrl = true;
        else if (token == "alt") res.alt = true;
        else if (token == "shift") res.shift = true;
        else if (token == "win" || token == "meta") res.win = true;
        else return res;
    }

    std::string keyToken = toLowerAscii(tokens.back());
    static const std::unordered_map<std::string, UINT> kSpecialKeyMap = {
        {"backspace", VK_BACK}, {"back", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
        {"space", VK_SPACE}, {"tab", VK_TAB},
        {"return", VK_RETURN}, {"enter", VK_RETURN},
        {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT},
        {"insert", VK_INSERT}, {"ins", VK_INSERT},
        {"pause", VK_PAUSE}, {"break", VK_PAUSE},
        {"printscreen", VK_SNAPSHOT},
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"f13", VK_F13}, {"f14", VK_F14}, {"f15", VK_F15}, {"f16", VK_F16},
        {"f17", VK_F17}, {"f18", VK_F18}, {"f19", VK_F19}, {"f20", VK_F20},
        {"f21", VK_F21}, {"f22", VK_F22}, {"f23", VK_F23}, {"f24", VK_F24},
    };

    auto it = kSpecialKeyMap.find(keyToken);
    if (it != kSpecialKeyMap.end()) {
        res.vk = it->second;
        res.valid = true;
    } else if (keyToken.size() == 1 && std::isalnum(static_cast<unsigned char>(keyToken[0]))) {
        res.vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(keyToken[0])));
        res.valid = true;
    } else if (keyToken.starts_with("0x")) {
        try {
            unsigned long val = std::stoul(keyToken.substr(2), nullptr, 16);
            if (val > 0 && val <= 0xFF) {
                res.vk = static_cast<UINT>(val);
                res.valid = true;
            }
        } catch (...) {}
    }

    return res;
}

LPARAM RemoteMasterEngine::makeKeyLParam(
    DWORD scanCode,
    bool isExtended,
    bool isKeyUp,
    bool isAltDown,
    bool isRepeat
) {
    LPARAM lp = 1; // repeat count = 1
    lp |= ((scanCode & 0xFF) << 16);
    if (isExtended) lp |= (1 << 24);
    if (isAltDown)  lp |= (1 << 29);
    if (isRepeat)   lp |= (1 << 30);
    if (isKeyUp) {
        lp |= (1 << 30); // previous key state was down
        lp |= (1 << 31); // transition state (being released)
    }
    return lp;
}

bool RemoteMasterEngine::checkWindowIsRemote(HWND hwnd, std::string* outProcessName) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    // 快速命中缓存
    {
        std::lock_guard lock(m_mutex);
        if (hwnd == m_cachedForegroundHwnd) {
            if (outProcessName) *outProcessName = m_cachedProcessName;
            return m_cachedIsRemote;
        }
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return false;

    std::wstring processImage;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        wchar_t pathBuf[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, pathBuf, &size)) {
            processImage = pathBuf;
        }
        CloseHandle(hProcess);
    }

    wchar_t classBuf[256] = {0};
    GetClassNameW(hwnd, classBuf, 256);

    std::vector<std::string> targets;
    std::vector<std::string> classes;
    {
        std::lock_guard lock(m_mutex);
        targets = m_settings.targetProcesses;
        classes = m_settings.targetClasses;
    }

    bool matched = isRemoteProcessOrClass(processImage, classBuf, targets, classes);

    // 提取纯进程名用于显示
    std::string procUtf8;
    if (!processImage.empty()) {
        std::wstring pOnly = processImage;
        size_t slash = pOnly.find_last_of(L"\\/");
        if (slash != std::wstring::npos) pOnly = pOnly.substr(slash + 1);
        procUtf8 = wstringToUtf8(pOnly);
    }

    {
        std::lock_guard lock(m_mutex);
        m_cachedForegroundHwnd = hwnd;
        m_cachedIsRemote = matched;
        m_cachedProcessName = procUtf8;
    }

    if (outProcessName) *outProcessName = procUtf8;
    return matched;
}

void RemoteMasterEngine::onRemoteForegroundGained(HWND hwnd) {
    // 复位输入热键与连按状态机，确保进入远控前台时干净无状态污染
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    if (!m_settings.imeSanitizerEnabled) return;
    if (m_imeSanitized.load(std::memory_order_relaxed)) return;

    DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
    if (threadId) {
        m_savedHkl = GetKeyboardLayout(threadId);
    }
    if (!m_savedHkl) {
        m_savedHkl = GetKeyboardLayout(0);
    }

    // 切换至纯美式英文 (00000409)
    HKL hklEng = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE);
    if (hklEng) {
        ActivateKeyboardLayout(hklEng, KLF_SETFORPROCESS);
    }
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hklEng));

        // 跨进程通知输入法窗口与宿主窗口强制关闭候选状态 (IMC_SETOPENSTATUS = 0)
        HWND imeWnd = ImmGetDefaultIMEWnd(hwnd);
        if (imeWnd) {
            SendMessageTimeoutW(imeWnd, WM_IME_CONTROL, 0x0006 /* IMC_SETOPENSTATUS */, 0, SMTO_ABORTIFHUNG, 50, nullptr);
        }
        SendMessageTimeoutW(hwnd, WM_IME_CONTROL, 0x0006 /* IMC_SETOPENSTATUS */, 0, SMTO_ABORTIFHUNG, 50, nullptr);
    }
    m_imeSanitized.store(true, std::memory_order_relaxed);
    LOG_INFO("[RemoteMaster] IME Sanitizer: Switched input layout to pure US English (0409)");
}

void RemoteMasterEngine::onRemoteForegroundLost(HWND hwnd) {
    // 关键状态重置：离开远控前台时，必须无条件复位 Win 键与右 Ctrl 状态机，防止修饰键滞留粘滞
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    if (!m_imeSanitized.load(std::memory_order_relaxed)) return;

    if (m_savedHkl) {
        ActivateKeyboardLayout(m_savedHkl, KLF_SETFORPROCESS);
        if (hwnd && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(m_savedHkl));
        }
        m_savedHkl = nullptr;
    }
    m_imeSanitized.store(false, std::memory_order_relaxed);
    LOG_INFO("[RemoteMaster] IME Sanitizer: Restored previous input layout");
}

void RemoteMasterEngine::postKeyToTarget(HWND fg, UINT msg, DWORD vk, DWORD scanCode, bool isExtended, bool isAltDown) {
    if (!fg) return;

    HWND target = fg;
    DWORD threadId = GetWindowThreadProcessId(fg, nullptr);
    if (threadId) {
        GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
        if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus) {
            target = gti.hwndFocus;
        }
    }

    DWORD sc = scanCode;
    if (sc == 0) {
        sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }

    LPARAM lParam = makeKeyLParam(sc, isExtended, (msg == WM_KEYUP || msg == WM_SYSKEYUP), isAltDown);
    PostMessageW(target, msg, vk, lParam);
}

void RemoteMasterEngine::flushModifiers(HWND targetHwnd) {
    // 1. 原子化 SendInput 发送 11 组 KEYUP 与 MOUSEUP 硬件脉冲
    auto inputs = buildEmergencyFlushInputs();
    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }

    // 2. 向前台远控画布精准直投 8 组修饰键的 WM_KEYUP / WM_SYSKEYUP
    HWND target = targetHwnd ? targetHwnd : m_activeRemoteHwnd.load(std::memory_order_relaxed);
    if (!target) target = GetForegroundWindow();

    if (target && IsWindow(target)) {
        HWND canvas = target;
        DWORD threadId = GetWindowThreadProcessId(target, nullptr);
        if (threadId) {
            GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
            if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus) {
                canvas = gti.hwndFocus;
            }
        }

        static const struct {
            UINT msg;
            DWORD vk;
            DWORD scan;
            bool ext;
            bool alt;
        } kDirectMsgs[8] = {
            { WM_KEYUP,    VK_LCONTROL, 0x1D, false, false },
            { WM_KEYUP,    VK_RCONTROL, 0x1D, true,  false },
            { WM_SYSKEYUP, VK_LMENU,    0x38, false, true  },
            { WM_SYSKEYUP, VK_RMENU,    0x38, true,  true  },
            { WM_KEYUP,    VK_LSHIFT,   0x2A, false, false },
            { WM_KEYUP,    VK_RSHIFT,   0x36, false, false },
            { WM_KEYUP,    VK_LWIN,     0x5B, true,  false },
            { WM_KEYUP,    VK_RWIN,     0x5C, true,  false }
        };

        for (const auto& dm : kDirectMsgs) {
            LPARAM lp = makeKeyLParam(dm.scan, dm.ext, true, dm.alt);
            PostMessageW(canvas, dm.msg, dm.vk, lp);
            if (canvas != target) {
                PostMessageW(target, dm.msg, dm.vk, lp);
            }
        }

        // 鼠标按键弹起直投
        PostMessageW(canvas, WM_LBUTTONUP, 0, 0);
        PostMessageW(canvas, WM_RBUTTONUP, 0, 0);
        PostMessageW(canvas, WM_MBUTTONUP, 0, 0);
        if (canvas != target) {
            PostMessageW(target, WM_LBUTTONUP, 0, 0);
            PostMessageW(target, WM_RBUTTONUP, 0, 0);
            PostMessageW(target, WM_MBUTTONUP, 0, 0);
        }
    }

    // 3. 复位状态机
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    // 4. 触发全局轻量通知反馈
    EventBus::instance().publish(ShowToastEvent{ L"远程修饰键与鼠标按键已急救释放" });
    LOG_INFO("[RemoteMaster] Emergency flush executed: 8 modifier keys and 3 mouse buttons released");
}

bool RemoteMasterEngine::onLowLevelKeyboardEvent(const KBDLLHOOKSTRUCT& data, WPARAM wParam) {
    if (!m_settings.enabled) return false;

    DWORD vk = data.vkCode;
    bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    // ── 引擎 2: 远程修饰键卡死急救 (快捷热键触发) ──────────────────────────
    if (m_settings.emergencyFlushEnabled) {
        // 双击右 Ctrl 触发急救 (400ms 内连按两次释放，兼容不同键盘驱动模式)
        if (m_settings.doubleRightCtrlTrigger) {
            bool isRightCtrl = (vk == VK_RCONTROL) ||
                               (vk == VK_CONTROL && ((data.flags & LLKHF_EXTENDED) != 0));
            if (isRightCtrl) {
                if (isKeyUp) {
                    ULONGLONG now = GetTickCount64();
                    ULONGLONG last = m_lastRightCtrlUpTime.load(std::memory_order_relaxed);
                    bool otherKey = m_otherKeyPressedSinceRightCtrl.load(std::memory_order_relaxed);

                    if (last > 0 && (now - last) <= 400 && !otherKey) {
                        flushModifiers();
                        m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
                        m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);
                        return true; // 拦截并消费第二次弹起的 Right Ctrl
                    } else {
                        m_lastRightCtrlUpTime.store(now, std::memory_order_relaxed);
                        m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);
                    }
                }
            } else if (isKeyDown) {
                m_otherKeyPressedSinceRightCtrl.store(true, std::memory_order_relaxed);
            }
        }

        // 专用急救快捷键 (支持完全动态配置，默认 Ctrl+Alt+Backspace)
        ParsedEmergencyShortcut targetShortcut;
        {
            std::lock_guard lock(m_mutex);
            targetShortcut = m_parsedEmergencyShortcut;
        }
        if (targetShortcut.valid && isKeyDown && vk == targetShortcut.vk) {
            bool hasCtrl = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                           (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL);
            bool hasAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
                          ((data.flags & LLKHF_ALTDOWN) != 0) ||
                          (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
            bool hasShift = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) ||
                            (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT);
            bool hasWin = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                          ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) ||
                          m_winKeyDown.load(std::memory_order_relaxed) ||
                          (vk == VK_LWIN || vk == VK_RWIN);

            if (hasCtrl == targetShortcut.ctrl &&
                hasAlt == targetShortcut.alt &&
                hasShift == targetShortcut.shift &&
                hasWin == targetShortcut.win) {
                flushModifiers();
                return true; // 拦截并消费急救按键
            }
        }
    }

    // ── 引擎 1: 沉浸式系统热键直通 (Immersive Remote Hotkey Tunnel) ────────
    if (!m_settings.hotkeyTunnelEnabled) return false;

    HWND fg = m_mockActive ? m_mockHwnd : GetForegroundWindow();
    if (!fg) return false;

    bool isRemote = m_mockActive ? m_mockIsRemote : checkWindowIsRemote(fg);
    if (!isRemote) return false;

    // 1. Win 键单独按下与弹起 (VK_LWIN / VK_RWIN)
    if (vk == VK_LWIN || vk == VK_RWIN) {
        if (isKeyDown) {
            m_winKeyDown.store(true, std::memory_order_relaxed);
            postKeyToTarget(fg, WM_KEYDOWN, vk, data.scanCode, true);
            return true; // 拦截，防止弹出本地开始菜单
        } else if (isKeyUp) {
            m_winKeyDown.store(false, std::memory_order_relaxed);
            postKeyToTarget(fg, WM_KEYUP, vk, data.scanCode, true);
            return true; // 拦截，防止弹出本地开始菜单
        }
    }

    // 2. Win 键按住期间的组合快捷键 (Win+R, Win+E, Win+D, Win+X, Win+A, Win+S, Win+1..9 等)
    bool hasWin = m_winKeyDown.load(std::memory_order_relaxed) ||
                  ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                  ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

    if (hasWin) {
        if (isKeyDown) {
            postKeyToTarget(fg, WM_KEYDOWN, vk, data.scanCode, (data.flags & LLKHF_EXTENDED) != 0);
            return true; // 拦截本地运行、资源管理器、显示桌面等弹出
        } else if (isKeyUp) {
            postKeyToTarget(fg, WM_KEYUP, vk, data.scanCode, (data.flags & LLKHF_EXTENDED) != 0);
            return true;
        }
    }

    // 3. Alt + Tab 直通远端
    if (vk == VK_TAB) {
        bool hasAlt = ((data.flags & LLKHF_ALTDOWN) != 0) ||
                      ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
                      (wParam == WM_SYSKEYDOWN) || (wParam == WM_SYSKEYUP);
        if (hasAlt) {
            if (isKeyDown) {
                postKeyToTarget(fg, WM_SYSKEYDOWN, VK_TAB, data.scanCode, false, true);
                return true; // 拦截本地任务切换器
            } else if (isKeyUp) {
                postKeyToTarget(fg, WM_SYSKEYUP, VK_TAB, data.scanCode, false, true);
                return true;
            }
        }
    }

    // 4. Ctrl + Shift + Esc 直通远端任务管理器
    if (vk == VK_ESCAPE) {
        bool hasCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (hasCtrl && hasShift) {
            if (isKeyDown) {
                postKeyToTarget(fg, WM_KEYDOWN, VK_ESCAPE, data.scanCode, false);
                return true; // 拦截本地任务管理器
            } else if (isKeyUp) {
                postKeyToTarget(fg, WM_KEYUP, VK_ESCAPE, data.scanCode, false);
                return true;
            }
        }
    }

    return false;
}

void CALLBACK RemoteMasterEngine::winEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/
) {
    if (event != EVENT_SYSTEM_FOREGROUND || !hwnd) return;

    auto& engine = RemoteMasterEngine::instance();
    std::string procName;
    bool isRemote = engine.checkWindowIsRemote(hwnd, &procName);

    bool prevIsRemote = engine.m_isRemoteForeground.exchange(isRemote, std::memory_order_relaxed);
    HWND prevHwnd = engine.m_activeRemoteHwnd.exchange(isRemote ? hwnd : nullptr, std::memory_order_relaxed);

    if (isRemote) {
        {
            std::lock_guard lock(engine.m_mutex);
            engine.m_activeRemoteProcess = procName;
        }
        if (!prevIsRemote || prevHwnd != hwnd) {
            engine.onRemoteForegroundGained(hwnd);
        }
    } else {
        {
            std::lock_guard lock(engine.m_mutex);
            engine.m_activeRemoteProcess.clear();
        }
        if (prevIsRemote) {
            engine.onRemoteForegroundLost(prevHwnd);
        }
    }
}

} // namespace easy::core
