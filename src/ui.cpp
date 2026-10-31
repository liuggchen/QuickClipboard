#include "ui.hpp"
#include <commctrl.h>
#include <algorithm>
#include <numeric>

namespace qc {
std::vector<Group> groupFilters(const std::vector<Group>& groups) {
    std::vector<Group> result{{-1, L"全部"}};
    result.insert(result.end(), groups.begin(), groups.end());
    result.push_back({0, L"未分组"});
    return result;
}

GroupPacking packGroupChips(const std::vector<int>& widths, int available, size_t selected, const std::function<int(size_t)>& moreWidth, int gap) {
    GroupPacking result;
    if (widths.empty()) return result;
    available = std::max(0, available);
    auto used = [&](const std::vector<size_t>& indices) {
        Id total = indices.empty() ? 0 : static_cast<Id>(indices.size() - 1) * gap;
        for (size_t index : indices) total += widths[index];
        return total;
    };
    std::vector<size_t> all(widths.size()); std::iota(all.begin(), all.end(), 0);
    if (used(all) <= available) { result.visible = std::move(all); return result; }
    auto fits = [&](const std::vector<size_t>& indices) {
        return used(indices) + (indices.empty() ? 0 : gap) + moreWidth(widths.size() - indices.size()) <= available;
    };
    for (size_t i = 0; i < widths.size(); ++i) {
        auto next = result.visible; next.push_back(i);
        if (!fits(next)) break;
        result.visible = std::move(next);
    }
    if (selected < widths.size() && std::find(result.visible.begin(), result.visible.end(), selected) == result.visible.end()) {
        auto pinned = result.visible; pinned.push_back(selected);
        while (pinned.size() > 1 && !fits(pinned)) pinned.erase(pinned.end() - 2);
        if (fits(pinned)) result.visible = std::move(pinned);
    }
    for (size_t i = 0; i < widths.size(); ++i) if (std::find(result.visible.begin(), result.visible.end(), i) == result.visible.end()) result.hidden.push_back(i);
    if (!result.hidden.empty()) result.moreWidth = std::clamp(moreWidth(result.hidden.size()), 0, std::max(0, available - static_cast<int>(used(result.visible)) - (result.visible.empty() ? 0 : gap)));
    return result;
}

TextWrapping wrapFieldText(HDC dc, std::wstring_view value, int width, size_t maxLines) {
    TextWrapping result;
    size_t offset = 0;
    for (;;) {
        size_t end = value.find_first_of(L"\r\n", offset);
        if (end == std::wstring_view::npos) end = value.size();
        size_t count = 0;
        if (offset < end) {
            int fit{}; SIZE size{};
            int available = static_cast<int>(std::min<size_t>(std::clamp(width * 2, 64, 4096), end - offset));
            GetTextExtentExPointW(dc, value.data() + offset, available, std::max(1, width), &fit, nullptr, &size);
            count = static_cast<size_t>(std::max(1, fit));
            if (offset + count < end && count > 2) {
                size_t space = value.find_last_of(L" \t", offset + count - 1);
                if (space != std::wstring_view::npos && space > offset + count / 3) count = space - offset + 1;
            }
            if (count && offset + count < end && value[offset + count - 1] >= 0xd800 && value[offset + count - 1] <= 0xdbff) --count;
            if (!count) count = std::min<size_t>(2, end - offset);
        }
        result.lines.push_back({offset, count});
        offset += count;
        if (maxLines && result.lines.size() >= maxLines) { result.truncated = offset < value.size(); break; }
        if (offset < end) continue;
        if (end == value.size()) break;
        offset = end + 1;
        if (value[end] == L'\r' && offset < value.size() && value[offset] == L'\n') ++offset;
    }
    return result;
}

namespace {
// 拦截发生在原生组合框处理之前，保留点击和方向键的正常选择行为。
LRESULT CALLBACK comboWheelProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        if (id == 0xcb21 && message == WM_MOUSEWHEEL && !SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0)) {
            auto target = reinterpret_cast<HWND>(ref);
            if (IsWindow(target)) SendMessageW(target, message, wp, lp);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, comboWheelProc, id);
    return DefSubclassProc(window, message, wp, lp);
}
}
void guardComboWheel(HWND combo, HWND scrollTarget) {
    SetWindowSubclass(combo, comboWheelProc, 0xcb21, reinterpret_cast<DWORD_PTR>(scrollTarget));
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info) && info.hwndList) SetWindowSubclass(info.hwndList, comboWheelProc, 0xcb22, 0);
}
}
