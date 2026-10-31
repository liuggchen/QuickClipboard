#include "app.hpp"
#include <shellapi.h>
#include <stdexcept>
#include <memory>

// 按数据目录保持单实例，静默启动不唤出已有界面。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    bool hidden = false;
    try {
        int argc{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("无法读取启动参数");
        auto freeArguments = [](wchar_t** value) { LocalFree(value); };
        std::unique_ptr<wchar_t*, decltype(freeArguments)> arguments(argv, freeArguments);
        for (int i = 1; i < argc; ++i) if (std::wstring(argv[i]) == L"--hidden") hidden = true;
        auto dir = qc::defaultDataDir();
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--data-dir") {
                if (i + 1 >= argc || !*argv[i + 1]) throw std::runtime_error("--data-dir 需要指定数据目录");
                dir = std::filesystem::absolute(argv[++i]).lexically_normal();
            }
        }
        auto identity = qc::wide(qc::sha256(dir.wstring())).substr(0, 24);
        auto closeHandle = [](void* handle) { CloseHandle(handle); };
        auto name = L"Local\\QuickClipboard-" + identity;
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex) throw std::runtime_error("无法建立单实例锁");
        bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
        std::unique_ptr<void, decltype(closeHandle)> lock(mutex, closeHandle);
        if (alreadyRunning) {
            auto window = FindWindowW((L"QuickClipboard-" + identity).c_str(), nullptr);
            if (window && !hidden) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(window, WM_APP + 1, 0, 0); }
            return 0;
        }
        std::filesystem::create_directories(dir);
        int result{};
        { qc::App app(instance, dir); result = app.run(hidden); }
        return result;
    } catch (const std::exception& e) {
        if (!hidden) MessageBoxW(nullptr, qc::errorText(e).c_str(), L"随手贴 · 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}
