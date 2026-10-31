#include "database.hpp"
#include <stdexcept>
#include <algorithm>

namespace qc {
namespace {
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) { if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) fail(); }
    ~Statement() { sqlite3_finalize(stmt_); }
    void number(int n, Id value) { if (sqlite3_bind_int64(stmt_, n, value) != SQLITE_OK) fail(); }
    void text(int n, const std::wstring& value) { if (sqlite3_bind_text16(stmt_, n, value.data(), static_cast<int>(value.size() * 2), SQLITE_TRANSIENT) != SQLITE_OK) fail(); }
    void ascii(int n, const std::string& value) { if (sqlite3_bind_text(stmt_, n, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) fail(); }
    void blob(int n, const std::vector<unsigned char>& value) { if (sqlite3_bind_blob(stmt_, n, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) fail(); }
    bool next() { int r = sqlite3_step(stmt_); if (r == SQLITE_ROW) return true; if (r != SQLITE_DONE) fail(); return false; }
    Id number(int n) { return sqlite3_column_int64(stmt_, n); }
    std::wstring text(int n) { auto p = static_cast<const wchar_t*>(sqlite3_column_text16(stmt_, n)); return p ? std::wstring(p, sqlite3_column_bytes16(stmt_, n) / 2) : L""; }
    std::vector<unsigned char> blob(int n) { auto p = static_cast<const unsigned char*>(sqlite3_column_blob(stmt_, n)); int size = sqlite3_column_bytes(stmt_, n); return p ? std::vector<unsigned char>(p, p + size) : std::vector<unsigned char>{}; }
private:
    [[noreturn]] void fail() { throw std::runtime_error(sqlite3_errmsg(db_)); }
    sqlite3* db_;
    sqlite3_stmt* stmt_{};
};
class Transaction {
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Transaction() { if (!done_) { try { db_.exec("ROLLBACK"); } catch (...) {} } }
    void commit() { db_.exec("COMMIT"); done_ = true; }
private:
    Database& db_;
    bool done_{};
};
// 将百分号、下划线和转义符作为普通查询字符处理。
std::wstring pattern(const std::wstring& query) {
    std::wstring result = L"%";
    for (wchar_t c : query) { if (c == L'%' || c == L'_' || c == L'!') result += L'!'; result += c; }
    return result + L"%";
}
Field readField(Statement& s) { return {s.number(0), s.text(1), s.text(2), s.blob(3), s.number(4) != 0}; }
}

// 首次运行直接建立当前结构；现有数据库必须使用相同结构版本。
Database::Database(const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    if (sqlite3_open16(file.c_str(), &db_) != SQLITE_OK) { std::string error = db_ ? sqlite3_errmsg(db_) : "Database open failed"; if (db_) sqlite3_close(db_); db_ = nullptr; throw std::runtime_error(error); }
    try {
        sqlite3_busy_timeout(db_, 2000);
        Id v{};
        { Statement version(db_, "PRAGMA user_version"); version.next(); v = version.number(0); }
        if (v > 3) throw std::runtime_error("数据库版本较新，请使用新版随手贴");
        if (v != 0 && v != 3) throw std::runtime_error("数据库结构不受支持，请使用当前版本的数据文件");
        exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA secure_delete=ON; PRAGMA cache_size=-2048; PRAGMA temp_store=MEMORY;");
        if (v == 0) {
            Transaction tx(*this);
            exec(R"SQL(
CREATE TABLE groups(id INTEGER PRIMARY KEY,name TEXT NOT NULL UNIQUE,sort_order INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);
CREATE TABLE records(id INTEGER PRIMARY KEY,group_id INTEGER REFERENCES groups(id) ON DELETE SET NULL,name TEXT NOT NULL,note TEXT NOT NULL DEFAULT '',is_pinned INTEGER NOT NULL DEFAULT 0,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,last_used_at INTEGER NOT NULL DEFAULT 0);
CREATE TABLE record_fields(id INTEGER PRIMARY KEY,record_id INTEGER NOT NULL REFERENCES records(id) ON DELETE CASCADE,name TEXT NOT NULL,plain_value TEXT,encrypted_value BLOB,is_sensitive INTEGER NOT NULL CHECK(is_sensitive IN (0,1)),sort_order INTEGER NOT NULL,CHECK((is_sensitive=0 AND plain_value IS NOT NULL AND encrypted_value IS NULL) OR (is_sensitive=1 AND plain_value IS NULL AND encrypted_value IS NOT NULL)));
CREATE TABLE clipboard_history(id INTEGER PRIMARY KEY,content TEXT NOT NULL,content_hash TEXT NOT NULL UNIQUE,source_app TEXT NOT NULL DEFAULT '',created_at INTEGER NOT NULL,last_copied_at INTEGER NOT NULL,copy_count INTEGER NOT NULL DEFAULT 1);
CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE INDEX idx_history_recent ON clipboard_history(last_copied_at DESC,id DESC);
CREATE INDEX idx_records_recent ON records(is_pinned DESC,updated_at DESC,id DESC);
CREATE INDEX idx_records_group ON records(group_id);
CREATE INDEX idx_fields_record ON record_fields(record_id,sort_order);
PRAGMA user_version=3;
)SQL");
            tx.commit();
        }
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Database::~Database() { if (db_) { sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr); sqlite3_close(db_); } }
void Database::exec(const char* sql) { char* error{}; if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) { std::string text = error ? error : "SQLite error"; sqlite3_free(error); throw std::runtime_error(text); } }
Settings Database::settings() {
    Settings s;
    Statement q(db_, "SELECT key,value FROM settings");
    while (q.next()) {
        auto key = q.text(0); int value{};
        try { value = std::stoi(q.text(1)); } catch (...) { continue; }
        if (key == L"maxHistory") s.maxHistory = value;
        if (key == L"retentionDays") s.retentionDays = value;
        if (key == L"maxBytes") s.maxBytes = value;
        if (key == L"clearSeconds") s.clearSeconds = value;
        if (key == L"modifiers") s.modifiers = value;
        if (key == L"key") s.key = value;
        if (key == L"hideOutside") s.hideOutside = value != 0;
        if (key == L"paused") s.paused = value != 0;
        if (key == L"startup") s.startup = value != 0;
        if (key == L"theme") s.theme = value;
    }
    s.maxHistory = std::clamp(s.maxHistory, 1, 100000);
    s.maxBytes = std::clamp(s.maxBytes, 1024, 16 * 1024 * 1024);
    s.retentionDays = std::clamp(s.retentionDays, 0, 3650);
    s.clearSeconds = std::clamp(s.clearSeconds, 0, 3600);
    s.theme = std::clamp(s.theme, 0, 2);
    return s;
}
void Database::saveSettings(const Settings& s) {
    Transaction tx(*this);
    auto put = [&](const wchar_t* key, int value) { setSetting(key, value); };
    put(L"maxHistory", s.maxHistory); put(L"retentionDays", s.retentionDays); put(L"maxBytes", s.maxBytes); put(L"clearSeconds", s.clearSeconds); put(L"modifiers", s.modifiers); put(L"key", s.key); put(L"hideOutside", s.hideOutside); put(L"paused", s.paused); put(L"startup", s.startup); put(L"theme", s.theme);
    tx.commit();
}
void Database::setSetting(const std::wstring& key, int value) {
    Statement q(db_, "INSERT INTO settings(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.text(1, key); q.text(2, std::to_wstring(value)); q.next();
}

void Database::capture(const std::wstring& content, const std::wstring& source, const Settings& s) {
    if (content.empty() || content.size() * sizeof(wchar_t) > static_cast<size_t>(s.maxBytes)) return;
    Transaction tx(*this);
    Statement latest(db_, "SELECT coalesce(max(last_copied_at),0) FROM clipboard_history"); latest.next();
    // 同毫秒内连续复制仍保持操作先后，重复内容也能回到列表顶部。
    Id copiedAt = std::max(nowMillis(), latest.number(0) + 1);
    Statement q(db_, "INSERT INTO clipboard_history(content,content_hash,source_app,created_at,last_copied_at) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(content_hash) DO UPDATE SET last_copied_at=excluded.last_copied_at,source_app=excluded.source_app,copy_count=copy_count+1");
    q.text(1, content); q.ascii(2, sha256(content)); q.text(3, source); q.number(4, now()); q.number(5, copiedAt); q.next();
    prune(s); tx.commit();
}
void Database::prune(const Settings& s) {
    if (s.retentionDays > 0) { Statement q(db_, "DELETE FROM clipboard_history WHERE last_copied_at<?1"); q.number(1, (now() - static_cast<Id>(s.retentionDays) * 86400) * 1000); q.next(); }
    Statement q(db_, "DELETE FROM clipboard_history WHERE id IN(SELECT id FROM clipboard_history ORDER BY last_copied_at DESC,id DESC LIMIT -1 OFFSET ?1)"); q.number(1, s.maxHistory); q.next();
}
std::vector<Item> Database::history(const std::wstring& query, int offset, int limit) {
    Statement q(db_, "SELECT id,substr(content,1,240),source_app,last_copied_at FROM clipboard_history WHERE content LIKE ?1 ESCAPE '!' OR source_app LIKE ?1 ESCAPE '!' ORDER BY last_copied_at DESC,id DESC LIMIT ?2 OFFSET ?3");
    q.text(1, pattern(query)); q.number(2, limit); q.number(3, offset);
    std::vector<Item> items;
    while (q.next()) items.push_back({q.number(0), q.text(1), q.text(2), q.number(3) / 1000, false});
    return items;
}
std::vector<Item> Database::records(const std::wstring& query, Id group, int offset, int limit) {
    Statement q(db_, R"SQL(SELECT r.id,r.name,g.name,r.is_pinned,
      (SELECT count(*) FROM record_fields WHERE record_id=r.id),
      (SELECT CASE WHEN is_sensitive=0 THEN substr(plain_value,1,150) ELSE '••••••' END FROM record_fields WHERE record_id=r.id ORDER BY sort_order,id LIMIT 1)
      FROM records r LEFT JOIN groups g ON g.id=r.group_id
      WHERE (?2=-1 OR coalesce(r.group_id,0)=?2) AND
      (r.name LIKE ?1 ESCAPE '!' OR r.note LIKE ?1 ESCAPE '!' OR g.name LIKE ?1 ESCAPE '!' OR EXISTS
       (SELECT 1 FROM record_fields f WHERE f.record_id=r.id AND (f.name LIKE ?1 ESCAPE '!' OR (f.is_sensitive=0 AND f.plain_value LIKE ?1 ESCAPE '!'))))
      ORDER BY r.is_pinned DESC,r.updated_at DESC,r.id DESC LIMIT ?3 OFFSET ?4)SQL");
    q.text(1, pattern(query)); q.number(2, group); q.number(3, limit); q.number(4, offset);
    std::vector<Item> items;
    while (q.next()) {
        auto count = q.number(4); auto groupName = q.text(2);
        auto summary = count == 1 ? q.text(5) : std::to_wstring(count) + L" 个字段";
        if (!groupName.empty()) summary += L" · " + groupName;
        items.push_back({q.number(0), q.text(1), summary, 0, q.number(3) != 0});
    }
    return items;
}
std::wstring Database::historyText(Id id) { Statement q(db_, "SELECT content FROM clipboard_history WHERE id=?1"); q.number(1, id); if (!q.next()) throw std::runtime_error("历史已不存在"); return q.text(0); }
Record Database::record(Id id) {
    Statement q(db_, "SELECT id,coalesce(group_id,0),name,note,is_pinned FROM records WHERE id=?1"); q.number(1, id);
    if (!q.next()) throw std::runtime_error("资料已不存在");
    Record r{q.number(0), q.number(1), q.text(2), q.text(3), q.number(4) != 0, {}};
    Statement f(db_, "SELECT id,name,plain_value,encrypted_value,is_sensitive FROM record_fields WHERE record_id=?1 ORDER BY sort_order,id"); f.number(1, id);
    while (f.next()) r.fields.push_back(readField(f));
    return r;
}
Field Database::field(Id id) { Statement q(db_, "SELECT id,name,plain_value,encrypted_value,is_sensitive FROM record_fields WHERE id=?1"); q.number(1, id); if (!q.next()) throw std::runtime_error("字段已不存在"); return readField(q); }

// 保留已有字段 ID；敏感值永不绑定到明文列，全部修改在一个事务内提交。
void Database::insertField(Id recordId, const DraftField& f, int order) {
    Statement q(db_, "INSERT INTO record_fields(id,record_id,name,plain_value,encrypted_value,is_sensitive,sort_order) VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(id) DO UPDATE SET name=excluded.name,plain_value=excluded.plain_value,encrypted_value=excluded.encrypted_value,is_sensitive=excluded.is_sensitive,sort_order=excluded.sort_order WHERE record_id=excluded.record_id");
    if (f.id) q.number(1, f.id);
    q.number(2, recordId); q.text(3, f.name);
    if (f.sensitive) q.blob(5, protect(f.value)); else q.text(4, f.value);
    q.number(6, f.sensitive); q.number(7, order); q.next();
}
Id Database::save(const Draft& d) {
    if (d.name.empty() || d.fields.empty()) throw std::runtime_error("请输入资料名称并保留至少一个有效字段");
    for (const auto& f : d.fields) if (f.name.empty() || f.value.empty()) throw std::runtime_error("字段名称和值不能为空");
    Transaction tx(*this);
    Id id = d.id;
    if (id) {
        Statement q(db_, "UPDATE records SET group_id=?2,name=?3,note=?4,is_pinned=?5,updated_at=?6 WHERE id=?1");
        q.number(1, id); if (d.groupId) q.number(2, d.groupId); q.text(3, d.name); q.text(4, d.note); q.number(5, d.pinned); q.number(6, now()); q.next();
        if (sqlite3_changes(db_) != 1) throw std::runtime_error("资料已不存在");
        // 移除未保留的字段，不重建其余字段，以维持稳定标识。
        auto original = record(id);
        for (const auto& f : original.fields) {
            bool retained = false; for (const auto& v : d.fields) if (v.id == f.id) retained = true;
            if (!retained) { Statement del(db_, "DELETE FROM record_fields WHERE id=?1"); del.number(1, f.id); del.next(); }
        }
    } else {
        Statement q(db_, "INSERT INTO records(group_id,name,note,is_pinned,created_at,updated_at) VALUES(?1,?2,?3,?4,?5,?5)");
        if (d.groupId) q.number(1, d.groupId); q.text(2, d.name); q.text(3, d.note); q.number(4, d.pinned); q.number(5, now()); q.next(); id = sqlite3_last_insert_rowid(db_);
    }
    bool secret = false;
    for (size_t i = 0; i < d.fields.size(); ++i) { insertField(id, d.fields[i], static_cast<int>(i)); secret |= d.fields[i].sensitive; }
    if (secret && d.historyId) removeHistory(d.historyId);
    tx.commit();
    return id;
}
void Database::append(Id id, const DraftField& f, Id historyId) {
    if (f.name.empty() || f.value.empty()) throw std::runtime_error("字段名称和值不能为空");
    Transaction tx(*this);
    record(id);
    Statement q(db_, "SELECT coalesce(max(sort_order),-1)+1 FROM record_fields WHERE record_id=?1"); q.number(1, id); q.next();
    insertField(id, f, static_cast<int>(q.number(0)));
    Statement u(db_, "UPDATE records SET updated_at=?2 WHERE id=?1"); u.number(1, id); u.number(2, now()); u.next();
    if (f.sensitive && historyId) removeHistory(historyId);
    tx.commit();
}
void Database::removeHistory(Id id) { Statement q(db_, "DELETE FROM clipboard_history WHERE id=?1"); q.number(1, id); q.next(); }
void Database::clearHistory() { exec("DELETE FROM clipboard_history; PRAGMA wal_checkpoint(TRUNCATE);"); }
void Database::removeRecord(Id id) { Statement q(db_, "DELETE FROM records WHERE id=?1"); q.number(1, id); q.next(); }
void Database::pin(Id id, bool pinned) { Statement q(db_, "UPDATE records SET is_pinned=?2 WHERE id=?1"); q.number(1, id); q.number(2, pinned); q.next(); }
void Database::used(Id id) { Statement q(db_, "UPDATE records SET last_used_at=?2 WHERE id=?1"); q.number(1, id); q.number(2, now()); q.next(); }
std::vector<Group> Database::groups() { Statement q(db_, "SELECT id,name FROM groups ORDER BY sort_order,id"); std::vector<Group> result; while (q.next()) result.push_back({q.number(0), q.text(1)}); return result; }
Id Database::saveGroup(Id id, const std::wstring& name) {
    if (name.empty()) throw std::runtime_error("分组名称不能为空");
    Statement q(db_, "INSERT INTO groups(id,name,created_at,updated_at) VALUES(?1,?2,?3,?3) ON CONFLICT(id) DO UPDATE SET name=excluded.name,updated_at=excluded.updated_at");
    if (id) q.number(1, id); q.text(2, name); q.number(3, now()); q.next(); return id ? id : sqlite3_last_insert_rowid(db_);
}
void Database::removeGroup(Id id) { Transaction tx(*this); Statement q(db_, "DELETE FROM groups WHERE id=?1"); q.number(1, id); q.next(); tx.commit(); }
void complete(HWND window, std::function<void()> callback) { auto p = new std::function<void()>(std::move(callback)); if (!PostMessageW(window, WM_COMPLETE, 0, reinterpret_cast<LPARAM>(p))) delete p; }
Worker::Worker(HWND window, const std::filesystem::path& path) : thread_([this, window, path] {
    try {
        Database db(path);
        for (;;) {
            std::function<void(Database&)> job;
            { std::unique_lock lock(mutex_); cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); }); if (stopping_ && jobs_.empty()) break; job = std::move(jobs_.front()); jobs_.pop_front(); }
            try { job(db); } catch (const std::exception& e) { auto message = new DatabaseError{errorText(e)}; if (!PostMessageW(window, WM_DATABASE_ERROR, 0, reinterpret_cast<LPARAM>(message))) delete message; }
        }
    } catch (const std::exception& e) { auto message = new DatabaseError{errorText(e), true}; if (!PostMessageW(window, WM_DATABASE_ERROR, 0, reinterpret_cast<LPARAM>(message))) delete message; }
}) {}
Worker::~Worker() { stop(); }
void Worker::stop() { { std::lock_guard lock(mutex_); stopping_ = true; } cv_.notify_one(); if (thread_.joinable()) thread_.join(); }
void Worker::post(std::function<void(Database&)> job) { { std::lock_guard lock(mutex_); if (stopping_) return; jobs_.push_back(std::move(job)); } cv_.notify_one(); }
}
