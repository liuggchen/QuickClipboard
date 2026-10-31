#include "startup.hpp"
#include <shellapi.h>
#include <cstring>
#include <stdexcept>

namespace qc {
namespace {
struct RegistryKey { HKEY value{}; ~RegistryKey() { if (value) RegCloseKey(value); } };
[[noreturn]] void registryError(LSTATUS result) { throw std::runtime_error("无法更新当前用户的登录启动项，系统错误码：" + std::to_string(result)); }
// 按 Windows 路径习惯忽略大小写与多余的末尾目录分隔符。
bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    auto normalize = [](const std::filesystem::path& path) {
        auto value = std::filesystem::absolute(path).lexically_normal();
        if (!value.has_filename() && value != value.root_path()) value = value.parent_path();
        return value.wstring();
    };
    auto a = normalize(left), b = normalize(right);
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
// 参数末尾的反斜杠需要加倍，避免吞掉结束引号。
std::wstring quote(const std::filesystem::path& path) {
    auto text = path.wstring();
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
// 只解析完整的字符串启动项，不将异常注册表内容作为命令使用。
std::vector<std::wstring> arguments(const StartupValue& value) {
    if (!value.exists || value.type != REG_SZ || value.bytes.empty() || value.bytes.size() % sizeof(wchar_t)) return {};
    std::wstring command(value.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), value.bytes.data(), value.bytes.size());
    if (command.back() != L'\0') return {};
    command.pop_back();
    if (command.empty() || command.find(L'\0') != std::wstring::npos) return {};
    int count{}; auto argv = CommandLineToArgvW(command.c_str(), &count);
    if (!argv) return {};
    std::vector<std::wstring> result(argv, argv + count); LocalFree(argv); return result;
}
}

StartupValue StartupRegistration::read() const {
    RegistryKey key;
    auto status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey_.c_str(), 0, KEY_QUERY_VALUE, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status != ERROR_SUCCESS) registryError(status);
    StartupValue result; DWORD bytes{};
    status = RegQueryValueExW(key.value, L"QuickClipboard", nullptr, &result.type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status != ERROR_SUCCESS) registryError(status);
    result.bytes.resize(bytes);
    status = RegQueryValueExW(key.value, L"QuickClipboard", nullptr, &result.type, result.bytes.data(), &bytes);
    if (status != ERROR_SUCCESS) registryError(status);
    result.bytes.resize(bytes); result.exists = true;
    return result;
}
void StartupRegistration::restore(const StartupValue& value) const {
    RegistryKey key;
    LSTATUS status;
    if (value.exists) status = RegCreateKeyExW(HKEY_CURRENT_USER, subkey_.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value, nullptr);
    else status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey_.c_str(), 0, KEY_SET_VALUE, &key.value);
    if (!value.exists && status == ERROR_FILE_NOT_FOUND) return;
    if (status != ERROR_SUCCESS) registryError(status);
    if (value.exists) status = RegSetValueExW(key.value, L"QuickClipboard", 0, value.type, value.bytes.data(), static_cast<DWORD>(value.bytes.size()));
    else status = RegDeleteValueW(key.value, L"QuickClipboard");
    if (status != ERROR_SUCCESS && !(status == ERROR_FILE_NOT_FOUND && !value.exists)) registryError(status);
}

// 默认便携目录不必写入参数；显式数据目录随启动项保留。
std::wstring startupCommand(const std::filesystem::path& executable, const std::filesystem::path& dataDir) {
    auto command = quote(executable) + L" --hidden";
    if (!samePath(dataDir, executable.parent_path() / L"data")) command += L" --data-dir " + quote(dataDir);
    if (command.size() > 260) throw std::runtime_error("登录启动命令超过 Windows 的 260 字符限制，请缩短程序或数据目录路径");
    return command;
}
void StartupRegistration::set(bool enabled, const std::filesystem::path& executable, const std::filesystem::path& dataDir) const {
    StartupValue value;
    if (enabled) {
        auto command = startupCommand(executable, dataDir); value.exists = true;
        value.bytes.resize((command.size() + 1) * sizeof(wchar_t));
        std::memcpy(value.bytes.data(), command.c_str(), value.bytes.size());
    }
    restore(value);
}
bool startupTargetsExecutable(const StartupValue& value, const std::filesystem::path& executable) {
    auto args = arguments(value);
    return !args.empty() && std::filesystem::path(args[0]).is_absolute() && samePath(args[0], executable);
}
bool startupMatches(const StartupValue& value, const std::filesystem::path& executable, const std::filesystem::path& dataDir) {
    auto args = arguments(value);
    if (!startupTargetsExecutable(value, executable)) return false;
    bool hidden = false; auto registeredDir = executable.parent_path() / L"data";
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--hidden") hidden = true;
        else if (args[i] == L"--data-dir" && i + 1 < args.size()) registeredDir = args[++i];
        else return false;
    }
    return hidden && registeredDir.is_absolute() && samePath(registeredDir, dataDir);
}
void updateStartup(Database& db, bool enabled, const std::filesystem::path& executable, const std::filesystem::path& dataDir, const StartupRegistration& registration) {
    auto previous = registration.read();
    registration.set(enabled, executable, dataDir);
    try { db.setSetting(L"startup", enabled); }
    catch (const std::exception& e) {
        auto message = std::string(e.what());
        try { registration.restore(previous); }
        catch (const std::exception& rollback) { throw std::runtime_error(message + "；恢复原启动项也失败：" + rollback.what()); }
        throw std::runtime_error(message);
    }
}
bool synchronizeStartup(Database& db, const std::filesystem::path& executable, const std::filesystem::path& dataDir, const StartupRegistration& registration) {
    bool enabled = startupMatches(registration.read(), executable, dataDir);
    if (db.settings().startup != enabled) db.setSetting(L"startup", enabled);
    return enabled;
}
}
