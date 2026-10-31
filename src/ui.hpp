#pragma once
#include "core.hpp"
#include <functional>
#include <string_view>

namespace qc {
struct GroupPacking {
    std::vector<size_t> visible;
    std::vector<size_t> hidden;
    int moreWidth{};
};
struct TextWrapping {
    std::vector<std::pair<size_t, size_t>> lines;
    bool truncated{};
};
// 统一筛选与编辑下拉框的顺序，未分组始终位于业务分组之后。
std::vector<Group> groupFilters(const std::vector<Group>& groups);
// 按文字宽度填满单行，只在必要时收纳，并优先保留当前分组。
GroupPacking packGroupChips(const std::vector<int>& widths, int available, size_t selected, const std::function<int(size_t)>& moreWidth, int gap = 5);
// 按字体宽度换行，长 URL 也可断开；只保存原文索引，不复制敏感文本。
TextWrapping wrapFieldText(HDC dc, std::wstring_view value, int width, size_t maxLines = 0);
// 禁止组合框和其弹出列表通过滚轮改选；关闭状态把滚动交给外层列表。
void guardComboWheel(HWND combo, HWND scrollTarget);
}
