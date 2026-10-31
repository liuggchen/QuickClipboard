#pragma once
#include "core.hpp"
#include "sqlite3.h"
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace qc {
class Database {
public:
    explicit Database(const std::filesystem::path& file);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    void exec(const char* sql);
    Settings settings();
    void saveSettings(const Settings& settings);
    void setSetting(const std::wstring& key, int value);
    void capture(const std::wstring& content, const std::wstring& source, const Settings& settings);
    void prune(const Settings& settings);
    std::vector<Item> history(const std::wstring& query, int offset = 0, int limit = 80);
    std::vector<Item> records(const std::wstring& query, Id group, int offset = 0, int limit = 80);
    std::wstring historyText(Id id);
    Record record(Id id);
    Field field(Id id);
    Id save(const Draft& draft);
    void append(Id record, const DraftField& field, Id history);
    void removeHistory(Id id);
    void clearHistory();
    void removeRecord(Id id);
    void pin(Id id, bool pinned);
    void used(Id id);
    // 将资料移动到另一条资料前后，置顶与普通资料分别排序。
    void moveRecord(Id id, Id anchor, bool after);
    std::vector<Group> groups();
    Id saveGroup(Id id, const std::wstring& name);
    void removeGroup(Id id);
    sqlite3* handle() { return db_; }
private:
    sqlite3* db_{};
    void insertField(Id record, const DraftField& field, int order);
};

// 串行处理数据库任务；窗口线程只接收完成结果，不执行查询或等待锁。
class Worker {
public:
    Worker(HWND window, const std::filesystem::path& path);
    ~Worker();
    void post(std::function<void(Database&)> job);
    void stop();
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void(Database&)>> jobs_;
    bool stopping_{};
    std::thread thread_;
};
constexpr UINT WM_COMPLETE = WM_APP + 10;
constexpr UINT WM_DATABASE_ERROR = WM_APP + 12;
struct DatabaseError { std::wstring text; bool fatal{}; };
void complete(HWND window, std::function<void()> callback);
}
