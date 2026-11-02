#include "app.hpp"
#include "startup.hpp"
#include <windowsx.h>
#include <richedit.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <ole2.h>
#include <algorithm>
#include <stdexcept>

namespace qc {
namespace {
constexpr int HistoryTab = 101, RecordsTab = 102, Search = 103, New = 105, SettingsButton = 106, Minimize = 107, Close = 108, GroupMore = 109, SearchClear = 110, HistoryClear = 111, QrTab = 112, GroupFilterBase = 10000;
constexpr int Name = 200, GroupCombo = 201, Note = 202, AddField = 203, Save = 204, Cancel = 205, DeleteRecord = 206;
constexpr int FieldBase = 1000;
constexpr int Open = 1, PasteField = 2, CopyField = 3, Reveal = 4, Edit = 5, LoadMore = 6, Append = 7, FullText = 8;
constexpr int RecordHeader = 44, FoldedRecord = 66, EditorHeader = 66, FieldStart = 44;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT SearchTimer = 1, CaptureTimer = 2, SecretTimer = 3, PasteTimer = 4, CopyTimer = 5, StatusTimer = 6, PruneTimer = 7;
constexpr int PageSize = 80;

std::wstring age(std::int64_t value) {
    auto seconds = std::max<Id>(0, now() - value);
    if (seconds < 60) return L"刚刚";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" 分钟前";
    if (seconds < 86400) return std::to_wstring(seconds / 3600) + L" 小时前";
    return std::to_wstring(seconds / 86400) + L" 天前";
}
std::wstring oneLine(std::wstring value) {
    for (auto& c : value) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    return value;
}
bool checked(HWND parent, int id) { return SendDlgItemMessageW(parent, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void check(HWND parent, int id, bool value) { SendDlgItemMessageW(parent, id, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
int integer(HWND parent, int id, int low, int high) {
    auto text = windowText(GetDlgItem(parent, id));
    size_t end{}; int n{};
    try { n = std::stoi(text, &end); } catch (...) { throw std::runtime_error("请输入有效的数字"); }
    if (end != text.size() || n < low || n > high) throw std::runtime_error("设置数值超出允许范围");
    return n;
}
Id comboId(HWND combo) {
    auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return index == CB_ERR ? 0 : SendMessageW(combo, CB_GETITEMDATA, index, 0);
}
std::wstring hotkeyLabel(const Settings& settings) {
    std::wstring result;
    if (settings.modifiers & MOD_CONTROL) result += L"Ctrl + ";
    if (settings.modifiers & MOD_SHIFT) result += L"Shift + ";
    if (settings.modifiers & MOD_ALT) result += L"Alt + ";
    if (settings.modifiers & MOD_WIN) result += L"Win + ";
    wchar_t name[64]{}; auto scan = MapVirtualKeyW(settings.key, MAPVK_VK_TO_VSC);
    GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64);
    return result + name;
}
DWORD integrity(HANDLE process) {
    HANDLE token{};
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return MAXDWORD;
    DWORD size{}; GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    DWORD result = MAXDWORD;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
        auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
        auto count = *GetSidSubAuthorityCount(label->Label.Sid);
        result = *GetSidSubAuthority(label->Label.Sid, count - 1);
    }
    CloseHandle(token); return result;
}
}

App::App(HINSTANCE instance, std::filesystem::path dir) : instance_(instance), dir_(std::move(dir)) {}
App::~App() {
    stopQr();
    winVHotkey_.stop();
    if (worker_) worker_->stop();
    MSG msg{};
    while (PeekMessageW(&msg, window_, WM_COMPLETE, WM_COMPLETE, PM_REMOVE)) delete reinterpret_cast<std::function<void()>*>(msg.lParam);
    while (PeekMessageW(&msg, window_, WM_DATABASE_ERROR, WM_DATABASE_ERROR, PM_REMOVE)) delete reinterpret_cast<DatabaseError*>(msg.lParam);
    for (auto& value : revealed_) wipe(value.second);
    wipe(appendValue_);
    if (pendingCopy_) wipe(*pendingCopy_);
    for (HFONT font : {font_, bold_, titleFont_, small_}) if (font) DeleteObject(font);
    if (background_) DeleteObject(background_);
    if (cardBrush_) DeleteObject(cardBrush_);
    if (icon_) DestroyIcon(icon_);
    if (smallIcon_) DestroyIcon(smallIcon_);
    if (richEdit_) FreeLibrary(richEdit_);
    if (oleInitialized_) OleUninitialize();
}
void App::loadIcons() {
    auto nextLarge = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, px(32), px(32), 0));
    auto nextSmall = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, px(16), px(16), 0));
    if (!nextLarge || !nextSmall) { if (nextLarge) DestroyIcon(nextLarge); if (nextSmall) DestroyIcon(nextSmall); throw std::runtime_error("随手贴图标资源无法加载"); }
    if (window_) {
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(nextLarge));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(nextSmall));
        SetClassLongPtrW(window_, GCLP_HICON, reinterpret_cast<LONG_PTR>(nextLarge));
        SetClassLongPtrW(window_, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(nextSmall));
    }
    if (icon_) DestroyIcon(icon_); if (smallIcon_) DestroyIcon(smallIcon_);
    icon_ = nextLarge; smallIcon_ = nextSmall;
    if (window_) tray(false);
}
int App::run(bool hidden) {
    silentLaunch_ = hidden;
    if (FAILED(OleInitialize(nullptr))) throw std::runtime_error("无法初始化图片剪贴板和拖放支持。");
    oleInitialized_ = true;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS}; InitCommonControlsEx(&common);
    richEdit_ = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit_) throw std::runtime_error("Windows Rich Edit 控件无法加载");
    dpi_ = GetDpiForSystem(); loadIcons();
    auto className = L"QuickClipboard-" + wide(sha256(dir_.wstring())).substr(0, 24);
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = windowProc; cls.hInstance = instance_; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = className.c_str(); cls.hIcon = icon_; cls.hIconSm = smallIcon_;
    RegisterClassExW(&cls);
    cls.lpszClassName = L"QuickClipboardList"; cls.lpfnWndProc = listProc; cls.hIcon = nullptr; cls.hIconSm = nullptr; RegisterClassExW(&cls);
    cls.lpszClassName = L"QuickClipboardRecordDrag"; cls.lpfnWndProc = recordGhostProc; RegisterClassExW(&cls);
    constexpr DWORD style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    window_ = CreateWindowExW(WS_EX_APPWINDOW, className.c_str(), AppDisplayName, style, CW_USEDEFAULT, CW_USEDEFAULT, px(440), px(548), nullptr, nullptr, instance_, this);
    if (!window_) throw std::runtime_error("无法创建随手贴窗口");
    initialize();
    if (!hidden) show();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (key(msg)) continue;
        if (!IsDialogMessageW(window_, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return static_cast<int>(msg.wParam);
}
LRESULT CALLBACK App::windowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); app->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window, msg, wp, lp);
    try { return app->message(msg, wp, lp); } catch (const std::exception& e) { app->error(errorText(e)); return 0; }
}
LRESULT CALLBACK App::listProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); app->list_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); }
    if (!app) return DefWindowProcW(window, msg, wp, lp);
    try { return app->listMessage(msg, wp, lp); } catch (const std::exception& e) { app->error(errorText(e)); return 0; }
}
// 保留原生控件操作，并统一处理输入提示、焦点与按钮悬停反馈。
LRESULT CALLBACK App::controlProc(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto app = reinterpret_cast<App*>(ref);
    if (window == app->qrInput_ && (msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION)) {
        app->qrComposing_ = msg == WM_IME_STARTCOMPOSITION;
        auto result = DefSubclassProc(window, msg, wp, lp);
        app->qrInputChanged(); return result;
    }
    if (window == app->search_ && (msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION)) {
        // 输入法组合期间由原生控件绘制，避免提示文字遮挡未提交的中文。
        app->searchComposing_ = msg == WM_IME_STARTCOMPOSITION; InvalidateRect(window, nullptr, FALSE);
    }
    // 保留原生输入行为，空内容提示与应用主题一致，二维码输入不需要额外标签。
    if ((window == app->search_ || window == app->qrInput_) && msg == WM_PAINT) {
        auto result = DefSubclassProc(window, msg, wp, lp);
        bool qr = window == app->qrInput_;
        if (!GetWindowTextLengthW(window) && !(qr ? app->qrComposing_ : app->searchComposing_)) {
            bool focused = GetFocus() == window; if (focused) HideCaret(window);
            auto dc = GetDC(window); RECT rect{}; GetClientRect(window, &rect); FillRect(dc, &rect, app->cardBrush_);
            auto hint = qr ? L"输入或粘贴文字、网址，自动生成二维码" : (app->tab_ ? L"搜索名称、分组或普通字段…" : L"搜索剪贴板历史…");
            app->text(dc, hint, rect, app->font_, app->muted_, qr ? DT_LEFT | DT_TOP | DT_WORDBREAK : DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ReleaseDC(window, dc); if (focused) ShowCaret(window);
        }
        return result;
    }
    if (window == app->qrInput_) {
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) { InvalidateRect(app->list_, nullptr, FALSE); InvalidateRect(window, nullptr, FALSE); }
        return DefSubclassProc(window, msg, wp, lp);
    }
    auto form = std::find_if(app->controls_.begin(), app->controls_.end(), [window](const Control& c) { return c.window == window; });
    bool combo = form != app->controls_.end() && form->kind == ControlKind::Combo;
    bool check = form != app->controls_.end() && form->kind == ControlKind::Check;
    bool button = form != app->controls_.end() && form->kind == ControlKind::Button;
    bool hotkey = form != app->controls_.end() && form->kind == ControlKind::Hotkey;
    if ((combo || check) && msg == WM_PAINT) {
        PAINTSTRUCT ps{}; auto dc = BeginPaint(window, &ps); app->paintChoice(window, dc, combo); EndPaint(window, &ps); return 0;
    }
    if (hotkey && msg == WM_PAINT) {
        PAINTSTRUCT ps{}; auto dc = BeginPaint(window, &ps); RECT rect{}; GetClientRect(window, &rect); FillRect(dc, &rect, app->cardBrush_);
        WORD value = static_cast<WORD>(SendMessageW(window, HKM_GETHOTKEY, 0, 0)); BYTE modifiers = HIBYTE(value);
        Settings settings; settings.key = LOBYTE(value); settings.modifiers = 0;
        if (modifiers & HOTKEYF_CONTROL) settings.modifiers |= MOD_CONTROL;
        if (modifiers & HOTKEYF_SHIFT) settings.modifiers |= MOD_SHIFT;
        if (modifiers & HOTKEYF_ALT) settings.modifiers |= MOD_ALT;
        bool enabled = IsWindowEnabled(window);
        app->text(dc, enabled ? (settings.key ? hotkeyLabel(settings) : L"按下快捷键…") : L"无需额外设置", rect, app->font_, enabled ? app->ink_ : app->muted_);
        EndPaint(window, &ps); return 0;
    }
    if ((combo || check || hotkey) && msg == WM_ERASEBKGND) return 1;
    if (hotkey && msg == WM_NCPAINT) return 0;
    bool qrLink = window == app->qrSelectImageButton_;
    if (qrLink && msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && IsWindowEnabled(window)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    }
    if (GetParent(window) == app->window_ || combo || check || button || qrLink) {
        if (msg == WM_MOUSEMOVE && app->hoveredButton_ != window) {
            auto previous = app->hoveredButton_; app->hoveredButton_ = window;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
            if (previous) InvalidateRect(previous, nullptr, FALSE);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (msg == WM_MOUSELEAVE) { if (app->hoveredButton_ == window) app->hoveredButton_ = nullptr; InvalidateRect(window, nullptr, FALSE); }
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE) {
        InvalidateRect(GetParent(window), nullptr, FALSE); InvalidateRect(window, nullptr, FALSE);
    }
    if (GetParent(window) == app->window_) return DefSubclassProc(window, msg, wp, lp);
    int controlId = GetDlgCtrlID(window);
    bool handle = controlId >= FieldBase && (controlId - FieldBase) % 10 == 6;
    if (handle && app->draft_) {
        if (msg == WM_LBUTTONDOWN) { app->dragIndex_ = (controlId - FieldBase) / 10; SetCapture(window); return 0; }
        if (msg == WM_MOUSEMOVE && GetCapture() == window) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; MapWindowPoints(window, app->list_, &p, 1);
            if (p.y < app->px(20)) app->setScroll(app->scroll_ - 18);
            if (p.y > app->px(app->viewportHeight_ - 20)) app->setScroll(app->scroll_ + 18);
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return 0;
        }
        if (msg == WM_LBUTTONUP && GetCapture() == window) {
            int from = app->dragIndex_; POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; MapWindowPoints(window, app->list_, &p, 1);
            ReleaseCapture(); app->dragIndex_ = -1;
            int first = 0; for (size_t i = 0; i < app->items_.size(); ++i) if (app->items_[i].id == app->expanded_.id) { first = app->rows_[i].first + EditorHeader + 231; break; }
            int to = std::clamp((app->logical(p.y) + app->scroll_ - first) / 153, 0, static_cast<int>(app->draft_->fields.size()) - 1);
            if (from >= 0 && from != to) { app->collectDraft(); auto field = app->draft_->fields[from]; app->draft_->fields.erase(app->draft_->fields.begin() + from); app->draft_->fields.insert(app->draft_->fields.begin() + to, field); app->dirty_ = true; app->buildEditor(); }
            return 0;
        }
        if (msg == WM_CAPTURECHANGED) app->dragIndex_ = -1;
    }
    if (msg == WM_MOUSEWHEEL) return SendMessageW(app->list_, msg, wp, lp);
    if (msg == WM_SETFOCUS && GetParent(window) == app->list_) {
        RECT r{}; GetWindowRect(window, &r); MapWindowPoints(nullptr, app->list_, reinterpret_cast<POINT*>(&r), 2);
        RECT view{}; GetClientRect(app->list_, &view);
        if (r.top < 0) app->setScroll(app->scroll_ + app->logical(r.top) - 8);
        else if (r.bottom > view.bottom) app->setScroll(app->scroll_ + app->logical(r.bottom - view.bottom) + 8);
    }
    return DefSubclassProc(window, msg, wp, lp);
}

// 建立监听和后台数据任务，并以实际系统启动项同步自启动开关。
void App::initialize() {
    theme(); fonts();
    auto button = [&](const wchar_t* label, int id) { auto w = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr); SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font_), 0); SetWindowSubclass(w, controlProc, 1, reinterpret_cast<DWORD_PTR>(this)); return w; };
    historyTab_ = button(L"剪贴板历史", HistoryTab); recordsTab_ = button(L"常用资料", RecordsTab); settingsButton_ = button(L"设置", SettingsButton); new_ = button(L"+ 新建", New);
    qrTab_ = button(L"二维码", QrTab);
    minimizeButton_ = button(L"最小化", Minimize); closeButton_ = button(L"关闭", Close); groupMore_ = button(L"更多 ▾", GroupMore);
    footerSave_ = button(L"保存", Save); footerCancel_ = button(L"取消", Cancel);
    searchClear_ = button(L"清除搜索", SearchClear); historyClear_ = button(L"清空历史", HistoryClear);
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Search)), instance_, nullptr);
    SetWindowSubclass(search_, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), 0); SendMessageW(search_, EM_SETLIMITTEXT, 512, 0);
    list_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"QuickClipboardList", L"记录列表", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 0, 0, window_, nullptr, instance_, this);
    if (!list_ || !search_) throw std::runtime_error("原生界面控件创建失败");
    initializeQr();
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    SendMessageW(tooltips_, TTM_SETMAXTIPWIDTH, 0, px(320));
    TOOLINFOW listTip{sizeof(listTip)}; listTip.hwnd = list_; listTip.uId = 1; listTip.uFlags = TTF_SUBCLASS; listTip.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&listTip));
    MARGINS margins{1, 1, 1, 1}; DwmExtendFrameIntoClientArea(window_, &margins);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated"); tray(true);
    worker_ = std::make_unique<Worker>(window_, dir_ / DatabaseFileName);
    worker_->post([this](Database& db) {
        auto settings = db.settings(); db.prune(settings); auto groups = db.groups();
        std::wstring startupError;
        try {
            settings.startup = synchronizeStartup(db, executablePath(), dir_);
        } catch (const std::exception& e) { startupError = errorText(e); }
        complete(window_, [this, settings, groups = std::move(groups), startupError] {
            settings_ = settings; groups_ = groups; ready_ = true; theme();
            if (!startupError.empty()) notice(startupError);
            if (!applyHotkey(settings_)) notice(L"呼出快捷键接管失败，请在设置中选择其他组合键。");
            if (!AddClipboardFormatListener(window_)) error(L"剪贴板监听注册失败。请重启应用重试。");
            SetTimer(window_, PruneTimer, 60 * 60 * 1000, nullptr);
            updateGroupFilters(); layout(); refresh(); tray(false);
        });
    });
    layout();
}

// 根据系统应用主题或用户选择更新原生绘制颜色。
void App::theme() {
    DWORD light = 1, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    dark_ = settings_.theme == 2 || (settings_.theme == 0 && light == 0);
    bg_ = dark_ ? RGB(17, 17, 17) : RGB(247, 247, 247);
    card_ = dark_ ? RGB(25, 25, 25) : RGB(255, 255, 255);
    ink_ = dark_ ? RGB(220, 220, 220) : RGB(25, 25, 25);
    muted_ = dark_ ? RGB(153, 153, 153) : RGB(136, 136, 136);
    line_ = dark_ ? RGB(51, 51, 51) : RGB(229, 229, 229);
    accent_ = dark_ ? RGB(7, 193, 96) : RGB(6, 174, 86);
    accentFill_ = RGB(7, 193, 96);
    accentInk_ = RGB(255, 255, 255);
    accentTint_ = dark_ ? RGB(22, 57, 38) : RGB(230, 247, 238);
    if (background_) DeleteObject(background_); background_ = CreateSolidBrush(bg_);
    if (cardBrush_) DeleteObject(cardBrush_); cardBrush_ = CreateSolidBrush(card_);
    bool wasInitializing = initializing_; initializing_ = true;
    if (qrInput_) {
        SendMessageW(qrInput_, EM_SETBKGNDCOLOR, 0, card_);
        CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = ink_;
        SendMessageW(qrInput_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    }
    for (const auto& control : controls_) {
        wchar_t kind[64]{}; GetClassNameW(control.window, kind, 64);
        if (wcscmp(kind, MSFTEDIT_CLASS) == 0) {
            SendMessageW(control.window, EM_SETBKGNDCOLOR, 0, card_);
            CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = ink_;
            SendMessageW(control.window, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
        }
    }
    initializing_ = wasInitializing;
    BOOL dark = dark_; DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    redraw();
}
void App::fonts() {
    for (HFONT f : {font_, bold_, titleFont_, small_}) if (f) DeleteObject(f);
    auto create = [&](int height, int weight) { return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI"); };
    font_ = create(13, FW_NORMAL); bold_ = create(13, FW_SEMIBOLD); titleFont_ = create(14, FW_SEMIBOLD); small_ = create(11, FW_NORMAL);
    for (auto w : {historyTab_, recordsTab_, qrTab_, settingsButton_, minimizeButton_, closeButton_, groupMore_, new_, search_, searchClear_, historyClear_, footerSave_, footerCancel_, qrInput_, qrRecognizeButton_, qrGenerateButton_, qrSelectImageButton_}) if (w) SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (auto w : groupButtons_) if (w) SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (auto& c : controls_) {
        SendMessageW(c.window, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (c.kind == ControlKind::Combo) { SendMessageW(c.window, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(28)); SendMessageW(c.window, CB_SETITEMHEIGHT, 0, px(30)); }
    }
}
void App::layout() {
    if (!list_) return;
    RECT r{}; GetClientRect(window_, &r); int w = logical(r.right), h = logical(r.bottom);
    HDWP positions = BeginDeferWindowPos(16);
    auto move = [&](HWND child, int x, int y, int width, int height) { if (positions) positions = DeferWindowPos(positions, child, nullptr, px(x), px(y), px(std::max(1, width)), px(height), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW); };
    move(settingsButton_, w - 132, 4, 48, 28); move(minimizeButton_, w - 80, 4, 34, 28); move(closeButton_, w - 42, 4, 34, 28);
    int tabWidth = (w - 36) / 3;
    move(historyTab_, 18, 43, tabWidth, 33); move(recordsTab_, 18 + tabWidth, 43, tabWidth, 33); move(qrTab_, 18 + tabWidth * 2, 43, w - 36 - tabWidth * 2, 33);
    bool normal = !settingsMode_ && !groupsMode_ && tab_ != 2;
    ShowWindow(search_, normal ? SW_SHOW : SW_HIDE);
    EnableWindow(search_, !draft_ && !saving_);
    bool groupsVisible = normal && tab_ == 1 && !appendMode_;
    ShowWindow(new_, groupsVisible ? SW_SHOW : SW_HIDE);
    move(new_, w - 90, 127, 70, 28);
    int top = normal ? (groupsVisible ? 166 : 130) : 87;
    bool footer = draft_ != nullptr || settingsMode_ || groupsMode_ || appendMode_;
    int footerHeight = footer ? 58 : 34;
    move(list_, 0, top, w, std::max(40, h - top - footerHeight));
    viewportHeight_ = std::max(40, h - top - footerHeight);
    ShowWindow(footerSave_, footer && !groupsMode_ && !appendMode_ ? SW_SHOW : SW_HIDE);
    ShowWindow(footerCancel_, footer ? SW_SHOW : SW_HIDE);
    SetWindowTextW(footerCancel_, groupsMode_ || appendMode_ ? L"返回" : L"取消");
    move(footerSave_, w - 170, h - 46, 72, 31); move(footerCancel_, w - 90, h - 46, 72, 31);
    ShowWindow(historyClear_, normal && tab_ == 0 && !footer ? SW_SHOW : SW_HIDE);
    EnableWindow(historyClear_, ready_ && !saving_);
    move(historyClear_, w - 110, h - 31, 92, 26);
    if (positions) EndDeferWindowPos(positions);
    layoutSearch();
    layoutGroups(w);
    TOOLINFOW tip{sizeof(tip)}; tip.hwnd = list_; tip.uId = 1; GetClientRect(list_, &tip.rect);
    SendMessageW(tooltips_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tip));
    updateListMetrics(); positionControls(); setScroll(scroll_);
    layoutQrControls();
    redraw();
}

void App::layoutSearch() {
    RECT rect{}; GetClientRect(window_, &rect); int width = logical(rect.right);
    bool clear = !settingsMode_ && !groupsMode_ && !draft_ && tab_ != 2 && GetWindowTextLengthW(search_) != 0;
    // 输入区始终与右边框间隔至少 12 像素，避免原生背景覆盖圆角边线。
    SetWindowPos(search_, nullptr, px(50), px(96), px(std::max(40, width - 82 - (clear ? 30 : 0))), px(19), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(searchClear_, nullptr, px(width - 52), px(91), px(28), px(28), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(searchClear_, clear ? SW_SHOW : SW_HIDE); EnableWindow(searchClear_, !saving_);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tab_ ? L"搜索名称、分组或普通字段…" : L"搜索剪贴板历史…"));
    RECT frame{px(20), px(87), rect.right - px(20), px(123)}; InvalidateRect(window_, &frame, FALSE);
}

void App::paintInput(HDC dc, RECT rect, bool focused, bool enabled) {
    auto brush = CreateSolidBrush(card_); auto pen = CreatePen(PS_SOLID, px(1), focused && enabled ? accent_ : line_);
    auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, px(10), px(10));
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}

RECT App::controlRect(const Control& control) const {
    RECT client{}; GetClientRect(list_, &client); int width = logical(client.right), top = 0;
    if (draft_) {
        for (const auto& item : items_) { if (item.id == expanded_.id) { top += EditorHeader + 4; break; } top += itemHeight(item) + 9; }
    }
    int x = control.x < 0 ? width + control.x : control.x;
    int y = control.y + top - scroll_;
    return {px(x), px(y), px(x + (control.width < 0 ? std::max(40, width + control.width - x) : control.width)), px(y + control.height)};
}

void App::paintForm(HDC dc) {
    for (const auto& control : controls_) {
        if (control.kind != ControlKind::Edit && control.kind != ControlKind::Multiline && control.kind != ControlKind::Hotkey) continue;
        auto rect = controlRect(control);
        if (rect.bottom >= 0 && rect.top < px(viewportHeight_)) paintInput(dc, rect, GetFocus() == control.window, IsWindowEnabled(control.window));
    }
}

void App::paintChoice(HWND control, HDC dc, bool combo) {
    RECT rect{}; GetClientRect(control, &rect);
    bool enabled = IsWindowEnabled(control), focused = GetFocus() == control, hot = hoveredButton_ == control;
    FillRect(dc, &rect, draft_ ? cardBrush_ : background_);
    if (combo) {
        paintInput(dc, rect, focused, enabled);
        RECT label = rect; label.left += px(12); label.right -= px(30);
        text(dc, windowText(control), label, font_, enabled ? ink_ : muted_);
        auto pen = CreatePen(PS_SOLID, px(1), enabled ? muted_ : line_); auto old = SelectObject(dc, pen);
        int x = rect.right - px(17), y = rect.bottom / 2;
        MoveToEx(dc, x - px(4), y - px(2), nullptr); LineTo(dc, x, y + px(2)); LineTo(dc, x + px(4), y - px(2));
        SelectObject(dc, old); DeleteObject(pen);
    } else {
        bool checked = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int y = rect.bottom / 2; RECT box{px(1), y - px(7), px(15), y + px(7)};
        auto brush = CreateSolidBrush(checked ? accentFill_ : card_); auto pen = CreatePen(PS_SOLID, px(1), focused || hot ? accent_ : (enabled ? muted_ : line_));
        auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, box.left, box.top, box.right, box.bottom, px(4), px(4));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
        if (checked) {
            pen = CreatePen(PS_SOLID, px(2), accentInk_); oldPen = SelectObject(dc, pen);
            MoveToEx(dc, px(4), y, nullptr); LineTo(dc, px(7), y + px(3)); LineTo(dc, px(12), y - px(3));
            SelectObject(dc, oldPen); DeleteObject(pen);
        }
        rect.left = px(24); text(dc, windowText(control), rect, font_, enabled ? ink_ : muted_);
        if (focused) { RECT focus{rect.left, y + px(10), std::min(rect.right, rect.left + px(24)), y + px(11)}; auto mark = CreateSolidBrush(accent_); FillRect(dc, &focus, mark); DeleteObject(mark); }
    }
}
void App::text(HDC dc, std::wstring_view value, RECT r, HFONT font, COLORREF color, UINT flags) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &r, flags | DT_NOPREFIX); SelectObject(dc, old);
}
void App::card(HDC dc, RECT r, bool active) {
    COLORREF selected = tab_ == 1 ? (dark_ ? RGB(44, 101, 65) : RGB(180, 220, 196)) : accent_;
    HBRUSH brush = CreateSolidBrush(card_); HPEN pen = CreatePen(PS_SOLID, px(1), active ? selected : line_);
    auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, px(10), px(10));
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}
void App::paint(HDC dc) {
    RECT r{}; GetClientRect(window_, &r); FillRect(dc, &r, background_);
    DrawIconEx(dc, px(13), px(10), smallIcon_, px(16), px(16), 0, nullptr, DI_NORMAL);
    text(dc, AppDisplayName, {px(36), px(3), r.right - px(140), px(34)}, titleFont_, ink_);
    auto border = CreateSolidBrush(line_); FrameRect(dc, &r, border); DeleteObject(border);
    int h = r.bottom; int w = r.right;
    if (!settingsMode_ && !groupsMode_ && tab_ != 2) {
        paintInput(dc, {px(20), px(87), w - px(20), px(123)}, GetFocus() == search_, IsWindowEnabled(search_));
        auto pen = CreatePen(PS_SOLID, px(1), GetFocus() == search_ ? accent_ : muted_); auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, px(31), px(99), px(42), px(110)); MoveToEx(dc, px(40), px(108), nullptr); LineTo(dc, px(45), px(113));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    }
    if (draft_ || settingsMode_ || groupsMode_ || appendMode_) {
        text(dc, saving_ ? L"正在保存…" : (draft_ ? (dirty_ ? L"● 未保存" : L"编辑资料") : (appendMode_ ? L"选择资料后添加字段" : L"")), {px(20), h - px(46), w - px(180), h - px(14)}, small_, muted_);
    } else {
        auto divider = CreateSolidBrush(line_); RECT line{px(20), h - px(35), w - px(20), h - px(34)}; FillRect(dc, &line, divider); DeleteObject(divider);
        auto hint = qrPage() ? (qrGenerateMode_ ? L"点击二维码，选择位置保存" : L"Ctrl + V · 拖拽 · 选择图片") : (settings_.paused ? L"记录已暂停  ·  Esc 隐藏" : L"↑↓ 选择   Enter 操作   Esc 隐藏");
        text(dc, status_.empty() ? hint : status_, {px(20), h - px(32), w - px(qrPage() ? 118 : (tab_ ? 20 : 120)), h - px(4)}, small_, muted_);
        if (qrPage()) text(dc, L"·", {w - px(66), h - px(31), w - px(58), h - px(5)}, small_, muted_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// 按钮每次从完整背景绘制到离屏位图，避免切页和焦点框留下旧像素。
void App::paintButton(const DRAWITEMSTRUCT& draw) {
    int width = draw.rcItem.right - draw.rcItem.left, height = draw.rcItem.bottom - draw.rcItem.top;
    HDC dc = CreateCompatibleDC(draw.hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(draw.hDC, std::max(1, width), std::max(1, height));
    auto oldBitmap = SelectObject(dc, bitmap); RECT rect{0, 0, width, height};
    int id = static_cast<int>(draw.CtlID);
    bool formButton = GetParent(draw.hwndItem) == list_;
    FillRect(dc, &rect, id == SearchClear || (formButton && draft_) ? cardBrush_ : background_);
    bool activeTab = (id == HistoryTab && tab_ == 0) || (id == RecordsTab && tab_ == 1) || (id == QrTab && tab_ == 2);
    bool qrMode = id == QrRecognize || id == QrGenerate;
    bool qrLink = id == QrSelectImage;
    bool qrTextButton = qrMode || qrLink;
    bool activeQrMode = (id == QrRecognize && !qrGenerateMode_) || (id == QrGenerate && qrGenerateMode_);
    bool activeGroup = id >= GroupFilterBase && static_cast<size_t>(id - GroupFilterBase) < visibleGroups_.size() && filters_[visibleGroups_[id - GroupFilterBase]].id == groupFilter_;
    if (id == GroupMore) activeGroup = false;
    bool primary = id == Save || activeTab;
    bool hot = hoveredButton_ == draw.hwndItem || (draw.itemState & ODS_SELECTED);
    bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    if (!enabled) hot = false;
    bool closeHot = id == Close && hot;
    bool danger = id == HistoryClear || id == DeleteRecord;
    COLORREF fill = primary ? (hot ? RGB(6, 174, 86) : accentFill_) : (activeGroup ? accentTint_ : (hot ? (dark_ ? RGB(42, 42, 42) : RGB(239, 239, 239)) : card_));
    if (closeHot) fill = RGB(211, 53, 67);
    if (danger && hot) fill = dark_ ? RGB(70, 38, 45) : RGB(255, 237, 239);
    if (!qrTextButton && (primary || activeGroup || hot || formButton)) {
        auto brush = CreateSolidBrush(fill); auto border = CreatePen(PS_SOLID, px(1), formButton && !primary ? line_ : fill);
        auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, border);
        RoundRect(dc, 1, 1, width - 1, height - 1, px(8), px(8));
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(border);
    }
    COLORREF color = closeHot ? RGB(255, 255, 255) : (primary ? accentInk_ : (activeGroup ? accent_ : muted_));
    if (formButton && !primary) color = ink_;
    if (danger && hot) color = dark_ ? RGB(255, 150, 160) : RGB(193, 52, 68);
    if (!enabled) color = muted_;
    if (qrTextButton && enabled) color = activeQrMode || hot ? accent_ : muted_;
    if (qrLink && enabled) color = hot ? (dark_ ? RGB(6, 174, 86) : RGB(5, 145, 72)) : accent_;
    if (id == Minimize || id == Close || id == SearchClear) {
        HPEN pen = CreatePen(PS_SOLID, px(1), color); auto oldPen = SelectObject(dc, pen);
        int cx = width / 2, cy = height / 2, radius = px(4);
        if (id == Minimize) text(dc, L"—", rect, font_, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else { MoveToEx(dc, cx - radius, cy - radius, nullptr); LineTo(dc, cx + radius + 1, cy + radius + 1); MoveToEx(dc, cx + radius, cy - radius, nullptr); LineTo(dc, cx - radius - 1, cy + radius + 1); }
        SelectObject(dc, oldPen); DeleteObject(pen);
    } else if (id == HistoryClear) {
        auto pen = CreatePen(PS_SOLID, px(1), color); auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        int x = px(12), y = height / 2;
        RoundRect(dc, x - px(4), y - px(3), x + px(5), y + px(6), px(2), px(2));
        MoveToEx(dc, x - px(6), y - px(5), nullptr); LineTo(dc, x + px(7), y - px(5));
        MoveToEx(dc, x - px(2), y - px(7), nullptr); LineTo(dc, x + px(3), y - px(7));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
        text(dc, L"清空历史", {px(25), 0, width - px(4), height}, small_, color);
    } else {
        RECT label = rect; InflateRect(&label, -px(5), 0);
        auto value = windowText(draw.hwndItem);
        auto font = qrMode ? small_ : (activeTab ? bold_ : font_);
        text(dc, value, label, font, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (qrLink) {
            auto oldFont = SelectObject(dc, font); SIZE size{};
            GetTextExtentPoint32W(dc, value.data(), static_cast<int>(value.size()), &size);
            SelectObject(dc, oldFont);
            int lineWidth = std::min(size.cx, label.right - label.left);
            int left = (width - lineWidth) / 2, top = std::min(height - px(2), (height + static_cast<int>(size.cy)) / 2);
            RECT underline{left, top, left + lineWidth, top + 1};
            auto brush = CreateSolidBrush(color); FillRect(dc, &underline, brush); DeleteObject(brush);
        }
    }
    bool focused = (draw.itemState & ODS_FOCUS) && !(draw.itemState & ODS_NOFOCUSRECT);
    if (qrLink && focused) {
        RECT focus = rect; InflateRect(&focus, -px(4), -px(4)); DrawFocusRect(dc, &focus);
    } else if (activeQrMode || focused) {
        RECT focus{width / 2 - px(8), height - px(3), width / 2 + px(8), height - px(2)};
        auto brush = CreateSolidBrush(color); FillRect(dc, &focus, brush); DeleteObject(brush);
    }
    BitBlt(draw.hDC, draw.rcItem.left, draw.rcItem.top, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
}

int App::itemHeight(const Item& item) const {
    if (!tab_) return 100;
    if (expanded_.id != item.id) return FoldedRecord;
    if (draft_) return EditorHeader + 285 + static_cast<int>(draft_->fields.size()) * 153;
    if (appendMode_) return 110;
    int height = RecordHeader + (expanded_.note.empty() ? 10 : 54);
    for (const auto& field : expanded_.fields) height += fieldHeight(field);
    return height;
}
int App::fieldHeight(const Field& field) const {
    auto layout = fieldLayouts_.find(field.id);
    return layout == fieldLayouts_.end() ? 40 : layout->second.height;
}
void App::measureFields() {
    fieldLayouts_.clear();
    if (!expanded_.id || draft_ || appendMode_) return;
    RECT client{}; GetClientRect(list_, &client);
    int width = logical(client.right);
    HDC dc = GetDC(list_); auto old = SelectObject(dc, font_);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    static const std::wstring mask = L"••••••••";
    for (const auto& field : expanded_.fields) {
        const std::wstring* value = &field.plain;
        if (field.sensitive) { auto reveal = revealed_.find(field.id); value = reveal == revealed_.end() ? &mask : &reveal->second; }
        bool full = fullFields_.contains(field.id);
        auto wrapped = wrapFieldText(dc, *value, px(std::max(20, width - 108 - (field.sensitive ? 108 : 76))), full ? 0 : 3);
        FieldLayout layout; layout.lineHeight = std::max(18, logical(metrics.tmHeight));
        layout.expandable = wrapped.truncated || wrapped.lines.size() > 2;
        if (!full && wrapped.lines.size() > 2) wrapped.lines.resize(2);
        layout.lines = std::move(wrapped.lines);
        layout.valueHeight = static_cast<int>(layout.lines.size()) * layout.lineHeight;
        layout.height = std::max(40, layout.valueHeight + 16) + (layout.expandable ? 22 : 0);
        fieldLayouts_[field.id] = std::move(layout);
    }
    SelectObject(dc, old); ReleaseDC(list_, dc);
}
void App::paintAction(HDC dc, RECT rect, int kind, int index) {
    bool hot = hoveredAction_ == kind && hoveredIndex_ == index;
    if (hot) { auto brush = CreateSolidBrush(accentTint_); FillRect(dc, &rect, brush); DeleteObject(brush); }
    int cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, px(1), hot ? accent_ : muted_);
    auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (kind == PasteField) {
        RoundRect(dc, cx - px(6), cy - px(5), cx + px(6), cy + px(8), px(2), px(2));
        Rectangle(dc, cx - px(3), cy - px(8), cx + px(3), cy - px(4));
        MoveToEx(dc, cx - px(2), cy, nullptr); LineTo(dc, cx + px(3), cy); MoveToEx(dc, cx - px(2), cy + px(3), nullptr); LineTo(dc, cx + px(3), cy + px(3));
    } else if (kind == Reveal) {
        Ellipse(dc, cx - px(8), cy - px(5), cx + px(8), cy + px(5));
        Ellipse(dc, cx - px(2), cy - px(2), cx + px(2), cy + px(2));
    } else if (kind == Edit) {
        POINT pencil[]{{cx - px(6), cy + px(6)}, {cx - px(5), cy + px(1)}, {cx + px(3), cy - px(7)}, {cx + px(7), cy - px(3)}, {cx - px(1), cy + px(5)}};
        Polygon(dc, pencil, 5);
    }
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}
// 内容或尺寸变化时更新滚动条，绘制过程中不触发布局与重绘。
void App::updateListMetrics() {
    hits_.clear(); hoveredAction_ = 0; hoveredIndex_ = -1;
    for (int pass = 0; pass < 2; ++pass) {
        RECT before{}; GetClientRect(list_, &before);
        if (qrPage()) measureQrResults();
        else {
            measureFields(); rows_.clear();
            int y = appendMode_ ? 139 : 4;
            for (const auto& item : items_) { int height = itemHeight(item); rows_.push_back({y, height}); y += height + 9; }
            totalHeight_ = settingsMode_ ? 725 : (groupsMode_ ? 345 : y + (more_ ? 44 : 0) + 8);
        }
        scroll_ = std::clamp(scroll_, 0, std::max(0, totalHeight_ - viewportHeight_));
        SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS}; si.nMax = std::max(0, totalHeight_ - 1); si.nPage = viewportHeight_; si.nPos = scroll_; SetScrollInfo(list_, SB_VERT, &si, TRUE);
        RECT after{}; GetClientRect(list_, &after);
        // 滚动条显隐会改变字段宽度，立即用最终可视宽度重算一次。
        if (before.right == after.right) break;
    }
    layoutQrControls();
}

// 列表只绘制可视卡片；完整字段仅在展开一条资料时取出。
void App::paintList(HDC dc) {
    RECT client{}; GetClientRect(list_, &client); FillRect(dc, &client, background_);
    hits_.clear();
    if (qrPage()) { paintQrResults(dc); return; }
    rows_.clear();
    if (settingsMode_ || groupsMode_) { paintForm(dc); return; }
    int w = logical(client.right), y = appendMode_ ? 139 : 4;
    if (appendMode_) text(dc, L"添加历史为字段 · 选择目标资料", {px(20), px(2 - scroll_), px(w - 20), px(30 - scroll_)}, bold_, ink_);
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i]; int height = itemHeight(item);
        rows_.push_back({y, height}); int top = y - scroll_; y += height + 9;
        if (top + height < 0 || top > viewportHeight_) continue;
        bool dragged = recordDrag_.active && recordDrag_.id == item.id;
        RECT outer{px(18), px(top), px(w - 18), px(top + height)}; card(dc, outer, dragged || selected_ == static_cast<int>(i));
        bool view = expanded_.id == item.id && !draft_ && !appendMode_;
        int headerHeight = !tab_ ? 100 : (view ? RecordHeader : FoldedRecord);
        RECT header{px(18), px(top), px(w - 18), px(top + headerHeight)};
        hits_.push_back({header, Open, static_cast<int>(i)});
        if (!tab_) {
            text(dc, item.title, {px(31), px(top + 12), px(w - 31), px(top + 61)}, font_, ink_, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
            auto meta = age(item.time); if (!item.subtitle.empty()) meta += L" · " + item.subtitle;
            text(dc, meta, {px(31), px(top + 68), px(w - 31), px(top + 90)}, small_, muted_);
        } else {
            int titleRight = w - (view ? 90 : 56);
            if (view && expanded_.groupId) {
                auto group = std::find_if(groups_.begin(), groups_.end(), [this](const Group& g) { return g.id == expanded_.groupId; });
                if (group != groups_.end()) {
                    auto oldFont = SelectObject(dc, small_); SIZE size{}; GetTextExtentPoint32W(dc, group->name.data(), static_cast<int>(group->name.size()), &size); SelectObject(dc, oldFont);
                    int badge = std::min(86, logical(size.cx) + 12);
                    RECT tag{px(titleRight - badge), px(top + 12), px(titleRight), px(top + 32)};
                    FillRect(dc, &tag, background_); text(dc, group->name, tag, small_, muted_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    titleRight -= badge + 8;
                }
            }
            if (dragged) paintRecordGrip(dc, px(31), px(top + 22));
            text(dc, (item.pinned ? L"★ " : L"") + item.title, {px(dragged ? 47 : 31), px(top + 8), px(titleRight), px(top + 35)}, bold_, ink_);
            if (!view) text(dc, oneLine(item.subtitle), {px(31), px(top + 34), px(w - 52), px(top + 57)}, small_, muted_);
            text(dc, expanded_.id == item.id ? L"⌄" : L"›", {px(w - 50), px(top + 8), px(w - 26), px(top + 35)}, font_, muted_);
            if (expanded_.id == item.id && !draft_) {
                if (appendMode_) {
                    RECT add{px(30), px(top + 70), px(w - 30), px(top + 103)};
                    text(dc, L"+ 添加到这条资料", add, bold_, accent_); hits_.push_back({add, Append, 0});
                } else {
                    RECT edit{px(w - 86), px(top + 8), px(w - 58), px(top + 36)};
                    paintAction(dc, edit, Edit, 0); hits_.push_back({edit, Edit, 0});
                    int fy = top + FieldStart;
                    HPEN line = CreatePen(PS_SOLID, px(1), line_); auto oldPen = SelectObject(dc, line);
                    MoveToEx(dc, px(31), px(fy), nullptr); LineTo(dc, px(w - 31), px(fy));
                    for (size_t f = 0; f < expanded_.fields.size(); ++f) {
                        const auto& field = expanded_.fields[f];
                        int fieldSize = fieldHeight(field);
                        RECT row{px(30), px(fy + 2), px(w - 30), px(fy + fieldSize - 2)};
                        if (fieldSelected_ == static_cast<int>(f)) { HBRUSH b = CreateSolidBrush(accentTint_); FillRect(dc, &row, b); DeleteObject(b); }
                        text(dc, field.name, {px(34), px(fy + 7), px(98), px(fy + 30)}, small_, muted_);
                        auto it = revealed_.find(field.id);
                        static const std::wstring mask = L"••••••••";
                        const std::wstring& value = field.sensitive ? (it != revealed_.end() ? it->second : mask) : field.plain;
                        auto layout = fieldLayouts_.find(field.id);
                        if (layout != fieldLayouts_.end()) {
                            const auto& shape = layout->second;
                            RECT valueRect{px(108), px(fy + 8), px(w - (field.sensitive ? 108 : 76)), px(fy + 8 + shape.valueHeight)};
                            hits_.push_back({valueRect, CopyField, static_cast<int>(f)});
                            for (size_t l = 0; l < shape.lines.size(); ++l) {
                                int ly = fy + 8 + static_cast<int>(l) * shape.lineHeight;
                                if (ly + shape.lineHeight < 0 || ly > viewportHeight_) continue;
                                auto [offset, count] = shape.lines[l];
                                text(dc, std::wstring_view(value).substr(offset, count), {valueRect.left, px(ly), valueRect.right, px(ly + shape.lineHeight)}, font_, ink_, DT_LEFT | DT_TOP | DT_SINGLELINE);
                            }
                            if (shape.expandable) {
                                RECT full{px(108), px(fy + fieldSize - 23), px(w - 76), px(fy + fieldSize - 2)};
                                text(dc, fullFields_.contains(field.id) ? L"收起全文" : L"展开全文", full, small_, accent_);
                                hits_.push_back({full, FullText, static_cast<int>(f)});
                            }
                        }
                        RECT pasteRect{px(w - 66), px(fy + 5), px(w - 34), px(fy + 35)};
                        paintAction(dc, pasteRect, PasteField, static_cast<int>(f)); hits_.push_back({pasteRect, PasteField, static_cast<int>(f)});
                        if (field.sensitive) { RECT eye{px(w - 101), px(fy + 5), px(w - 71), px(fy + 35)}; paintAction(dc, eye, Reveal, static_cast<int>(f)); hits_.push_back({eye, Reveal, static_cast<int>(f)}); }
                        fy += fieldSize;
                        if (f + 1 < expanded_.fields.size()) { MoveToEx(dc, px(31), px(fy), nullptr); LineTo(dc, px(w - 31), px(fy)); }
                    }
                    SelectObject(dc, oldPen); DeleteObject(line);
                    if (!expanded_.note.empty()) text(dc, L"备注：" + expanded_.note, {px(34), px(fy + 6), px(w - 34), px(fy + 49)}, small_, muted_, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
                }
            }
        }
    }
    if (items_.empty() && !draft_) {
        text(dc, !ready_ || loading_ ? L"正在读取…" : (windowText(search_).empty() ? (tab_ ? L"常用资料，随用随取" : L"复制过的文字会出现在这里") : L"没有找到匹配内容"), {px(30), px(70), px(w - 30), px(106)}, bold_, muted_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, tab_ ? L"点击「+ 新建」保存你的第一条资料" : hotkeyLabel(settings_) + L" 随时呼出", {px(25), px(109), px(w - 25), px(139)}, small_, muted_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (more_) { RECT r{px(20), px(y + 3 - scroll_), px(w - 20), px(y + 37 - scroll_)}; text(dc, loading_ ? L"正在加载…" : L"加载更多", r, font_, accent_, DT_CENTER | DT_VCENTER | DT_SINGLELINE); hits_.push_back({r, LoadMore, 0}); y += 44; }
    totalHeight_ = y + 8;
    paintForm(dc);
    if (recordDrag_.active && recordDrag_.anchor) { auto brush = CreateSolidBrush(accent_); FillRect(dc, &recordDrag_.marker, brush); DeleteObject(brush); }
}

LRESULT App::listMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; auto dc = BeginPaint(list_, &ps); RECT r{}; GetClientRect(list_, &r); auto mem = CreateCompatibleDC(dc); auto bitmap = CreateCompatibleBitmap(dc, std::max(1L, r.right), std::max(1L, r.bottom)); auto old = SelectObject(mem, bitmap); paintList(mem); BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, old); DeleteObject(bitmap); DeleteDC(mem); EndPaint(list_, &ps); return 0; }
    case WM_SIZE: viewportHeight_ = logical(HIWORD(lp)); positionControls(); return 0;
    case WM_VSCROLL: { SCROLLINFO si{sizeof(si), SIF_TRACKPOS}; GetScrollInfo(list_, SB_VERT, &si); int pos = scroll_; switch (LOWORD(wp)) { case SB_LINEUP: pos -= 28; break; case SB_LINEDOWN: pos += 28; break; case SB_PAGEUP: pos -= viewportHeight_ - 28; break; case SB_PAGEDOWN: pos += viewportHeight_ - 28; break; case SB_THUMBTRACK: pos = si.nTrackPos; break; case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = totalHeight_; break; } setScroll(pos); return 0; }
    case WM_MOUSEWHEEL: setScroll(scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 72); return 0;
    case WM_MOUSEMOVE: {
        if (recordDrag_.pressed) { moveRecordDrag({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0; }
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; int kind = 0, index = -1;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) if (PtInRect(&it->rect, point)) { kind = it->action; index = it->index; break; }
        if (kind != hoveredAction_ || index != hoveredIndex_) { hoveredAction_ = kind; hoveredIndex_ = index; SendMessageW(tooltips_, TTM_POP, 0, 0); InvalidateRect(list_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, list_, 0}; TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE: hoveredAction_ = 0; hoveredIndex_ = -1; InvalidateRect(list_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        if (qrPage()) { SetFocus(list_); return 0; }
        suppressRecordClick_ = false;
        if (tab_ == 1 && ready_ && !loading_ && !saving_ && !draft_ && !appendMode_ && !settingsMode_ && !groupsMode_) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) if (PtInRect(&it->rect, point)) {
                if (it->action == Open) {
                    auto origin = it->rect; origin.bottom = std::min(origin.bottom, origin.top + px(40)); origin.right -= px(40);
                    if (PtInRect(&origin, point)) { pressRecord(it->index, origin, point); return 0; }
                }
                break;
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (recordDrag_.pressed) { moveRecordDrag(p); finishRecordDrag(true); return 0; }
        if (suppressRecordClick_) { suppressRecordClick_ = false; return 0; }
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) if (PtInRect(&it->rect, p)) { auto hit = *it; action(hit.action, hit.index); break; } return 0;
    }
    case WM_CAPTURECHANGED: if (recordDrag_.pressed && reinterpret_cast<HWND>(lp) != list_) finishRecordDrag(false); return 0;
    case WM_CANCELMODE: finishRecordDrag(false); return 0;
    case WM_SETCURSOR:
        if (recordDrag_.active) { SetCursor(LoadCursorW(nullptr, recordDrag_.anchor ? IDC_SIZEALL : IDC_NO)); return TRUE; }
        if (qrPage() && (hoveredAction_ == CopyQrValue || hoveredAction_ == CopyQrId || hoveredAction_ == SaveQrImage)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_CONTEXTMENU: { if (qrPage()) return 0; POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, p = screen; if (screen.x == -1) { p = {px(40), px(30)}; screen = p; ClientToScreen(list_, &screen); } else ScreenToClient(list_, &p); for (size_t i = 0; i < rows_.size(); ++i) if (logical(p.y) + scroll_ >= rows_[i].first && logical(p.y) + scroll_ < rows_[i].first + rows_[i].second) { menu(static_cast<int>(i), screen); return 0; } if (selected_ < static_cast<int>(items_.size())) menu(selected_, screen); return 0; }
    case WM_COMMAND: case WM_NOTIFY: case WM_DRAWITEM: case WM_MEASUREITEM: return message(msg, wp, lp);
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: return message(msg, wp, lp);
    }
    return DefWindowProcW(list_, msg, wp, lp);
}

void App::setScroll(int position) {
    int next = std::clamp(position, 0, std::max(0, totalHeight_ - viewportHeight_));
    if (scroll_ == next) return;
    scroll_ = next; SetScrollPos(list_, SB_VERT, scroll_, TRUE); positionControls(); layoutQrControls(); InvalidateRect(list_, nullptr, TRUE);
    hits_.clear(); hoveredAction_ = 0; hoveredIndex_ = -1; SendMessageW(tooltips_, TTM_POP, 0, 0);
}
void App::ensureVisible(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return;
    int top = rows_[index].first, height = std::min(rows_[index].second, viewportHeight_);
    if (top < scroll_) setScroll(top - 4); else if (top + height > scroll_ + viewportHeight_) setScroll(top + height - viewportHeight_ + 4);
}
void App::positionControls() {
    for (const auto& c : controls_) {
        auto rect = controlRect(c);
        if (c.kind == ControlKind::Edit || c.kind == ControlKind::Hotkey) {
            int height = px(19); rect.top += (rect.bottom - rect.top - height) / 2; rect.bottom = rect.top + height;
            rect.left += px(10); rect.right -= px(10);
        } else if (c.kind == ControlKind::Multiline) InflateRect(&rect, -px(10), -px(8));
        MoveWindow(c.window, rect.left, rect.top, std::max(1L, rect.right - rect.left), std::max(1L, rect.bottom - rect.top), TRUE);
    }
}
void App::destroyControls() { initializing_ = true; for (auto& c : controls_) { SetWindowTextW(c.window, L""); DestroyWindow(c.window); } controls_.clear(); initializing_ = false; }
HWND App::control(const wchar_t* kind, const std::wstring& value, DWORD style, int id, int x, int y, int w, int h) {
    bool edit = wcscmp(kind, L"EDIT") == 0 || wcscmp(kind, MSFTEDIT_CLASS) == 0;
    bool rich = wcscmp(kind, MSFTEDIT_CLASS) == 0;
    bool combo = wcscmp(kind, L"COMBOBOX") == 0, button = wcscmp(kind, L"BUTTON") == 0, hotkey = wcscmp(kind, HOTKEY_CLASSW) == 0;
    bool check = button && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
    if (combo) style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    if (button && !check) style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    auto handle = CreateWindowExW(0, kind, rich ? L"" : value.c_str(), WS_CHILD | WS_VISIBLE | (id ? WS_TABSTOP : 0) | style, 0, 0, 0, 0, list_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (edit) SendMessageW(handle, EM_SETLIMITTEXT, settings_.maxBytes / 2, 0);
    if (edit || combo || button || hotkey) SetWindowSubclass(handle, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
    if (edit || combo || hotkey) SetWindowTheme(handle, L"", L"");
    if (combo) { guardComboWheel(handle, list_); SendMessageW(handle, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(28)); SendMessageW(handle, CB_SETITEMHEIGHT, 0, px(30)); }
    if (hotkey) {
        SetWindowLongPtrW(handle, GWL_STYLE, GetWindowLongPtrW(handle, GWL_STYLE) & ~WS_BORDER);
        SetWindowLongPtrW(handle, GWL_EXSTYLE, GetWindowLongPtrW(handle, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (rich) {
        SendMessageW(handle, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
        SendMessageW(handle, EM_SETEVENTMASK, 0, ENM_CHANGE);
        SendMessageW(handle, EM_SETBKGNDCOLOR, 0, card_);
        CHARFORMAT2W format{}; format.cbSize = sizeof(format); format.dwMask = CFM_COLOR; format.crTextColor = ink_; SendMessageW(handle, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
        SetWindowTextW(handle, value.c_str());
    }
    auto presentation = edit ? ((style & ES_MULTILINE) ? ControlKind::Multiline : ControlKind::Edit) : (combo ? ControlKind::Combo : (hotkey ? ControlKind::Hotkey : (check ? ControlKind::Check : (button ? ControlKind::Button : ControlKind::Plain))));
    controls_.push_back({handle, x, y, w, h, presentation}); return handle;
}

void App::refresh(bool append, bool keepExpanded) {
    if (!ready_ || tab_ == 2 || (loading_ && append) || draft_) return;
    if (!append) finishRecordDrag(false);
    if (!append) { ++revision_; loading_ = false; if (!keepExpanded) collapse(); selected_ = 0; fieldSelected_ = -1; setScroll(0); }
    const int revision = revision_, tab = tab_, offset = append ? static_cast<int>(items_.size()) : 0;
    auto query = windowText(search_); auto group = groupFilter_; loading_ = true;
    worker_->post([this, revision, tab, offset, append, query, group](Database& db) {
        auto items = tab ? db.records(query, group, offset) : db.history(query, offset);
        complete(window_, [this, revision, append, items = std::move(items)] {
            if (revision != revision_ || draft_) return;
            loading_ = false; more_ = items.size() == PageSize;
            if (append) items_.insert(items_.end(), items.begin(), items.end()); else items_ = items;
            selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(items_.size()) - 1));
            updateListMetrics();
            InvalidateRect(list_, nullptr, TRUE);
        });
    });
}
void App::populateGroups(HWND combo, bool filter, Id chosen) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    auto add = [&](const std::wstring& label, Id id) { auto i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())); SendMessageW(combo, CB_SETITEMDATA, i, static_cast<LPARAM>(id)); if (id == chosen) SendMessageW(combo, CB_SETCURSEL, i, 0); };
    for (const auto& group : groupFilters(groups_)) if (filter || group.id != -1) add(group.name, group.id);
    if (filter) add(L"管理分组…", -2);
    if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}
void App::refreshGroups() {
    worker_->post([this](Database& db) { auto groups = db.groups(); complete(window_, [this, groups = std::move(groups)] { groups_ = groups; saving_ = false; EnableWindow(list_, TRUE); updateGroupFilters(); if (groupsMode_) groupPanel(); else layout(); }); });
}
void App::updateGroupFilters() {
    filters_ = groupFilters(groups_);
    auto selected = std::find_if(filters_.begin(), filters_.end(), [this](const Group& group) { return group.id == groupFilter_; });
    if (selected == filters_.end()) { groupFilter_ = -1; selected = filters_.begin(); }
}
void App::layoutGroups(int width) {
    HDC dc = GetDC(window_); auto old = SelectObject(dc, font_);
    auto measure = [&](const std::wstring& label) { SIZE size{}; GetTextExtentPoint32W(dc, label.data(), static_cast<int>(label.size()), &size); return logical(size.cx); };
    std::vector<int> widths;
    for (const auto& group : filters_) widths.push_back(std::clamp(measure(group.name) + 22, 42, 112));
    size_t selected = 0; for (size_t i = 0; i < filters_.size(); ++i) if (filters_[i].id == groupFilter_) selected = i;
    auto packed = packGroupChips(widths, width - 118, selected, [&](size_t hidden) { return measure(L"更多 " + std::to_wstring(hidden) + L" ▾") + 20; });
    SelectObject(dc, old); ReleaseDC(window_, dc);
    visibleGroups_ = std::move(packed.visible); hiddenGroups_ = std::move(packed.hidden);
    while (groupButtons_.size() < visibleGroups_.size()) {
        int id = GroupFilterBase + static_cast<int>(groupButtons_.size());
        auto button = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), 0);
        SetWindowSubclass(button, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
        TOOLINFOW tip{sizeof(tip)}; tip.hwnd = window_; tip.uId = reinterpret_cast<UINT_PTR>(button); tip.uFlags = TTF_SUBCLASS | TTF_IDISHWND; tip.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
        groupButtons_.push_back(button);
    }
    bool visible = tab_ == 1 && !settingsMode_ && !groupsMode_ && !appendMode_;
    int x = 20;
    for (size_t i = 0; i < groupButtons_.size(); ++i) {
        bool used = i < visibleGroups_.size();
        if (used) {
            auto index = visibleGroups_[i];
            SetWindowTextW(groupButtons_[i], filters_[index].name.c_str());
            SetWindowPos(groupButtons_[i], nullptr, px(x), px(128), px(widths[index]), px(27), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            x += widths[index] + 5;
        }
        ShowWindow(groupButtons_[i], visible && used ? SW_SHOW : SW_HIDE);
    }
    SetWindowTextW(groupMore_, (L"更多 " + std::to_wstring(hiddenGroups_.size()) + L" ▾").c_str());
    SetWindowPos(groupMore_, nullptr, px(x), px(128), px(std::max(1, packed.moreWidth)), px(27), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    ShowWindow(groupMore_, visible && !hiddenGroups_.empty() ? SW_SHOW : SW_HIDE);
}
void App::selectGroup(Id id) {
    if (groupFilter_ == id || !leave()) return;
    groupFilter_ = id; updateGroupFilters(); layout(); refresh(); SetFocus(search_);
}
void App::groupMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    for (size_t i : hiddenGroups_) AppendMenuW(menu, MF_STRING | (filters_[i].id == groupFilter_ ? MF_CHECKED : 0), 100 + i, filters_[i].name.c_str());
    if (!hiddenGroups_.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, L"管理分组…");
    if (point.x == -1) { RECT rect{}; GetWindowRect(groupMore_, &rect); point = {rect.left, rect.bottom}; }
    inMenu_ = true;
    int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    inMenu_ = false; DestroyMenu(menu);
    if (selected == 1) groupPanel();
    else if (selected >= 100 && static_cast<size_t>(selected - 100) < filters_.size()) selectGroup(filters_[selected - 100].id);
}

void App::redraw() {
    if (window_) RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT App::hitTest(POINT screen) const {
    POINT point = screen; ScreenToClient(window_, &point);
    RECT rect{}; GetClientRect(window_, &rect); int edge = px(6);
    bool left = point.x < edge, right = point.x >= rect.right - edge, top = point.y < edge, bottom = point.y >= rect.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return point.y < px(36) && point.x < rect.right - px(136) ? HTCAPTION : HTCLIENT;
}

bool App::applyHotkey(const Settings& settings) {
    if (settings.modifiers == MOD_WIN && settings.key == 'V') {
        if (!winVHotkey_.start(window_)) return false;
        if (hotkeyRegistered_) UnregisterHotKey(window_, hotkeyId_);
        hotkeyRegistered_ = false;
    } else {
        if (hotkeyRegistered_ && settings.modifiers == boundModifiers_ && settings.key == boundKey_) return true;
        int id = hotkeyRegistered_ && hotkeyId_ == 1 ? 2 : 1;
        if (!RegisterHotKey(window_, id, settings.modifiers | MOD_NOREPEAT, settings.key)) return false;
        if (hotkeyRegistered_) UnregisterHotKey(window_, hotkeyId_);
        hotkeyId_ = id; hotkeyRegistered_ = true; winVHotkey_.stop();
    }
    boundModifiers_ = settings.modifiers; boundKey_ = settings.key;
    return true;
}
// 业务确认可指定默认选项，未指定时仍优先保留当前内容。
bool App::confirm(const std::wstring& value, bool defaultYes) {
    bool old = inMenu_; inMenu_ = true;
    bool yes = MessageBoxW(window_, value.c_str(), AppDisplayName, MB_YESNO | MB_ICONQUESTION | (defaultYes ? MB_DEFBUTTON1 : MB_DEFBUTTON2)) == IDYES;
    inMenu_ = old; return yes;
}
// 主动操作显示错误；静默启动期间只保留状态，避免登录时弹窗。
void App::error(const std::wstring& value) {
    if (saving_ && settingsMode_) applyHotkey(settings_);
    saving_ = false; loading_ = false; EnableWindow(list_, TRUE);
    EnableWindow(search_, !draft_); EnableWindow(historyClear_, ready_);
    if (silentLaunch_) { notice(value); return; }
    inMenu_ = true; MessageBoxW(window_, value.c_str(), L"随手贴 · 操作失败", MB_OK | MB_ICONERROR); inMenu_ = false;
}
bool App::leave() {
    if (saving_) return false;
    finishRecordDrag(false);
    if (dirty_ && !confirm(L"有尚未保存的更改，放弃这些更改吗？")) return false;
    ++revision_; loading_ = false;
    destroyControls(); draft_.reset(); settingsMode_ = groupsMode_ = appendMode_ = false; dirty_ = false; wipe(appendValue_); collapse(); return true;
}
void App::collapse() {
    ++recordRevision_; expanded_ = {}; fieldSelected_ = -1;
    for (auto& value : revealed_) wipe(value.second);
    revealed_.clear();
    fullFields_.clear(); fieldLayouts_.clear();
}
void App::switchTab(int tab) {
    if (tab == tab_ && !settingsMode_ && !groupsMode_) return;
    if (!leave()) return;
    tab_ = tab; initializing_ = true; SetWindowTextW(search_, L""); initializing_ = false;
    scroll_ = 0; status_.clear(); qrDragging_ = false;
    layout(); refresh(); SetFocus(qrFocus());
}
void App::show(bool reset) {
    silentLaunch_ = false;
    if (!status_.empty()) SetTimer(window_, StatusTimer, 7000, nullptr);
    rememberTarget();
    if (reset && !leave()) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); return; }
    if (reset) { tab_ = 0; groupFilter_ = -1; initializing_ = true; SetWindowTextW(search_, L""); initializing_ = false; updateGroupFilters(); scroll_ = 0; layout(); refresh(); }
    if (!IsWindowVisible(window_)) {
        POINT cursor{}; GetCursorPos(&cursor); MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info);
        RECT r{}; GetWindowRect(window_, &r); int w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(window_, HWND_TOP, info.rcWork.left + (info.rcWork.right - info.rcWork.left - w) / 2, info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - h) / 2, 0, 0, SWP_NOSIZE);
    }
    ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_); SetFocus(qrFocus());
}
bool App::hide() {
    if (!leave()) return false;
    ShowWindow(window_, SW_HIDE); return true;
}
void App::openRecord(int index, bool keyboard) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    Id id = items_[index].id;
    if (expanded_.id == id) { if (!appendMode_ && !leave()) return; collapse(); layout(); InvalidateRect(list_, nullptr, TRUE); return; }
    if (draft_ && !leave()) return;
    collapse(); selected_ = index; layout(); auto revision = ++recordRevision_;
    worker_->post([this, id, revision, keyboard](Database& db) { auto record = db.record(id); complete(window_, [this, revision, keyboard, record = std::move(record)] { if (recordRevision_ != revision || tab_ != 1) return; expanded_ = record; fieldSelected_ = keyboard ? 0 : -1; layout(); UpdateWindow(list_); ensureVisible(selected_); }); });
}
void App::activate(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size()) || saving_) return;
    if (tab_) { if (expanded_.id == items_[index].id && fieldSelected_ >= 0) action(PasteField, fieldSelected_); else openRecord(index, true); }
    else { auto id = items_[index].id; worker_->post([this, id](Database& db) { auto value = db.historyText(id); complete(window_, [this, value = std::move(value)]() mutable { copy(std::move(value), false, true); }); }); }
}
void App::navigate(int delta) {
    if (draft_ || settingsMode_ || groupsMode_ || items_.empty()) return;
    if (tab_ && expanded_.id && selected_ < static_cast<int>(items_.size()) && items_[selected_].id == expanded_.id && fieldSelected_ >= 0) {
        fieldSelected_ = std::clamp(fieldSelected_ + delta, 0, std::max(0, static_cast<int>(expanded_.fields.size()) - 1));
        if (selected_ >= static_cast<int>(rows_.size())) return;
        int y = rows_[selected_].first + FieldStart;
        for (int i = 0; i < fieldSelected_; ++i) y += fieldHeight(expanded_.fields[i]);
        int height = fieldHeight(expanded_.fields[fieldSelected_]);
        if (y < scroll_) setScroll(y); else if (y + height > scroll_ + viewportHeight_) setScroll(y + height - viewportHeight_);
    } else { selected_ = std::clamp(selected_ + delta, 0, static_cast<int>(items_.size()) - 1); ensureVisible(selected_); }
    InvalidateRect(list_, nullptr, TRUE);
}
void App::action(int kind, int index) {
    if (qrPage()) { if (kind == CopyQrValue || kind == CopyQrId) copyQrResult(index, kind == CopyQrId); else if (kind == SaveQrImage) saveQrImage(); return; }
    if (saving_) return;
    if (kind == LoadMore) { refresh(true, true); return; }
    if (kind == Open) { if (tab_) openRecord(index); else activate(index); return; }
    if (kind == Edit) { edit(); return; }
    if (kind == Append) { saveAppend(expanded_.id); return; }
    if (index < 0 || index >= static_cast<int>(expanded_.fields.size())) return;
    auto field = expanded_.fields[index]; fieldSelected_ = index;
    if (kind == FullText) {
        if (fullFields_.contains(field.id)) fullFields_.erase(field.id); else fullFields_.insert(field.id);
        layout(); return;
    }
    if (kind == Reveal) {
        auto it = revealed_.find(field.id);
        if (it != revealed_.end()) { wipe(it->second); revealed_.erase(it); fullFields_.erase(field.id); layout(); }
        else { auto rev = recordRevision_; worker_->post([this, field, rev](Database&) { auto value = std::make_shared<SecretText>(fieldValue(field)); complete(window_, [this, id = field.id, rev, value] { if (rev == recordRevision_) { revealed_[id] = std::move(value->value); layout(); } }); }); }
    } else if (kind == PasteField || kind == CopyField) {
        bool paste = kind == PasteField; auto record = expanded_.id;
        worker_->post([this, field, paste, record](Database& db) { auto value = std::make_shared<SecretText>(fieldValue(field)); db.used(record); complete(window_, [this, sensitive = field.sensitive, paste, value] { copy(std::move(value->value), sensitive, paste); }); });
        InvalidateRect(list_, nullptr, FALSE);
    }
}

void App::edit() {
    if (!expanded_.id || draft_ || saving_) return;
    auto record = expanded_; auto revision = recordRevision_;
    worker_->post([this, record, revision](Database&) {
        auto draft = std::make_shared<Draft>(makeDraft(record));
        complete(window_, [this, draft, revision] { if (revision != recordRevision_ || draft_) return; draft_ = std::make_unique<Draft>(*draft); dirty_ = false; buildEditor(); });
    });
}
void App::create(Id historyId) {
    if (!leave()) return;
    tab_ = 1; initializing_ = true; SetWindowTextW(search_, L""); initializing_ = false; ++revision_;
    auto begin = [this, historyId, revision = revision_](std::wstring value) {
        if (revision != revision_ || draft_) { wipe(value); return; }
        draft_ = std::make_unique<Draft>(); draft_->historyId = historyId; draft_->groupId = std::max<Id>(0, groupFilter_); draft_->fields.push_back({0, L"内容", value, false, false});
        expanded_ = {}; expanded_.id = -1; items_.insert(items_.begin(), {-1, L"新建资料", L"至少填写一个字段", 0, false});
        selected_ = 0; dirty_ = true; scroll_ = 0; buildEditor(); SetFocus(GetDlgItem(list_, Name)); wipe(value);
    };
    if (historyId) worker_->post([this, historyId, begin](Database& db) { auto value = db.historyText(historyId); complete(window_, [begin, value = std::move(value)]() mutable { begin(std::move(value)); }); });
    else begin(L"");
}

// 编辑控件仅属于当前展开卡片；添加、删除和排序操作只在此状态创建。
void App::buildEditor() {
    destroyControls(); initializing_ = true;
    control(L"STATIC", L"资料名称", 0, 0, 32, 5, 110, 21);
    control(L"EDIT", draft_->name, ES_AUTOHSCROLL, Name, 32, 30, -32, 31);
    control(L"STATIC", L"所属分组", 0, 0, 32, 70, 100, 21);
    auto group = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, GroupCombo, 32, 95, -32, 240); populateGroups(group, false, draft_->groupId);
    control(L"STATIC", L"备注（可选）", 0, 0, 32, 136, 150, 21);
    control(MSFTEDIT_CLASS, draft_->note, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, Note, 32, 161, -32, 52);
    int y = 231;
    for (size_t i = 0; i < draft_->fields.size(); ++i) {
        int id = FieldBase + static_cast<int>(i) * 10;
        auto drag = control(L"STATIC", L"≡", SS_NOTIFY, id + 6, 30, y, 17, 29); SetWindowSubclass(drag, controlProc, 1, reinterpret_cast<DWORD_PTR>(this));
        control(L"EDIT", draft_->fields[i].name, ES_AUTOHSCROLL, id, 50, y, -193, 29);
        control(L"BUTTON", L"敏感", BS_AUTOCHECKBOX, id + 2, -183, y, 62, 29);
        control(L"BUTTON", L"↑", BS_PUSHBUTTON, id + 3, -116, y, 26, 29);
        control(L"BUTTON", L"↓", BS_PUSHBUTTON, id + 4, -86, y, 26, 29);
        control(L"BUTTON", L"×", BS_PUSHBUTTON, id + 5, -56, y, 29, 29);
        auto value = control(MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, id + 1, 32, y + 35, -32, 88);
        if (draft_->fields[i].sensitive) SendMessageW(value, EM_SETPASSWORDCHAR, L'●', 0);
        SetWindowTextW(value, draft_->fields[i].value.c_str()); SendMessageW(value, EM_EMPTYUNDOBUFFER, 0, 0);
        check(list_, id + 2, draft_->fields[i].sensitive);
        EnableWindow(GetDlgItem(list_, id + 3), i > 0); EnableWindow(GetDlgItem(list_, id + 4), i + 1 < draft_->fields.size()); EnableWindow(GetDlgItem(list_, id + 5), draft_->fields.size() > 1);
        y += 153;
    }
    control(L"BUTTON", L"+ 添加字段", BS_PUSHBUTTON, AddField, 32, y, 120, 30);
    if (draft_->id) control(L"BUTTON", L"删除资料", BS_PUSHBUTTON, DeleteRecord, 166, y, 100, 30);
    initializing_ = false; layout(); InvalidateRect(list_, nullptr, TRUE);
}
void App::collectDraft() {
    if (!draft_) return;
    draft_->name = windowText(GetDlgItem(list_, Name)); draft_->note = windowText(GetDlgItem(list_, Note)); draft_->groupId = comboId(GetDlgItem(list_, GroupCombo));
    for (size_t i = 0; i < draft_->fields.size(); ++i) { int id = FieldBase + static_cast<int>(i) * 10; auto& f = draft_->fields[i]; f.name = windowText(GetDlgItem(list_, id)); wipe(f.value); f.value = windowText(GetDlgItem(list_, id + 1)); f.sensitive = checked(list_, id + 2); }
}
void App::saveDraft() {
    if (!draft_ || saving_) return;
    collectDraft();
    if (draft_->name.empty() || draft_->fields.empty()) { error(L"请输入资料名称，并保留至少一个字段。"); return; }
    bool downgrade = false, secret = false;
    for (const auto& f : draft_->fields) { if (f.name.empty() || f.value.empty()) { error(L"每个字段都需要名称和值。"); return; } downgrade |= f.wasSensitive && !f.sensitive; secret |= f.sensitive; }
    if (downgrade && !confirm(L"改为普通字段后，内容将以明文存储并参与搜索。继续保存吗？")) return;
    auto draft = std::make_shared<Draft>(*draft_); saving_ = true; EnableWindow(list_, FALSE);
    worker_->post([this, draft, secret](Database& db) {
        auto id = db.save(*draft); auto record = db.record(id);
        complete(window_, [this, record = std::move(record), secret, fromHistory = draft->historyId != 0] {
            saving_ = false; EnableWindow(list_, TRUE); destroyControls(); draft_.reset(); dirty_ = false; collapse(); expanded_ = record;
            initializing_ = true; SetWindowTextW(search_, L""); initializing_ = false;
            groupFilter_ = -1; updateGroupFilters(); layout(); refresh(false, true);
            if (secret && fromHistory) notice(L"已移除原普通历史。其他剪贴板工具可能仍有副本。"); else notice(L"资料已保存");
        });
    });
}
void App::cancelDraft() {
    if (saving_) return;
    if (dirty_ && !confirm(L"放弃尚未保存的更改吗？")) return;
    bool wasNew = draft_ && draft_->id == 0;
    destroyControls(); draft_.reset(); dirty_ = false; EnableWindow(list_, TRUE);
    if (wasNew) collapse();
    if (settingsMode_ || groupsMode_ || appendMode_) { settingsMode_ = groupsMode_ = appendMode_ = false; wipe(appendValue_); collapse(); }
    layout(); refresh(false, !wasNew); SetFocus(qrFocus());
}

void App::settingsPanel() {
    if (!leave()) return;
    settingsMode_ = true; scroll_ = 0; destroyControls(); initializing_ = true;
    control(L"STATIC", L"设置", 0, 0, 22, 5, 180, 25);
    control(L"STATIC", L"呼出快捷键", 0, 0, 22, 42, -22, 22);
    auto mode = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 310, 22, 70, 176, 150);
    for (auto label : {L"Win + V（替换系统）", L"自定义快捷键"}) SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    bool winV = settings_.modifiers == MOD_WIN && settings_.key == 'V';
    SendMessageW(mode, CB_SETCURSEL, winV ? 0 : 1, 0);
    auto hotkey = control(HOTKEY_CLASSW, L"", 0, 300, 210, 70, -22, 34);
    BYTE mods = 0; if (settings_.modifiers & MOD_CONTROL) mods |= HOTKEYF_CONTROL; if (settings_.modifiers & MOD_SHIFT) mods |= HOTKEYF_SHIFT; if (settings_.modifiers & MOD_ALT) mods |= HOTKEYF_ALT;
    SendMessageW(hotkey, HKM_SETHOTKEY, MAKEWORD(settings_.key, winV ? HOTKEYF_CONTROL | HOTKEYF_SHIFT : mods), 0);
    EnableWindow(hotkey, !winV);
    auto entry = [&](const wchar_t* label, int id, int value, int y) { control(L"STATIC", label, 0, 0, 22, y + 4, -145, 26); control(L"EDIT", std::to_wstring(value), ES_NUMBER | ES_AUTOHSCROLL | ES_RIGHT, id, -127, y, 105, 34); };
    entry(L"历史数量上限（1～100000）", 301, settings_.maxHistory, 121);
    entry(L"保留天数（0 表示不限）", 302, settings_.retentionDays, 165);
    entry(L"单条大小上限（KiB，1～16384）", 303, settings_.maxBytes / 1024, 209);
    entry(L"敏感剪贴板清理秒数（0 关闭）", 304, settings_.clearSeconds, 253);
    control(L"BUTTON", L"点击窗口外部时隐藏", BS_AUTOCHECKBOX, 305, 22, 303, -22, 28); check(list_, 305, settings_.hideOutside);
    control(L"BUTTON", L"登录 Windows 后静默启动（立即生效）", BS_AUTOCHECKBOX, 306, 22, 343, -22, 28); check(list_, 306, settings_.startup);
    control(L"BUTTON", L"暂停记录剪贴板", BS_AUTOCHECKBOX, 307, 22, 383, -22, 28); check(list_, 307, settings_.paused);
    control(L"STATIC", L"外观", 0, 0, 22, 430, 100, 25);
    auto theme = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 308, 132, 427, -22, 160);
    for (auto label : {L"跟随系统", L"浅色", L"深色"}) SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)); SendMessageW(theme, CB_SETCURSEL, settings_.theme, 0);
    control(L"STATIC", L"数据目录", 0, 0, 22, 480, -22, 22);
    control(L"EDIT", dir_.wstring(), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 311, 22, 507, -22, 64);
    control(L"STATIC", L"敏感值由 Windows DPAPI 加密；未知密码仍可能进入普通历史。\r\n其他剪贴板工具、数据库 WAL 和备份可能保留副本。\r\n高级凭据请优先使用专业密码管理器。", 0, 0, 22, 587, -22, 116);
    dirty_ = false; initializing_ = false; layout(); InvalidateRect(list_, nullptr, TRUE);
}

void App::changeStartup() {
    if (saving_) return;
    bool enabled = checked(list_, 306);
    saving_ = true; EnableWindow(list_, FALSE);
    worker_->post([this, enabled](Database& db) {
        std::wstring failure;
        bool actual = enabled;
        try { updateStartup(db, enabled, executablePath(), dir_); }
        catch (const std::exception& e) {
            failure = errorText(e);
            try { actual = startupMatches(StartupRegistration{}.read(), executablePath(), dir_); }
            catch (...) { actual = !enabled; }
        }
        complete(window_, [this, actual, failure] {
            settings_.startup = actual; check(list_, 306, actual);
            saving_ = false; EnableWindow(list_, TRUE);
            if (!failure.empty()) error(failure);
            else notice(actual ? L"已添加登录启动项，下次登录后将在托盘静默运行" : L"已移除登录启动项");
        });
    });
}

// 保存其他本地偏好，快捷键冲突时保留原设置。
void App::saveSettings() {
    if (saving_) return;
    Settings next = settings_;
    next.maxHistory = integer(list_, 301, 1, 100000); next.retentionDays = integer(list_, 302, 0, 3650); next.maxBytes = integer(list_, 303, 1, 16384) * 1024; next.clearSeconds = integer(list_, 304, 0, 3600);
    next.hideOutside = checked(list_, 305); next.paused = checked(list_, 307); next.theme = static_cast<int>(SendDlgItemMessageW(list_, 308, CB_GETCURSEL, 0, 0));
    if (SendDlgItemMessageW(list_, 310, CB_GETCURSEL, 0, 0) == 0) { next.modifiers = MOD_WIN; next.key = 'V'; }
    else {
        WORD hotkey = static_cast<WORD>(SendDlgItemMessageW(list_, 300, HKM_GETHOTKEY, 0, 0));
        BYTE mods = HIBYTE(hotkey); next.key = LOBYTE(hotkey); next.modifiers = 0;
        if (mods & HOTKEYF_CONTROL) next.modifiers |= MOD_CONTROL;
        if (mods & HOTKEYF_SHIFT) next.modifiers |= MOD_SHIFT;
        if (mods & HOTKEYF_ALT) next.modifiers |= MOD_ALT;
        if (!next.key || !(next.modifiers & (MOD_CONTROL | MOD_ALT))) { error(L"请选择包含 Ctrl 或 Alt 的自定义快捷键。"); return; }
    }
    if (!applyHotkey(next)) { error(L"快捷键已被占用或无法接管，请关闭其他随手贴实例或更换组合键。"); return; }
    saving_ = true; EnableWindow(list_, FALSE);
    worker_->post([this, next](Database& db) { db.saveSettings(next); db.prune(next); complete(window_, [this, next] {
        settings_ = next; saving_ = false; EnableWindow(list_, TRUE); dirty_ = false; theme(); cancelDraft(); tray(false);
        notice(L"设置已保存");
        if (next.clearSeconds == 0) KillTimer(window_, SecretTimer);
    }); });
}
void App::groupPanel() {
    if (!groupsMode_ && !leave()) return;
    destroyControls(); groupsMode_ = true; scroll_ = 0; initializing_ = true;
    control(L"STATIC", L"管理分组", 0, 0, 22, 8, -22, 28);
    auto combo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 400, 22, 53, -22, 240);
    auto i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"+ 新建分组")); SendMessageW(combo, CB_SETITEMDATA, i, 0);
    for (const auto& g : groups_) { i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g.name.c_str())); SendMessageW(combo, CB_SETITEMDATA, i, g.id); } SendMessageW(combo, CB_SETCURSEL, 0, 0);
    control(L"STATIC", L"分组名称", 0, 0, 22, 103, -22, 25);
    control(L"EDIT", L"", ES_AUTOHSCROLL, 401, 22, 135, -22, 32);
    control(L"BUTTON", L"保存分组", BS_PUSHBUTTON, 402, 22, 190, 110, 32);
    control(L"BUTTON", L"删除分组…", BS_PUSHBUTTON, 403, 148, 190, 110, 32);
    control(L"STATIC", L"删除分组后，其资料会移入「未分组」。", 0, 0, 22, 251, -22, 55);
    dirty_ = false; initializing_ = false; layout(); InvalidateRect(list_, nullptr, TRUE);
}
void App::appendHistory(Id id) {
    if (!leave()) return;
    auto rev = ++revision_;
    worker_->post([this, id, rev](Database& db) { auto value = db.historyText(id); complete(window_, [this, id, rev, value = std::move(value)]() mutable {
        if (rev != revision_) { wipe(value); return; }
        tab_ = 1; appendMode_ = true; appendHistory_ = id; appendValue_ = std::move(value); groupFilter_ = -1;
        initializing_ = true; SetWindowTextW(search_, L"");
        control(L"EDIT", L"内容", ES_AUTOHSCROLL, 500, 22, 39, -22, 31);
        control(L"BUTTON", L"敏感字段（保存后移除原普通历史）", BS_AUTOCHECKBOX, 501, 22, 82, -22, 30);
        dirty_ = false; initializing_ = false; layout(); refresh();
    }); });
}
void App::saveAppend(Id id) {
    if (saving_) return;
    auto field = std::make_shared<DraftField>(); field->name = windowText(GetDlgItem(list_, 500)); field->value = appendValue_; field->sensitive = checked(list_, 501);
    if (field->name.empty()) { error(L"请输入字段名称。"); return; }
    auto history = appendHistory_; saving_ = true; EnableWindow(list_, FALSE);
    worker_->post([this, field, id, history](Database& db) { db.append(id, *field, history); auto record = db.record(id); complete(window_, [this, record = std::move(record), secret = field->sensitive] {
        saving_ = false; EnableWindow(list_, TRUE); dirty_ = false; destroyControls(); appendMode_ = false; wipe(appendValue_); expanded_ = record; layout(); refresh(false, true);
        notice(secret ? L"字段已添加，原普通历史已移除；其他工具可能仍有副本。" : L"字段已添加");
    }); });
}
void App::menu(int index, POINT point) {
    if (draft_ || settingsMode_ || groupsMode_ || appendMode_ || index < 0 || index >= static_cast<int>(items_.size())) return;
    selected_ = index; InvalidateRect(list_, nullptr, TRUE);
    HMENU menu = CreatePopupMenu(); auto id = items_[index].id;
    if (!tab_) {
        AppendMenuW(menu, MF_STRING, 1, L"复制但不立即粘贴"); AppendMenuW(menu, MF_STRING, 2, L"保存为常用资料"); AppendMenuW(menu, MF_STRING, 3, L"添加为已有资料的字段"); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, 4, L"删除");
    } else { AppendMenuW(menu, MF_STRING, 5, items_[index].pinned ? L"取消置顶" : L"置顶"); AppendMenuW(menu, MF_STRING, 6, L"删除资料…"); }
    inMenu_ = true; int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr); inMenu_ = false; DestroyMenu(menu);
    if (choice == 1) worker_->post([this, id](Database& db) { auto value = db.historyText(id); complete(window_, [this, value = std::move(value)]() mutable { copy(std::move(value), false, false); }); });
    if (choice == 2) create(id);
    if (choice == 3) appendHistory(id);
    if (choice == 4) worker_->post([this, id](Database& db) { db.removeHistory(id); complete(window_, [this] { refresh(); }); });
    if (choice == 5) { bool pinned = !items_[index].pinned; worker_->post([this, id, pinned](Database& db) { db.pin(id, pinned); complete(window_, [this] { refresh(); }); }); }
    if (choice == 6 && confirm(L"删除这条资料及全部字段？此操作无法撤销。")) worker_->post([this, id](Database& db) { db.removeRecord(id); complete(window_, [this] { refresh(); }); });
}

// 以窗口、进程和创建时间共同固定呼出前的粘贴目标，防止句柄重用。
void App::rememberTarget() {
    auto foreground = GetForegroundWindow();
    if (!foreground || foreground == window_ || GetAncestor(foreground, GA_ROOTOWNER) == window_) return;
    target_ = GetAncestor(foreground, GA_ROOT); targetTid_ = GetWindowThreadProcessId(target_, &targetPid_); targetCreation_ = {};
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid_);
    FILETIME exit{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &targetCreation_, &exit, &kernel, &user)) target_ = nullptr;
    if (process) CloseHandle(process);
}
bool App::targetValid() {
    DWORD pid{};
    if (!target_ || !IsWindow(target_) || GetWindowThreadProcessId(target_, &pid) != targetTid_ || pid != targetPid_) return false;
    auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME creation{}, exit{}, kernel{}, user{};
    bool valid = GetProcessTimes(process, &creation, &exit, &kernel, &user) && CompareFileTime(&creation, &targetCreation_) == 0;
    DWORD targetLevel = integrity(process), ownLevel = integrity(GetCurrentProcess());
    valid = valid && targetLevel != MAXDWORD && ownLevel != MAXDWORD && targetLevel <= ownLevel;
    CloseHandle(process); return valid;
}
void App::pasteTarget() {
    if (!targetValid()) { notice(L"内容已复制，请在目标应用手动粘贴。"); return; }
    if (IsIconic(target_)) ShowWindow(target_, SW_RESTORE);
    if (!SetForegroundWindow(target_)) { notice(L"无法恢复目标应用，内容已复制，请手动粘贴。"); return; }
    SetTimer(window_, PasteTimer, 90, nullptr);
}
void App::finishPaste() {
    KillTimer(window_, PasteTimer);
    if (!targetValid() || GetForegroundWindow() != target_ || GetClipboardSequenceNumber() != clipboardSequence_) { notice(L"焦点或剪贴板已变化，请手动粘贴。"); return; }
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) if (GetAsyncKeyState(key) & 0x8000) { notice(L"请松开快捷键后手动粘贴。"); return; }
    INPUT inputs[4]{};
    for (auto& input : inputs) input.type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL; inputs[1].ki.wVk = 'V'; inputs[2].ki.wVk = 'V'; inputs[2].ki.dwFlags = KEYEVENTF_KEYUP; inputs[3].ki.wVk = VK_CONTROL; inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, inputs, sizeof(INPUT));
    if (sent != 4) {
        // 输入部分成功时释放本程序可能按下的按键，避免留下 Ctrl 状态。
        INPUT release[2]{}; release[0].type = release[1].type = INPUT_KEYBOARD; release[0].ki.wVk = 'V'; release[1].ki.wVk = VK_CONTROL; release[0].ki.dwFlags = release[1].ki.dwFlags = KEYEVENTF_KEYUP; SendInput(2, release, sizeof(INPUT));
        notice(L"自动粘贴未完成，内容仍在剪贴板，请手动粘贴。");
    }
}
void App::copy(std::wstring value, bool sensitive, bool paste) {
    if (pendingCopy_) wipe(*pendingCopy_);
    pendingCopy_ = std::make_unique<std::wstring>(std::move(value)); pendingSensitive_ = sensitive; pendingPaste_ = paste; copyRetries_ = 0; finishCopy();
}

// 在发布敏感文本之前先放入系统历史与云同步排除格式。
void App::finishCopy() {
    KillTimer(window_, CopyTimer);
    if (!pendingCopy_) return;
    if (!OpenClipboard(window_)) {
        if (++copyRetries_ <= 10) SetTimer(window_, CopyTimer, 25, nullptr);
        else { wipe(*pendingCopy_); pendingCopy_.reset(); error(L"剪贴板正被其他程序占用，请重试。"); }
        return;
    }
    auto size = (pendingCopy_->size() + 1) * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) { CloseClipboard(); wipe(*pendingCopy_); pendingCopy_.reset(); throw std::runtime_error("剪贴板内存分配失败"); }
    auto data = GlobalLock(memory);
    if (!data) { GlobalFree(memory); CloseClipboard(); wipe(*pendingCopy_); pendingCopy_.reset(); throw std::runtime_error("剪贴板内存锁定失败"); }
    memcpy(data, pendingCopy_->c_str(), size); GlobalUnlock(memory);
    bool okay = EmptyClipboard() != FALSE;
    if (pendingSensitive_ && okay) {
        for (const wchar_t* format : {L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
            auto block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
            UINT id = RegisterClipboardFormatW(format);
            if (!block || !id || !SetClipboardData(id, block)) { if (block) GlobalFree(block); okay = false; break; }
        }
    }
    if (okay) okay = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!okay) { auto p = GlobalLock(memory); if (p) { SecureZeroMemory(p, size); GlobalUnlock(memory); } GlobalFree(memory); }
    CloseClipboard(); clipboardSequence_ = GetClipboardSequenceNumber();
    bool paste = pendingPaste_, sensitive = pendingSensitive_; wipe(*pendingCopy_); pendingCopy_.reset();
    if (!okay) { error(L"写入剪贴板失败，请重试。"); return; }
    KillTimer(window_, SecretTimer); secretSequence_ = 0;
    if (sensitive && settings_.clearSeconds > 0) { secretSequence_ = clipboardSequence_; SetTimer(window_, SecretTimer, settings_.clearSeconds * 1000, nullptr); }
    if (paste) { if (hide()) pasteTarget(); } else notice(sensitive ? L"已复制敏感字段，将按设置自动清理。" : L"已复制");
}

// 只清理仍由本程序持有且序列未变化的敏感剪贴板。
void App::clearSecret() {
    if (!secretSequence_) { KillTimer(window_, SecretTimer); return; }
    if (GetClipboardSequenceNumber() != secretSequence_ || GetClipboardOwner() != window_) { secretSequence_ = 0; KillTimer(window_, SecretTimer); return; }
    if (!OpenClipboard(window_)) { SetTimer(window_, SecretTimer, 1000, nullptr); return; }
    if (GetClipboardSequenceNumber() == secretSequence_) EmptyClipboard();
    CloseClipboard(); secretSequence_ = 0; KillTimer(window_, SecretTimer);
}
void App::capture() {
    KillTimer(window_, CaptureTimer);
    if (!ready_ || settings_.paused || GetClipboardOwner() == window_ || !IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    auto sequence = GetClipboardSequenceNumber();
    if (sequence == clipboardSequence_) return;
    if (!OpenClipboard(window_)) { if (++captureRetries_ < 10) SetTimer(window_, CaptureTimer, 25, nullptr); return; }
    bool excluded = IsClipboardFormatAvailable(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing")) != FALSE;
    UINT historyFormat = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(historyFormat)) {
        auto block = GetClipboardData(historyFormat);
        if (block && GlobalSize(block) >= sizeof(DWORD)) { auto p = static_cast<DWORD*>(GlobalLock(block)); if (p) { excluded |= *p == 0; GlobalUnlock(block); } }
    }
    std::wstring value, source;
    if (!excluded) {
        auto memory = GetClipboardData(CF_UNICODETEXT); auto bytes = memory ? GlobalSize(memory) : 0;
        if (bytes >= sizeof(wchar_t) && bytes <= static_cast<SIZE_T>(settings_.maxBytes) + sizeof(wchar_t)) {
            auto p = static_cast<const wchar_t*>(GlobalLock(memory));
            if (p) { size_t max = bytes / sizeof(wchar_t), n = wcsnlen_s(p, max); if (n < max && n * 2 <= static_cast<size_t>(settings_.maxBytes)) value.assign(p, n); GlobalUnlock(memory); }
        }
        DWORD pid{}; auto owner = GetClipboardOwner(); if (owner) GetWindowThreadProcessId(owner, &pid);
        auto process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (process) { wchar_t path[32768]{}; DWORD n = 32768; if (QueryFullProcessImageNameW(process, 0, path, &n)) source = std::filesystem::path(path).stem().wstring(); CloseHandle(process); }
    }
    CloseClipboard(); clipboardSequence_ = sequence;
    if (value.empty()) return;
    auto settings = settings_;
    worker_->post([this, value = std::move(value), source, settings](Database& db) { db.capture(value, source, settings); complete(window_, [this] { if (tab_ == 0 && !settingsMode_ && !groupsMode_ && !appendMode_ && !draft_ && IsWindowVisible(window_)) refresh(); }); });
}

void App::tray(bool add) {
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1; data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data.uCallbackMessage = WM_TRAY; data.hIcon = smallIcon_;
    auto tip = std::wstring(AppDisplayName) + (settings_.paused ? L" · 记录已暂停" : L" · " + hotkeyLabel(settings_));
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data);
}
void App::notice(const std::wstring& value) {
    status_ = value; InvalidateRect(window_, nullptr, TRUE);
    if (silentLaunch_) return;
    SetTimer(window_, StatusTimer, 7000, nullptr);
    if (!IsWindowVisible(window_)) { NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1; data.uFlags = NIF_INFO; data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND; wcscpy_s(data.szInfoTitle, AppDisplayName); wcsncpy_s(data.szInfo, value.c_str(), _TRUNCATE); Shell_NotifyIconW(NIM_MODIFY, &data); }
}
void App::pause() {
    settings_.paused = !settings_.paused; auto paused = settings_.paused;
    worker_->post([paused](Database& db) { db.setSetting(L"paused", paused); }); tray(false); InvalidateRect(window_, nullptr, TRUE);
}

void App::clearHistory() {
    if (!ready_ || saving_ || tab_ != 0 || settingsMode_ || groupsMode_ || draft_ || appendMode_) return;
    if (!confirm(L"清空全部剪贴板历史？常用资料不会受影响。此操作无法撤销。", true)) return;
    KillTimer(window_, SearchTimer); ++revision_; loading_ = false; saving_ = true;
    EnableWindow(list_, FALSE); EnableWindow(search_, FALSE); EnableWindow(historyClear_, FALSE);
    worker_->post([this](Database& db) {
        db.clearHistory();
        complete(window_, [this] {
            saving_ = false; EnableWindow(list_, TRUE); items_.clear(); selected_ = 0; scroll_ = 0;
            layout(); refresh(); notice(L"剪贴板历史已清空");
        });
    });
}

bool App::key(MSG& msg) {
    if (msg.message != WM_KEYDOWN || GetAncestor(msg.hwnd, GA_ROOT) != window_) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (recordDrag_.pressed) { if (msg.wParam == VK_ESCAPE) finishRecordDrag(false); return true; }
    if (msg.wParam == VK_APPS || (msg.wParam == VK_F10 && GetKeyState(VK_SHIFT) < 0)) {
        if (std::find(groupButtons_.begin(), groupButtons_.end(), msg.hwnd) != groupButtons_.end()) { RECT rect{}; GetWindowRect(msg.hwnd, &rect); groupMenu({rect.left, rect.bottom}); return true; }
    }
    if (ctrl && msg.hwnd != qrInput_ && (msg.wParam == VK_RIGHT || msg.wParam == VK_LEFT)) { switchTab((tab_ + (msg.wParam == VK_RIGHT ? 1 : 2)) % 3); return true; }
    if (qrRecognitionPage() && ctrl && msg.wParam == 'V') { if (!(msg.lParam & (1LL << 30))) pasteQrImage(); return true; }
    if (qrPage() && msg.hwnd == qrInput_ && ctrl && msg.wParam == 'A') { SendMessageW(qrInput_, EM_SETSEL, 0, -1); return true; }
    if (msg.wParam == VK_ESCAPE) {
        if (draft_ || settingsMode_ || groupsMode_ || appendMode_) cancelDraft();
        else if (expanded_.id) { collapse(); layout(); SetFocus(search_); }
        else hide();
        return true;
    }
    if (ctrl && msg.wParam == 'S') { if (draft_) saveDraft(); else if (settingsMode_) saveSettings(); return true; }
    if (qrPage()) return false;
    bool root = msg.hwnd == search_ || msg.hwnd == list_ || msg.hwnd == historyTab_ || msg.hwnd == recordsTab_;
    if (!root || draft_ || settingsMode_ || groupsMode_) return false;
    if (msg.wParam == VK_DOWN || msg.wParam == VK_UP) { navigate(msg.wParam == VK_DOWN ? 1 : -1); return true; }
    if (msg.wParam == VK_RETURN) { activate(selected_); return true; }
    if (msg.wParam == VK_APPS || (msg.wParam == VK_F10 && GetKeyState(VK_SHIFT) < 0)) { POINT p{px(60), px(40)}; ClientToScreen(list_, &p); menu(selected_, p); return true; }
    return false;
}

LRESULT App::message(UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreated_ && msg == taskbarCreated_) { tray(true); return 0; }
    switch (msg) {
    case WM_NOTIFY: {
        auto header = reinterpret_cast<NMHDR*>(lp);
        if (header->hwndFrom != tooltips_ || header->code != TTN_GETDISPINFOW) break;
        tooltipText_.clear();
        auto button = std::find(groupButtons_.begin(), groupButtons_.end(), reinterpret_cast<HWND>(header->idFrom));
        if (button != groupButtons_.end()) {
            auto index = static_cast<size_t>(button - groupButtons_.begin());
            if (index < visibleGroups_.size()) tooltipText_ = filters_[visibleGroups_[index]].name;
        } else if (header->idFrom == 1) {
            POINT point{}; GetCursorPos(&point); ScreenToClient(list_, &point);
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) if (PtInRect(&it->rect, point)) {
                if (it->action == CopyField) tooltipText_ = L"复制此字段，保留窗口";
                if (it->action == PasteField) tooltipText_ = L"粘贴到呼出前的应用";
                if (it->action == Edit) tooltipText_ = L"编辑这条资料";
                if (it->action == Reveal) tooltipText_ = L"显示或隐藏敏感字段";
                if (it->action == FullText) tooltipText_ = L"原地展开或收起全文";
                if (qrPage() && it->action == CopyQrValue) tooltipText_ = L"点击复制原始值";
                if (qrPage() && it->action == CopyQrId) tooltipText_ = L"点击复制 ID";
                if (qrPage() && it->action == SaveQrImage) tooltipText_ = L"点击选择位置，保存为 PNG 图片";
                if (it->action == Open && tab_ == 1 && !draft_ && !appendMode_) tooltipText_ = L"点击展开资料，长按标题拖拽排序";
                break;
            }
        }
        reinterpret_cast<NMTTDISPINFOW*>(lp)->lpszText = tooltipText_.data();
        return 0;
    }
    case WM_CONTEXTMENU: {
        auto source = reinterpret_cast<HWND>(wp);
        if (std::find(groupButtons_.begin(), groupButtons_.end(), source) != groupButtons_.end()) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (point.x == -1) { RECT rect{}; GetWindowRect(source, &rect); point = {rect.left, rect.bottom}; }
            groupMenu(point); return 0;
        }
        break;
    }
    case WM_NCCALCSIZE: return 0;
    case WM_NCPAINT: return 0;
    case WM_NCACTIVATE: return TRUE;
    case WM_NCHITTEST: return hitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_WINV: show(); return 0;
    case WM_APP + 1: show(); return 0;
    case WM_DATABASE_ERROR: { std::unique_ptr<DatabaseError> value(reinterpret_cast<DatabaseError*>(lp)); error(value->text); if (value->fatal) { DestroyWindow(window_); PostQuitMessage(1); } return 0; }
    case WM_COMPLETE: { std::unique_ptr<std::function<void()>> callback(reinterpret_cast<std::function<void()>*>(lp)); (*callback)(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; auto dc = BeginPaint(window_, &ps); RECT r{}; GetClientRect(window_, &r); auto mem = CreateCompatibleDC(dc); auto bitmap = CreateCompatibleBitmap(dc, std::max(1L, r.right), std::max(1L, r.bottom)); auto old = SelectObject(mem, bitmap); paint(mem); BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, old); DeleteObject(bitmap); DeleteDC(mem); EndPaint(window_, &ps); return 0; }
    case WM_SIZE: finishRecordDrag(false); layout(); return 0;
    case WM_GETMINMAXINFO: { auto info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {px(400), px(420)}; return 0; }
    case WM_DPICHANGED: { if (draft_) collectDraft(); dpi_ = HIWORD(wp); loadIcons(); fonts(); auto r = reinterpret_cast<RECT*>(lp); SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); layout(); return 0; }
    case WM_SETTINGCHANGE: theme(); return 0;
    case WM_LBUTTONDOWN: {
        RECT client{}; GetClientRect(window_, &client); RECT search{px(20), px(87), client.right - px(20), px(123)};
        if (!settingsMode_ && !groupsMode_ && tab_ != 2 && IsWindowEnabled(search_) && PtInRect(&search, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) { SetFocus(search_); return 0; }
        break;
    }
    case WM_CLOSE: hide(); return 0;
    case WM_CANCELMODE: finishRecordDrag(false); return 0;
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_MINIMIZE) { hide(); return 0; } break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) finishRecordDrag(false);
        // 键盘切换和正常失焦保持可见；只有显式开启的窗外点击才隐藏。
        // 二维码页保持可见，便于切到文件管理器选择并拖入图片。
        if (LOWORD(wp) == WA_INACTIVE && settings_.hideOutside && !qrPage() && !inMenu_ && !saving_ && IsWindowVisible(window_) && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) {
            auto next = reinterpret_cast<HWND>(lp);
            if (!next || (GetAncestor(next, GA_ROOTOWNER) != window_ && !IsChild(window_, next))) hide();
        }
        return 0;
    case WM_HOTKEY: if (hotkeyRegistered_ && wp == static_cast<WPARAM>(hotkeyId_)) show(); return 0;
    case WM_CLIPBOARDUPDATE: captureRetries_ = 0; pendingSequence_ = GetClipboardSequenceNumber(); capture(); return 0;
    case WM_TIMER:
        if (wp == QrGenerateTimer) generateQrPreview();
        if (wp == RecordPressTimer) startRecordDrag();
        if (wp == RecordDragTimer) recordDragTick();
        if (wp == SearchTimer) { KillTimer(window_, SearchTimer); refresh(); }
        if (wp == CaptureTimer) capture();
        if (wp == SecretTimer) clearSecret();
        if (wp == PasteTimer) finishPaste();
        if (wp == CopyTimer) finishCopy();
        if (wp == StatusTimer) { KillTimer(window_, StatusTimer); status_.clear(); InvalidateRect(window_, nullptr, TRUE); }
        if (wp == PruneTimer) { auto s = settings_; worker_->post([s](Database& db) { db.prune(s); }); }
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) show();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"显示随手贴"); AppendMenuW(menu, MF_STRING, 2, settings_.paused ? L"恢复记录" : L"暂停记录"); AppendMenuW(menu, MF_STRING, 3, L"设置"); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, 4, L"退出");
            POINT p{}; GetCursorPos(&p); inMenu_ = true; SetForegroundWindow(window_); int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr); inMenu_ = false; DestroyMenu(menu); PostMessageW(window_, WM_NULL, 0, 0);
            if (choice == 1) show(); if (choice == 2) pause(); if (choice == 3) { show(false); settingsPanel(); } if (choice == 4 && leave()) DestroyWindow(window_);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), notification = HIWORD(wp);
        if (initializing_) return 0;
        if (id == QrInput && notification == EN_CHANGE) { qrInputChanged(); return 0; }
        if (notification == CBN_DROPDOWN) guardComboWheel(reinterpret_cast<HWND>(lp), list_);
        if (id == Search && notification == EN_CHANGE) {
            if (draft_) { if (!leave()) return 0; layout(); }
            layoutSearch(); SetTimer(window_, SearchTimer, 90, nullptr); return 0;
        }
        if (id == HistoryTab) switchTab(0);
        else if (id == RecordsTab) switchTab(1);
        else if (id == QrTab) switchTab(2);
        else if (id == QrRecognize || id == QrGenerate) switchQrMode(id == QrGenerate);
        else if (id == QrSelectImage && notification == BN_CLICKED) selectQrImage();
        else if (id == Minimize || id == Close) hide();
        else if (id >= GroupFilterBase && static_cast<size_t>(id - GroupFilterBase) < visibleGroups_.size() && GetParent(reinterpret_cast<HWND>(lp)) == window_) selectGroup(filters_[visibleGroups_[id - GroupFilterBase]].id);
        else if (id == GroupMore) groupMenu();
        else if (id == SearchClear) { if (!saving_) { SetWindowTextW(search_, L""); SetFocus(search_); } }
        else if (id == HistoryClear) clearHistory();
        else if (id == SettingsButton) { if (ready_) settingsPanel(); }
        else if (id == New) { if (ready_) create(); }
        else if (id == Save) { if (draft_) saveDraft(); else if (settingsMode_) saveSettings(); }
        else if (id == Cancel) cancelDraft();
        else if (draft_) {
            if (id == AddField) { collectDraft(); draft_->fields.push_back({}); dirty_ = true; buildEditor(); UpdateWindow(list_); setScroll(totalHeight_); SetFocus(GetDlgItem(list_, FieldBase + static_cast<int>(draft_->fields.size() - 1) * 10)); }
            else if (id == DeleteRecord && confirm(L"删除这条资料及全部字段？此操作无法撤销。")) { auto record = draft_->id; saving_ = true; worker_->post([this, record](Database& db) { db.removeRecord(record); complete(window_, [this] { saving_ = false; dirty_ = false; leave(); layout(); refresh(); }); }); }
            else if (id >= FieldBase && notification == BN_CLICKED) {
                int index = (id - FieldBase) / 10, kind = (id - FieldBase) % 10;
                if (index >= static_cast<int>(draft_->fields.size())) return 0;
                if (kind >= 2 && kind <= 5) {
                    collectDraft(); dirty_ = true;
                    if (kind == 2) { auto control = GetDlgItem(list_, FieldBase + index * 10 + 1); SendMessageW(control, EM_SETPASSWORDCHAR, draft_->fields[index].sensitive ? L'●' : 0, 0); InvalidateRect(control, nullptr, TRUE); }
                    if (kind == 3 && index > 0) { std::swap(draft_->fields[index], draft_->fields[index - 1]); buildEditor(); }
                    if (kind == 4 && index + 1 < static_cast<int>(draft_->fields.size())) { std::swap(draft_->fields[index], draft_->fields[index + 1]); buildEditor(); }
                    if (kind == 5 && draft_->fields.size() > 1) { draft_->fields.erase(draft_->fields.begin() + index); buildEditor(); }
                }
            }
            if (notification == EN_CHANGE || notification == CBN_SELCHANGE) dirty_ = true;
        }
        else if (settingsMode_) {
            if (id == 310 && notification == CBN_SELCHANGE) EnableWindow(GetDlgItem(list_, 300), SendDlgItemMessageW(list_, 310, CB_GETCURSEL, 0, 0) == 1);
            if (id == 306 && notification == BN_CLICKED) changeStartup();
            else if (notification == EN_CHANGE || notification == CBN_SELCHANGE || (id >= 305 && id <= 307 && notification == BN_CLICKED)) dirty_ = true;
        }
        else if (groupsMode_) {
            if (id == 400 && notification == CBN_SELCHANGE) { if (dirty_ && !confirm(L"放弃尚未保存的分组名称？")) return 0; auto group = comboId(GetDlgItem(list_, 400)); std::wstring name; for (const auto& g : groups_) if (g.id == group) name = g.name; initializing_ = true; SetWindowTextW(GetDlgItem(list_, 401), name.c_str()); initializing_ = false; dirty_ = false; }
            if (id == 401 && notification == EN_CHANGE) dirty_ = true;
            if (id == 402) { auto group = comboId(GetDlgItem(list_, 400)); auto name = windowText(GetDlgItem(list_, 401)); if (name.empty()) error(L"请输入分组名称。"); else { saving_ = true; EnableWindow(list_, FALSE); worker_->post([this, group, name](Database& db) { db.saveGroup(group, name); complete(window_, [this] { dirty_ = false; refreshGroups(); }); }); } }
            if (id == 403) { auto group = comboId(GetDlgItem(list_, 400)); if (group && confirm(L"删除分组并将其资料移入「未分组」？")) { saving_ = true; EnableWindow(list_, FALSE); worker_->post([this, group](Database& db) { db.removeGroup(group); complete(window_, [this] { groupFilter_ = -1; dirty_ = false; refreshGroups(); }); }); } }
        }
        else if (appendMode_ && (notification == EN_CHANGE || notification == BN_CLICKED)) dirty_ = true;
        InvalidateRect(window_, nullptr, TRUE); return 0;
    }
    case WM_MEASUREITEM: { auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp); if (item->CtlType == ODT_COMBOBOX) { item->itemHeight = px(30); return TRUE; } break; }
    case WM_DRAWITEM: {
        auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw->CtlType == ODT_COMBOBOX) {
            bool selected = (draw->itemState & ODS_SELECTED) != 0;
            auto fill = CreateSolidBrush(selected ? accentFill_ : card_); FillRect(draw->hDC, &draw->rcItem, fill); DeleteObject(fill);
            if (draw->itemID != static_cast<UINT>(-1)) {
                auto length = SendMessageW(draw->hwndItem, CB_GETLBTEXTLEN, draw->itemID, 0);
                if (length != CB_ERR) {
                    std::wstring label(static_cast<size_t>(length) + 1, L'\0'); SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, reinterpret_cast<LPARAM>(label.data())); label.resize(static_cast<size_t>(length));
                    RECT rect = draw->rcItem; rect.left += px(12); rect.right -= px(8);
                    text(draw->hDC, label, rect, font_, selected ? accentInk_ : ink_);
                }
            }
        } else paintButton(*draw);
        return TRUE;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: {
        auto control = reinterpret_cast<HWND>(lp); wchar_t kind[64]{}; GetClassNameW(control, kind, 64);
        bool surface = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || wcscmp(kind, L"EDIT") == 0 || wcscmp(kind, HOTKEY_CLASSW) == 0 || draft_;
        SetTextColor(reinterpret_cast<HDC>(wp), IsWindowEnabled(control) ? ink_ : muted_);
        SetBkColor(reinterpret_cast<HDC>(wp), surface ? card_ : bg_);
        return reinterpret_cast<LRESULT>(surface ? cardBrush_ : background_);
    }
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wp) DestroyWindow(window_); return 0;
    case WM_DESTROY: {
        stopQr();
        finishRecordDrag(false);
        if (recordGhost_) { DestroyWindow(recordGhost_); recordGhost_ = nullptr; }
        winVHotkey_.stop();
        if (worker_) worker_->stop();
        clearSecret();
        KillTimer(window_, PasteTimer); KillTimer(window_, SecretTimer); KillTimer(window_, CopyTimer); KillTimer(window_, PruneTimer); RemoveClipboardFormatListener(window_); if (hotkeyRegistered_) UnregisterHotKey(window_, hotkeyId_);
        NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.uID = 1; Shell_NotifyIconW(NIM_DELETE, &data); PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(window_, msg, wp, lp);
}
}
