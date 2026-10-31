#pragma once
#include <windows.h>
#include <array>
#include <thread>

namespace qc {
struct ChordResult {
    bool suppress{};
    bool activate{};
};

// 仅识别不带其他修饰键的 Win+V，按键连发只触发一次。
class WinVChord {
public:
    // 按事件维护修饰键状态，吞掉已捕获组合的连发和抬起。
    ChordResult process(UINT key, bool down);
    void seed(UINT key, bool down) { if (key < keys_.size()) keys_[key] = down; }
private:
    std::array<bool, 256> keys_{};
    bool captured_{};
};

// 在独立消息线程接管 Win+V；停止后由 Windows 自动恢复原快捷键。
class WinVHotkey {
public:
    ~WinVHotkey() { stop(); }
    // 安装专用消息线程和键盘回调，独占当前会话的 Win+V 接管。
    bool start(HWND target);
    // 卸载回调并等待线程退出，释放对系统快捷键的接管。
    void stop();
    bool active() const { return thread_.joinable(); }
private:
    static LRESULT CALLBACK hook(int code, WPARAM message, LPARAM data);
    static thread_local WinVHotkey* current_;
    static constexpr ULONG_PTR MaskTag = 0x51435756;
    std::thread thread_;
    HANDLE mutex_{};
    DWORD threadId_{};
    HWND target_{};
    WinVChord chord_;
};
constexpr UINT WM_WINV = WM_APP + 20;
}
