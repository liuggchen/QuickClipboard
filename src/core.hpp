#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace qc {
using Id = std::int64_t;
inline constexpr wchar_t AppDisplayName[] = L"随手贴";
inline constexpr wchar_t DatabaseFileName[] = L"database.db";
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
void wipe(std::wstring& text);
void wipe(std::string& text);
std::wstring windowText(HWND window);
std::int64_t now();
std::int64_t nowMillis();
std::string sha256(const std::wstring& text);
std::vector<unsigned char> protect(const std::wstring& text);
std::wstring unprotect(const std::vector<unsigned char>& data);
std::filesystem::path executablePath();
std::filesystem::path defaultDataDir();
std::wstring errorText(const std::exception& error);

struct Group { Id id{}; std::wstring name; };
struct Field {
    Id id{};
    std::wstring name;
    std::wstring plain;
    std::vector<unsigned char> encrypted;
    bool sensitive{};
    ~Field() { wipe(plain); }
};
struct SecretText {
    explicit SecretText(std::wstring text) : value(std::move(text)) {}
    ~SecretText() { wipe(value); }
    std::wstring value;
};
struct Record {
    Id id{};
    Id groupId{};
    std::wstring name;
    std::wstring note;
    bool pinned{};
    std::vector<Field> fields;
};
struct DraftField {
    Id id{};
    std::wstring name = L"内容";
    std::wstring value;
    bool sensitive{};
    bool wasSensitive{};
    ~DraftField() { wipe(value); }
};
struct Draft {
    Id id{};
    Id groupId{};
    Id historyId{};
    std::wstring name;
    std::wstring note;
    bool pinned{};
    std::vector<DraftField> fields;
};
struct Item {
    Id id{};
    std::wstring title;
    std::wstring subtitle;
    std::int64_t time{};
    bool pinned{};
};
struct Settings {
    int maxHistory = 5000;
    int retentionDays = 90;
    int maxBytes = 1024 * 1024;
    int clearSeconds = 30;
    UINT modifiers = MOD_WIN;
    UINT key = 'V';
    bool hideOutside = false;
    bool paused = false;
    bool startup = false;
    int theme = 0;
};
std::wstring fieldValue(const Field& field);
Draft makeDraft(const Record& record);
}
