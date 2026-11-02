#include "qrcode.hpp"
#include "CreateBarcode.h"
#include "ImageView.h"
#include <ole2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>

namespace qc {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}
}

QrCode generateQrCode(const std::wstring& text) {
    if (text.empty()) return {};
    try {
        if (text.size() > 10000) throw std::invalid_argument("Data too big");
        // 自带编码器的 ecLevel=3 对应 M；非 ASCII 文本会写入 UTF-8 ECI。
        auto barcode = ZXing::CreateBarcodeFromText(utf8(text), ZXing::CreatorOptions(ZXing::BarcodeFormat::QRCode, "ecLevel=3"));
        auto symbol = barcode.symbol();
        QrCode code; code.size = symbol.width() + 8;
        code.pixels.assign(static_cast<size_t>(code.size) * code.size, 0xffffffff);
        for (int y = 0; y < symbol.height(); ++y)
            for (int x = 0; x < symbol.width(); ++x)
                if (*symbol.data(x, y) == 0) code.pixels[static_cast<size_t>(y + 4) * code.size + x + 4] = 0xff000000;
        return code;
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("内容过长或无法编码，请缩短内容后重试。");
    }
}

std::wstring qrFileName(const std::wstring& text) {
    auto name = extractQrId(text);
    if (name.empty()) {
        SYSTEMTIME time{}; GetLocalTime(&time);
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%04u%02u%02u%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        name = buffer;
    }
    for (auto& c : name) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    if (name.size() > 128) { name.resize(128); if (name.back() >= 0xd800 && name.back() <= 0xdbff) name.pop_back(); }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"_";
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    CharUpperBuffW(stem.data(), static_cast<DWORD>(stem.size()));
    bool device = stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL";
    if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")))
        device = wcschr(L"123456789¹²³", stem.back()) != nullptr;
    if (device) name.insert(name.begin(), L'_');
    return name + L".png";
}

void saveQrCode(const QrCode& code, const std::filesystem::path& path) {
    if (code.size <= 0 || code.size > 185 || code.pixels.size() != static_cast<size_t>(code.size) * code.size)
        throw std::runtime_error("没有可保存的二维码。");
    int scale = std::max(1, 600 / code.size), size = code.size * scale;
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 3);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            auto value = static_cast<unsigned char>(code.pixels[static_cast<size_t>(y / scale) * code.size + x / scale]);
            auto offset = (static_cast<size_t>(y) * size + x) * 3;
            pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = value;
        }
    GUID guid{}; checked(CoCreateGuid(&guid), "无法创建图片临时文件名。");
    wchar_t suffix[40]{}; StringFromGUID2(guid, suffix, 40);
    auto temporary = path.parent_path() / (std::wstring(L".qrcode-") + suffix + L".tmp");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入所选位置，请检查目录权限或选择其他位置。");
    CloseHandle(file);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { DeleteFileW(path.c_str()); } } cleanup{temporary};
    {
        ComPtr<IWICImagingFactory> factory;
        checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "无法初始化 PNG 编码器。");
        ComPtr<IWICStream> stream;
        checked(factory->CreateStream(&stream), "无法创建图片数据流。");
        checked(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE), "无法写入图片文件。");
        ComPtr<IWICBitmapEncoder> encoder;
        checked(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "无法创建 PNG 编码器。");
        checked(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "无法初始化 PNG 文件。");
        ComPtr<IWICBitmapFrameEncode> frame;
        checked(encoder->CreateNewFrame(&frame, nullptr), "无法创建 PNG 图像。");
        checked(frame->Initialize(nullptr), "无法初始化 PNG 图像。");
        checked(frame->SetSize(size, size), "无法设置图片尺寸。");
        auto format = GUID_WICPixelFormat24bppBGR;
        checked(frame->SetPixelFormat(&format), "无法设置图片格式。");
        if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("PNG 编码器不支持所需的图片格式。");
        checked(frame->WritePixels(size, size * 3, static_cast<UINT>(pixels.size()), pixels.data()), "图片写入失败，请检查可用磁盘空间。");
        checked(frame->Commit(), "PNG 图像保存失败。");
        checked(encoder->Commit(), "PNG 文件保存失败。");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("图片保存失败，请检查文件是否被占用或选择其他位置。");
}
}
