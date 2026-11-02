#include "qrcode.hpp"
#include "database.hpp"
#include "ReadBarcode.h"
#include "Barcode.h"
#include "ImageView.h"
#include <ole2.h>
#include <shellapi.h>
#include <urlmon.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <tuple>

namespace qc {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t MaxImageBytes = 128 * 1024 * 1024;
constexpr std::uint64_t MaxImagePixels = 24 * 1024 * 1024;

void checked(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}

// 在分配像素缓冲前限制图片尺寸，避免损坏图片或超大图耗尽内存。
void checkSize(std::uint64_t width, std::uint64_t height) {
    if (!width || !height || width > 32768 || height > 32768 || width * height > MaxImagePixels)
        throw std::runtime_error("图片尺寸过大或无效，请截取二维码区域后重试。");
}

std::array<FORMATETC, 5> imageFormats() {
    return {{{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
             {CF_DIBV5, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
             {CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
             {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
             {CF_BITMAP, nullptr, DVASPECT_CONTENT, -1, TYMED_GDI}}};
}

// 复制共享内存，确保剪贴板随后改变也不会影响正在识别的图片。
std::vector<unsigned char> copyGlobal(HGLOBAL handle) {
    auto size = GlobalSize(handle);
    if (!size || size > MaxImageBytes) throw std::runtime_error("图片数据为空或超过 128 MB。");
    auto source = static_cast<const unsigned char*>(GlobalLock(handle));
    if (!source) throw std::runtime_error("无法读取图片数据，请重试。");
    struct Unlock { HGLOBAL handle; ~Unlock() { GlobalUnlock(handle); } } unlock{handle};
    return {source, source + size};
}

// 将仅提供 GDI 位图的来源转换为独立的、从上到下排列的 32 位 DIB。
QrImage copyBitmap(HBITMAP bitmap) {
    BITMAP description{};
    if (!GetObjectW(bitmap, sizeof(description), &description) || description.bmWidth <= 0 || description.bmHeight <= 0)
        throw std::runtime_error("无法读取剪贴板位图。");
    checkSize(description.bmWidth, description.bmHeight);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = description.bmWidth;
    info.bmiHeader.biHeight = -description.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    QrImage image; image.dib = true;
    image.bytes.resize(sizeof(BITMAPINFOHEADER) + static_cast<size_t>(description.bmWidth) * description.bmHeight * 4);
    auto dc = GetDC(nullptr);
    int rows = GetDIBits(dc, bitmap, 0, description.bmHeight, image.bytes.data() + sizeof(BITMAPINFOHEADER), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (rows != description.bmHeight) throw std::runtime_error("无法复制剪贴板位图。");
    std::memcpy(image.bytes.data(), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
    return image;
}

// 校验 DIB 的表头、颜色表和像素边界，再补上 WIC 所需的 BMP 文件头。
std::vector<unsigned char> dibFile(const std::vector<unsigned char>& dib, bool& opaque) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) throw std::runtime_error("剪贴板位图数据不完整。");
    BITMAPINFOHEADER header{}; std::memcpy(&header, dib.data(), sizeof(header));
    if ((header.biSize != 40 && header.biSize != 52 && header.biSize != 56 && header.biSize != 108 && header.biSize != 124) ||
        header.biSize > dib.size() || header.biPlanes != 1 || header.biWidth <= 0 || header.biHeight == 0)
        throw std::runtime_error("不支持此剪贴板位图格式。");
    auto height = static_cast<std::uint64_t>(header.biHeight < 0 ? -static_cast<std::int64_t>(header.biHeight) : header.biHeight);
    checkSize(header.biWidth, height);
    if (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8 && header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32)
        throw std::runtime_error("不支持此剪贴板位图色深。");
    if (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS && header.biCompression != 6)
        throw std::runtime_error("不支持此剪贴板位图压缩格式。");
    if (header.biCompression != BI_RGB && header.biBitCount != 16 && header.biBitCount != 32)
        throw std::runtime_error("剪贴板位图颜色掩码无效。");
    size_t colors = header.biClrUsed;
    if (header.biBitCount <= 8) {
        size_t maximum = size_t{1} << header.biBitCount;
        if (colors > maximum) throw std::runtime_error("剪贴板位图颜色表无效。");
        if (!colors) colors = maximum;
    } else if (colors > 256) throw std::runtime_error("剪贴板位图颜色表过大。");
    size_t masks = header.biSize == 40 && header.biCompression != BI_RGB ? (header.biCompression == 6 ? 16 : 12) : 0;
    size_t offset = header.biSize + masks + colors * sizeof(RGBQUAD);
    auto stride = (static_cast<std::uint64_t>(header.biWidth) * header.biBitCount + 31) / 32 * 4;
    if (offset > dib.size() || stride * height > dib.size() - offset) throw std::runtime_error("剪贴板位图像素数据不完整。");
    // 普通 32 位 DIB 的第四字节不是透明度，不能把全零保留字节当成透明像素。
    DWORD alphaMask{};
    if (header.biBitCount == 32 && (header.biSize >= 56 || (header.biSize == 40 && masks == 16)))
        std::memcpy(&alphaMask, dib.data() + 52, sizeof(alphaMask));
    opaque = alphaMask == 0;
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof(file) + offset);
    std::vector<unsigned char> bytes(file.bfSize);
    std::memcpy(bytes.data(), &file, sizeof(file));
    std::memcpy(bytes.data() + sizeof(file), dib.data(), dib.size());
    return bytes;
}

// 图片文件读取也在后台完成，原图不写入数据库或临时文件。
std::vector<unsigned char> readImageFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("无法打开图片文件。");
    auto length = file.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > MaxImageBytes)
        throw std::runtime_error("图片文件为空或超过 128 MB。");
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("图片文件读取不完整，请重试。");
    return bytes;
}

// 以复制方式接受图片拖放，仅在二维码页显示可放入状态。
class QrDropTarget final : public IDropTarget {
public:
    QrDropTarget(std::function<bool()> enabled, std::function<void(IDataObject*)> drop, std::function<void(bool)> hover)
        : enabled_(std::move(enabled)), drop_(std::move(drop)), hover_(std::move(hover)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
        *object = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override { auto remaining = InterlockedDecrement(&references_); if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        supported_ = false;
        if (data && enabled_()) for (auto format : imageFormats()) if (data->QueryGetData(&format) == S_OK) { supported_ = true; break; }
        return update(effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override { return update(effect); }
    HRESULT STDMETHODCALLTYPE DragLeave() override { supported_ = false; hover_(false); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        if (!effect) return E_POINTER;
        bool accepted = supported_ && enabled_() && (*effect & DROPEFFECT_COPY);
        *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        supported_ = false; hover_(false);
        if (accepted) drop_(data);
        return S_OK;
    }
private:
    HRESULT update(DWORD* effect) {
        if (!effect) return E_POINTER;
        bool accepted = supported_ && enabled_() && (*effect & DROPEFFECT_COPY);
        *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE; hover_(accepted); return S_OK;
    }
    LONG references_ = 1;
    bool supported_{};
    std::function<bool()> enabled_;
    std::function<void(IDataObject*)> drop_;
    std::function<void(bool)> hover_;
};
}

QrImage readQrImage(IDataObject* data) {
    if (!data) throw std::runtime_error("没有可读取的图片。");
    for (auto format : imageFormats()) {
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&format, &medium))) continue;
        struct Release { STGMEDIUM& medium; ~Release() { ReleaseStgMedium(&medium); } } release{medium};
        if (format.cfFormat == CF_BITMAP) {
            if (medium.tymed == TYMED_GDI && medium.hBitmap) return copyBitmap(medium.hBitmap);
            continue;
        }
        if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) continue;
        if (format.cfFormat == CF_HDROP) {
            auto drop = static_cast<HDROP>(medium.hGlobal);
            if (DragQueryFileW(drop, 0xffffffff, nullptr, 0) != 1) throw std::runtime_error("请一次放入一张图片。");
            UINT size = DragQueryFileW(drop, 0, nullptr, 0);
            if (!size || size >= 32768) throw std::runtime_error("图片文件路径无效。");
            std::wstring path(size + 1, L'\0');
            if (DragQueryFileW(drop, 0, path.data(), size + 1) != size) throw std::runtime_error("无法读取图片文件路径。");
            path.resize(size); QrImage image; image.file = std::move(path); return image;
        }
        QrImage image; image.dib = format.cfFormat == CF_DIB || format.cfFormat == CF_DIBV5;
        image.bytes = copyGlobal(medium.hGlobal); return image;
    }
    throw std::runtime_error("没有可识别的图片，请复制图片或拖入本地图片文件。");
}

std::wstring extractQrId(const std::wstring& value) {
    auto scheme = value.find(L"://");
    if (scheme == std::wstring::npos || std::any_of(value.begin(), value.end(), [](wchar_t c) { return c <= L' ' || c == L'\\'; })) return {};
    auto authorityEnd = value.find_first_of(L"/?#", scheme + 3);
    if (authorityEnd == std::wstring::npos || authorityEnd == scheme + 3) return {};
    auto separator = value.rfind(L'=');
    if (separator == std::wstring::npos) separator = value.rfind(L'/');
    if (separator == std::wstring::npos || separator < authorityEnd || separator + 1 == value.size()) return {};
    ComPtr<IUri> uri;
    if (FAILED(CreateUri(value.c_str(), Uri_CREATE_NO_CANONICALIZE | Uri_CREATE_NO_DECODE_EXTRA_INFO, 0, &uri))) return {};
    DWORD hostLength{};
    if (FAILED(uri->GetPropertyLength(Uri_PROPERTY_HOST, &hostLength, 0)) || !hostLength) return {};
    // URL 解析仅用于校验，截取仍使用原文，保留前导零和百分号编码等内容。
    return value.substr(separator + 1);
}

QrResult recognizeQrImage(const QrImage& image) {
    bool opaque = false;
    auto bytes = image.file.empty() ? (image.dib ? dibFile(image.bytes, opaque) : image.bytes) : readImageFile(image.file);
    if (bytes.empty() || bytes.size() > MaxImageBytes + sizeof(BITMAPFILEHEADER)) throw std::runtime_error("图片数据大小无效。");
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "无法初始化 Windows 图片解码器。");
    ComPtr<IWICStream> stream;
    checked(factory->CreateStream(&stream), "无法创建图片数据流。");
    checked(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size())), "无法读取图片数据流。");
    ComPtr<IWICBitmapDecoder> decoder;
    checked(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "无法读取图片，请使用 PNG、JPEG 或 BMP 等图片文件。");
    ComPtr<IWICBitmapFrameDecode> frame;
    checked(decoder->GetFrame(0, &frame), "无法读取图片内容。");
    UINT width{}, height{}; checked(frame->GetSize(&width, &height), "无法读取图片尺寸。");
    checkSize(width, height);
    ComPtr<IWICFormatConverter> converter;
    checked(factory->CreateFormatConverter(&converter), "无法转换图片格式。");
    checked(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "无法转换图片像素。");
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    checked(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()), "图片像素读取失败。");
    std::vector<unsigned char> gray(static_cast<size_t>(width) * height);
    // 透明背景按白色合成，避免透明二维码在灰度转换后变成全黑。
    for (size_t i = 0; i < gray.size(); ++i) {
        const auto* pixel = pixels.data() + i * 4;
        unsigned alpha = opaque ? 255 : pixel[3];
        unsigned luminance = (29 * pixel[0] + 150 * pixel[1] + 77 * pixel[2] + 128) >> 8;
        gray[i] = static_cast<unsigned char>((luminance * alpha + 255 * (255 - alpha) + 127) / 255);
    }
    auto options = ZXing::ReaderOptions().formats(ZXing::BarcodeFormat::QRCode).tryHarder(true).tryRotate(true).tryInvert(true).tryDownscale(true).maxNumberOfSymbols(64);
    auto found = ZXing::ReadBarcodes(ZXing::ImageView(gray.data(), static_cast<int>(width), static_cast<int>(height), ZXing::ImageFormat::Lum), options);
    auto position = [](const ZXing::Barcode& code) { int x = 32768, y = 32768; for (auto point : code.position()) { x = std::min(x, point.x); y = std::min(y, point.y); } return std::pair{y, x}; };
    std::stable_sort(found.begin(), found.end(), [&](const auto& a, const auto& b) { return position(a) < position(b); });
    QrResult result;
    bool binary = false;
    for (const auto& code : found) {
        if (!code.isValid()) continue;
        auto value = wide(code.text(ZXing::TextMode::Plain));
        if (value.find(L'\0') != std::wstring::npos) { binary = true; continue; }
        if (std::none_of(result.values.begin(), result.values.end(), [&](const auto& item) { return item.original == value; })) {
            auto id = extractQrId(value);
            result.values.push_back({std::move(value), std::move(id)});
        }
    }
    if (result.values.empty() && binary) result.error = L"二维码包含非文本数据，无法复制为文字。";
    return result;
}

QrWorker::QrWorker(HWND window) : thread_([this, window] {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Request request;
        {
            std::unique_lock lock(mutex_); cv_.wait(lock, [this] { return stopping_ || pending_ || pendingGeneration_; });
            if (stopping_) break;
            auto& pending = pending_ ? pending_ : pendingGeneration_;
            request = std::move(*pending); pending.reset();
        }
        QrResult result; QrCode code;
        try {
            checked(com, "无法初始化二维码处理线程。");
            if (request.generated) code = generateQrCode(request.text);
            else result = recognizeQrImage(request.image);
        } catch (const std::exception& e) {
            if (request.generated) code.error = errorText(e);
            else result.error = errorText(e);
        }
        {
            std::lock_guard lock(mutex_);
            if (stopping_) break;
        }
        if (request.generated) complete(window, [done = std::move(request.generated), code = std::move(code)]() mutable { done(std::move(code)); });
        else complete(window, [done = std::move(request.done), result = std::move(result)]() mutable { done(std::move(result)); });
    }
    if (SUCCEEDED(com)) CoUninitialize();
}) {}
QrWorker::~QrWorker() { stop(); }
void QrWorker::submit(QrImage image, std::function<void(QrResult)> done) {
    { std::lock_guard lock(mutex_); if (stopping_) return; pending_ = Request{std::move(image), std::move(done), {}, {}}; }
    cv_.notify_one();
}
void QrWorker::generate(std::wstring text, std::function<void(QrCode)> done) {
    { std::lock_guard lock(mutex_); if (stopping_) return; pendingGeneration_ = Request{{}, {}, std::move(text), std::move(done)}; }
    cv_.notify_one();
}
void QrWorker::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; pending_.reset(); pendingGeneration_.reset(); }
    cv_.notify_one(); if (thread_.joinable()) thread_.join();
}

IDropTarget* createQrDropTarget(std::function<bool()> enabled, std::function<void(IDataObject*)> drop, std::function<void(bool)> hover) {
    return new QrDropTarget(std::move(enabled), std::move(drop), std::move(hover));
}
}
