#include "core.hpp"
#include <bcrypt.h>
#include <wincrypt.h>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace qc {
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode text");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
void wipe(std::wstring& text) { if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t)); text.clear(); }
void wipe(std::string& text) { if (!text.empty()) SecureZeroMemory(text.data(), text.size()); text.clear(); }
std::wstring windowText(HWND window) {
    int n = GetWindowTextLengthW(window);
    std::wstring out(n + 1, L'\0');
    out.resize(GetWindowTextW(window, out.data(), n + 1));
    return out;
}
std::int64_t now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t nowMillis() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// 使用内容摘要去重，保留最近复制时间和累计次数。
std::string sha256(const std::wstring& text) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    unsigned char bytes[32]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA256 provider failed");
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0) status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(text.data())), static_cast<ULONG>(text.size() * sizeof(wchar_t)), 0);
    if (status >= 0) status = BCryptFinishHash(hash, bytes, sizeof(bytes), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto byte : bytes) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}

// 敏感值只使用当前 Windows 用户的 DPAPI 密钥保护。
std::vector<unsigned char> protect(const std::wstring& text) {
    DATA_BLOB input{static_cast<DWORD>(text.size() * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(text.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"QuickClipboard field", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) throw std::runtime_error("DPAPI encryption failed");
    std::vector<unsigned char> result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}
std::wstring unprotect(const std::vector<unsigned char>& data) {
    DATA_BLOB input{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) throw std::runtime_error("当前 Windows 用户无法解密此字段");
    std::wstring result(reinterpret_cast<wchar_t*>(output.pbData), output.cbData / sizeof(wchar_t));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
// 以可执行文件位置定位便携数据，不受登录启动时的工作目录影响。
std::filesystem::path executablePath() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) throw std::runtime_error("无法获取程序所在目录");
        if (length < buffer.size()) { buffer.resize(length); return buffer; }
        if (buffer.size() >= 32768) throw std::runtime_error("程序路径过长");
        buffer.resize(std::min<size_t>(buffer.size() * 2, 32768));
    }
}
std::filesystem::path defaultDataDir() { return executablePath().parent_path() / L"data"; }
std::wstring errorText(const std::exception& error) { return wide(error.what()); }
std::wstring fieldValue(const Field& field) { return field.sensitive ? unprotect(field.encrypted) : field.plain; }
Draft makeDraft(const Record& record) {
    Draft out;
    out.id = record.id; out.groupId = record.groupId; out.name = record.name; out.note = record.note; out.pinned = record.pinned;
    for (const auto& field : record.fields) out.fields.push_back({field.id, field.name, fieldValue(field), field.sensitive, field.sensitive});
    return out;
}
}
