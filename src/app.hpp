#pragma once
#include "database.hpp"
#include "hotkey.hpp"
#include "ui.hpp"
#include <commctrl.h>
#include <memory>
#include <map>
#include <set>
#include <string_view>

namespace qc {
class App {
public:
    App(HINSTANCE instance, std::filesystem::path dir);
    ~App();
    int run(bool hidden);
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK listProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK controlProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK recordGhostProc(HWND, UINT, WPARAM, LPARAM);
private:
    struct Hit { RECT rect; int action; int index; };
    struct RecordDrag {
        bool pressed{}, active{}, after{};
        Id id{}, anchor{};
        POINT start{}, point{};
        RECT origin{}, marker{};
    };
    static constexpr UINT RecordPressTimer = 8, RecordDragTimer = 9;
    RecordDrag recordDrag_;
    HWND recordGhost_{};
    bool suppressRecordClick_{};
    enum class ControlKind { Plain, Edit, Multiline, Combo, Hotkey, Check, Button };
    struct Control { HWND window; int x; int y; int width; int height; ControlKind kind = ControlKind::Plain; };
    struct FieldLayout {
        int height = 40;
        int valueHeight = 20;
        int lineHeight = 18;
        bool expandable{};
        std::vector<std::pair<size_t, size_t>> lines;
    };
    HINSTANCE instance_;
    std::filesystem::path dir_;
    HWND window_{}, list_{}, search_{}, searchClear_{}, historyClear_{}, historyTab_{}, recordsTab_{}, new_{}, settingsButton_{}, minimizeButton_{}, closeButton_{}, groupMore_{}, footerSave_{}, footerCancel_{};
    HWND hoveredButton_{};
    HWND tooltips_{};
    std::vector<HWND> groupButtons_;
    std::vector<Group> filters_;
    std::vector<size_t> visibleGroups_;
    std::vector<size_t> hiddenGroups_;
    HFONT font_{}, bold_{}, titleFont_{}, small_{};
    HBRUSH background_{}, cardBrush_{};
    HICON icon_{}, smallIcon_{};
    HMODULE richEdit_{};
    UINT dpi_ = 96;
    COLORREF bg_{}, card_{}, ink_{}, muted_{}, line_{}, accent_{}, accentFill_{}, accentInk_{}, accentTint_{};
    std::unique_ptr<Worker> worker_;
    Settings settings_;
    std::vector<Item> items_;
    std::vector<Group> groups_;
    std::vector<Hit> hits_;
    std::vector<Control> controls_;
    std::vector<std::pair<int, int>> rows_;
    Record expanded_;
    std::unique_ptr<Draft> draft_;
    std::map<Id, std::wstring> revealed_;
    std::map<Id, FieldLayout> fieldLayouts_;
    std::set<Id> fullFields_;
    std::wstring tooltipText_;
    std::wstring status_;
    int tab_{}, selected_{}, fieldSelected_ = -1, scroll_{}, totalHeight_{}, viewportHeight_{};
    int revision_{}, recordRevision_{};
    int dragIndex_ = -1;
    int hoveredAction_{};
    int hoveredIndex_ = -1;
    Id groupFilter_ = -1;
    bool ready_{}, more_{}, loading_{}, dirty_{}, initializing_{}, saving_{}, settingsMode_{}, groupsMode_{}, appendMode_{}, inMenu_{}, dark_{}, silentLaunch_{}, searchComposing_{};
    Id appendHistory_{};
    std::wstring appendValue_;
    DWORD clipboardSequence_{}, secretSequence_{}, pendingSequence_{};
    unsigned captureRetries_{};
    std::unique_ptr<std::wstring> pendingCopy_;
    bool pendingSensitive_{}, pendingPaste_{};
    int copyRetries_{};
    HWND target_{};
    DWORD targetPid_{}, targetTid_{};
    FILETIME targetCreation_{};
    UINT taskbarCreated_{};
    bool hotkeyRegistered_{};
    int hotkeyId_ = 1;
    UINT boundModifiers_{}, boundKey_{};
    WinVHotkey winVHotkey_;
    int px(int n) const { return MulDiv(n, dpi_, 96); }
    int logical(int n) const { return MulDiv(n, 96, dpi_); }
    LRESULT message(UINT, WPARAM, LPARAM);
    LRESULT listMessage(UINT, WPARAM, LPARAM);
    void initialize();
    void theme();
    void fonts();
    // 从嵌入的多尺寸资源加载窗口与托盘图标，并跟随显示缩放更新。
    void loadIcons();
    void layout();
    // 在统一搜索框内安排图标、输入区与按内容显示的清除按钮。
    void layoutSearch();
    // 圆角输入容器共用留白和焦点边框，保留原生文本输入行为。
    void paintInput(HDC dc, RECT rect, bool focused, bool enabled = true);
    // 绘制表单输入容器，坐标随卡片位置和表单滚动同步。
    void paintForm(HDC dc);
    // 原生下拉框和复选框保留交互与可访问性，外观跟随应用主题。
    void paintChoice(HWND control, HDC dc, bool combo);
    // 将表单布局转换成当前卡片与滚动位置下的客户区坐标。
    RECT controlRect(const Control& control) const;
    void updateListMetrics();
    // 重建全部、业务分组、未分组的筛选顺序。
    void updateGroupFilters();
    // 按可用宽度创建所需数量的分组按钮，其余放入更多菜单。
    void layoutGroups(int width);
    // 检查未保存内容后切换分组，并重新查询当前资料。
    void selectGroup(Id id);
    // 更多菜单和分组右键菜单共用选择与管理入口。
    void groupMenu(POINT point = {-1, -1});
    // 长按资料标题进入排序，普通单击仍展开或收起卡片。
    void pressRecord(int index, RECT origin, POINT point);
    void startRecordDrag();
    // 计算卡片间的插入线，拖动到列表边缘时滚动并按需加载下一页。
    void moveRecordDrag(POINT point);
    void recordDragTick();
    // 有效松手后保存资料顺序，取消和保存失败均保留原顺序。
    void finishRecordDrag(bool drop);
    void paintRecordGhost(HDC dc);
    void paintRecordGrip(HDC dc, int x, int y);
    // 设置主题、状态或布局变化后同步刷新父子区域，清除旧背景。
    void redraw();
    void paintButton(const DRAWITEMSTRUCT& draw);
    // 在紧凑字段行和标题中绘制编辑、粘贴及敏感值显示操作。
    void paintAction(HDC dc, RECT rect, int action, int index);
    // 按实际字体和字段宽度缓存多行文本高度，绘制时不重新测量全文。
    void measureFields();
    // 用窗口边缘调整尺寸，顶栏空白区域用于拖动整个窗口。
    LRESULT hitTest(POINT screen) const;
    // 普通组合键注册系统热键，Win+V 使用专用钩子。
    bool applyHotkey(const Settings& settings);
    void paint(HDC dc);
    void paintList(HDC dc);
    void text(HDC dc, std::wstring_view, RECT, HFONT, COLORREF, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    void card(HDC dc, RECT, bool active);
    // 按当前标签和查询异步读取一页结果，丢弃过期查询的返回值。
    void refresh(bool append = false, bool keepExpanded = false);
    void refreshGroups();
    void populateGroups(HWND combo, bool filter, Id chosen);
    // 呼出时记住原应用，并复位历史标签、查询和展开状态。
    void show(bool reset = true);
    // 处理未保存更改后隐藏窗口，保留托盘和监听。
    bool hide();
    // 固定用户呼出前的原应用，供随后安全恢复和粘贴。
    void rememberTarget();
    // 校验原窗口仍属于同一进程实例，并且权限允许模拟输入。
    bool targetValid();
    // 恢复原应用，待其获得前台焦点后再发送粘贴按键。
    void pasteTarget();
    void finishPaste();
    // 写入选中内容；敏感值同时设置系统历史排除和定时清理。
    void copy(std::wstring value, bool sensitive, bool paste);
    void finishCopy();
    // 读取剪贴板事件中的受限长度文本，跳过自身写入和排除格式。
    void capture();
    void clearSecret();
    void tray(bool add);
    // 可见时更新状态栏，隐藏时用托盘提示保留剪贴板的回退结果。
    void notice(const std::wstring& text);
    bool confirm(const std::wstring& text, bool defaultYes = false);
    // 离开当前业务状态前确认草稿，并销毁暂存的明文和控件。
    bool leave();
    void collapse();
    // 切换当前标签并清空查询，不注册额外系统快捷键。
    void switchTab(int tab);
    // 在当前列表中展开唯一一条资料，键盘打开时选中首字段。
    void openRecord(int index, bool keyboard = false);
    // 历史直接粘贴；资料先展开，展开后的 Enter 操作选中字段。
    void activate(int index);
    // 在卡片或展开字段间移动键盘选择，并保持其可见。
    void navigate(int delta);
    void ensureVisible(int index);
    void setScroll(int position);
    int itemHeight(const Item&) const;
    int fieldHeight(const Field&) const;
    // 分发卡片内查看、编辑、复制和粘贴操作。
    void action(int action, int index);
    // 按当前标签提供历史转换、删除和资料置顶等临时菜单。
    void menu(int index, POINT point);
    // 仅在主动编辑时解密敏感字段并建立临时草稿。
    void edit();
    // 在当前列表顶部插入草稿，可从独立历史内容初始化。
    void create(Id historyId = 0);
    void buildEditor();
    // 收集当前表单内容，排序和重新创建控件前保留草稿。
    void collectDraft();
    // 校验全部字段，确认敏感降级风险后使用事务一次性保存。
    void saveDraft();
    // 取消草稿并恢复查看状态，未保存修改必须先确认。
    void cancelDraft();
    // 销毁编辑控件前清空文本，缩短敏感明文驻留时间。
    void destroyControls();
    HWND control(const wchar_t* kind, const std::wstring& text, DWORD style, int id, int x, int y, int w, int h);
    // 按可用宽度和滚动位置安排控件，并给输入文字保留内边距。
    void positionControls();
    // 在原窗口列表区域展示轻量设置面板。
    void settingsPanel();
    void saveSettings();
    // 勾选后立即同步系统启动项，不依赖其他设置的保存和取消。
    void changeStartup();
    // 在原窗口管理一级分组，不创建独立业务窗口。
    void groupPanel();
    // 展示历史追加字段的内嵌资料选择流程。
    void appendHistory(Id id);
    // 将历史作为新字段追加到指定资料，敏感时移除源历史。
    void saveAppend(Id id);
    // 在编辑控件消费消息前处理应用级键盘导航。
    bool key(MSG& msg);
    // 持久化暂停状态并同步托盘提示。
    void pause();
    // 确认后异步清空全部历史，保留常用资料并刷新当前搜索结果。
    void clearHistory();
    void error(const std::wstring& text);
};
}
