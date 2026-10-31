#include "app.hpp"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace qc {
LRESULT CALLBACK App::recordGhostProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wp, lp);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; auto dc = BeginPaint(window, &ps); RECT rect{}; GetClientRect(window, &rect);
        auto mem = CreateCompatibleDC(dc); auto bitmap = CreateCompatibleBitmap(dc, std::max(1L, rect.right), std::max(1L, rect.bottom)); auto old = SelectObject(mem, bitmap);
        app->paintRecordGhost(mem); BitBlt(dc, 0, 0, rect.right, rect.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bitmap); DeleteDC(mem); EndPaint(window, &ps); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

// 按下资料标题只记录手势，普通松手保留原来的展开行为。
void App::pressRecord(int index, RECT origin, POINT point) {
    finishRecordDrag(false);
    recordDrag_.pressed = true; recordDrag_.id = items_.at(index).id;
    recordDrag_.origin = origin; recordDrag_.start = recordDrag_.point = point;
    SetCapture(list_); SetTimer(window_, RecordPressTimer, 400, nullptr);
}

// 长按后显示浮动标题与拖拽柄，并切换为四向移动指针。
void App::startRecordDrag() {
    KillTimer(window_, RecordPressTimer);
    if (!recordDrag_.pressed || saving_ || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { finishRecordDrag(false); return; }
    auto source = std::find_if(items_.begin(), items_.end(), [this](const Item& item) { return item.id == recordDrag_.id; });
    if (source == items_.end()) { finishRecordDrag(false); return; }
    recordDrag_.active = true; SendMessageW(tooltips_, TTM_POP, 0, 0);
    if (!recordGhost_) recordGhost_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"QuickClipboardRecordDrag", L"", WS_POPUP, 0, 0, 1, 1, window_, nullptr, instance_, this);
    if (!recordGhost_) { finishRecordDrag(false); throw std::runtime_error("无法显示资料拖拽提示"); }
    SetLayeredWindowAttributes(recordGhost_, 0, 235, LWA_ALPHA);
    auto dc = GetDC(list_); auto oldFont = SelectObject(dc, font_); SIZE size{};
    GetTextExtentPoint32W(dc, source->title.data(), static_cast<int>(source->title.size()), &size);
    SelectObject(dc, oldFont); ReleaseDC(list_, dc);
    SetWindowPos(recordGhost_, HWND_TOPMOST, 0, 0, px(std::clamp(logical(size.cx) + 42, 100, 280)), px(34), SWP_NOMOVE | SWP_NOACTIVATE);
    SetTimer(window_, RecordDragTimer, 50, nullptr); moveRecordDrag(recordDrag_.point);
}

// 卡片顺序在松手前保持稳定，仅移动插入线；置顶边界和列表外不可放置。
void App::moveRecordDrag(POINT point) {
    if (!recordDrag_.pressed) return;
    recordDrag_.point = point;
    if (!recordDrag_.active) {
        if (std::abs(point.x - recordDrag_.start.x) > std::max(px(6), GetSystemMetricsForDpi(SM_CXDRAG, dpi_)) || std::abs(point.y - recordDrag_.start.y) > std::max(px(6), GetSystemMetricsForDpi(SM_CYDRAG, dpi_))) KillTimer(window_, RecordPressTimer);
        return;
    }
    recordDrag_.anchor = 0; recordDrag_.marker = {};
    RECT client{}; GetClientRect(list_, &client);
    auto source = std::find_if(items_.begin(), items_.end(), [this](const Item& item) { return item.id == recordDrag_.id; });
    if (source == items_.end()) { finishRecordDrag(false); return; }
    if (point.x >= px(18) && point.x < client.right - px(18) && point.y >= 0 && point.y < client.bottom && rows_.size() == items_.size()) {
        int y = logical(point.y) + scroll_;
        for (size_t i = 0; i < rows_.size(); ++i) {
            auto [top, height] = rows_[i];
            bool before = y < top + height / 2;
            if (!before && i + 1 < rows_.size() && y >= rows_[i + 1].first) continue;
            if (items_[i].pinned == source->pinned) {
                recordDrag_.anchor = items_[i].id; recordDrag_.after = !before;
                int line = (before ? top - 4 : top + height + 4) - scroll_;
                line = std::clamp(px(line), 0, std::max(0, static_cast<int>(client.bottom) - px(2)));
                recordDrag_.marker = {px(18), line, client.right - px(18), line + px(2)};
            }
            break;
        }
    }
    POINT screen = point; ClientToScreen(list_, &screen);
    SetWindowPos(recordGhost_, HWND_TOPMOST, screen.x + px(14), screen.y + px(16), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCursor(LoadCursorW(nullptr, recordDrag_.anchor ? IDC_SIZEALL : IDC_NO));
    InvalidateRect(recordGhost_, nullptr, FALSE); InvalidateRect(list_, nullptr, FALSE);
}

// 贴近列表边缘持续滚动，底部还有资料时追加加载，拖拽始终按 ID 定位。
void App::recordDragTick() {
    if (!recordDrag_.active) return;
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { finishRecordDrag(false); return; }
    POINT point{}; GetCursorPos(&point); ScreenToClient(list_, &point);
    RECT client{}; GetClientRect(list_, &client);
    if (PtInRect(&client, point)) {
        if (point.y < px(28)) setScroll(scroll_ - 12);
        else if (point.y >= client.bottom - px(28)) {
            setScroll(scroll_ + 12);
            if (more_ && !loading_ && scroll_ + viewportHeight_ >= totalHeight_ - 60) refresh(true, true);
        }
    }
    moveRecordDrag(point);
}

// 在当前筛选内预览移动，后台保存全局资料顺序；失败恢复原列表。
void App::finishRecordDrag(bool drop) {
    if (!recordDrag_.pressed) return;
    auto drag = recordDrag_; recordDrag_ = {}; suppressRecordClick_ = true;
    KillTimer(window_, RecordPressTimer); KillTimer(window_, RecordDragTimer);
    if (GetCapture() == list_) ReleaseCapture();
    if (recordGhost_) ShowWindow(recordGhost_, SW_HIDE);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW)); InvalidateRect(list_, nullptr, FALSE);
    auto find = [this](Id id) { return std::find_if(items_.begin(), items_.end(), [id](const Item& item) { return item.id == id; }); };
    auto source = find(drag.id);
    if (!drop || source == items_.end()) return;
    if (!drag.active) { if (PtInRect(&drag.origin, drag.point)) openRecord(static_cast<int>(source - items_.begin())); return; }
    if (!drag.anchor || drag.anchor == drag.id || find(drag.anchor) == items_.end()) return;
    auto previous = items_;
    Id selection = selected_ >= 0 && selected_ < static_cast<int>(items_.size()) ? items_[selected_].id : drag.id;
    auto item = *source; items_.erase(source); auto target = find(drag.anchor); items_.insert(target + (drag.after ? 1 : 0), item);
    bool changed = false; for (size_t i = 0; i < items_.size(); ++i) if (items_[i].id != previous[i].id) { changed = true; break; }
    if (!changed) return;
    selected_ = static_cast<int>(find(selection) - items_.begin());
    ++revision_; loading_ = false; saving_ = true; EnableWindow(list_, FALSE); layout();
    worker_->post([this, drag, previous = std::move(previous), selection](Database& db) {
        try {
            db.moveRecord(drag.id, drag.anchor, drag.after);
            complete(window_, [this] { saving_ = false; EnableWindow(list_, TRUE); layout(); notice(L"资料顺序已保存"); });
        } catch (const std::exception& e) {
            auto message = errorText(e);
            complete(window_, [this, previous, selection, message] {
                items_ = previous; saving_ = false; EnableWindow(list_, TRUE);
                auto selected = std::find_if(items_.begin(), items_.end(), [selection](const Item& item) { return item.id == selection; });
                selected_ = selected == items_.end() ? 0 : static_cast<int>(selected - items_.begin());
                layout(); error(L"资料排序保存失败，已恢复原顺序。\n" + message);
            });
        }
    });
}

// 六点拖拽柄只在长按激活后显示，保持平时卡片的紧凑布局。
void App::paintRecordGrip(HDC dc, int x, int y) {
    auto brush = CreateSolidBrush(accent_);
    for (int row = -1; row <= 1; ++row) for (int column = 0; column < 2; ++column) {
        RECT dot{x + px(column * 4), y + px(row * 4 - 1), x + px(column * 4 + 2), y + px(row * 4 + 1)}; FillRect(dc, &dot, brush);
    }
    DeleteObject(brush);
}

// 浮动提示只显示资料名称，不复制或解密卡片中的字段内容。
void App::paintRecordGhost(HDC dc) {
    RECT rect{}; GetClientRect(recordGhost_, &rect); FillRect(dc, &rect, cardBrush_);
    auto pen = CreatePen(PS_SOLID, px(1), accent_); auto brush = CreateSolidBrush(accentTint_);
    auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, brush);
    RoundRect(dc, 0, 0, rect.right, rect.bottom, px(7), px(7));
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen); DeleteObject(brush);
    paintRecordGrip(dc, px(10), rect.bottom / 2);
    auto item = std::find_if(items_.begin(), items_.end(), [this](const Item& row) { return row.id == recordDrag_.id; });
    if (item != items_.end()) text(dc, item->title, {px(26), 0, rect.right - px(8), rect.bottom}, font_, accent_);
}
}
