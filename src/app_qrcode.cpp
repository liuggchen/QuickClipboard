#include "app.hpp"
#include <ole2.h>
#include <richedit.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>

namespace qc {

void App::initializeQr() {
    auto button = [&](const wchar_t* label, int id, HWND parent) {
        auto handle = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(small_), 0);
        SetWindowSubclass(handle, controlProc, 1, reinterpret_cast<DWORD_PTR>(this)); return handle;
    };
    qrRecognizeButton_ = button(L"识别", QrRecognize, window_); qrGenerateButton_ = button(L"生成", QrGenerate, window_);
    qrSelectImageButton_ = button(L"选择图片", QrSelectImage, list_);
    // 纯文本 Rich Edit 按内容自动显隐滚动条，空内容和短文本不占用右侧空间。
    qrInput_ = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, list_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(QrInput)), instance_, nullptr);
    if (!qrInput_ || !qrRecognizeButton_ || !qrGenerateButton_ || !qrSelectImageButton_) throw std::runtime_error("二维码界面创建失败。");
    SendMessageW(qrInput_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), 0);
    SendMessageW(qrInput_, EM_SETLIMITTEXT, 1000000, 0);
    SendMessageW(qrInput_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    SendMessageW(qrInput_, EM_SETBKGNDCOLOR, 0, card_);
    CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = ink_;
    SendMessageW(qrInput_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(qrInput_, EM_SETEVENTMASK, 0, ENM_CHANGE);
    SetWindowSubclass(qrInput_, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
    qrWorker_ = std::make_unique<QrWorker>(window_);
    qrDropTarget_ = createQrDropTarget([this] { return qrRecognitionPage() && ready_ && !saving_; },
        [this](IDataObject* data) { receiveQrImage(data); },
        [this](bool active) { if (qrDragging_ != active) { qrDragging_ = active; layoutQrControls(); InvalidateRect(list_, nullptr, FALSE); } });
    if (FAILED(RegisterDragDrop(window_, qrDropTarget_)) || FAILED(RegisterDragDrop(list_, qrDropTarget_)))
        throw std::runtime_error("图片拖放注册失败。");
}

// 撤销拖放后等待后台退出，确保窗口销毁时没有遗留回调访问界面。
void App::stopQr() {
    KillTimer(window_, QrGenerateTimer);
    if (qrDropTarget_) {
        RevokeDragDrop(list_); RevokeDragDrop(window_);
        qrDropTarget_->Release(); qrDropTarget_ = nullptr;
    }
    if (qrWorker_) qrWorker_->stop();
}

void App::pasteQrImage() {
    if (!qrRecognitionPage() || !ready_) return;
    Microsoft::WRL::ComPtr<IDataObject> data;
    if (FAILED(OleGetClipboard(&data))) { notice(L"剪贴板暂时无法读取，请稍后再按 Ctrl + V。"); return; }
    receiveQrImage(data.Get());
}

void App::selectQrImage() {
    if (!qrRecognitionPage() || !ready_ || saving_) return;
    try {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            throw std::runtime_error("无法打开图片选择对话框。");
        COMDLG_FILTERSPEC types[]{{L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp)", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico;*.webp"},
                                 {L"所有文件 (*.*)", L"*.*"}};
        FILEOPENDIALOGOPTIONS options{};
        if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR)) ||
            FAILED(dialog->SetFileTypes(2, types)) || FAILED(dialog->SetTitle(L"选择二维码图片")))
            throw std::runtime_error("无法设置图片选择对话框。");
        auto result = dialog->Show(window_);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { SetFocus(qrFocus()); return; }
        if (FAILED(result)) throw std::runtime_error("无法选择图片。");
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("无法读取所选图片。");
        PWSTR path{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) throw std::runtime_error("请选择本地图片文件。");
        struct Free { PWSTR value; ~Free() { CoTaskMemFree(value); } } free{path};
        QrImage image; image.file = path; scanQrImage(std::move(image));
    } catch (const std::exception& e) {
        notice(errorText(e));
        if (qrValues_.empty() && !qrScanning_) { qrMessage_ = errorText(e); InvalidateRect(list_, nullptr, FALSE); }
    }
    SetFocus(qrFocus());
}

void App::receiveQrImage(IDataObject* data) {
    if (!qrRecognitionPage() || !ready_) return;
    try { scanQrImage(readQrImage(data)); }
    catch (const std::exception& e) {
        notice(errorText(e));
        if (qrValues_.empty() && !qrScanning_) { qrMessage_ = errorText(e); InvalidateRect(list_, nullptr, FALSE); }
    }
}

void App::scanQrImage(QrImage image) {
    auto revision = ++qrRevision_;
    qrValues_.clear(); qrLayouts_.clear(); qrMessage_.clear(); status_.clear();
    qrScanning_ = true; scroll_ = 0; updateListMetrics(); redraw(); SetFocus(list_);
    qrWorker_->submit(std::move(image), [this, revision](QrResult result) {
        if (revision != qrRevision_) return;
        qrScanning_ = false; qrValues_ = std::move(result.values);
        qrMessage_ = result.error.empty() && qrValues_.empty() ? L"未识别到二维码，请换一张更清晰的图片。" : std::move(result.error);
        if (qrRecognitionPage()) { scroll_ = 0; updateListMetrics(); redraw(); }
    });
}

void App::measureQrResults() {
    qrLayouts_.clear(); rows_.clear();
    if (qrGenerateMode_) { totalHeight_ = 0; return; }
    RECT client{}; GetClientRect(list_, &client);
    HDC dc = GetDC(list_);
    auto measure = [&](const std::wstring& value, HFONT font, int minimum) {
        auto old = SelectObject(dc, font);
        TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
        auto wrapped = wrapFieldText(dc, value, std::max(px(20), static_cast<int>(client.right) - px(112)));
        FieldLayout field; field.lineHeight = std::max(20, logical(metrics.tmHeight));
        field.lines = std::move(wrapped.lines); field.valueHeight = static_cast<int>(field.lines.size()) * field.lineHeight;
        field.height = std::max(minimum, field.valueHeight + 28);
        SelectObject(dc, old); return field;
    };
    int y = qrValues_.empty() ? 0 : 42;
    for (const auto& value : qrValues_) {
        QrLayout shape; shape.original = measure(value.original, font_, 54);
        shape.height = shape.original.height;
        if (!value.id.empty()) { shape.id = measure(value.id, bold_, 48); shape.height += shape.id.height; }
        rows_.push_back({y, shape.height}); y += shape.height + 10;
        qrLayouts_.push_back(std::move(shape));
    }
    ReleaseDC(list_, dc);
    totalHeight_ = qrValues_.empty() ? 0 : y + 8;
}

void App::paintQrResults(HDC dc) {
    if (qrGenerateMode_) { paintQrGenerated(dc); return; }
    RECT client{}; GetClientRect(list_, &client);
    if (qrDragging_) {
        RECT area{px(18), px(8), client.right - px(18), client.bottom - px(8)};
        auto fill = CreateSolidBrush(accentTint_); FillRect(dc, &area, fill); DeleteObject(fill);
        text(dc, L"松开鼠标，识别图片中的二维码", area, bold_, accent_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (qrValues_.empty()) {
        int middle = std::max(20, viewportHeight_ / 2 - 75);
        auto title = qrScanning_ ? L"正在识别图片…" : L"解码二维码图片";
        text(dc, title, {px(20), px(middle), client.right - px(20), px(middle + 30)}, titleFont_, ink_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"Ctrl + V 粘贴图片", {px(28), px(middle + 43), client.right - px(28), px(middle + 70)}, font_, muted_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"拖拽图片到此处", {px(28), px(middle + 71), client.right - px(28), px(middle + 98)}, font_, muted_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!qrMessage_.empty() && !qrScanning_)
            text(dc, qrMessage_, {px(28), px(middle + 138), client.right - px(28), px(middle + 199)}, font_, muted_, DT_CENTER | DT_TOP | DT_WORDBREAK);
        return;
    }
    auto title = L"识别到 " + std::to_wstring(qrValues_.size()) + L" 个二维码 · 点击内容复制";
    text(dc, title, {px(20), px(6 - scroll_), client.right - px(118), px(32 - scroll_)}, small_, muted_);
    for (size_t i = 0; i < qrValues_.size() && i < qrLayouts_.size() && i < rows_.size(); ++i) {
        const auto& shape = qrLayouts_[i]; int top = rows_[i].first - scroll_;
        if (top + shape.height < 0 || top > viewportHeight_) continue;
        RECT outer{px(18), px(top), client.right - px(18), px(top + shape.height)};
        bool hot = (hoveredAction_ == CopyQrValue || hoveredAction_ == CopyQrId) && hoveredIndex_ == static_cast<int>(i);
        card(dc, outer, hot);
        auto paintValue = [&](const std::wstring& value, const FieldLayout& field, int rowTop, const wchar_t* label, int action, HFONT font, COLORREF color) {
            text(dc, label, {px(31), px(rowTop + 14), px(70), px(rowTop + 14 + field.lineHeight)}, small_, muted_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT valueRect{px(79), px(rowTop + 12), client.right - px(31), px(rowTop + field.height - 12)};
            hits_.push_back({valueRect, action, static_cast<int>(i)});
            if (value.empty()) text(dc, L"（空内容）", valueRect, font, muted_);
            for (size_t line = 0; line < field.lines.size(); ++line) {
                int y = rowTop + 14 + static_cast<int>(line) * field.lineHeight;
                if (y + field.lineHeight < 0 || y > viewportHeight_) continue;
                auto [offset, count] = field.lines[line];
                text(dc, std::wstring_view(value).substr(offset, count), {px(79), px(y), client.right - px(31), px(y + field.lineHeight)}, font, color, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
        };
        paintValue(qrValues_[i].original, shape.original, top, L"原始", CopyQrValue, font_, ink_);
        if (!qrValues_[i].id.empty()) {
            int idTop = top + shape.original.height;
            RECT divider{px(31), px(idTop), client.right - px(31), px(idTop + 1)};
            auto brush = CreateSolidBrush(line_); FillRect(dc, &divider, brush); DeleteObject(brush);
            paintValue(qrValues_[i].id, shape.id, idTop, L"ID", CopyQrId, bold_, accent_);
        }
    }
}

void App::copyQrResult(int index, bool id) {
    if (!qrRecognitionPage() || index < 0 || index >= static_cast<int>(qrValues_.size())) return;
    const auto& value = qrValues_[index];
    if (id && value.id.empty()) return;
    copy(id ? value.id : value.original, false, false);
}

void App::switchQrMode(bool generate) {
    if (!qrPage() || !ready_) return;
    qrGenerateMode_ = generate; qrDragging_ = false; scroll_ = 0; status_.clear();
    layout(); SetFocus(qrFocus());
}

// 选择图片入口在空页居中、结果页随标题滚动，底部模式切换保持在右侧。
void App::layoutQrControls() {
    bool visible = qrPage();
    ShowWindow(qrRecognizeButton_, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(qrGenerateButton_, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(qrInput_, visible && qrGenerateMode_ ? SW_SHOW : SW_HIDE);
    EnableWindow(qrInput_, ready_); EnableWindow(qrRecognizeButton_, ready_); EnableWindow(qrGenerateButton_, ready_);
    RECT client{}; GetClientRect(window_, &client);
    MoveWindow(qrRecognizeButton_, client.right - px(106), client.bottom - px(31), px(40), px(26), TRUE);
    MoveWindow(qrGenerateButton_, client.right - px(58), client.bottom - px(31), px(40), px(26), TRUE);
    GetClientRect(list_, &client);
    MoveWindow(qrInput_, px(32), px(20), std::max(px(40), static_cast<int>(client.right) - px(64)), px(66), TRUE);
    bool empty = qrValues_.empty();
    int width = px(empty ? 112 : 90);
    int left = empty ? (client.right - width) / 2 : client.right - px(18) - width;
    int top = empty ? std::max(20, viewportHeight_ / 2 - 75) + 99 : 2 - scroll_;
    MoveWindow(qrSelectImageButton_, left, px(top), width, px(empty ? 27 : 34), TRUE);
    ShowWindow(qrSelectImageButton_, qrRecognitionPage() && !qrDragging_ && (empty || top + 34 > 0) ? SW_SHOW : SW_HIDE);
    EnableWindow(qrSelectImageButton_, ready_ && !saving_);
}

void App::qrInputChanged() {
    KillTimer(window_, QrGenerateTimer); ++qrGenerateRevision_;
    qrText_ = windowText(qrInput_); qrCode_ = {}; status_.clear();
    qrGenerating_ = !qrText_.empty();
    if (qrGenerating_ && !qrComposing_) SetTimer(window_, QrGenerateTimer, 300, nullptr);
    if (qrPage()) { updateListMetrics(); redraw(); }
}

// 每次提交绑定输入版本，后台完成时只接受仍与输入框一致的结果。
void App::generateQrPreview() {
    KillTimer(window_, QrGenerateTimer);
    if (qrComposing_ || qrText_.empty()) return;
    auto revision = qrGenerateRevision_;
    qrWorker_->generate(qrText_, [this, revision](QrCode code) {
        if (revision != qrGenerateRevision_) return;
        qrGenerating_ = false; qrCode_ = std::move(code);
        if (qrPage() && qrGenerateMode_) { updateListMetrics(); redraw(); }
    });
}

void App::paintQrGenerated(HDC dc) {
    RECT client{}; GetClientRect(list_, &client);
    paintInput(dc, {px(20), px(8), client.right - px(20), px(98)}, GetFocus() == qrInput_, ready_);
    RECT area{px(24), px(114), client.right - px(24), client.bottom - px(16)};
    if (!qrCode_.size || qrGenerating_) {
        auto hint = qrGenerating_ ? L"正在生成二维码…" : (qrCode_.error.empty() ? L"输入内容后显示二维码" : qrCode_.error.c_str());
        RECT hintRect{area.left, (area.top + area.bottom) / 2 - px(22), area.right, (area.top + area.bottom) / 2 + px(42)};
        text(dc, hint, hintRect, font_, muted_, DT_CENTER | DT_TOP | DT_WORDBREAK); return;
    }
    int available = std::min({px(232), static_cast<int>(area.right - area.left), static_cast<int>(area.bottom - area.top)});
    int size = qrCode_.size * std::max(1, available / qrCode_.size);
    int left = (client.right - size) / 2, top = (area.top + area.bottom - size) / 2;
    BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = qrCode_.size; bitmap.bmiHeader.biHeight = -qrCode_.size;
    bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32; bitmap.bmiHeader.biCompression = BI_RGB;
    int previous = SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, left, top, size, size, 0, 0, qrCode_.size, qrCode_.size, qrCode_.pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, previous);
    hits_.push_back({{left, top, left + size, top + size}, SaveQrImage, 0});
}

void App::saveQrImage() {
    if (!qrPage() || !qrGenerateMode_ || qrGenerating_ || !qrCode_.size) return;
    // 系统对话框运行自己的消息循环，保存快照避免期间到达的异步结果改变内容。
    auto code = qrCode_; auto name = qrFileName(qrText_);
    try {
        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            throw std::runtime_error("无法打开保存对话框。");
        COMDLG_FILTERSPEC type{L"PNG 图片 (*.png)", L"*.png"};
        FILEOPENDIALOGOPTIONS options{};
        if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR | FOS_STRICTFILETYPES)) ||
            FAILED(dialog->SetFileTypes(1, &type)) || FAILED(dialog->SetDefaultExtension(L"png")) || FAILED(dialog->SetFileName(name.c_str())))
            throw std::runtime_error("无法设置图片保存位置。");
        dialog->SetTitle(L"保存二维码");
        auto result = dialog->Show(window_);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { SetFocus(qrFocus()); return; }
        if (FAILED(result)) throw std::runtime_error("无法选择保存位置。");
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("无法读取保存位置。");
        PWSTR path{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) throw std::runtime_error("请选择本地文件夹。");
        struct Free { PWSTR value; ~Free() { CoTaskMemFree(value); } } free{path};
        saveQrCode(code, path); notice(L"二维码已保存");
    } catch (const std::exception& e) { error(errorText(e)); }
    SetFocus(qrFocus());
}
}
