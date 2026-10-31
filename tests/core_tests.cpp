#include "database.hpp"
#include "hotkey.hpp"
#include "ui.hpp"
#include "startup.hpp"
#include <commctrl.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>


using namespace qc;
namespace {
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
Id scalar(Database& db, const char* sql) {
    sqlite3_stmt* statement{};
    require(sqlite3_prepare_v2(db.handle(), sql, -1, &statement, nullptr) == SQLITE_OK, "prepare failed");
    require(sqlite3_step(statement) == SQLITE_ROW, "query failed");
    auto result = sqlite3_column_int64(statement, 0); sqlite3_finalize(statement); return result;
}
double millis(std::chrono::steady_clock::time_point start) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
void resources(const char* label) {
    PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    std::cout << label << " private_bytes=" << memory.PrivateUsage << " working_set=" << memory.WorkingSetSize << " gdi=" << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) << " user=" << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) << '\n';
}
// 用虚拟事件检查快捷键边界，不向桌面注入真实 Windows 按键。
void testWinV() {
    WinVChord chord;
    require(!chord.process('V', true).suppress, "ordinary V passes");
    chord.process('V', false); chord.process(VK_LWIN, true);
    auto trigger = chord.process('V', true); require(trigger.activate && trigger.suppress, "left Win V intercepts");
    auto repeat = chord.process('V', true); require(repeat.suppress && !repeat.activate, "repeat triggers once");
    require(!chord.process(VK_LWIN, false).suppress, "Windows modifier release passes");
    require(chord.process('V', false).suppress, "V release suppressed after modifier release");
    require(!chord.process('V', true).suppress, "ordinary V restored");
    for (UINT modifier : {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU}) {
        WinVChord modified; modified.process(VK_RWIN, true); modified.process(modifier, true);
        require(!modified.process('V', true).suppress, "modified Win V passes");
    }
    WinVChord right; right.process(VK_RWIN, true); require(right.process('V', true).activate, "right Win V");
    WinVChord other; other.process(VK_LWIN, true); require(!other.process('R', true).suppress, "other Windows shortcuts pass");
}
// 覆盖宽窄窗口、完整显示与隐藏分组被选中后的边界。
void testGroupPacking() {
    auto filters = groupFilters({{8, L"AWS"}, {9, L"服务器"}, {10, L"开发"}});
    require(filters.front().id == -1 && filters.back().id == 0 && filters[1].id == 8, "ungrouped follows business groups");
    std::vector<int> widths{48, 52, 80, 66, 112, 70, 62};
    auto more = [](size_t) { return 72; };
    auto wide = packGroupChips(widths, 700, 5, more);
    require(wide.visible.size() == widths.size() && wide.hidden.empty() && !wide.moreWidth, "wide row needs no more menu");
    for (int width = 120; width < 800; width += 7) {
        for (size_t selected = 0; selected < widths.size(); ++selected) {
            auto packed = packGroupChips(widths, width, selected, more);
            int used = packed.visible.empty() ? 0 : static_cast<int>(packed.visible.size() - 1) * 5;
            for (auto index : packed.visible) used += widths[index];
            if (!packed.hidden.empty()) used += packed.moreWidth + (packed.visible.empty() ? 0 : 5);
            require(used <= width, "group row overflows");
            require(packed.visible.size() + packed.hidden.size() == widths.size(), "group lost or duplicated");
            if (width >= 270) {
                require(std::find(packed.visible.begin(), packed.visible.end(), selected) != packed.visible.end(), "selected group must remain visible");
                require(packed.visible.front() == 0, "all remains first at supported widths");
            }
        }
    }
}
// 在隐藏控件中验证滚轮拦截，不操作用户桌面的输入焦点。
LRESULT CALLBACK countWheel(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (message == WM_MOUSEWHEEL) ++*reinterpret_cast<int*>(ref);
    return DefSubclassProc(window, message, wp, lp);
}
void testComboWheel() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 160, 150, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(parent && combo, "hidden combo construction");
    for (auto label : {L"第一组", L"第二组", L"未分组"}) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    int wheels = 0; SetWindowSubclass(parent, countWheel, 1, reinterpret_cast<DWORD_PTR>(&wheels));
    guardComboWheel(combo, parent);
    SendMessageW(combo, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    require(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1 && wheels == 1, "closed combo forwards scroll without changing selection");
    COMBOBOXINFO info{sizeof(info)}; require(GetComboBoxInfo(combo, &info) && info.hwndList, "popup list exists");
    SendMessageW(info.hwndList, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    require(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1 && wheels == 1, "popup wheel must not select or scroll parent");
    SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
    require(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 2, "keyboard selection remains available");
    DestroyWindow(parent);
}
// 检查代码符号、硬换行和长 URL 的原文索引，避免 UTF-16 字符被拆开。
void testTextWrapping() {
    HDC dc = GetDC(nullptr); auto old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    std::wstring code = L"https://example.test/" + std::wstring(200, L'a') + L"?x=1&name=中文";
    auto preview = wrapFieldText(dc, code, 100, 2);
    require(preview.lines.size() == 2 && preview.truncated, "long URL preview limit");
    auto full = wrapFieldText(dc, code, 100);
    size_t characters = 0; for (auto [offset, count] : full.lines) { require(offset == characters, "URL characters must remain contiguous"); characters += count; }
    require(!full.truncated && characters == code.size(), "full URL remains intact");
    auto paragraphs = wrapFieldText(dc, L"第一行\r\n\r\n末行", 1000);
    require(paragraphs.lines.size() == 3 && paragraphs.lines[1].second == 0, "CRLF and empty lines");
    auto emoji = wrapFieldText(dc, L"😀😀😀😀😀😀😀😀", 22);
    for (auto [offset, count] : emoji.lines) require(offset % 2 == 0 && count % 2 == 0, "UTF16 surrogate boundary");
    SelectObject(dc, old); ReleaseDC(nullptr, dc);
}

// 从其他工作目录运行时，数据目录仍定位到程序旁。
void testPortableData(const std::filesystem::path& dir) {
    auto original = std::filesystem::current_path();
    std::filesystem::current_path(dir);
    auto actual = defaultDataDir();
    std::filesystem::current_path(original);
    require(actual == executablePath().parent_path() / L"data", "data directory follows executable, not working directory");
}
// 在非启动目录下验证注册表增删和失败回滚，不改变本机真正的登录配置。
void testStartup(const std::filesystem::path& dir) {
    struct TestKey {
        std::wstring path = L"Software\\QuickClipboard\\Tests\\Startup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        ~TestKey() { RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } key;
    StartupRegistration registration(key.path);
    Database db(dir / L"startup.db"); db.setSetting(L"theme", 2);
    std::filesystem::path executable = L"C:\\测试 portable app\\QuickClipboard.exe", data = executable.parent_path() / L"data";
    require(!registration.read().exists, "test startup begins absent");
    updateStartup(db, true, executable, data, registration);
    auto enabled = registration.read();
    require(startupMatches(enabled, executable, data) && db.settings().startup, "enable writes registry and database immediately");
    require(startupCommand(executable, data).find(L"--data-dir") == std::wstring::npos, "portable startup uses executable-based default");
    for (auto path : {std::filesystem::path(L"D:\\单独 data\\"), std::filesystem::path(L"D:\\")}) {
        updateStartup(db, true, executable, path, registration);
        require(startupMatches(registration.read(), executable, path), "startup command preserves spaces, Unicode, root and trailing slash");
    }
    require(!startupMatches(registration.read(), executable, data), "different data directories do not match");
    updateStartup(db, true, executable, data, registration);
    db.exec("PRAGMA query_only=ON;");
    bool refused = false;
    try { updateStartup(db, false, executable, data, registration); } catch (...) { refused = true; }
    auto restored = registration.read();
    require(refused && restored.bytes == enabled.bytes && restored.type == enabled.type && db.settings().startup, "database failure restores original startup value");
    db.exec("PRAGMA query_only=OFF;");
    updateStartup(db, false, executable, data, registration);
    require(!registration.read().exists && !db.settings().startup, "disable removes system registration and local setting");
    updateStartup(db, false, executable, data, registration);
    require(db.settings().theme == 2, "startup update preserves unrelated preferences");
    StartupValue raw{true, REG_BINARY, {1, 0, 2, 3}};
    registration.restore(raw); db.exec("PRAGMA query_only=ON;"); refused = false;
    try { updateStartup(db, true, executable, data, registration); } catch (...) { refused = true; }
    require(refused && registration.read().type == REG_BINARY && registration.read().bytes == raw.bytes, "rollback preserves exact registry type and bytes");
    db.exec("PRAGMA query_only=OFF;"); registration.restore({});
    refused = false;
    try { startupCommand(std::filesystem::path(L"C:\\" + std::wstring(260, L'a') + L"\\QuickClipboard.exe"), data); } catch (...) { refused = true; }
    require(refused && !registration.read().exists, "overlong Run command is rejected before registration");
    db.setSetting(L"startup", true);
    require(!synchronizeStartup(db, executable, data, registration) && !registration.read().exists && !db.settings().startup, "externally removed startup is not recreated");
    registration.set(true, executable, data);
    require(synchronizeStartup(db, executable, data, registration) && db.settings().startup, "startup reflects the actual current registration");
    updateStartup(db, false, executable, data, registration);
    std::cout << "PASS portable path, startup registry and rollback checks\n";
}

// 验证发布程序确实嵌入多尺寸图标，避免只更新窗口内的绘制。
void testApplicationResources() {
    auto executable = executablePath().parent_path() / L"QuickClipboard.exe";
    auto module = LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    require(module != nullptr, "load renamed application resources");
    require(FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON) != nullptr, "embedded application icon group");
    require(FindResourceW(module, MAKEINTRESOURCEW(1), RT_MANIFEST) != nullptr, "embedded renamed manifest");
    for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        auto icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
        require(icon != nullptr, "load embedded icon size");
        ICONINFO info{}; require(GetIconInfo(icon, &info), "read embedded icon bitmap");
        BITMAP bitmap{}; GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
        require(bitmap.bmWidth == size && bitmap.bmHeight == size, "embedded icon dimensions");
        DeleteObject(info.hbmColor); DeleteObject(info.hbmMask); DestroyIcon(icon);
    }
    FreeLibrary(module);
    std::cout << "PASS QuickClipboard embedded icon sizes and manifest\n";
}
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fixture") {
        auto fixture = std::filesystem::absolute(std::filesystem::path(argv[2])) / DatabaseFileName;
        if (std::filesystem::exists(fixture)) { std::cerr << "Fixture already exists\n"; return 1; }
        Database db(fixture); Settings s; s.hideOutside = false; db.saveSettings(s);
        auto group = db.saveGroup(0, L"开发环境");
        for (auto name : {L"工作", L"命令", L"云账号", L"参考", L"个人"}) db.saveGroup(0, name);
        Draft d; d.name = L"测试服务器"; d.groupId = group; d.note = L"仅用于界面验收的虚构资料";
        d.fields.push_back({0, L"IP 地址", L"203.0.113.10", false, false});
        d.fields.push_back({0, L"用户名", L"demo-user", false, false});
        d.fields.push_back({0, L"密码", L"EXAMPLE-ONLY-NOT-A-REAL-SECRET", true, false});
        db.save(d);
        d.id = 0; d.name = L"常用 SQL"; d.note = L""; d.fields.clear(); d.fields.push_back({0, L"SQL", L"SELECT id, name\nFROM users\nWHERE active = 1;", false, false}); db.save(d);
        d.groupId = 0; d.name = L"测试单字段"; d.fields = {{0, L"账号", L"qa@example.test", false, false}}; db.save(d);
        d.name = L"长地址"; d.fields = {{0, L"地址", L"https://example.test/" + std::wstring(180, L'a') + L"?x=1&name=中文", false, false}}; db.save(d);
        s.maxHistory = 10000; db.saveSettings(s);
        for (int i = 0; i < 10000; ++i) db.capture(L"测试剪贴板内容 " + std::to_wstring(i) + L"\n支持中文、SQL 与多行文本", L"TestEditor", s);
        std::cout << "Fixture created\n"; return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--crash-writer") {
        Database db(std::filesystem::path(argv[2]) / L"test.db");
        db.exec("BEGIN IMMEDIATE; UPDATE records SET name='uncommitted-crash';");
        TerminateProcess(GetCurrentProcess(), 99);
        return 99;
    }
    auto dir = std::filesystem::temp_directory_path() / (L"QuickClipboard-tests-" + std::to_wstring(GetCurrentProcessId()));
    try {
        testWinV();
        testGroupPacking(); testComboWheel(); testTextWrapping();
        std::filesystem::create_directories(dir);
        testPortableData(dir); testStartup(dir); testApplicationResources();
        auto startup = std::chrono::steady_clock::now();
        {
            Database db(dir / L"test.db"); Settings settings; settings.maxHistory = 10000; settings.retentionDays = 0;
            require(scalar(db, "PRAGMA user_version") == 4, "schema version");
            require(db.settings().modifiers == MOD_WIN && db.settings().key == 'V' && !db.settings().hideOutside, "new interaction defaults");
            settings.theme = 2; db.saveSettings(settings); require(db.settings().theme == 2, "settings persistence");
            db.capture(L"中文 SELECT * FROM data WHERE a LIKE '100%_\\'", L"UnitTest", settings);
            db.capture(L"中文 SELECT * FROM data WHERE a LIKE '100%_\\'", L"UnitTest", settings);
            require(db.history(L"中文").size() == 1, "history dedup");
            require(scalar(db, "SELECT copy_count FROM clipboard_history LIMIT 1") == 2, "copy count");
            require(db.history(L"%_\\").size() == 1, "literal wildcard escaping");
            db.capture(L"literal ! marker", L"", settings); require(db.history(L"!").size() == 1, "escape marker is literal");
            db.capture(L"中文 SELECT * FROM data WHERE a LIKE '100%_\\'", L"UnitTest", settings);
            require(db.history(L"").front().title.find(L"中文") == 0, "rapid duplicate moves to top");
            require(db.history(L"SELECT").size() == 1 && db.history(L"select").size() == 1, "case insensitive search");
            require(db.history(L"' OR 1=1--").empty(), "SQL injection protection");
            auto historyId = db.history(L"中文")[0].id;
            Id group = db.saveGroup(0, L"云账号");
            Draft draft; draft.name = L"测试服务器"; draft.note = L"欧洲\nSSH"; draft.groupId = group;
            draft.fields.push_back({0, L"用户", L"qa@example.test\r\n多行", false, false});
            draft.fields.push_back({0, L"密码", L"QC-TEST-SECRET-DO-NOT-INDEX-471928\r\n第二行", true, false});
            Id recordId = db.save(draft); auto record = db.record(recordId);
            require(record.fields.size() == 2, "multiple fields");
            require(record.fields[1].plain.empty() && !record.fields[1].encrypted.empty(), "encrypted column only");
            require(fieldValue(record.fields[1]) == draft.fields[1].value, "DPAPI roundtrip Unicode multiline");
            require(db.records(L"QC-TEST-SECRET", -1).empty(), "sensitive content excluded from search");
            require(db.records(L"密码", -1).size() == 1, "field name searchable");
            require(db.records(L"qa@", -1).size() == 1, "ordinary field searchable");
            require(db.records(L"云账", -1).size() == 1, "group searchable");
            auto edited = makeDraft(record); Id originalField = edited.fields[0].id;
            edited.fields[0].name = L"用户名"; edited.fields.push_back({0, L"端口", L"22", false, false}); db.save(edited);
            require(db.record(recordId).fields[0].id == originalField, "stable field identity");
            require(db.record(recordId).note == draft.note && db.record(recordId).groupId == group, "record metadata preserved");
            auto bad = edited; bad.name = L"不应保存"; bad.groupId = 999999;
            bool rollback = false; try { db.save(bad); } catch (...) { rollback = true; }
            require(rollback && db.record(recordId).name == draft.name, "transaction rollback");
            db.removeGroup(group); require(db.record(recordId).groupId == 0, "group deletion moves to ungrouped");
            DraftField extra{0, L"附加", L"SECRET-APPEND-885613", true, false}; db.append(recordId, extra, historyId);
            require(db.record(recordId).fields.size() == 4 && db.history(L"中文").empty(), "secret append removes source history only");
            db.capture(L"保留为资料", L"", settings); auto h = db.history(L"保留为资料")[0].id;
            Draft saved; saved.name = L"独立资料"; saved.historyId = h; saved.fields.push_back({0, L"内容", db.historyText(h), false, false}); auto independent = db.save(saved);
            db.clearHistory(); require(db.record(independent).fields[0].plain == L"保留为资料", "records survive history clearing");
            auto ordinaryToSecret = makeDraft(db.record(independent)); ordinaryToSecret.fields[0].sensitive = true; db.save(ordinaryToSecret);
            require(db.records(L"保留为资料", -1).empty(), "ordinary to secret removes searchable plaintext");
            auto downgrade = makeDraft(db.record(independent)); downgrade.fields[0].sensitive = false; db.save(downgrade);
            require(db.records(L"保留为资料", -1).size() == 1, "secret to ordinary");
            require(scalar(db, "SELECT count(*) FROM record_fields WHERE is_sensitive=1 AND plain_value IS NOT NULL") == 0, "no secret plaintext columns");
            // 新增敏感值不能以 UTF-8 或 UTF-16 明文出现在数据库及 WAL 文件中。
            for (auto file : {dir / L"test.db", dir / L"test.db-wal"}) {
                std::ifstream input(file, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(input)), {});
                for (auto secret : {L"QC-TEST-SECRET-DO-NOT-INDEX-471928", L"SECRET-APPEND-885613"}) {
                    std::wstring value(secret); auto narrow = utf8(value);
                    require(bytes.find(narrow) == std::string::npos, "secret UTF-8 bytes on disk");
                    require(bytes.find(std::string(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t))) == std::string::npos, "secret UTF-16 bytes on disk");
                }
            }
            auto captureStart = std::chrono::steady_clock::now();
            for (int i = 0; i < 10000; ++i) {
                db.capture(L"历史 SQL https://example.test/api?item=" + std::to_wstring(i) + L" SELECT * FROM records WHERE name='测试'; 100%_", L"TestEditor", settings);
                if (i == 999) { auto start = std::chrono::steady_clock::now(); db.history(L"不存在的内容"); std::cout << "search_1000_ms=" << millis(start) << '\n'; }
            }
            require(scalar(db, "SELECT count(*) FROM clipboard_history") == 10000, "10000 rows");
            std::cout << "insert_10000_ms=" << millis(captureStart) << '\n';
            std::vector<double> latency;
            for (int i = 0; i < 200; ++i) { auto start = std::chrono::steady_clock::now(); auto rows = db.history(i % 2 ? L"不存在的内容" : L"item=999"); require(rows.size() <= 80, "bounded results"); latency.push_back(millis(start)); }
            std::sort(latency.begin(), latency.end());
            std::cout << "search_10000_p50_ms=" << latency[latency.size() / 2] << " p95_ms=" << latency[latency.size() * 95 / 100] << " max_ms=" << latency.back() << '\n';
            require(db.history(L"", 80).size() == 80, "pagination");
            settings.maxHistory = 120; db.prune(settings); require(scalar(db, "SELECT count(*) FROM clipboard_history") == 120, "history limit");
            db.exec("UPDATE clipboard_history SET last_copied_at=1"); settings.retentionDays = 1; db.prune(settings); require(db.history(L"").empty(), "retention");
            db.removeRecord(recordId); require(scalar(db, "SELECT count(*) FROM record_fields WHERE record_id=1") == 0, "field cascading deletion");
            require(scalar(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0, "foreign key integrity");
            resources("core_tests");
        }
        {
            wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --crash-writer \"" + dir.wstring() + L"\"";
            STARTUPINFOW startupInfo{sizeof(startupInfo)}; PROCESS_INFORMATION process{};
            require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &process) != FALSE, "start crash recovery test");
            require(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "crash test timeout");
            DWORD code{}; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
            require(code == 99, "expected abrupt child exit");
            Database reopened(dir / L"test.db"); require(!reopened.records(L"独立", -1).empty(), "uncommitted transaction recovery");
            reopened.exec("PRAGMA user_version=5;");
            bool refused = false; try { Database future(dir / L"test.db"); } catch (...) { refused = true; }
            require(refused, "newer schema refuses downgrade");
        }
        {
            Database unsupported(dir / L"unsupported.db"); Settings settings; settings.modifiers = MOD_ALT | MOD_CONTROL; settings.key = 'K'; settings.hideOutside = true; unsupported.saveSettings(settings);
            unsupported.exec("PRAGMA user_version=2;");
            bool refused = false; try { Database other(dir / L"unsupported.db"); } catch (...) { refused = true; }
            auto unchanged = unsupported.settings();
            require(refused && scalar(unsupported, "PRAGMA user_version") == 2 && unchanged.modifiers == settings.modifiers && unchanged.key == 'K' && unchanged.hideOutside, "unsupported schema is rejected without rewriting settings");
        }
        std::cout << "PASS all database/security checks; total_ms=" << millis(startup) << '\n';
        std::filesystem::remove_all(dir);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << "\nTest data: " << utf8(dir.wstring()) << '\n'; return 1; }
}
