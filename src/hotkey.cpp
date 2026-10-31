#include "hotkey.hpp"
#include <future>

namespace qc {
thread_local WinVHotkey* WinVHotkey::current_{};

ChordResult WinVChord::process(UINT key, bool down) {
    if (key >= keys_.size()) return {};
    if (key == 'V' && captured_) {
        if (!down) captured_ = false;
        keys_[key] = down;
        return {true, false};
    }
    bool win = keys_[VK_LWIN] || keys_[VK_RWIN];
    bool ctrl = keys_[VK_CONTROL] || keys_[VK_LCONTROL] || keys_[VK_RCONTROL];
    bool shift = keys_[VK_SHIFT] || keys_[VK_LSHIFT] || keys_[VK_RSHIFT];
    bool alt = keys_[VK_MENU] || keys_[VK_LMENU] || keys_[VK_RMENU];
    keys_[key] = down;
    if (key == 'V' && down && win && !ctrl && !shift && !alt) {
        captured_ = true;
        return {true, true};
    }
    return {};
}

bool WinVHotkey::start(HWND target) {
    if (active()) return true;
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\QuickClipboard.WinV");
    if (!mutex_) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex_); mutex_ = nullptr; return false; }
    target_ = target;
    std::promise<bool> ready;
    auto result = ready.get_future();
    thread_ = std::thread([this, &ready] {
        current_ = this;
        threadId_ = GetCurrentThreadId();
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        chord_ = {};
        for (UINT key : {VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU}) chord_.seed(key, (GetAsyncKeyState(key) & 0x8000) != 0);
        auto handle = SetWindowsHookExW(WH_KEYBOARD_LL, hook, GetModuleHandleW(nullptr), 0);
        ready.set_value(handle != nullptr);
        if (handle) {
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            UnhookWindowsHookEx(handle);
        }
        current_ = nullptr;
    });
    if (result.get()) return true;
    stop();
    return false;
}

void WinVHotkey::stop() {
    if (thread_.joinable()) { PostThreadMessageW(threadId_, WM_QUIT, 0, 0); thread_.join(); }
    if (mutex_) { CloseHandle(mutex_); mutex_ = nullptr; }
    threadId_ = 0;
}

LRESULT CALLBACK WinVHotkey::hook(int code, WPARAM message, LPARAM data) {
    if (code != HC_ACTION || !current_) return CallNextHookEx(nullptr, code, message, data);
    auto input = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    if (input->dwExtraInfo == MaskTag) return CallNextHookEx(nullptr, code, message, data);
    bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    auto result = current_->chord_.process(input->vkCode, down);
    if (result.activate) {
        // 与 PowerToys 相同，用无文字的虚拟键事件避免松开 Win 后误弹开始菜单。
        INPUT mask[2]{};
        for (auto& event : mask) { event.type = INPUT_KEYBOARD; event.ki.wVk = 0xff; event.ki.dwExtraInfo = MaskTag; }
        mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, mask, sizeof(INPUT));
        PostMessageW(current_->target_, WM_WINV, 0, 0);
    }
    return result.suppress ? 1 : CallNextHookEx(nullptr, code, message, data);
}
}
