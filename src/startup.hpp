#pragma once
#include "database.hpp"

namespace qc {
struct StartupValue {
    bool exists{};
    DWORD type = REG_SZ;
    std::vector<BYTE> bytes;
};

// 管理当前用户的登录启动项；可指定独立键路径进行隔离验证。
class StartupRegistration {
public:
    explicit StartupRegistration(std::wstring subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run") : subkey_(std::move(subkey)) {}
    StartupValue read() const;
    // 恢复原注册表值的类型和内容，缺省快照表示移除启动项。
    void restore(const StartupValue& value) const;
    // 启用时注册静默命令，禁用时仅删除本程序的值。
    void set(bool enabled, const std::filesystem::path& executable, const std::filesystem::path& dataDir) const;
private:
    std::wstring subkey_;
};
// 生成与 Windows 参数解析规则一致的静默启动命令。
std::wstring startupCommand(const std::filesystem::path& executable, const std::filesystem::path& dataDir);
bool startupTargetsExecutable(const StartupValue& value, const std::filesystem::path& executable);
bool startupMatches(const StartupValue& value, const std::filesystem::path& executable, const std::filesystem::path& dataDir);
// 同步系统启动项和本地偏好；数据库写入失败时恢复原系统配置。
void updateStartup(Database& db, bool enabled, const std::filesystem::path& executable, const std::filesystem::path& dataDir, const StartupRegistration& registration = StartupRegistration{});
// 按实际系统启动项同步开关，不在启动时重新注册命令。
bool synchronizeStartup(Database& db, const std::filesystem::path& executable, const std::filesystem::path& dataDir, const StartupRegistration& registration = StartupRegistration{});
}
