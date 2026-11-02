#pragma once
#include "core.hpp"
#include <oleidl.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace qc {
// 图片在窗口线程取得独立快照，后台不再访问剪贴板或拖放对象。
struct QrImage {
    std::filesystem::path file;
    std::vector<unsigned char> bytes;
    bool dib{};
};
struct QrValue {
    std::wstring original;
    std::wstring id;
};
struct QrResult {
    std::vector<QrValue> values;
    std::wstring error;
};
// 带四模块白边的黑白像素矩阵，预览和 PNG 保存使用同一份内容。
struct QrCode {
    int size{};
    std::vector<std::uint32_t> pixels;
    std::wstring error;
};

// 从有效 URL 原文取最后一个等号后的内容，没有等号则取最后一个路径斜杠后的内容。
std::wstring extractQrId(const std::wstring& value);
// 从剪贴板或拖放对象读取一张图片，支持图片数据和本地图片文件。
QrImage readQrImage(IDataObject* data);
// 解码原图并识别二维码，合并相同内容；调用线程需要初始化 COM。
QrResult recognizeQrImage(const QrImage& image);
// 原样编码文本，使用 UTF-8 和 M 级纠错，空文本不生成二维码。
QrCode generateQrCode(const std::wstring& text);
// 按 URL 的 ID 规则生成安全文件名，无 ID 时使用当前本地时间 yyyyMMddHHmmss。
std::wstring qrFileName(const std::wstring& text);
// 整数倍放大并保存为 PNG，完整写入临时文件后再替换目标；调用线程需要初始化 COM。
void saveQrCode(const QrCode& code, const std::filesystem::path& path);

// 后台串行处理二维码，识别和生成分别保留最新等待请求，结果回到窗口线程。
class QrWorker {
public:
    explicit QrWorker(HWND window);
    ~QrWorker();
    // 新图片替代尚未开始的请求，避免连续粘贴时积压识别任务。
    void submit(QrImage image, std::function<void(QrResult)> done);
    // 连续输入只处理最新待生成文本，不丢弃另一模式的识别任务。
    void generate(std::wstring text, std::function<void(QrCode)> done);
    // 丢弃等待任务并结束后台线程，返回后不会再投递结果。
    void stop();
private:
    struct Request {
        QrImage image;
        std::function<void(QrResult)> done;
        std::wstring text;
        std::function<void(QrCode)> generated;
    };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Request> pending_;
    std::optional<Request> pendingGeneration_;
    bool stopping_{};
    std::thread thread_;
};

// 创建仅在识别页接收图片的 OLE 拖放目标；调用方负责注册和释放。
IDropTarget* createQrDropTarget(std::function<bool()> enabled, std::function<void(IDataObject*)> drop, std::function<void(bool)> hover);
}
