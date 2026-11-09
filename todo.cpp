// ============================================================
// MiniTodo —— 极简待办清单 (纯 Win32 API, 单文件, 无外部依赖)
// 功能: 托盘常驻 / 开机自启 / 置顶悬浮 / 透明度 / 子任务 / WebDAV 备份
// 编译: windres -i todo.rc -o todo.res -O coff
//       g++ -O2 -municode -mwindows -static -o MiniTodo.exe todo.cpp todo.res
//       -lgdi32 -lshell32 -ladvapi32 -ldwmapi -lwinhttp  (或直接运行 build.bat)
// ============================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <imm.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cstdio>
#include <cstdarg>
#include <functional>

// ---------------- 常量 ----------------
#define APP_NAME     L"MiniTodo"
#define APP_VER      L"2.0.8"    // 每次功能修改后递增 (功能+0.1 / 修复+0.0.1)
#define WM_APP_TRAY  (WM_APP + 1)   // 托盘回调
#define WM_APP_DLG   (WM_APP + 2)   // 对话框结果 (lParam = new DlgResult)

// ---------------- DPI 缩放 ----------------
static double g_s = 1.0;   // DPI 缩放系数 (96dpi=1.0)
static UINT g_dpiNow = 96; // 当前窗口 DPI (WM_DPICHANGED 去重防乒乓)
static int  g_logW = 360, g_logH = 340;   // 窗口逻辑尺寸 (96dpi 基准, 由 WM_SIZE 维护)
static bool g_inMove = false; // 正在交互拖动/缩放 (期间不做屏幕包含校验, 避免影响跨屏拖动)
static int SC(int v) { return (int)(v * g_s + 0.5); }   // 像素值按 DPI 缩放
static void RecalcLayout();   // 前置声明: 定义在 MainProc 之后 (依赖 MkFont)
static void CheckDisplayFit(HWND h);   // 前置声明: DPI 自检 (定义在 MainProc 之后)
static void FitWindowToScreen(HWND h);   // 前置声明: DPI 校正 + 窗口完整包含于屏幕工作区 (定义在 MainProc 之后)
static UINT GetWindowDpi(HWND h);   // 前置声明: 取窗口当前生效的 DPI (定义在 MainProc 之后)
static UINT GetMonitorDpi(HWND h);   // 前置声明: 取窗口所在屏幕的真实 DPI (定义在 MainProc 之后)
static UINT GetMonitorDpiOf(HMONITOR mon);   // 前置声明: 取指定屏幕的真实 DPI (定义在 MainProc 之后)
static void RebuildRows();           // 前置声明: 重建显示行 (定义在主窗口绘制区)
static void UpdateTrayTip();         // 前置声明: 更新托盘提示 (定义在托盘区)

// 主窗口布局常量 (96dpi 基准, WinMain 中按实际 DPI 重新赋值)
static int TOPBAR_H = 34;
static int BOTBAR_H = 24;
static int ROW_H    = 26;
static int SUBROW_H = 22;
static int EDGE     = 6;              // 窗口边缘拖拽热区

// 未文档化消息: 系统经主题服务重绘经典非客户区 (无边框窗口必须拦截)
#ifndef WM_NCUAHDRAWCAPTION
#define WM_NCUAHDRAWCAPTION 0x00AE
#endif
#ifndef WM_NCUAHDRAWFRAME
#define WM_NCUAHDRAWFRAME   0x00AF
#endif

// 顶栏按钮 ID
enum { B_ADD = 1, B_TAB0, B_TAB1, B_PIN, B_OPM, B_OPTXT, B_OPP, B_CLOSE, B_SET, B_BG };
// 对话框模式
enum { DLG_ADDTASK = 1, DLG_EDITTASK, DLG_SUBTASK, DLG_SETTINGS, DLG_DAVREST, DLG_COLORPOP, DLG_BKDIFF, DLG_CONFIRM };
// 对话框内部控件 ID
enum { IDC_E1 = 200, IDC_E2, IDC_E3, IDC_LIST, IDC_ADDSUB, IDC_EDITSUB, IDC_DELSUB, IDC_CHKAUTO, IDC_HK, IDC_AUTO, IDC_FOLDER, IDC_KEEP, IDC_REST, IDC_DELB, IDC_BKMODE0, IDC_BKMODE1, IDC_BKMODE2, IDC_HOURS, IDC_SYNC };
#define BGCOUNT 9
static int DLG_TOP = 30;   // 弹窗自绘标题栏高度 (96dpi 基准, WinMain 中缩放)

// ---------------- 数据模型 ----------------
struct SubTask { std::wstring text, due; bool done = false; };
struct Task    { std::wstring text, due; bool done = false, expanded = false; std::vector<SubTask> subs; };
struct RowRef  { int t, s; };   // s=-1 表示任务行
struct TBtn    { int id; RECT r; const wchar_t* txt; };

// ---------------- 全局状态 ----------------
static std::vector<Task> g_tasks;
static std::vector<RowRef> g_rows;
static int  g_view = 0;          // 0=待完成 1=已完成
static int  g_scroll = 0;
static BYTE g_alpha = 240;       // 窗口透明度
static bool g_topmost = true;
static int  g_hoverBtn = 0, g_hoverRow = -1;
static std::wstring g_webdavUrl, g_webdavUser, g_webdavPass;
static std::wstring g_webdavDir = L"miniTodo";   // 远端备份文件夹
static int g_webdavKeepDays = 7;                 // 备份保留天数 (超过自动清理)
static std::string g_lastBackupSig;              // 上次成功备份时的任务数据指纹 (内存态)
static std::string g_lastSyncSig;                // 实时同步: 上次推送到云端 state.json 的内容指纹 (内存态)
static bool g_syncDialogOpen = false;            // 同步冲突对话框打开中 (暂停轮询, 避免重复弹窗)
static int g_wdBkMode = 0;                       // 自动备份模式: 0=关 1=实时 2=定时
static int g_wdSync = 0;                         // 实时同步开关 (与备份相互独立): 1=多台电脑间保持一致
static int g_wdBkHours = 4;                      // 定时备份间隔 (小时)
static bool g_showStart = true;                  // 启动时显示主窗口

// 定时器 ID
#define TIMER_AUTOBK_DEBOUNCE 2   // 实时备份防抖 (10 秒, 单次)
#define TIMER_AUTOBK_TICK     3   // 定时备份 (可配置小时数, 周期)
#define TIMER_DPICHECK        4   // DPI/拓扑自检 (1 秒兜底, 部分显示模式切换不触发任何消息)
#define TIMER_SYNC_POLL       5   // 实时同步拉取轮询 (30 秒)
static COLORREF g_bg = RGB(250, 250, 250);     // 窗口背景色
static HBRUSH   g_bgBrush = nullptr;           // 背景色画刷 (随 g_bg 重建)
static UINT g_hkMods = MOD_CONTROL | MOD_ALT;  // 显示/隐藏快捷键
static UINT g_hkVk = 'M';
static std::wstring g_hkStr = L"Ctrl+Alt+M";

static HWND  g_hwnd = nullptr;
static HFONT fUI, fRow, fSub, fStrike, fStrikeSub, fGear;
static HICON g_icon;
static UINT  g_msgTaskbarCreated;
static NOTIFYICONDATAW g_nid = {};
static TBtn  g_btns[10];
static int   g_nbtns = 0;
static RECT  g_tabRects[2];          // 底栏标签 (待完成/已完成)
static int   g_hoverTab = -1;
static wchar_t g_pct[16] = L"94%";   // 透明度文本 (静态缓冲, 避免悬空指针)
static std::wstring g_cfgPath;

// ---------------- 设置辅助 ----------------
static int  PctOfAlpha(BYTE a)  { return (a * 100 + 127) / 255; }          // alpha -> 百分比
static BYTE AlphaOfPct(int p)   { if (p < 30) p = 30; if (p > 100) p = 100; return (BYTE)(p * 255 / 100); }
static COLORREF DarkBg(int d) {   // 背景色加深 (顶栏/底栏)
    auto c = [d](int v) { return (COLORREF)std::max(0, v - d); };
    return RGB(c(GetRValue(g_bg)), c(GetGValue(g_bg)), c(GetBValue(g_bg)));
}
// ---------------- GDI 对象缓存 (避免每帧反复创建/销毁画刷画笔) ----------------
struct BrEntry { COLORREF c; HBRUSH h; };
static BrEntry g_brCache[16];
static HBRUSH CachedBrush(COLORREF c) {
    for (auto& e : g_brCache) if (e.h && e.c == c) return e.h;
    for (auto& e : g_brCache) if (!e.h) { e.c = c; e.h = CreateSolidBrush(c); return e.h; }
    return CreateSolidBrush(c);   // 缓存满: 退回临时创建 (调用方负责删除)
}
static void ClearBrushCache() { for (auto& e : g_brCache) { if (e.h) DeleteObject(e.h); e.h = nullptr; } }
struct PenEntry { COLORREF c; int w; HPEN h; };
static PenEntry g_penCache[16];
static HPEN CachedPen(COLORREF c, int w) {
    for (auto& e : g_penCache) if (e.h && e.c == c && e.w == w) return e.h;
    for (auto& e : g_penCache) if (!e.h) { e.c = c; e.w = w; e.h = CreatePen(PS_SOLID, w, c); return e.h; }
    return CreatePen(PS_SOLID, w, c);
}
static void ClearPenCache() { for (auto& e : g_penCache) { if (e.h) DeleteObject(e.h); e.h = nullptr; } }
static void UpdateBgBrush() {
    if (g_bgBrush) DeleteObject(g_bgBrush);
    g_bgBrush = CreateSolidBrush(g_bg);
    ClearBrushCache(); ClearPenCache();   // 依赖背景色的加深色画刷随之失效
}
// 可选背景色 (浅色系, 与深色文字兼容)
static const COLORREF kBgSwatches[BGCOUNT] = {
    RGB(250, 250, 250), RGB(236, 236, 236), RGB(234, 242, 250), RGB(234, 247, 236),
    RGB(250, 243, 222), RGB(250, 234, 238), RGB(240, 234, 250), RGB(226, 240, 238),
    RGB(245, 230, 219),
};
static std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_WIN)     s += L"Win+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    wchar_t buf[32] = L"";
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { buf[0] = (wchar_t)vk; buf[1] = 0; }
    else if (vk >= VK_F1 && vk <= VK_F12) wsprintfW(buf, L"F%d", vk - VK_F1 + 1);
    else GetKeyNameTextW((LONG)(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16), buf, 32);
    return s + buf;
}
static bool ParseHotkey(const std::wstring& s, UINT* mods, UINT* vk) {
    UINT m = 0;
    std::wstring key;
    size_t start = 0;
    while (true) {
        size_t p = s.find(L'+', start);
        std::wstring tok = (p == std::wstring::npos) ? s.substr(start) : s.substr(start, p - start);
        if (p == std::wstring::npos) { key = tok; break; }
        if (_wcsicmp(tok.c_str(), L"ctrl") == 0 || _wcsicmp(tok.c_str(), L"control") == 0) m |= MOD_CONTROL;
        else if (_wcsicmp(tok.c_str(), L"alt") == 0)  m |= MOD_ALT;
        else if (_wcsicmp(tok.c_str(), L"win") == 0 || _wcsicmp(tok.c_str(), L"windows") == 0) m |= MOD_WIN;
        else if (_wcsicmp(tok.c_str(), L"shift") == 0) m |= MOD_SHIFT;
        else return false;
        start = p + 1;
        if (start >= s.size()) return false;
    }
    UINT v = 0;
    if (key.size() == 1) {
        wchar_t ch = towupper(key[0]);
        if ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z')) v = ch;
    } else if (key.size() >= 2 && (key[0] == L'F' || key[0] == L'f')) {
        int n = _wtoi(key.c_str() + 1);
        if (n >= 1 && n <= 12) v = VK_F1 + n - 1;
    }
    if (!v || !m) return false;
    *mods = m; *vk = v;
    return true;
}

// ---------------- 基础工具 ----------------
static std::wstring U8W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
// 多行编辑框输入的任务文本压成单行: 换行/回车/Tab → 空格并合并连续空白 (列表行内不显示硬换行)
static std::wstring OneLine(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    bool sp = false;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') { sp = true; continue; }
        if (sp && !o.empty()) o += L' ';
        sp = false;
        o += c;
    }
    return o;
}
// 编译日期时间 (取自编译器宏, 每次编译自动更新)
static std::wstring BuildStamp() {
    const char* mons[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    const char* m = __DATE__;   // "Mmm dd yyyy"
    int mo = 0;
    for (int i = 0; i < 12; ++i)
        if (m[0] == mons[i][0] && m[1] == mons[i][1] && m[2] == mons[i][2]) { mo = i + 1; break; }
    wchar_t buf[40];
    wsprintfW(buf, L"%04d-%02d-%02d ", atoi(m + 7), mo, atoi(m + 4));
    return std::wstring(buf) + U8W(__TIME__);
}
static std::wstring GetExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}
static bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    if (sz.QuadPart > 0) ReadFile(h, &out[0], (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);
    out.resize(rd);
    return true;
}
static bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    if (!data.empty()) WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return true;
}
static void UpdatePctText() {
    wsprintfW(g_pct, L"%d%%", PctOfAlpha(g_alpha));
}

// ---------------- 临时调试日志: 跨屏 DPI 适配排查 (问题解决后移除) ----------------
static void DpiLog(const wchar_t* fmt, ...) {
    static wchar_t path[MAX_PATH] = L"";
    if (!path[0]) {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* s = wcsrchr(path, L'\\');
        if (s) *(s + 1) = 0;
        wcscat_s(path, MAX_PATH, L"dpi_debug.log");
    }
    wchar_t msg[400];
    va_list ap; va_start(ap, fmt);
    vswprintf(msg, 400, fmt, ap);
    va_end(ap);
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"[%u ms] %s\n", GetTickCount(), msg);
    fclose(f);
}

// ---------------- JSON 解析 (极简, 仅满足自身配置需求) ----------------
struct JVal {
    int t = 0;   // 0=null 1=bool 2=num 3=str 4=arr 5=obj
    bool b = false; double num = 0; std::wstring s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::wstring, JVal>> obj;
    const JVal* get(const wchar_t* k) const {
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool boolOf(const wchar_t* k, bool dft) const { const JVal* v = get(k); return (v && v->t == 1) ? v->b : dft; }
    std::wstring strOf(const wchar_t* k, const wchar_t* dft = L"") const { const JVal* v = get(k); return (v && v->t == 3) ? v->s : dft; }
    long long intOf(const wchar_t* k, long long dft) const { const JVal* v = get(k); return (v && v->t == 2) ? (long long)v->num : dft; }
};
static int HexV(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void Utf8Push(unsigned cp, std::string& out) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}
struct JParser {
    const char* p; const char* e;
    JParser(const std::string& s) : p(s.data()), e(s.data() + s.size()) {}
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool parse(JVal& v) { ws(); return p < e && parseVal(v); }
    bool parseVal(JVal& v) {
        ws(); if (p >= e) return false;
        char c = *p;
        if (c == '{') return parseObj(v);
        if (c == '[') return parseArr(v);
        if (c == '"') { v.t = 3; return parseStr(v.s); }
        if (c == 't' && e - p >= 4 && !memcmp(p, "true", 4))  { v.t = 1; v.b = true;  p += 4; return true; }
        if (c == 'f' && e - p >= 5 && !memcmp(p, "false", 5)) { v.t = 1; v.b = false; p += 5; return true; }
        if (c == 'n' && e - p >= 4 && !memcmp(p, "null", 4))  { v.t = 0; p += 4; return true; }
        char* end = nullptr; v.t = 2; v.num = strtod(p, &end);
        if (end == p) return false; p = end; return true;
    }
    bool parseStr(std::wstring& out) {
        ++p; std::string u8;
        while (p < e && *p != '"') {
            if (*p == '\\') {
                ++p; if (p >= e) return false;
                switch (*p) {
                case '"':  u8 += '"';  break;
                case '\\': u8 += '\\'; break;
                case '/':  u8 += '/';  break;
                case 'n':  u8 += '\n'; break;
                case 't':  u8 += '\t'; break;
                case 'r':  u8 += '\r'; break;
                case 'b':  u8 += '\b'; break;
                case 'f':  u8 += '\f'; break;
                case 'u': {
                    if (e - p < 5) return false;
                    int h1 = HexV(p[1]), h2 = HexV(p[2]), h3 = HexV(p[3]), h4 = HexV(p[4]);
                    if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return false;
                    unsigned cp = (unsigned)(h1 << 12 | h2 << 8 | h3 << 4 | h4);
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 7 && p[1] == '\\' && p[2] == 'u') {
                        int l1 = HexV(p[3]), l2 = HexV(p[4]), l3 = HexV(p[5]), l4 = HexV(p[6]);
                        if (l1 >= 0 && l2 >= 0 && l3 >= 0 && l4 >= 0) {
                            unsigned lo = (unsigned)(l1 << 12 | l2 << 8 | l3 << 4 | l4);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                        }
                    }
                    Utf8Push(cp, u8);
                    break;
                }
                default: return false;
                }
                ++p;
            } else u8 += *p++;
        }
        if (p >= e) return false;
        ++p;
        out = U8W(u8);
        return true;
    }
    bool parseArr(JVal& v) {
        v.t = 4; ++p;
        ws(); if (p < e && *p == ']') { ++p; return true; }
        while (p < e) {
            JVal c; if (!parseVal(c)) return false;
            v.arr.push_back(std::move(c));
            ws();
            if (p < e && *p == ',') { ++p; continue; }
            if (p < e && *p == ']') { ++p; return true; }
            return false;
        }
        return false;
    }
    bool parseObj(JVal& v) {
        v.t = 5; ++p;
        ws(); if (p < e && *p == '}') { ++p; return true; }
        while (p < e) {
            JVal k;
            ws(); if (p >= e || *p != '"') return false;
            if (!parseStr(k.s)) return false;
            ws(); if (p >= e || *p != ':') return false;
            ++p;
            JVal c; if (!parseVal(c)) return false;
            v.obj.emplace_back(std::move(k.s), std::move(c));
            ws();
            if (p < e && *p == ',') { ++p; continue; }
            if (p < e && *p == '}') { ++p; return true; }
            return false;
        }
        return false;
    }
};
static void JsonEsc(const std::wstring& s, std::string& out) {
    out += '"';
    std::string u8 = WU8(s);
    for (unsigned char c : u8) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}

// ---------------- 配置读写 (绿色软件: 保存在 exe 同目录) ----------------
static void InitCfgPath() {
    std::wstring exe = GetExePath();
    size_t s = exe.find_last_of(L"\\/");
    std::wstring dir = (s == std::wstring::npos) ? L"." : exe.substr(0, s);
    g_cfgPath = dir + L"\\todo.json";
    // 一次性迁移: 旧版本数据存放在 %APPDATA%\MiniTodo
    if (GetFileAttributesW(g_cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t appdata[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            std::wstring old = std::wstring(appdata) + L"\\MiniTodo\\todo.json";
            if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(old.c_str(), g_cfgPath.c_str(), FALSE);
        }
    }
}
static std::string SerializeApp() {
    std::string o = "{\"tasks\":[";
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (i) o += ',';
        o += "{\"text\":";  JsonEsc(t.text, o);
        o += ",\"due\":";   JsonEsc(t.due, o);
        o += t.done ? ",\"done\":true" : ",\"done\":false";
        o += t.expanded ? ",\"expanded\":true" : ",\"expanded\":false";
        o += ",\"subs\":[";
        for (size_t j = 0; j < t.subs.size(); ++j) {
            const SubTask& s = t.subs[j];
            if (j) o += ',';
            o += "{\"text\":"; JsonEsc(s.text, o);
            o += ",\"due\":";  JsonEsc(s.due, o);
            o += s.done ? ",\"done\":true}" : ",\"done\":false}";
        }
        o += "]}";
    }
    RECT rc = { 80, 60, 500, 620 };
    long long lw = g_logW, lh = g_logH, sdpi = g_dpiNow;   // 逻辑尺寸 (96dpi 基准) 与保存时屏幕 DPI
    long long wax = 0, way = 0, waw = 0, wah = 0;          // 保存时所在屏幕的工作区 (用于按相对位置跨屏还原)
    if (g_hwnd) {
        GetWindowRect(g_hwnd, &rc);
        UINT mdpi = GetMonitorDpi(g_hwnd);
        if (mdpi) sdpi = mdpi;
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (mon && GetMonitorInfoW(mon, &mi)) {
            wax = mi.rcWork.left; way = mi.rcWork.top;
            waw = mi.rcWork.right - mi.rcWork.left;
            wah = mi.rcWork.bottom - mi.rcWork.top;
        }
    }
    char ui[512];
    sprintf(ui, "],\"ui\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"alpha\":%d,\"topmost\":%s,\"hotkey\":",
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, (int)g_alpha, g_topmost ? "true" : "false");
    o += ui;
    JsonEsc(g_hkStr, o);
    char extra[192], bg[64];
    sprintf(extra, ",\"lw\":%lld,\"lh\":%lld,\"dpi\":%lld,\"wax\":%lld,\"way\":%lld,\"waw\":%lld,\"wah\":%lld",
            lw, lh, sdpi, wax, way, waw, wah);   // 跨屏/跨 DPI 还原用 (v1.9+)
    o += extra;
    sprintf(bg, ",\"bg\":\"%02X%02X%02X\",\"showstart\":%s}",
            GetRValue(g_bg), GetGValue(g_bg), GetBValue(g_bg), g_showStart ? "true" : "false");
    o += bg;
    o += ",\"webdav\":{\"url\":";  JsonEsc(g_webdavUrl, o);
    o += ",\"user\":";             JsonEsc(g_webdavUser, o);
    o += ",\"pass\":";             JsonEsc(g_webdavPass, o);
    o += ",\"dir\":";              JsonEsc(g_webdavDir, o);
    o += ",\"keepdays\":"; o += std::to_string(g_webdavKeepDays);
    o += ",\"autobk\":"; o += std::to_string(g_wdBkMode);
    o += ",\"sync\":";   o += std::to_string(g_wdSync);
    o += ",\"bkhours\":"; o += std::to_string(g_wdBkHours);
    o += "}";
    o += "}";
    return o;
}
// 仅序列化任务清单 (WebDAV 备份只含待办数据, 不含窗口位置/透明度/WebDAV 配置等本机信息)
static std::string SerializeTasksJson(const std::vector<Task>& ts) {
    std::string o = "{\"tasks\":[";
    for (size_t i = 0; i < ts.size(); ++i) {
        const Task& t = ts[i];
        if (i) o += ',';
        o += "{\"text\":";  JsonEsc(t.text, o);
        o += ",\"due\":";   JsonEsc(t.due, o);
        o += t.done ? ",\"done\":true" : ",\"done\":false";
        o += t.expanded ? ",\"expanded\":true" : ",\"expanded\":false";
        o += ",\"subs\":[";
        for (size_t j = 0; j < t.subs.size(); ++j) {
            const SubTask& s = t.subs[j];
            if (j) o += ',';
            o += "{\"text\":"; JsonEsc(s.text, o);
            o += ",\"due\":";  JsonEsc(s.due, o);
            o += s.done ? ",\"done\":true}" : ",\"done\":false}";
        }
        o += "]}";
    }
    o += "]}";
    return o;
}
static void LoadAppData() {
    std::string data;
    if (!ReadAll(g_cfgPath, data)) {
        // 首次运行 (无配置): 窗口居中显示
        if (g_hwnd) {
            RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int x = wa.left + ((wa.right - wa.left) - SC(420)) / 2;
            int y = wa.top + ((wa.bottom - wa.top) - SC(560)) / 3;
            SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return;
    }
    // 跳过 UTF-8 BOM
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    JVal root;
    if (!JParser(data).parse(root) || root.t != 5) return;
    if (const JVal* ts = root.get(L"tasks")) {
        for (auto& tv : ts->arr) {
            Task t;
            t.text = tv.strOf(L"text");
            t.due = tv.strOf(L"due");
            t.done = tv.boolOf(L"done", false);
            t.expanded = tv.boolOf(L"expanded", false);
            if (const JVal* ss = tv.get(L"subs")) {
                for (auto& sv : ss->arr) {
                    SubTask s;
                    s.text = sv.strOf(L"text");
                    s.due = sv.strOf(L"due");
                    s.done = sv.boolOf(L"done", false);
                    t.subs.push_back(std::move(s));
                }
            }
            g_tasks.push_back(std::move(t));
        }
    }
    if (const JVal* ui = root.get(L"ui")) {
        g_alpha = (BYTE)ui->intOf(L"alpha", 240);
        g_topmost = ui->boolOf(L"topmost", true);
        // 快捷键
        g_hkStr = ui->strOf(L"hotkey", L"Ctrl+Alt+M");
        UINT m2, v2;
        if (ParseHotkey(g_hkStr, &m2, &v2)) { g_hkMods = m2; g_hkVk = v2; }
        else { g_hkMods = MOD_CONTROL | MOD_ALT; g_hkVk = 'M'; g_hkStr = L"Ctrl+Alt+M"; }
        // 背景色 (RRGGBB 十六进制)
        std::wstring bg = ui->strOf(L"bg", L"");
        if (bg.size() == 6) {
            auto hex = [&](const std::wstring& t) { return (int)wcstol(t.c_str(), nullptr, 16); };
            g_bg = RGB(hex(bg.substr(0, 2)), hex(bg.substr(2, 2)), hex(bg.substr(4, 2)));
            UpdateBgBrush();
        }
        g_showStart = ui->boolOf(L"showstart", true);
        long long x = ui->intOf(L"x", 80), y = ui->intOf(L"y", 60);
        long long w = ui->intOf(L"w", SC(420)), h = ui->intOf(L"h", SC(560));
        long long lw = ui->intOf(L"lw", -1), lh = ui->intOf(L"lh", -1);   // 逻辑尺寸 (96dpi 基准, v1.9+)
        long long wax = ui->intOf(L"wax", 0), way = ui->intOf(L"way", 0);
        long long waw = ui->intOf(L"waw", 0), wah = ui->intOf(L"wah", 0);
        if (g_hwnd) {
            // 目标屏: 保存坐标点最近的那块屏 (外接屏被拔掉后自动落到剩余屏)
            POINT spt = { (LONG)x, (LONG)y };
            HMONITOR mon = MonitorFromPoint(spt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            RECT wa;
            if (mon && GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
            else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            // 布局/字体先按目标屏 DPI 重算; 尺寸按"逻辑尺寸 × 目标屏缩放"还原,
            // 笔记本高分屏/外接 FHD 屏等不同缩放率之间物理大小基本一致, 启动后也无需二次跳变
            UINT mdpi = mon ? GetMonitorDpiOf(mon) : 0;
            if (mdpi && mdpi != g_dpiNow) { g_dpiNow = mdpi; g_s = mdpi / 96.0; RecalcLayout(); }
            LONG nw, nh;
            if (lw > 0 && lh > 0) { nw = SC((int)lw); nh = SC((int)lh); }
            else { nw = (LONG)w; nh = (LONG)h; }   // 旧版配置: 沿用像素值
            if (nw < SC(360)) nw = SC(360);
            if (nh < SC(240)) nh = SC(240);
            // 屏幕环境变化 (换屏/改缩放率/拔掉外接屏): 按保存时工作区的相对比例还原位置
            bool sameWa = waw > 0 && wah > 0 && wax == wa.left && way == wa.top
                          && waw == wa.right - wa.left && wah == wa.bottom - wa.top;
            if (!sameWa && waw > 0 && wah > 0) {
                double fx = (x - wax) / (double)waw;
                double fy = (y - way) / (double)wah;
                x = wa.left + (LONG)(fx * (wa.right - wa.left) + 0.5);
                y = wa.top  + (LONG)(fy * (wa.bottom - wa.top) + 0.5);
            }
            // 完整夹入目标屏工作区
            if (x > wa.right - nw)  x = wa.right - nw;
            if (x < wa.left)        x = wa.left;
            if (y > wa.bottom - nh) y = wa.bottom - nh;
            if (y < wa.top)         y = wa.top;
            SetWindowPos(g_hwnd, nullptr, (int)x, (int)y, (int)nw, (int)nh, SWP_NOZORDER | SWP_NOACTIVATE);
            FitWindowToScreen(g_hwnd);   // 兜底: DPI 一致性与包含性自检
        }
    }
    if (const JVal* wd = root.get(L"webdav")) {
        g_webdavUrl = wd->strOf(L"url");
        g_webdavUser = wd->strOf(L"user");
        g_webdavPass = wd->strOf(L"pass");
        g_webdavDir = wd->strOf(L"dir", L"miniTodo");
        if (g_webdavDir.empty()) g_webdavDir = L"miniTodo";
        g_webdavKeepDays = (int)wd->intOf(L"keepdays", 7);
        if (g_webdavKeepDays < 1) g_webdavKeepDays = 1;
        if (g_webdavKeepDays > 365) g_webdavKeepDays = 365;
        // 自动备份模式: 新字段 autobk (0=关 1=实时 2=定时); 兼容旧字段 auto (true → 实时备份)
        if (const JVal* ab = wd->get(L"autobk"))
            g_wdBkMode = (int)ab->num;
        else
            g_wdBkMode = wd->boolOf(L"auto", false) ? 1 : 0;
        g_wdSync = (int)wd->intOf(L"sync", 0) != 0;
        if (g_wdBkMode == 3) {   // 1.7.x: 实时同步曾并入 autobk, 迁移为独立开关
            g_wdSync = 1;
            g_wdBkMode = 0;
        }
        if (g_wdBkMode < 0 || g_wdBkMode > 2) g_wdBkMode = 0;
        g_wdBkHours = (int)wd->intOf(L"bkhours", 4);
        if (g_wdBkHours < 1) g_wdBkHours = 1;
        if (g_wdBkHours > 24) g_wdBkHours = 24;
    }
}
// 实时自动备份防抖 (定义在 WebDAV 模块之后)
static void ScheduleAutoBk();
static void SaveAppData() {
    // 保存前备份上一份, 防止意外丢失
    if (GetFileAttributesW(g_cfgPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(g_cfgPath.c_str(), (g_cfgPath + L".bak").c_str(), FALSE);
    // 原子写入: 先写临时文件再替换, 避免读到写了一半的内容
    std::wstring tmp = g_cfgPath + L".tmp";
    WriteAll(tmp, SerializeApp());
    MoveFileExW(tmp.c_str(), g_cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    // 实时自动备份: 任何数据落盘后安排一次防抖上传 (无任务变化时指纹相同, 不会真正上传)
    ScheduleAutoBk();
}
// 任务数据指纹: 仅任务/子任务的内容、日期、完成状态 (窗口位置等 UI 状态不参与)
static std::string TasksSignature(const std::vector<Task>& ts) {
    std::string s;
    for (auto& t : ts) {
        s += WU8(t.text); s += "|"; s += WU8(t.due); s += t.done ? "|1|" : "|0|";
        for (auto& x : t.subs) {
            s += WU8(x.text); s += "|"; s += WU8(x.due); s += x.done ? "|1|" : "|0|";
        }
        s += "#";
    }
    return s;
}
static std::string TasksSignature() { return TasksSignature(g_tasks); }
// 从配置/备份 JSON 中解析任务清单 (供恢复备份使用)
static bool ParseTasksFromJson(const std::string& data0, std::vector<Task>& out) {
    out.clear();
    std::string data = data0;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    JVal root;
    if (!JParser(data).parse(root) || root.t != 5) return false;
    const JVal* ts = root.get(L"tasks");
    if (!ts || ts->t != 4) return false;
    for (auto& tv : ts->arr) {
        Task t;
        t.text = tv.strOf(L"text");
        t.due = tv.strOf(L"due");
        t.done = tv.boolOf(L"done", false);
        t.expanded = false;
        if (const JVal* ss = tv.get(L"subs")) {
            for (auto& sv : ss->arr) {
                SubTask s;
                s.text = sv.strOf(L"text");
                s.due = sv.strOf(L"due");
                s.done = sv.boolOf(L"done", false);
                t.subs.push_back(std::move(s));
            }
        }
        out.push_back(std::move(t));
    }
    return true;
}

// ---------------- 开机自启 (注册表 HKCU\\...\\Run) ----------------
static bool GetAutoStart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, sz = 0;
    bool ok = RegQueryValueExW(k, APP_NAME, nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && type == REG_SZ && sz > 2;
    RegCloseKey(k);
    return ok;
}
static void SetAutoStart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + GetExePath() + L"\"";
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ---------------- Base64 + WebDAV 备份 ----------------
static std::string B64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned v = 0; size_t i = 0;
    for (unsigned char c : in) {
        v = (v << 8) | c;
        if (++i % 3 == 0) { out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63]; v = 0; }
    }
    size_t rem = in.size() % 3;
    if (rem == 1) { v <<= 16; out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "=="; }
    else if (rem == 2) { v <<= 8; out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += "="; }
    return out;
}
// 通用 WebDAV 请求: 成功返回空串, 否则为错误信息; resp/codeOut 可为空
// extraHdr: 附加 HTTP 头 (CRLF 结尾, 如 MOVE 的 Destination)
static std::wstring WebDavRequest(const wchar_t* verb, const std::wstring& remotePath,
                                  const std::string* send, std::string* resp, DWORD* codeOut,
                                  const char* extraHdr = nullptr) {
    if (codeOut) *codeOut = 0;
    if (Trim(g_webdavUrl).empty()) return L"未配置 WebDAV 地址";
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = (DWORD)-1;
    if (!WinHttpCrackUrl(g_webdavUrl.c_str(), 0, 0, &uc)) return L"WebDAV 地址格式错误";
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);

    std::string auth;
    if (!g_webdavUser.empty() || !g_webdavPass.empty())
        auth = "Authorization: Basic " + B64(WU8(g_webdavUser) + ":" + WU8(g_webdavPass)) + "\r\n";

    std::wstring err;
    HINTERNET hs = WinHttpOpen(APP_NAME L"/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return L"网络初始化失败";
    WinHttpSetTimeouts(hs, 5000, 5000, 5000, 10000);
    HINTERNET hc = WinHttpConnect(hs, host.c_str(), uc.nPort, 0);
    HINTERNET hr = nullptr;
    do {
        if (!hc) { err = L"连接服务器失败"; break; }
        hr = WinHttpOpenRequest(hc, verb, remotePath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!hr) { err = L"创建请求失败"; break; }
        std::wstring headers = L"Content-Type: application/octet-stream\r\n";
        if (wcscmp(verb, L"PROPFIND") == 0) headers += L"Depth: 1\r\n";
        if (!auth.empty()) headers += U8W(auth);
        if (extraHdr) headers += U8W(extraHdr);
        BOOL ok;
        if (send) ok = WinHttpSendRequest(hr, headers.c_str(), -1L, (LPVOID)send->data(), (int)send->size(), (int)send->size(), 0);
        else      ok = WinHttpSendRequest(hr, headers.c_str(), -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!ok) { err = L"发送数据失败"; break; }
        if (!WinHttpReceiveResponse(hr, nullptr)) { err = L"服务器无响应"; break; }
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (codeOut) *codeOut = code;
        if (resp && code < 400) {
            resp->clear();
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(hr, &avail) || !avail) break;
                size_t old = resp->size();
                resp->resize(old + avail);
                DWORD rd = 0;
                if (!WinHttpReadData(hr, &(*resp)[old], avail, &rd)) { resp->resize(old); break; }
                if (!rd) { resp->resize(old); break; }
                resp->resize(old + rd);
            }
        }
        if (code >= 200 && code < 400) err = L"";
        else err = L"服务器返回状态码 " + std::to_wstring(code);
    } while (0);
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return err;
}
// 远端路径: <URL路径>/<备份文件夹>/<文件名>  (fname 为空表示文件夹本身)
static std::wstring DavPath(const std::wstring& fname) {
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    std::wstring path;
    if (WinHttpCrackUrl(g_webdavUrl.c_str(), 0, 0, &uc))
        path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    while (!path.empty() && path.back() == L'/') path.pop_back();
    std::wstring dir = g_webdavDir;
    while (!dir.empty() && (dir.front() == L'/' || dir.front() == L'\\')) dir.erase(0, 1);
    while (!dir.empty() && (dir.back() == L'/' || dir.back() == L'\\')) dir.pop_back();
    if (!dir.empty()) path += L"/" + dir;
    if (!fname.empty()) path += L"/" + fname;
    return path;
}
// 备份文件名: todo-20261103-163045.json (字典序即时间序)
static std::wstring BackupFileName() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    wsprintfW(buf, L"todo-%04u%02u%02u-%02u%02u%02u.json",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}
// 上传备份 (先确保文件夹存在); 返回空串表示成功
static std::wstring WebDavUploadBackup(const std::string& body, const std::wstring& fname) {
    DWORD code = 0;
    WebDavRequest(L"MKCOL", DavPath(L""), nullptr, nullptr, &code);   // 已存在时服务器报 405, 忽略
    return WebDavRequest(L"PUT", DavPath(fname), &body, nullptr, &code);
}
// 列出备份文件 (按时间新→旧排序)
static std::wstring WebDavListBackups(std::vector<std::wstring>& out) {
    out.clear();
    std::string resp;
    DWORD code = 0;
    std::wstring err = WebDavRequest(L"PROPFIND", DavPath(L""), nullptr, &resp, &code);
    if (!err.empty()) return err;
    if (code != 207 && code != 200) return L"服务器返回状态码 " + std::to_wstring(code);
    std::wstring w = U8W(resp);
    std::wstring low = w;
    for (auto& ch : low) ch = towlower(ch);
    size_t pos = 0;
    while ((pos = low.find(L"href", pos)) != std::wstring::npos) {
        size_t a = w.find(L'>', pos);
        if (a == std::wstring::npos) break;
        size_t b = w.find(L'<', a);
        if (b == std::wstring::npos) break;
        std::wstring v = w.substr(a + 1, b - a - 1);
        pos = b;
        size_t slash = v.find_last_of(L'/');
        std::wstring name = slash == std::wstring::npos ? v : v.substr(slash + 1);
        if (name.size() > 10 && name.compare(0, 5, L"todo-") == 0 &&
            name.compare(name.size() - 5, 5, L".json") == 0)
            out.push_back(name);
    }
    std::sort(out.begin(), out.end(), std::greater<std::wstring>());
    return L"";
}
static std::wstring WebDavDownload(const std::wstring& fname, std::string& body) {
    DWORD code = 0;
    return WebDavRequest(L"GET", DavPath(fname), nullptr, &body, &code);
}
// 构造远端文件的完整 URL (MOVE 的 Destination 头要求绝对地址)
static std::wstring DavFullUrl(const std::wstring& fname) {
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(g_webdavUrl.c_str(), 0, 0, &uc)) return DavPath(fname);   // 解析失败退回相对路径
    std::wstring full = std::wstring(uc.lpszScheme, uc.dwSchemeLength) + L"://"
                      + std::wstring(uc.lpszHostName, uc.dwHostNameLength);
    if ((uc.nScheme == INTERNET_SCHEME_HTTPS && uc.nPort != 443)
        || (uc.nScheme == INTERNET_SCHEME_HTTP && uc.nPort != 80))
        full += L":" + std::to_wstring(uc.nPort);
    return full + DavPath(fname);
}
// 上传实时同步主文件 state.json: 先 PUT 到 state.tmp 再 MOVE 原子替换,
// 避免其他机器 GET 到写了一半的内容; MOVE 不被支持时回退为直接覆盖 PUT
static std::wstring WebDavUploadState(const std::string& body) {
    WebDavRequest(L"MKCOL", DavPath(L""), nullptr, nullptr, nullptr);   // 已存在时 405, 忽略
    std::wstring err = WebDavRequest(L"PUT", DavPath(L"state.tmp"), &body, nullptr, nullptr);
    if (!err.empty()) return err;
    std::wstring dest = L"Destination: " + DavFullUrl(L"state.json") + L"\r\nOverwrite: T\r\n";
    err = WebDavRequest(L"MOVE", DavPath(L"state.tmp"), nullptr, nullptr, nullptr, WU8(dest).c_str());
    if (err.empty()) return L"";
    return WebDavRequest(L"PUT", DavPath(L"state.json"), &body, nullptr, nullptr);   // 回退: 直接覆盖
}
// 从备份文件名解析日期 (todo-YYYYMMDD-...) -> FILETIME 当天零点; 失败返回 false
static bool BackupFileDate(const std::wstring& name, FILETIME& ft) {
    if (name.size() < 13 || name.compare(0, 5, L"todo-") != 0) return false;
    auto num = [&](int a, int b) { return (WORD)wcstol(name.substr(a, b).c_str(), nullptr, 10); };
    SYSTEMTIME st = {};
    st.wYear = num(5, 4); st.wMonth = num(9, 2); st.wDay = num(11, 2);
    if (!st.wYear || st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1 || st.wDay > 31) return false;
    return SystemTimeToFileTime(&st, &ft) != 0;
}
// 从备份文件名解析完整时间 (todo-YYYYMMDD-HHMMSS.json) -> FILETIME; 仅含日期也能解析
static bool BackupFileDateTime(const std::wstring& name, FILETIME& ft) {
    auto num = [&](int a, int b) { return (WORD)wcstol(name.substr(a, b).c_str(), nullptr, 10); };
    SYSTEMTIME st = {};
    st.wYear = num(5, 4); st.wMonth = num(9, 2); st.wDay = num(11, 2);
    if (!st.wYear || st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1 || st.wDay > 31) return false;
    if (name.size() >= 25) {   // 含时间部分
        st.wHour = num(14, 2); st.wMinute = num(16, 2); st.wSecond = num(18, 2);
        if (st.wHour > 23 || st.wMinute > 59 || st.wSecond > 59) return false;
    }
    return SystemTimeToFileTime(&st, &ft) != 0;
}
// 清理超过保留天数的备份 (按文件名中的日期计算)
static void RotateBackups() {
    if (Trim(g_webdavUrl).empty()) return;
    std::vector<std::wstring> files;
    if (!WebDavListBackups(files).empty()) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    now.wHour = now.wMinute = now.wSecond = now.wMilliseconds = 0;
    FILETIME ftNow;
    if (!SystemTimeToFileTime(&now, &ftNow)) return;
    for (auto& f : files) {
        FILETIME ft;
        if (!BackupFileDate(f, ft)) continue;   // 无法识别日期的文件不动
        long long ageSec = (*(long long*)&ftNow - *(long long*)&ft) / 10000000LL;
        if (ageSec > (long long)g_webdavKeepDays * 86400)
            WebDavRequest(L"DELETE", DavPath(f), nullptr, nullptr, nullptr);
    }
}
// ---------------- 自动备份 ----------------
// 判断两个任务是否内容完全一致 (文字/日期/完成状态/子任务)
static bool TaskEqual(const Task& x, const Task& y) {
    if (x.text != y.text || x.due != y.due || x.done != y.done || x.subs.size() != y.subs.size()) return false;
    for (size_t i = 0; i < x.subs.size(); ++i)
        if (x.subs[i].text != y.subs[i].text || x.subs[i].due != y.subs[i].due || x.subs[i].done != y.subs[i].done)
            return false;
    return true;
}
// 对比本地与云端任务差异 (以任务文字为标识), 生成提示行
static std::vector<std::wstring> BuildTaskDiff(const std::vector<Task>& loc, const std::vector<Task>& cld) {
    std::vector<std::wstring> out;
    auto fmtDue = [](const Task& t) { return t.due.empty() ? std::wstring() : (L" (截止 " + t.due + L")"); };
    for (auto& t : loc) {
        const Task* m = nullptr;
        for (auto& u : cld) if (u.text == t.text) { m = &u; break; }
        if (!m) out.push_back(L"仅本地有: " + t.text + fmtDue(t));
        else if (!TaskEqual(t, *m)) out.push_back(L"内容不同: " + t.text);
    }
    for (auto& u : cld) {
        bool has = false;
        for (auto& t : loc) if (t.text == u.text) { has = true; break; }
        if (!has) out.push_back(L"仅云端有: " + u.text + fmtDue(u));
    }
    if (out.empty()) out.push_back(L"任务条目顺序存在差异");
    if (out.size() > 12) {
        out.resize(12);
        out.push_back(L"…… 差异较多, 仅显示前 12 条");
    }
    return out;
}
static void OpenBkDiffDialog(HWND owner, const std::vector<std::wstring>& diffs,
                             const std::vector<Task>& cloudTasks, const std::wstring& bkName,
                             bool isSync = false);   // 定义在对话框框架之后
// 实时同步推送: 本地有未上传的修改时, 序列化任务清单覆盖云端 state.json
static void DoSyncPush() {
    if (!g_wdSync || Trim(g_webdavUrl).empty()) return;
    std::string sig = TasksSignature();
    if (sig == g_lastSyncSig) return;   // 无未上传修改
    SaveAppData();
    std::string body = SerializeTasksJson(g_tasks);   // 同步只含待办数据, 不含本机配置
    if (!WebDavUploadState(body).empty()) return;     // 失败静默, 下次触发/轮询时重试
    g_lastSyncSig = sig;
}
// 实时同步拉取 (30 秒轮询): 云端有其他机器的修改则应用; 两端都有修改时弹出差异选择
static void SyncPoll() {
    if (!g_wdSync || Trim(g_webdavUrl).empty() || !g_hwnd || g_syncDialogOpen) return;
    std::string body;
    DWORD code = 0;
    std::wstring err = WebDavRequest(L"GET", DavPath(L"state.json"), nullptr, &body, &code);
    if (!err.empty()) return;                        // 网络/服务器错误: 静默, 等下次轮询
    if (code == 404) { DoSyncPush(); return; }       // 云端还没有主文件: 推送本地
    std::vector<Task> ts;
    if (!ParseTasksFromJson(body, ts)) return;       // 内容异常: 忽略, 不覆盖本地
    std::string csig = TasksSignature(ts), lsig = TasksSignature();
    if (csig == lsig) { g_lastSyncSig = csig; g_lastBackupSig = csig; return; }   // 云端与本地一致
    if (csig == g_lastSyncSig) return;               // 云端就是本地上次推的内容: 本地较新, 等防抖上传
    if (lsig == g_lastSyncSig) {                     // 本地无未上传修改: 静默应用云端
        g_tasks = ts;
        for (auto& t : g_tasks) t.expanded = false;
        g_lastSyncSig = csig;
        SaveAppData(); RebuildRows(); UpdateTrayTip();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    // 两端都有修改: 列出差异, 由用户决定用哪边
    OpenBkDiffDialog(g_hwnd, BuildTaskDiff(g_tasks, ts), ts, L"state.json", true);
}
// 启动时同步"上次备份指纹": 对比本地任务与云端最新备份, 相同则不视为有变化,
// 避免程序重启后第一次到期/防抖备份重复上传相同内容
// 云端备份比本地新 (多台电脑使用场景) 时弹出差异提示, 由用户决定是否恢复, 不自动覆盖
static void SyncBackupSigOnStart() {
    if (Trim(g_webdavUrl).empty()) return;
    if (g_wdSync) {   // 实时同步: 以云端 state.json 为主
        std::string body;
        DWORD code = 0;
        std::wstring err = WebDavRequest(L"GET", DavPath(L"state.json"), nullptr, &body, &code);
        if (!err.empty()) return;                    // 网络问题: 先用本地启动, 之后轮询自动同步
        if (code == 404) { DoSyncPush(); return; }   // 云端还没有主文件: 推送本地
        std::vector<Task> ts;
        if (!ParseTasksFromJson(body, ts)) return;
        std::string csig = TasksSignature(ts), lsig = TasksSignature();
        if (csig == lsig) { g_lastSyncSig = csig; return; }   // 内容一致: 无需动作
        // 内容不同: 启动时无法确定哪边更新, 列出差异由用户选择
        OpenBkDiffDialog(g_hwnd, BuildTaskDiff(g_tasks, ts), ts, L"state.json", true);
        return;
    }
    if (g_wdBkMode == 0) return;   // 仅备份模式启用时才比对云端备份
    std::vector<std::wstring> files;
    if (!WebDavListBackups(files).empty() || files.empty()) return;
    std::string body;
    DWORD code = 0;
    if (!WebDavDownload(files.front(), body).empty()) return;   // files 按时间新→旧排序
    std::vector<Task> ts;
    if (!ParseTasksFromJson(body, ts)) return;
    std::string csig = TasksSignature(ts), lsig = TasksSignature(g_tasks);
    if (csig == lsig) { g_lastBackupSig = lsig; return; }   // 内容一致: 无需备份
    // 内容不同: 用云端备份文件名中的时间与本地配置文件最后写入时间判断新旧
    FILETIME ftCloud = {}, ftLocal = {};
    bool haveCloud = BackupFileDateTime(files.front(), ftCloud);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool haveLocal = GetFileAttributesExW(g_cfgPath.c_str(), GetFileExInfoStandard, &fa) != 0
                     && FileTimeToLocalFileTime(&fa.ftLastWriteTime, &ftLocal) != 0;
    if (haveCloud && haveLocal && *(long long*)&ftCloud > *(long long*)&ftLocal) {
        // 云端较新: 列出差异, 由用户选择 (取消/保留本地 -> 之后自动备份会把本地上传覆盖云端)
        OpenBkDiffDialog(g_hwnd, BuildTaskDiff(g_tasks, ts), ts, files.front());
    }
    // 云端较旧或时间无法判断: 保持本地 (原行为)
}
// 执行一次自动备份 (静默: 失败不提示, 等下次触发重试)
static void DoAutoBk() {
    if (g_wdBkMode == 0 || Trim(g_webdavUrl).empty()) return;
    if (TasksSignature() == g_lastBackupSig) return;   // 无变化
    SaveAppData();
    std::string body = SerializeTasksJson(g_tasks);   // 备份只含待办数据, 不含本机配置
    if (!WebDavUploadBackup(body, BackupFileName()).empty()) return;
    g_lastBackupSig = TasksSignature();
    RotateBackups();
}
// 实时备份/同步: 任务变化后 10 秒防抖, 合并连续修改为一次上传
static void ScheduleAutoBk() {
    if (!g_hwnd) return;
    if (g_wdBkMode != 1 && !g_wdSync) return;
    SetTimer(g_hwnd, TIMER_AUTOBK_DEBOUNCE, 10000, nullptr);
}
// (重)设定时备份/同步轮询的周期定时器; 模式切换后调用
static void UpdateAutoBkTimer() {
    if (!g_hwnd) return;
    KillTimer(g_hwnd, TIMER_AUTOBK_TICK);
    KillTimer(g_hwnd, TIMER_SYNC_POLL);
    if (g_wdBkMode == 2)
        SetTimer(g_hwnd, TIMER_AUTOBK_TICK, (UINT)g_wdBkHours * 3600 * 1000, nullptr);
    if (g_wdSync)
        SetTimer(g_hwnd, TIMER_SYNC_POLL, 30000, nullptr);   // 实时同步: 30 秒轮询云端
}

// ---------------- 日期 ----------------
static bool ParseDate(const std::wstring& s, int& y, int& m, int& d) {
    return swscanf(s.c_str(), L"%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}
// -2 无效 -1 已逾期 0 今天 1 未来
// 比较两个截止日期字符串 (YYYY-MM-DD): a<b 返回 -1, a>b 返回 1, 相等或无法解析返回 0
static int DueCmp(const std::wstring& a, const std::wstring& b) {
    int ay = 0, am = 0, ad = 0, by = 0, bm = 0, bd = 0;
    swscanf(a.c_str(), L"%d-%d-%d", &ay, &am, &ad);
    swscanf(b.c_str(), L"%d-%d-%d", &by, &bm, &bd);
    if (!ay || !by) return 0;
    if (ay != by) return ay < by ? -1 : 1;
    if (am != bm) return am < bm ? -1 : 1;
    if (ad != bd) return ad < bd ? -1 : 1;
    return 0;
}
static int DateState(const std::wstring& s) {
    int y, m, d;
    if (!ParseDate(s, y, m, d)) return -2;
    SYSTEMTIME st; GetLocalTime(&st);
    if (y != (int)st.wYear) return y < (int)st.wYear ? -1 : 1;
    if (m != (int)st.wMonth) return m < (int)st.wMonth ? -1 : 1;
    if (d != (int)st.wDay) return d < (int)st.wDay ? -1 : 1;
    return 0;
}

// ---------------- 图标 (绘制: 绿色圆角 + 白色对勾) ----------------
static HICON MakeIcon(int size) {
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HDC ddc = CreateCompatibleDC(sdc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, size, size);
    HBITMAP om = (HBITMAP)SelectObject(mdc, mask);
    HBITMAP od = (HBITMAP)SelectObject(ddc, bmp);
    RECT rc = { 0, 0, size, size };
    HBRUSH blk = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(ddc, &rc, blk);
    HRGN rgn = CreateRoundRectRgn(size / 8, size / 8, size * 7 / 8, size * 7 / 8, size / 3, size / 3);
    HBRUSH grn = CreateSolidBrush(RGB(52, 168, 83));
    FillRgn(ddc, rgn, grn);
    int lw = size / 6 < 1 ? 1 : size / 6;
    HPEN wp = CreatePen(PS_SOLID, lw, RGB(255, 255, 255));
    HPEN op = (HPEN)SelectObject(ddc, wp);
    MoveToEx(ddc, size * 28 / 100, size * 46 / 100, nullptr);
    LineTo(ddc, size * 44 / 100, size * 62 / 100);
    LineTo(ddc, size * 72 / 100, size * 30 / 100);
    SelectObject(ddc, op);
    DeleteObject(wp); DeleteObject(grn); DeleteObject(rgn);
    FillRect(mdc, &rc, blk);
    rgn = CreateRoundRectRgn(size / 8, size / 8, size * 7 / 8, size * 7 / 8, size / 3, size / 3);
    HBRUSH wht = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRgn(mdc, rgn, wht);
    DeleteObject(rgn);
    ICONINFO ii = {};
    ii.fIcon = TRUE; ii.hbmMask = mask; ii.hbmColor = bmp;
    HICON ic = CreateIconIndirect(&ii);
    SelectObject(mdc, om); SelectObject(ddc, od);
    DeleteObject(mask); DeleteObject(bmp);
    DeleteDC(mdc); DeleteDC(ddc); ReleaseDC(nullptr, sdc);
    return ic;
}

// ---------------- 托盘提示 (悬停显示待完成列表) ----------------
static void UpdateTrayTip() {
    int n = 0;
    std::wstring list;
    for (auto& t : g_tasks) {
        if (t.done) continue;
        ++n;
        if (list.size() < 90) {
            list += L"\n· " + t.text;
            if (!t.due.empty()) list += L" (" + t.due + L")";
        }
    }
    std::wstring tip = APP_NAME L" 待完成 " + std::to_wstring(n) + L" 项" + list;
    if (tip.size() > 120) tip.resize(120);
    lstrcpynW(g_nid.szTip, tip.c_str(), 128);
    if (g_hwnd) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------- 视图 ----------------
static void InvalidateRowHeights();   // 前置声明: 使行高缓存失效 (定义在下方行高区)
static void RebuildRows() {
    g_rows.clear();
    InvalidateRowHeights();   // 任务内容可能已变化, 行高缓存需重算
    for (int i = 0; i < (int)g_tasks.size(); ++i) {
        const Task& t = g_tasks[i];
        if (t.done == (g_view == 1)) {
            g_rows.push_back({ i, -1 });
            if (t.expanded)
                for (int s = 0; s < (int)t.subs.size(); ++s)
                    g_rows.push_back({ i, s });
        }
    }
    g_scroll = 0;
    UpdateTrayTip();
    if (g_hwnd) {
        RECT rc; GetClientRect(g_hwnd, &rc);
        SendMessageW(g_hwnd, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    }
}
// 行高缓存: 文本超宽时自动换行, 行高按内容动态计算 (键: 行数/客户区宽度/DPI)
static std::vector<int> g_rowHc;
static int g_rowHcW = -1, g_rowHcDpi = 0;
// 手动按可用宽度断行: DrawText 的 DT_WORDBREAK 只在空白处断行, 中文连续文本不会折行,
// 故按字符宽度自行断行 —— 中文逐字断, 英文尽量按单词断; out 非空时输出断点插入 \n 的文本
static bool WsChar(wchar_t c) { return c == L' ' || c == L'\t'; }
static int WrapText(HDC dc, const std::wstring& text, int availW, std::wstring* out) {
    int n = (int)text.size();
    int lines = 0, start = 0;
    if (out) out->clear();
    while (start < n) {
        // 二分求当前行能容纳的字符数
        int fit = 0, lo = 1, hi = n - start;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            SIZE sz; GetTextExtentPoint32W(dc, text.c_str() + start, mid, &sz);
            if (sz.cx <= availW) { fit = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (fit == 0) fit = 1;   // 单字超宽也至少放一个
        int next = start + fit;
        if (next < n && !WsChar(text[next])) {
            // 断点落在单词中间: 尝试回退到最近的空白处按词断行 (英文);
            // 找不到空白 (中文连续文本) 则保持逐字断行
            int sp = next;
            while (sp > start && !WsChar(text[sp - 1])) --sp;
            if (sp > start) next = sp;
        }
        // 断点后的空白留在行尾, 下一行从非空白开始
        while (next < n && WsChar(text[next])) ++next;
        if (next <= start) next = start + 1;
        if (out) { if (lines) *out += L'\n'; *out += text.substr(start, next - start); }
        start = next;
        ++lines;
        if (lines > 100) break;   // 防御上限
    }
    return lines > 0 ? lines : 1;
}
// 计算某行显示所需高度: 文本按可用宽度折行测量, 不足基础行高则取基础行高
static int CalcRowHeight(int idx, int w) {
    const RowRef& rr = g_rows[idx];
    const Task& t = g_tasks[rr.t];
    bool isSub = rr.s >= 0;
    const std::wstring& text = isSub ? t.subs[rr.s].text : t.text;
    const std::wstring& due = isSub ? t.subs[rr.s].due : t.due;
    bool done = isSub ? t.subs[rr.s].done : t.done;
    int baseH = isSub ? SUBROW_H : ROW_H;
    bool showDue = !due.empty() && !done && DateState(due) != -2;
    int tx = isSub ? SC(46) : SC(40);
    int textRight = showDue ? w - SC(96) : w - SC(8);
    int availW = textRight - tx;
    if (text.empty() || availW <= 0) return baseH;
    HDC dc = GetDC(g_hwnd);
    if (!dc) return baseH;
    HFONT f = done ? (isSub ? fStrikeSub : fStrike) : (isSub ? fSub : fRow);
    HFONT of = (HFONT)SelectObject(dc, f);
    RECT r1 = { 0, 0, 0, 0 };
    DrawTextW(dc, L"Mg", -1, &r1, DT_SINGLELINE | DT_CALCRECT);   // 单行文字高 (算留白用)
    std::wstring wrapped;
    WrapText(dc, text, availW, &wrapped);
    RECT r2 = { 0, 0, availW, 0 };
    DrawTextW(dc, wrapped.c_str(), -1, &r2, DT_WORDBREAK | DT_CALCRECT);   // 折行后总高
    SelectObject(dc, of);
    ReleaseDC(g_hwnd, dc);
    int need = r2.bottom + (baseH - r1.bottom);   // 文本高 + 与基础行高相同的上下留白
    return need > baseH ? need : baseH;
}
static void InvalidateRowHeights() { g_rowHcW = -1; }
static int RowHeight(int idx) {
    if (g_hwnd) {
        RECT rc; GetClientRect(g_hwnd, &rc);
        if ((int)g_rowHc.size() != (int)g_rows.size() || g_rowHcW != rc.right || g_rowHcDpi != g_dpiNow) {
            g_rowHc.resize(g_rows.size());
            for (int i = 0; i < (int)g_rows.size(); ++i) g_rowHc[i] = CalcRowHeight(i, rc.right);
            g_rowHcW = rc.right; g_rowHcDpi = g_dpiNow;
        }
        if (idx >= 0 && idx < (int)g_rowHc.size()) return g_rowHc[idx];
    }
    return g_rows[idx].s < 0 ? ROW_H : SUBROW_H;
}
static int RowsContentH() {
    int h = 0;
    for (int i = 0; i < (int)g_rows.size(); ++i) h += RowHeight(i);
    return h;
}
static int RowTop(int idx) {
    int y = 0;
    for (int i = 0; i < idx; ++i) y += RowHeight(i);
    return y;
}

// ---------------- 布局 ----------------
static void LayoutButtons(int w) {
    g_nbtns = 0;
    auto add = [&](int id, int x, int cw, const wchar_t* txt) {
        g_btns[g_nbtns].id = id;
        g_btns[g_nbtns].r = { x, SC(4), x + cw, TOPBAR_H - SC(4) };
        g_btns[g_nbtns].txt = txt;
        ++g_nbtns;
    };
    int x = SC(6);
    add(B_ADD, x, SC(54), L"＋添加"); x += SC(60);
    int rx = w - SC(6);
    auto addr = [&](int id, int cw, const wchar_t* txt) {
        rx -= cw;
        g_btns[g_nbtns].id = id;
        g_btns[g_nbtns].r = { rx, SC(4), rx + cw, TOPBAR_H - SC(4) };
        g_btns[g_nbtns].txt = txt;
        ++g_nbtns;
    };
    addr(B_CLOSE, SC(28), L"×");
    addr(B_OPP, SC(24), L"＋");
    UpdatePctText();
    addr(B_OPTXT, SC(44), g_pct);
    addr(B_OPM, SC(24), L"－");
    addr(B_SET, SC(26), L"⚙");
    addr(B_BG, SC(26), L"");
    addr(B_PIN, SC(30), L"顶");
}
static int HitButton(int x, int y) {
    for (int i = 0; i < g_nbtns; ++i)
        if (PtInRect(&g_btns[i].r, { x, y }) && g_btns[i].id != B_OPTXT) return g_btns[i].id;
    return 0;
}
static int HitButtonRect(int x, int y) {
    for (int i = 0; i < g_nbtns; ++i)
        if (PtInRect(&g_btns[i].r, { x, y })) return g_btns[i].id;
    return 0;
}

// ---------------- 绘制 ----------------
static HFONT MkFont(int h, bool strike, int weight = FW_SEMIBOLD) {
    return CreateFontW(-SC(h), 0, 0, 0, weight, 0, 0, strike, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}
static void DrawCenterText(HDC dc, const RECT& r, const wchar_t* s, HFONT f, COLORREF c) {
    HFONT of = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c);
    SIZE sz; GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    TextOutW(dc, (r.left + r.right - sz.cx) / 2, (r.top + r.bottom - sz.cy) / 2, s, (int)wcslen(s));
    SelectObject(dc, of);
}
static void DrawCheckbox(HDC dc, int x, int y, bool checked, bool hover) {
    HBRUSH bg = CachedBrush(checked ? RGB(52, 168, 83) : (hover ? DarkBg(18) : g_bg));
    HBRUSH ob = (HBRUSH)SelectObject(dc, bg);
    HPEN pen = CachedPen(checked ? RGB(52, 168, 83) : RGB(160, 160, 160), 1);
    HPEN op = (HPEN)SelectObject(dc, pen);
    Rectangle(dc, x, y, x + SC(14), y + SC(14));
    SelectObject(dc, ob); SelectObject(dc, op);
    if (checked) {
        HPEN wp = CachedPen(RGB(255, 255, 255), 2);
        HPEN op2 = (HPEN)SelectObject(dc, wp);
        MoveToEx(dc, x + SC(3), y + SC(7), nullptr); LineTo(dc, x + SC(6), y + SC(10)); LineTo(dc, x + SC(11), y + SC(3));
        SelectObject(dc, op2);
    }
}
static void DrawTriangle(HDC dc, int cx, int cy, bool down) {
    POINT pts[3];
    if (down) { pts[0] = { cx - SC(4), cy - SC(2) }; pts[1] = { cx + SC(5), cy - SC(2) }; pts[2] = { cx, cy + SC(4) }; }
    else      { pts[0] = { cx - SC(2), cy - SC(4) }; pts[1] = { cx + SC(4), cy + SC(2) }; pts[2] = { cx - SC(2), cy + SC(5) }; }
    HBRUSH b = CachedBrush(RGB(90, 90, 90));
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN op = (HPEN)SelectObject(dc, (HPEN)GetStockObject(NULL_PEN));
    Polygon(dc, pts, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
}
static void DrawTopBar(HDC dc, int w) {
    RECT bar = { 0, 0, w, TOPBAR_H };
    FillRect(dc, &bar, CachedBrush(DarkBg(10)));
    HPEN pen = CachedPen(RGB(220, 220, 220), 1);
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, 0, TOPBAR_H - 1, nullptr); LineTo(dc, w, TOPBAR_H - 1);
    SelectObject(dc, op);
    for (int i = 0; i < g_nbtns; ++i) {
        const TBtn& bt = g_btns[i];
        bool active = (bt.id == B_TAB0 && g_view == 0) || (bt.id == B_TAB1 && g_view == 1) ||
                      (bt.id == B_PIN && g_topmost);
        bool hover = (g_hoverBtn == bt.id);
        if (active) {
            HBRUSH ab = CachedBrush(bt.id == B_PIN ? RGB(52, 168, 83) : RGB(68, 138, 255));
            HBRUSH oab = (HBRUSH)SelectObject(dc, ab);
            FillRect(dc, &bt.r, ab); SelectObject(dc, oab);
        } else if (hover) {
            HBRUSH hb = CachedBrush(RGB(225, 231, 240));
            HBRUSH ohb = (HBRUSH)SelectObject(dc, hb);
            FillRect(dc, &bt.r, hb); SelectObject(dc, ohb);
        }
        COLORREF tc = active ? RGB(255, 255, 255) : RGB(25, 25, 25);
        if (bt.id == B_BG) {   // 背景色按钮: 自绘当前背景色小色块
            int cw = bt.r.right - bt.r.left, chh = bt.r.bottom - bt.r.top;
            RECT sq = { bt.r.left + (cw - SC(14)) / 2, bt.r.top + (chh - SC(14)) / 2,
                        bt.r.left + (cw + SC(14)) / 2, bt.r.top + (chh + SC(14)) / 2 };
            HBRUSH sb = CachedBrush(g_bg);
            HBRUSH osb = (HBRUSH)SelectObject(dc, sb);
            HPEN spen = CachedPen(hover ? RGB(68, 138, 255) : RGB(150, 150, 150), 1);
            HPEN osp = (HPEN)SelectObject(dc, spen);
            Rectangle(dc, sq.left, sq.top, sq.right, sq.bottom);
            SelectObject(dc, osb); SelectObject(dc, osp);
            continue;
        }
        DrawCenterText(dc, bt.r, bt.txt, bt.id == B_SET && fGear ? fGear : fUI, tc);
    }
}
static void DrawRow(HDC dc, int idx, int y, int w) {
    const RowRef& rr = g_rows[idx];
    const Task& t = g_tasks[rr.t];
    int h = RowHeight(idx);
    int baseH = rr.s < 0 ? ROW_H : SUBROW_H;
    bool isSub = rr.s >= 0;
    const std::wstring& text = isSub ? t.subs[rr.s].text : t.text;
    const std::wstring& due = isSub ? t.subs[rr.s].due : t.due;
    bool done = isSub ? t.subs[rr.s].done : t.done;

    if (g_hoverRow == idx) {
        RECT rowRc = { 0, y, w, y + h };
        FillRect(dc, &rowRc, CachedBrush(RGB(238, 242, 248)));
    }

    int cbx, tx;
    if (isSub) { cbx = SC(26); tx = SC(46); }
    else {
        cbx = SC(8); tx = SC(40);
        if (!t.subs.empty()) DrawTriangle(dc, SC(28), y + baseH / 2, t.expanded);   // 与首行对齐
    }
    DrawCheckbox(dc, cbx, y + (baseH - SC(13)) / 2, done, g_hoverRow == idx);   // 与首行对齐

    HFONT f = done ? (isSub ? fStrikeSub : fStrike) : (isSub ? fSub : fRow);
    HFONT of = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, done ? RGB(150, 150, 150) : RGB(15, 15, 15));
    // 仅当确实要显示截止日期时才在右侧保留日期列, 否则文字用满整行宽度
    bool showDue = !due.empty() && !done && DateState(due) != -2;
    int textRight = showDue ? w - SC(96) : w - SC(8);
    // 首行垂直位置与单行时一致 (基础行高居中), 超宽文本自动换行多行显示
    RECT r1 = { tx, 0, textRight, 0 };
    DrawTextW(dc, L"Mg", -1, &r1, DT_SINGLELINE | DT_CALCRECT);
    int vpad = (baseH - r1.bottom) / 2; if (vpad < 0) vpad = 0;
    std::wstring wrapped;
    WrapText(dc, text, textRight - tx, &wrapped);
    RECT tr = { tx, y + vpad, textRight, y + h };
    DrawTextW(dc, wrapped.c_str(), -1, &tr, DT_WORDBREAK);
    SelectObject(dc, of);

    if (showDue) {
        int ds = DateState(due);
        COLORREF c = ds == -1 ? RGB(229, 57, 53) : (ds == 0 ? RGB(230, 126, 34) : RGB(110, 110, 110));
        RECT dr = { w - SC(94), y, w - SC(8), y + h };
        SetTextColor(dc, c);
        DrawTextW(dc, due.c_str(), -1, &dr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
    }
    if (done && h == baseH) {  // 删除线 (多行折行时字体自带逐行删除线, 不再画横线)
        HFONT tmp = (HFONT)SelectObject(dc, isSub ? fSub : fRow);
        SIZE sz; GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
        SelectObject(dc, tmp);
        int avail = textRight - tx;
        int lw = sz.cx < avail ? sz.cx : avail;
        HPEN sp = CachedPen(RGB(150, 150, 150), 1);
        HPEN osp = (HPEN)SelectObject(dc, sp);
        MoveToEx(dc, tx, y + h / 2, nullptr); LineTo(dc, tx + lw, y + h / 2);
        SelectObject(dc, osp);
    }
}
static void DrawBottomBar(HDC dc, int w, int h) {
    RECT bar = { 0, h - BOTBAR_H, w, h };
    FillRect(dc, &bar, CachedBrush(DarkBg(10)));
    HPEN pen = CachedPen(RGB(220, 220, 220), 1);
    HPEN op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, 0, h - BOTBAR_H, nullptr); LineTo(dc, w, h - BOTBAR_H);
    SelectObject(dc, op);

    // 标签页 (待完成 / 已完成)
    g_tabRects[0] = { SC(6), h - BOTBAR_H + SC(3), SC(62), h - SC(3) };
    g_tabRects[1] = { SC(62), h - BOTBAR_H + SC(3), SC(118), h - SC(3) };
    const wchar_t* tabTxt[2] = { L"待完成", L"已完成" };
    for (int i = 0; i < 2; ++i) {
        bool active = (g_view == i);
        if (active) {
            HBRUSH ab = CachedBrush(RGB(68, 138, 255));
            HBRUSH oab = (HBRUSH)SelectObject(dc, ab);
            FillRect(dc, &g_tabRects[i], ab); SelectObject(dc, oab);
        } else if (g_hoverTab == i) {
            HBRUSH hb = CachedBrush(RGB(225, 231, 240));
            HBRUSH ohb = (HBRUSH)SelectObject(dc, hb);
            FillRect(dc, &g_tabRects[i], hb); SelectObject(dc, ohb);
        }
        COLORREF tc = active ? RGB(255, 255, 255) : RGB(25, 25, 25);
        DrawCenterText(dc, g_tabRects[i], tabTxt[i], fUI, tc);
    }

    // 右侧计数: 按主任务统计 (不含子任务, 与折叠状态无关)
    int n = 0, od = 0;
    for (auto& t : g_tasks) {
        if (t.done != (g_view == 1)) continue;
        ++n;
        if (!t.done && DateState(t.due) == -1) ++od;
    }
    wchar_t s[128];
    if (g_view == 0 && od > 0) wsprintfW(s, L"%d 项 · 逾期 %d 项", n, od);
    else wsprintfW(s, L"%d 项", n);
    RECT tr = { w / 2, h - BOTBAR_H, w - SC(8), h };
    DrawCenterText(dc, tr, s, fUI, RGB(85, 85, 85));
}
static void Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(g_hwnd, &ps);
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) { EndPaint(g_hwnd, &ps); return; }
    RECT iv = ps.rcPaint;   // 只重绘失效区域, 其余内容上次已画好无需重复
    if (iv.left < 0) iv.left = 0;
    if (iv.top < 0) iv.top = 0;
    if (iv.right > w) iv.right = w;
    if (iv.bottom > h) iv.bottom = h;
    if (iv.right <= iv.left || iv.bottom <= iv.top) { EndPaint(g_hwnd, &ps); return; }

    // 内存 DC 与位图跨帧缓存 (仅窗口尺寸变化时重建), 避免每帧创建销毁 GDI 对象
    static HDC g_memDC = nullptr;
    static HBITMAP g_memBmp = nullptr;
    static int g_memW = 0, g_memH = 0;
    if (!g_memDC) {
        g_memDC = CreateCompatibleDC(hdc);
        if (!g_memDC) { EndPaint(g_hwnd, &ps); return; }
        SetBkMode(g_memDC, TRANSPARENT);
    }
    if (g_memW != w || g_memH != h || !g_memBmp) {
        if (g_memBmp) DeleteObject(g_memBmp);
        g_memBmp = CreateCompatibleBitmap(hdc, w, h);
        g_memW = w; g_memH = h;
        if (!g_memBmp) { DeleteDC(g_memDC); g_memDC = nullptr; EndPaint(g_hwnd, &ps); return; }
        InvalidateRect(g_hwnd, nullptr, FALSE);   // 新位图内容未初始化, 下帧全量重绘
    }
    HDC mem = g_memDC;
    HBITMAP ob = (HBITMAP)SelectObject(mem, g_memBmp);

    if (iv.top < h - BOTBAR_H && iv.bottom > TOPBAR_H) {   // 背景与任务行 (仅与失效区域相交的部分)
        RECT bg = { 0, std::max<LONG>(iv.top, TOPBAR_H), w, std::min<LONG>(iv.bottom, h - BOTBAR_H) };
        FillRect(mem, &bg, g_bgBrush ? g_bgBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
    }
    int y = TOPBAR_H - g_scroll;
    for (int i = 0; i < (int)g_rows.size(); ++i) {
        int rh = RowHeight(i);
        if (y + rh > std::max<LONG>(iv.top, TOPBAR_H) && y < std::min<LONG>(iv.bottom, h - BOTBAR_H))
            DrawRow(mem, i, y, w);
        y += rh;
    }
    if (iv.top < TOPBAR_H) DrawTopBar(mem, w);
    if (iv.bottom > h - BOTBAR_H) DrawBottomBar(mem, w, h);

    {   // 自绘 1px 窗口描边 (系统边框已禁用)
        HPEN pen = CachedPen(RGB(202, 202, 202), 1);
        HPEN op = (HPEN)SelectObject(mem, pen);
        HBRUSH ob2 = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH prev = (HBRUSH)SelectObject(mem, ob2);
        Rectangle(mem, 0, 0, w, h);
        SelectObject(mem, prev); SelectObject(mem, op);
    }

    BitBlt(hdc, iv.left, iv.top, iv.right - iv.left, iv.bottom - iv.top, mem, iv.left, iv.top, SRCCOPY);
    SelectObject(mem, ob);
    EndPaint(g_hwnd, &ps);
}

// ---------------- 行命中测试 ----------------
static int HitRow(int x, int y) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    (void)x;
    if (y < TOPBAR_H || y >= rc.bottom - BOTBAR_H) return -1;
    int ly = y - TOPBAR_H + g_scroll;
    int acc = 0;
    for (int i = 0; i < (int)g_rows.size(); ++i) {
        acc += RowHeight(i);
        if (ly < acc) return i;
    }
    return -1;
}

// ---------------- 对话框 ----------------
struct DlgResult {
    int mode = 0; bool ok = false;
    std::wstring f1, f2, f3, f4; bool chk = false;
    int keep = 7;                                          // WebDAV 备份保留天数
    std::string fbody;                                     // 恢复备份: 已下载的备份内容
    int bkMode = 0, hours = 4;                             // WebDAV 备份策略 (0=不备份 1=实时 2=定时)
    bool sync = false;                                     // WebDAV 实时同步开关
    std::vector<SubTask> subs;
    int taskIdx = -1, subIdx = -1;
    UINT hkMods = 0, hkVk = 0;                             // 设置对话框: 快捷键
};
struct DlgCtx {
    int mode = 0; HWND h = nullptr, owner = nullptr;
    HWND e1 = nullptr, e2 = nullptr, e3 = nullptr, list = nullptr, chk = nullptr;
    HWND hkey = nullptr;                                   // 设置: 快捷键捕获框
    HWND e4 = nullptr, keepEd = nullptr;                   // WebDAV: 备份文件夹 / 保留天数
    HWND bkRb0 = nullptr, bkRb1 = nullptr, bkRb2 = nullptr, syncChk = nullptr, hoursEd = nullptr, showChk = nullptr;   // 备份策略 / 实时同步 / 启动显示
    HWND chkAuto = nullptr;                                // 设置: 开机自启
    UINT pMods = 0, pVk = 0;                               // 捕获到的快捷键
    int selBg = -1;                                        // 选中的背景色
    RECT sw[BGCOUNT];                                      // 色块矩形
    RECT tabRc[3] = {};                                    // 设置窗口 tab 页矩形
    int curTab = 0;                                        // 当前 tab (0=通用 1=WebDAV 2=关于)
    std::vector<HWND> pg0, pg1, pg2;                       // 各 tab 页的控件 (切页显隐)
    int pendX = 0, pendY = 0;                              // 颜色气泡弹出位置
    RECT closeRc = {};                                     // 自绘关闭按钮
    WNDPROC oldProc = nullptr;
    int taskIdx = -1, subIdx = -1;
    std::vector<SubTask> tmpSubs;
    std::wstring pend1, pend2, pend3, pend4;   // 编辑框初值 (创建前设置)
    int pendKeep = 7, pendHours = 4, pendBkMode = 0; bool pendSync = false;
    int e1H = 22;                                          // 任务内容输入框高度 (文本长时自动增高, 创建前计算)
    bool pendChk = false;
    std::wstring pendTitle;                                // 自绘消息框: 标题栏文本
    bool pendYesNo = false;                                // 自绘消息框: false=单个"确定" true=是/否
    bool resultPosted = false;                             // 已投递结果 (用于区分 × 关闭时丢弃挂起回调)
    std::string previewBody; std::wstring prevName; bool previewOk = false;   // 恢复对话框: 已下载的备份
    std::vector<std::wstring> diffs; std::vector<Task> cloudTasks; std::wstring bkName;   // 启动差异提示: 云端较新
    bool isSync = false;                                   // 差异提示: 实时同步模式 (取消=保留本地并上传)
};
// 通用自绘消息框挂起回调 (DLG_CONFIRM 关闭且确认时执行; true=确定/是)
static std::function<void(bool)> g_pendMsgCb;
static void OpenMsgBox(HWND owner, const std::wstring& title, const std::wstring& msg, bool yesNo, std::function<void(bool)> cb);   // 前置声明 (定义在托盘区之后)
#define PROP_CTX L"mt_ctx"
#define PROP_OLDPROC L"mt_oldproc"   // 普通编辑框子类化前的原窗口过程

// Ctrl+A 诊断日志 (默认关闭): 编译时加 -DMT_DEBUG 才输出到 exe 同目录 mt_debug.log (UTF-8)
#ifdef MT_DEBUG
static void DbgLog(const wchar_t* fmt, ...) {
    wchar_t buf[1024] = {};
    va_list ap; va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    wsprintfW(line, L"[%02d:%02d:%02d.%03d] %ls\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, line, -1, &out[0], n, nullptr, nullptr);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    size_t s = p.find_last_of(L"\\/");
    std::wstring path = (s == std::wstring::npos) ? L"mt_debug.log" : p.substr(0, s + 1) + L"mt_debug.log";
    HANDLE hf = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hf, 0, nullptr, FILE_END);
    DWORD w = 0;
    WriteFile(hf, out.data(), (DWORD)out.size() - 1, &w, nullptr);
    CloseHandle(hf);
}
#define MTLOG(...) DbgLog(__VA_ARGS__)
#else
#define MTLOG(...)   // 正常构建: 零开销空操作
#endif

// 普通编辑框子类化: 补充 Ctrl+A 全选 (标准 EDIT 无此快捷键; Ctrl+C/X/V 系统已内置)
// Ctrl+A 的可能消息链 (按到达顺序, 全覆盖):
//   1) WM_KEYDOWN(VK_A)          — 正常路径
//   2) WM_KEYDOWN(VK_PROCESSKEY) — IME 拦截了按键, 原始虚拟键经 ImmGetVirtualKey 还原
//   3) WM_CHAR(0x01)             — TranslateMessage 兜底生成 (IME 未拦截时必有)
static LRESULT CALLBACK EditStdProc(HWND e, UINT m, WPARAM wp, LPARAM lp) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (m == WM_KEYDOWN && ctrl && wp != VK_PROCESSKEY)
        MTLOG(L"WM_KEYDOWN vk=0x%02X ctrl=1 alt=%d", (int)wp, alt ? 1 : 0);
    if (m == WM_KEYDOWN && wp == VK_PROCESSKEY)
        MTLOG(L"WM_KEYDOWN VK_PROCESSKEY ctrl=%d 原始键=0x%04X", ctrl ? 1 : 0, ImmGetVirtualKey(e));
    if (m == WM_KEYDOWN && ctrl && !alt) {
        UINT vk = (wp == VK_PROCESSKEY) ? (UINT)ImmGetVirtualKey(e) : (UINT)wp;
        if (vk == 'A') { SendMessageW(e, EM_SETSEL, 0, -1); MTLOG(L"Ctrl+A -> EM_SETSEL (KEYDOWN路径)"); return 0; }
    }
    if (m == WM_CHAR) {
        if (wp < 0x20) MTLOG(L"WM_CHAR wp=0x%02X ctrl=%d", (int)wp, ctrl ? 1 : 0);
        if (wp == 1) { SendMessageW(e, EM_SETSEL, 0, -1); MTLOG(L"Ctrl+A -> EM_SETSEL (WM_CHAR兜底)"); return 0; }
    }
    WNDPROC old = (WNDPROC)GetPropW(e, PROP_OLDPROC);
    return old ? CallWindowProcW(old, e, m, wp, lp) : DefWindowProcW(e, m, wp, lp);
}

// 快捷键捕获框子类化: 按下组合键即显示
static LRESULT CALLBACK HkEditProc(HWND e, UINT m, WPARAM wp, LPARAM lp) {
    DlgCtx* c = (DlgCtx*)GetPropW(GetParent(e), PROP_CTX);
    if (!c) return DefWindowProcW(e, m, wp, lp);   // 父对话框销毁后 PROP 已移除, 防空指针
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        UINT vk = (UINT)wp;
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
        bool isModKey = (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN);
        if (!isModKey && mods && vk != VK_TAB && vk != VK_SPACE) {
            c->pMods = mods; c->pVk = vk;
            std::wstring s = FormatHotkey(mods, vk);
            SetWindowTextW(e, s.c_str());
        }
        return 0;
    }
    if (m == WM_CHAR) return 0;
    return c->oldProc ? CallWindowProcW(c->oldProc, e, m, wp, lp) : DefWindowProcW(e, m, wp, lp);
}

static void OpenSubDialog(HWND owner, int taskIdx, int subIdx, const std::wstring& text, const std::wstring& due);

static std::wstring ComposeSubItem(const SubTask& s) {
    std::wstring t = s.done ? L"[✓] " : L"[　] ";
    t += s.text;
    if (!s.due.empty()) t += L"   (截至 " + s.due + L")";
    return t;
}
static void RefreshSubList(DlgCtx* c) {
    if (!c->list) return;
    SendMessage(c->list, LB_RESETCONTENT, 0, 0);
    for (auto& s : c->tmpSubs)
        SendMessageW(c->list, LB_ADDSTRING, 0, (LPARAM)ComposeSubItem(s).c_str());
}
static std::wstring ReadEdit(HWND e) {
    if (!e) return L"";
    int n = GetWindowTextLengthW(e);
    std::wstring s(n + 1, 0);
    GetWindowTextW(e, &s[0], n + 1);
    s.resize(n);
    return s;
}
// ---------------- WebDAV 备份恢复 ----------------
static std::vector<std::wstring> g_davBackups;   // 恢复对话框的备份文件列表
// 把任务清单渲染为多行文本 (预览用)
static std::wstring RenderTasksPreview(const std::vector<Task>& ts) {
    wchar_t head[64];
    wsprintfW(head, L"共 %d 项任务\r\n\r\n", (int)ts.size());
    std::wstring o = head;
    for (auto& t : ts) {
        o += t.done ? L"[已完成] " : L"[待办]   ";
        o += t.text;
        if (!t.due.empty()) o += L"  (截止 " + t.due + L")";
        o += L"\r\n";
        for (auto& s : t.subs) {
            o += L"        ";
            o += s.done ? L"[x] " : L"[ ] ";
            o += s.text;
            if (!s.due.empty()) o += L"  (" + s.due + L")";
            o += L"\r\n";
        }
    }
    return o;
}
// 下载并预览选中的备份
static void DavPreviewBackup(DlgCtx* c, int sel) {
    if (sel < 0 || sel >= (int)g_davBackups.size()) return;
    const std::wstring& name = g_davBackups[sel];
    if (c->prevName != name) {
        std::string body;
        DWORD code = 0;
        std::wstring err = WebDavDownload(name, body);
        if (!err.empty() || code >= 400) {
            SetWindowTextW(c->e1, (L"下载失败: " + err).c_str());
            c->previewOk = false; c->prevName = name;
            return;
        }
        c->previewBody = body; c->prevName = name;
    }
    std::vector<Task> ts;
    if (!ParseTasksFromJson(c->previewBody, ts)) {
        SetWindowTextW(c->e1, L"备份文件解析失败 (内容不是有效的任务配置)。");
        c->previewOk = false;
        return;
    }
    SetWindowTextW(c->e1, RenderTasksPreview(ts).c_str());
    c->previewOk = true;
}
// 打开恢复对话框: 先列出远端备份, 再弹出选择窗口
static void OpenDavRestoreDialog(HWND owner) {
    if (Trim(g_webdavUrl).empty()) {
        OpenMsgBox(owner, APP_NAME, L"请先在 WebDAV 配置中填写服务器地址并保存。", false, nullptr);
        return;
    }
    g_davBackups.clear();
    std::wstring err = WebDavListBackups(g_davBackups);
    if (!err.empty()) {
        OpenMsgBox(owner, APP_NAME, L"获取备份列表失败:\n" + err, false, nullptr);
        return;
    }
    if (g_davBackups.empty()) {
        OpenMsgBox(owner, APP_NAME, L"备份文件夹 " + g_webdavDir + L" 中还没有备份文件。", false, nullptr);
        return;
    }
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_DAVREST;
    c->owner = owner;
    CreateWindowExW(0, L"MTDlg", L"恢复 WebDAV 备份", WS_POPUP, 0, 0, SC(420), SC(420), owner, nullptr,
                    GetModuleHandleW(nullptr), c);
}
// 启动差异提示对话框: 云端备份比本地新, 列出差异, 用户选择是否恢复
// isSync: 实时同步模式 (state.json) — "用云端覆盖本地"后标记已同步, "保留本地"立即上传覆盖云端
static void OpenBkDiffDialog(HWND owner, const std::vector<std::wstring>& diffs,
                             const std::vector<Task>& cloudTasks, const std::wstring& bkName,
                             bool isSync) {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_BKDIFF;
    c->owner = owner;
    c->diffs = diffs;
    c->cloudTasks = cloudTasks;
    c->bkName = bkName;
    c->isSync = isSync;
    g_syncDialogOpen = true;
    CreateWindowExW(0, L"MTDlg", isSync ? L"云端同步冲突" : L"云端备份较新", WS_POPUP, 0, 0,
                    SC(430), SC(430), owner, nullptr, GetModuleHandleW(nullptr), c);
}
static void DlgOnSave(DlgCtx* c) {
    // 启动差异提示: 用户选择"用云端覆盖本地" -> 直接恢复, 无需经主窗口结果分发
    if (c->mode == DLG_BKDIFF) {
        g_tasks = c->cloudTasks;
        for (auto& t : g_tasks) t.expanded = false;
        g_lastBackupSig = TasksSignature();   // 云端已有该内容, 之后无需重复备份
        if (c->isSync) g_lastSyncSig = g_lastBackupSig;   // 同步模式: 与云端已一致
        SaveAppData();
        RebuildRows();
        UpdateTrayTip();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        DestroyWindow(c->h);
        return;
    }
    DlgResult* r = new DlgResult();
    r->mode = c->mode; r->ok = true;
    r->f1 = ReadEdit(c->e1);
    r->f2 = ReadEdit(c->e2);
    r->f3 = ReadEdit(c->e3);
    r->chk = c->chk && SendMessage(c->chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    r->taskIdx = c->taskIdx; r->subIdx = c->subIdx;
    r->subs = c->tmpSubs;
    // 日期冲突校验: 子任务截止日期不得晚于主任务截止日期 (均填写时), 冲突则阻止保存
    if (c->mode == DLG_SUBTASK && c->taskIdx >= 0 && c->taskIdx < (int)g_tasks.size()) {
        const std::wstring& md = g_tasks[c->taskIdx].due;
        std::wstring sd = Trim(r->f2);
        if (!md.empty() && !sd.empty() && DueCmp(sd, md) > 0) {
            std::wstring msg = L"子任务截止日期 (" + sd + L") 晚于主任务截止日期 (" + md +
                               L"), 请调整后再保存。";
            OpenMsgBox(c->h, APP_NAME, msg, false, nullptr);   // 子任务截止日期晚于主任务
            SetFocus(c->e2);
            return;   // 保持窗口打开
        }
    }
    if (c->mode == DLG_EDITTASK) {
        std::wstring md = Trim(r->f2);
        if (!md.empty()) {
            for (auto& s : c->tmpSubs) {
                if (!s.due.empty() && DueCmp(s.due, md) > 0) {
                    std::wstring msg = L"子任务 \"" + s.text + L"\" 的截止日期 (" + s.due +
                                       L") 晚于主任务截止日期 (" + md + L"), 请调整后再保存。";
                    OpenMsgBox(c->h, APP_NAME, msg, false, nullptr);   // 子任务截止日期晚于主任务
                    SetFocus(c->e2);
                    return;   // 保持窗口打开
                }
            }
        }
    }
    if (c->mode == DLG_SETTINGS) {
        r->hkMods = c->pMods; r->hkVk = c->pVk;
        r->f4 = ReadEdit(c->e4);
        r->keep = c->keepEd ? _wtoi(ReadEdit(c->keepEd).c_str()) : g_webdavKeepDays;
        r->bkMode = (c->bkRb1 && SendMessage(c->bkRb1, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1
                  : (c->bkRb2 && SendMessage(c->bkRb2, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 2 : 0;
        r->sync = c->syncChk && SendMessage(c->syncChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
        r->hours = c->hoursEd ? _wtoi(ReadEdit(c->hoursEd).c_str()) : g_wdBkHours;
        // 开机自启: 保存时写注册表, 与托盘菜单保持同步 (菜单每次读取注册表实时状态)
        if (c->chkAuto)
            SetAutoStart(SendMessage(c->chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    if (c->mode == DLG_DAVREST) {
        int sel = c->list ? (int)SendMessageW(c->list, LB_GETCURSEL, 0, 0) : -1;
        if (sel < 0 || sel >= (int)g_davBackups.size()) {
            OpenMsgBox(c->h, APP_NAME, L"请先在列表中选择一个备份。", false, nullptr);
            return;   // 未选中, 保持窗口打开
        }
        if (!c->previewOk) {
            OpenMsgBox(c->h, APP_NAME, L"当前备份内容无效或未成功预览, 无法恢复。", false, nullptr);
            return;
        }
        r->taskIdx = sel;
        r->fbody = c->previewBody;   // 复用预览时已下载的内容, 避免二次下载
    }
    PostMessageW(c->owner, WM_APP_DLG, 0, (LPARAM)r);
    c->resultPosted = true;
    DestroyWindow(c->h);
}
static void DlgOnCreate(DlgCtx* c) {
    MTLOG(L"DlgOnCreate mode=%d", (int)c->mode);
    // 任务/子任务内容输入框: 文本较长时自动增高为多行编辑框, 弹窗同步增高 (上限约 12 行后内部滚动)
    c->e1H = SC(22);
    if (c->mode == DLG_ADDTASK || c->mode == DLG_SUBTASK || c->mode == DLG_EDITTASK) {
        RECT rc0; GetClientRect(c->h, &rc0);
        int availW = rc0.right - SC(98) - SC(8);   // 输入框可用文本宽度 (减内边距)
        HDC dc = GetDC(g_hwnd);
        HFONT of = (HFONT)SelectObject(dc, fUI);
        RECT r1 = { 0, 0, 0, 0 };
        DrawTextW(dc, L"Mg", -1, &r1, DT_SINGLELINE | DT_CALCRECT);
        int lines = WrapText(dc, c->pend1, availW, nullptr);
        SelectObject(dc, of); ReleaseDC(g_hwnd, dc);
        int four = 4 * r1.bottom + SC(10);          // 默认 4 行高度
        int want = lines * r1.bottom + SC(10);
        if (want < four) want = four;
        if (want > SC(210)) want = SC(210);
        c->e1H = want;
        int extra = want - SC(22);
        if (extra > 0) {   // 弹窗整体增高, 底部按钮等控件按新高度布局
            RECT wr; GetWindowRect(c->h, &wr);
            SetWindowPos(c->h, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + extra,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    RECT rc; GetClientRect(c->h, &rc);
    int w = rc.right, h = rc.bottom;
    auto label = [&](const wchar_t* txt, int x, int y, int cw) -> HWND {
        return CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE, x, y, cw, SC(18), c->h, nullptr, hi, nullptr);
    };
    auto edit = [&](int id, int x, int y, int cw, DWORD extra = 0, int chh = SC(22)) {
        DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra;
        if (!(extra & ES_MULTILINE)) st |= ES_AUTOHSCROLL;   // 多行时自动换行, 不做水平滚动
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", st,
                               x, y, cw, chh, c->h, (HMENU)(INT_PTR)id, hi, nullptr);
    };
    auto btn = [&](const wchar_t* txt, int id, int x, int y, int cw) -> HWND {
        return CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        x, y, cw, SC(24), c->h, (HMENU)(INT_PTR)id, hi, nullptr);
    };
    c->closeRc = { w - SC(28), SC(3), w - SC(8), DLG_TOP - SC(3) };

    if (c->mode == DLG_CONFIRM) {
        // 自绘消息框: 消息多行标签 (长行按宽度预折行) + 确定/是/否 按钮, 高度按行数自适应
        HDC dc = GetDC(c->h);
        HFONT of = (HFONT)SelectObject(dc, fUI);
        RECT r1 = { 0, 0, 0, 0 };
        DrawTextW(dc, L"Mg", -1, &r1, DT_SINGLELINE | DT_CALCRECT);
        int availW = w - SC(36);
        int lines = 0;
        std::wstring msgText;
        size_t start = 0;
        while (true) {   // WrapText 不识别 \n, 先按 \n 分段各自折行再拼接
            size_t p = c->pend1.find(L'\n', start);
            std::wstring seg = (p == std::wstring::npos) ? c->pend1.substr(start) : c->pend1.substr(start, p - start);
            std::wstring wseg;
            int ln = WrapText(dc, seg, availW, &wseg);
            if (ln < 1) ln = 1;
            lines += ln;
            if (!msgText.empty()) msgText += L'\n';
            msgText += wseg.empty() ? seg : wseg;
            if (p == std::wstring::npos) break;
            start = p + 1;
        }
        SelectObject(dc, of); ReleaseDC(c->h, dc);
        int textH = lines * r1.bottom;
        int wantH = DLG_TOP + SC(14) + textH + SC(18) + SC(24) + SC(14);
        SetWindowPos(c->h, nullptr, 0, 0, w, wantH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        h = wantH;
        CreateWindowExW(0, L"STATIC", msgText.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                        SC(18), DLG_TOP + SC(14), availW, textH + SC(4), c->h, nullptr, hi, nullptr);
        if (c->pendYesNo) {
            btn(L"是", IDOK, w / 2 - SC(72), h - SC(38), SC(66));
            btn(L"否", IDCANCEL, w / 2 + SC(6), h - SC(38), SC(66));
        } else {
            btn(L"确定", IDOK, w / 2 - SC(33), h - SC(38), SC(66));
        }
    } else if (c->mode == DLG_ADDTASK || c->mode == DLG_SUBTASK) {
        const wchar_t* l1 = c->mode == DLG_ADDTASK ? L"任务内容:" : L"子任务内容:";
        label(l1, SC(10), DLG_TOP + SC(12), SC(72));
        c->e1 = edit(IDC_E1, SC(88), DLG_TOP + SC(10), w - SC(98),
                     ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, c->e1H);   // 多行自动换行, 高度随内容
        int dateY = DLG_TOP + SC(10) + c->e1H + SC(10);   // 后续控件随内容框高度顺移
        label(L"截止日期 (YYYY-MM-DD, 留空为无):", SC(10), dateY + SC(2), w - SC(20));
        c->e2 = edit(IDC_E2, SC(88), dateY + SC(20), w - SC(98));
        btn(L"保存", IDOK, w - SC(152), h - SC(36), SC(66));
        btn(L"取消", IDCANCEL, w - SC(78), h - SC(36), SC(66));
        SetWindowTextW(c->e1, c->pend1.c_str());
        SetWindowTextW(c->e2, c->pend2.c_str());
        SetFocus(c->e1);
    } else if (c->mode == DLG_EDITTASK) {
        label(L"任务内容:", SC(10), DLG_TOP + SC(12), SC(72));
        c->e1 = edit(IDC_E1, SC(88), DLG_TOP + SC(10), w - SC(98),
                     ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, c->e1H);   // 多行自动换行, 高度随内容
        int dateY = DLG_TOP + SC(10) + c->e1H + SC(10);   // 后续控件随内容框高度顺移
        label(L"截止日期 (YYYY-MM-DD, 留空为无):", SC(10), dateY + SC(2), w - SC(20));
        c->e2 = edit(IDC_E2, SC(88), dateY + SC(20), w - SC(98));
        int subY = dateY + SC(50);   // 子任务区起点
        label(L"子任务列表 (双击编辑子任务):", SC(10), subY, w - SC(20));
        c->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS,
                                  SC(10), subY + SC(16), w - SC(20), h - subY - SC(108), c->h, (HMENU)(INT_PTR)IDC_LIST, hi, nullptr);
        btn(L"添加子任务", IDC_ADDSUB, SC(10), h - SC(80), SC(80));
        btn(L"编辑", IDC_EDITSUB, SC(98), h - SC(80), SC(60));
        btn(L"删除", IDC_DELSUB, SC(166), h - SC(80), SC(60));
        btn(L"保存", IDOK, w - SC(152), h - SC(36), SC(66));
        btn(L"取消", IDCANCEL, w - SC(78), h - SC(36), SC(66));
        SetWindowTextW(c->e1, c->pend1.c_str());
        SetWindowTextW(c->e2, c->pend2.c_str());
        RefreshSubList(c);
        SetFocus(c->e1);
    } else if (c->mode == DLG_DAVREST) {
        label(L"选择备份 (点击条目自动预览内容):", SC(10), DLG_TOP + SC(8), w - SC(20));
        c->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS,
                                  SC(10), DLG_TOP + SC(26), w - SC(20), SC(110), c->h, (HMENU)(INT_PTR)IDC_LIST, hi, nullptr);
        label(L"备份内容预览:", SC(10), DLG_TOP + SC(144), SC(200));
        c->e1 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                SC(10), DLG_TOP + SC(162), w - SC(20), h - DLG_TOP - SC(208), c->h, (HMENU)(INT_PTR)IDC_E1, hi, nullptr);
        btn(L"恢复所选", IDOK, w - SC(158), h - SC(36), SC(72));
        btn(L"删除…", IDC_DELB, w - SC(238), h - SC(36), SC(72));
        btn(L"取消", IDCANCEL, w - SC(78), h - SC(36), SC(66));
        for (auto& f : g_davBackups)
            SendMessageW(c->list, LB_ADDSTRING, 0, (LPARAM)f.c_str());
        SendMessageW(c->list, LB_SETCURSEL, g_davBackups.empty() ? -1 : 0, 0);
        if (!g_davBackups.empty()) DavPreviewBackup(c, 0);
    } else if (c->mode == DLG_SETTINGS) {
        // Tab 页: 0=通用 (快捷键)  1=WebDAV
        c->tabRc[0] = { SC(8), DLG_TOP, SC(92), DLG_TOP + SC(28) };
        c->tabRc[1] = { SC(92), DLG_TOP, SC(208), DLG_TOP + SC(28) };
        c->tabRc[2] = { SC(208), DLG_TOP, SC(268), DLG_TOP + SC(28) };
        int cy = DLG_TOP + SC(40);   // 内容区起始 y
        // ---- 页 0: 通用 ----
        c->pg0.push_back(label(L"显示/隐藏快捷键:", SC(10), cy + SC(14), SC(110)));
        c->hkey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", FormatHotkey(c->pMods, c->pVk).c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY,
                                  SC(122), cy + SC(10), w - SC(132), SC(22), c->h, (HMENU)(INT_PTR)IDC_HK, hi, nullptr);
        c->oldProc = (WNDPROC)SetWindowLongPtrW(c->hkey, GWLP_WNDPROC, (LONG_PTR)HkEditProc);
        c->pg0.push_back(c->hkey);
        c->chkAuto = CreateWindowExW(0, L"BUTTON", L"开机自启 (登录 Windows 后自动运行)",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     SC(10), cy + SC(46), SC(300), SC(20), c->h, (HMENU)(INT_PTR)IDC_AUTO, hi, nullptr);
        SendMessage(c->chkAuto, BM_SETCHECK, GetAutoStart() ? BST_CHECKED : BST_UNCHECKED, 0);
        c->pg0.push_back(c->chkAuto);
        // ---- 页 2: 关于 ----
        c->pg2.push_back(label(L"MiniTodo —— 极简待办清单", SC(10), cy + SC(10), w - SC(20)));
        std::wstring vInfo = std::wstring(L"版本: ") + APP_VER;
        c->pg2.push_back(label(vInfo.c_str(), SC(10), cy + SC(44), w - SC(20)));
        std::wstring bInfo = L"编译日期: " + BuildStamp();
        c->pg2.push_back(label(bInfo.c_str(), SC(10), cy + SC(70), w - SC(20)));
        c->pg2.push_back(label(L"纯 Win32 API 单文件程序, 无外部依赖", SC(10), cy + SC(112), w - SC(20)));
        c->pg2.push_back(label(L"数据保存在程序目录 todo.json (含 .bak 滚动备份)", SC(10), cy + SC(138), w - SC(20)));
        c->pg2.push_back(label(L"WebDAV 备份: /miniTodo/todo-日期-时间.json", SC(10), cy + SC(164), w - SC(20)));
        c->pg2.push_back(label(L"实时同步主文件: /miniTodo/state.json", SC(10), cy + SC(190), w - SC(20)));
        // ---- 页 1: WebDAV ----
        c->pg1.push_back(label(L"WebDAV 地址 (目录, 如 https://dav.example.com/dav/):", SC(10), cy + SC(10), w - SC(20)));
        c->e1 = edit(IDC_E1, SC(10), cy + SC(28), w - SC(20));
        c->pg1.push_back(c->e1);
        c->pg1.push_back(label(L"备份文件夹:", SC(10), cy + SC(60), SC(84)));
        c->e4 = edit(IDC_FOLDER, SC(96), cy + SC(58), SC(110));
        c->pg1.push_back(c->e4);
        c->pg1.push_back(label(L"备份保留天数:", SC(218), cy + SC(60), SC(100)));
        c->keepEd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(c->pendKeep).c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                    SC(340), cy + SC(58), SC(50), SC(22), c->h, (HMENU)(INT_PTR)IDC_KEEP, hi, nullptr);
        c->pg1.push_back(c->keepEd);
        c->pg1.push_back(label(L"用户名:", SC(10), cy + SC(92), SC(60)));
        c->e2 = edit(IDC_E2, SC(88), cy + SC(90), w - SC(98));
        c->pg1.push_back(c->e2);
        c->pg1.push_back(label(L"密码:", SC(10), cy + SC(122), SC(60)));
        c->e3 = edit(IDC_E3, SC(88), cy + SC(120), w - SC(98), ES_PASSWORD);
        c->pg1.push_back(c->e3);
        c->chk = CreateWindowExW(0, L"BUTTON", L"启用 WebDAV", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 SC(10), cy + SC(150), SC(160), SC(20), c->h, (HMENU)(INT_PTR)IDC_CHKAUTO, hi, nullptr);
        SendMessage(c->chk, BM_SETCHECK, c->pendChk ? BST_CHECKED : BST_UNCHECKED, 0);
        c->pg1.push_back(c->chk);
        // 实时同步: 独立于备份的开关
        c->syncChk = CreateWindowExW(0, L"BUTTON", L"实时同步 (多台电脑间自动保持一致)",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     SC(10), cy + SC(174), SC(360), SC(20), c->h, (HMENU)(INT_PTR)IDC_SYNC, hi, nullptr);
        SendMessage(c->syncChk, BM_SETCHECK, c->pendSync ? BST_CHECKED : BST_UNCHECKED, 0);
        c->pg1.push_back(c->syncChk);
        c->pg1.push_back(label(L"备份策略:", SC(10), cy + SC(202), SC(120)));
        c->bkRb0 = CreateWindowExW(0, L"BUTTON", L"不备份",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                                   SC(24), cy + SC(224), SC(120), SC(20), c->h, (HMENU)(INT_PTR)IDC_BKMODE0, hi, nullptr);
        c->bkRb1 = CreateWindowExW(0, L"BUTTON", L"实时备份 (任务变化后自动上传)",
                                   WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   SC(24), cy + SC(248), SC(360), SC(20), c->h, (HMENU)(INT_PTR)IDC_BKMODE1, hi, nullptr);
        c->bkRb2 = CreateWindowExW(0, L"BUTTON", L"定时备份, 每",
                                   WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   SC(24), cy + SC(272), SC(110), SC(20), c->h, (HMENU)(INT_PTR)IDC_BKMODE2, hi, nullptr);
        c->hoursEd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(c->pendHours).c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                                     SC(140), cy + SC(271), SC(36), SC(22), c->h, (HMENU)(INT_PTR)IDC_HOURS, hi, nullptr);
        c->pg1.push_back(c->bkRb0); c->pg1.push_back(c->bkRb1); c->pg1.push_back(c->bkRb2); c->pg1.push_back(c->hoursEd);
        c->pg1.push_back(label(L"小时", SC(184), cy + SC(275), SC(60)));
        HWND restBtn = btn(L"浏览备份并恢复…", IDC_REST, SC(10), h - SC(36), SC(130));
        c->pg1.push_back(restBtn);
        SendMessage(c->bkRb0, BM_SETCHECK, c->pendBkMode == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(c->bkRb1, BM_SETCHECK, c->pendBkMode == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(c->bkRb2, BM_SETCHECK, c->pendBkMode == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(c->e1, c->pend1.c_str());
        SetWindowTextW(c->e2, c->pend2.c_str());
        SetWindowTextW(c->e3, c->pend3.c_str());
        SetWindowTextW(c->e4, c->pend4.c_str());
        // 初始只显示页 0
        for (HWND ch : c->pg1) ShowWindow(ch, SW_HIDE);
        for (HWND ch : c->pg2) ShowWindow(ch, SW_HIDE);
        btn(L"保存", IDOK, w - SC(152), h - SC(36), SC(66));
        btn(L"取消", IDCANCEL, w - SC(78), h - SC(36), SC(66));
    } else if (c->mode == DLG_COLORPOP) {
        // 颜色气泡: 色块 + 启动最小化复选框, 即点即生效
        for (int i = 0; i < BGCOUNT; ++i) {
            int row = i / 5, col = i % 5;
            c->sw[i] = { SC(10) + col * SC(36), DLG_TOP + SC(10) + row * SC(32),
                         SC(34) + col * SC(36), DLG_TOP + SC(34) + row * SC(32) };
        }
        c->showChk = CreateWindowExW(0, L"BUTTON", L"启动时最小化到托盘",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     SC(10), DLG_TOP + SC(78), SC(180), SC(20), c->h, (HMENU)(INT_PTR)IDC_CHKAUTO, hi, nullptr);
        SendMessage(c->showChk, BM_SETCHECK, !g_showStart ? BST_CHECKED : BST_UNCHECKED, 0);
    } else if (c->mode == DLG_BKDIFF) {
        std::wstring head = c->isSync
            ? L"云端数据 (" + c->bkName + L") 与本地记录不一致 (两端可能都有修改):"
            : L"云端备份 " + c->bkName + L" 比本地记录更新 (可能来自其他电脑):";
        label(head.c_str(), SC(10), DLG_TOP + SC(8), w - SC(20));
        c->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS,
                                  SC(10), DLG_TOP + SC(32), w - SC(20), h - DLG_TOP - SC(108),
                                  c->h, (HMENU)(INT_PTR)IDC_LIST, hi, nullptr);
        for (auto& d : c->diffs)
            SendMessageW(c->list, LB_ADDSTRING, 0, (LPARAM)d.c_str());
        btn(L"用云端覆盖本地", IDOK, w - SC(228), h - SC(36), SC(116));
        btn(c->isSync ? L"保留本地上传" : L"保留本地记录", IDCANCEL, w - SC(104), h - SC(36), SC(94));
    }
    // 统一设置字体; 普通编辑框子类化补充 Ctrl+A 全选 (热键捕获框有自己的子类处理, 排除)
    EnumChildWindows(c->h, [](HWND ch, LPARAM lp) -> BOOL {
        wchar_t cls[16] = {};
        GetClassNameW(ch, cls, 16);
        SendMessageW(ch, WM_SETFONT, (WPARAM)fUI, TRUE);
        if (!_wcsicmp(cls, L"edit")) {   // 系统类名为混合大小写 "Edit", 必须不区分大小写比较
            DlgCtx* ctx = (DlgCtx*)lp;
            if (ch != ctx->hkey) {
                SetPropW(ch, PROP_OLDPROC, (HANDLE)GetWindowLongPtrW(ch, GWLP_WNDPROC));
                SetWindowLongPtrW(ch, GWLP_WNDPROC, (LONG_PTR)EditStdProc);
                MTLOG(L"SUBCLASS 安装 hwnd=%p mode=%d", (void*)ch, (int)ctx->mode);
            }
        }
        return TRUE;
    }, (LPARAM)c);
}
static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    DlgCtx* c = (DlgCtx*)GetPropW(h, PROP_CTX);
    if (!c && m != WM_CREATE) return DefWindowProcW(h, m, wp, lp);   // 销毁后 PROP 已移除, 防空指针
    switch (m) {
    case WM_CREATE: {
        c = (DlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        c->h = h;
        SetPropW(h, PROP_CTX, c);
        if (c->owner && c->mode != DLG_COLORPOP) EnableWindow(c->owner, FALSE);   // 模态: 弹窗打开期间禁用属主窗口 (颜色气泡除外)
        DlgOnCreate(c);
        if (c->mode == DLG_COLORPOP) {   // 气泡: 锚定在颜色按钮下方
            SetWindowPos(h, HWND_TOPMOST, c->pendX, c->pendY, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(h);
            return 0;
        }
        RECT ro, rd;
        GetWindowRect(c->owner, &ro); GetWindowRect(h, &rd);
        int x = ro.left + ((ro.right - ro.left) - (rd.right - rd.left)) / 2;
        int y = ro.top + ((ro.bottom - ro.top) - (rd.bottom - rd.top)) / 2;
        SetWindowPos(h, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        return 0;
    }
    case WM_ACTIVATE:   // 颜色气泡: 点击外部 (失活) 自动关闭
        if (c->mode == DLG_COLORPOP && LOWORD(wp) == WA_INACTIVE) DestroyWindow(h);
        return 0;
    case WM_APP_DLG: {   // 子任务编辑结果 (owner 为编辑任务对话框时)
        DlgResult* r = (DlgResult*)lp;
        if (r->ok && r->mode == DLG_SUBTASK && c->mode == DLG_EDITTASK) {
            SubTask s; s.text = OneLine(Trim(r->f1)); s.due = Trim(r->f2);
            if (r->subIdx >= 0 && r->subIdx < (int)c->tmpSubs.size()) {
                if (!s.text.empty()) c->tmpSubs[r->subIdx] = s;
            } else if (!s.text.empty()) {
                c->tmpSubs.push_back(s);
            }
            RefreshSubList(c);
        }
        delete r;
        return 0;
    }
    // 列表条目动态高度: 长文本换行显示 (自绘 LISTBOX)
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (c && mi && mi->CtlType == ODT_LISTBOX) {
            HWND lb = GetDlgItem(h, (int)mi->CtlID);   // MEASUREITEMSTRUCT 无 hwndItem, 经控件 ID 取句柄
            if (!lb) break;
            int n = (int)SendMessageW(lb, LB_GETTEXTLEN, mi->itemID, 0);
            std::wstring s;
            if (n > 0) { s.resize(n); SendMessageW(lb, LB_GETTEXT, mi->itemID, (LPARAM)&s[0]); }
            RECT rc; GetClientRect(lb, &rc);
            HDC dc = GetDC(lb);
            HFONT of = (HFONT)SelectObject(dc, fUI);
            RECT r1 = { 0, 0, 0, 0 };
            DrawTextW(dc, L"Mg", -1, &r1, DT_SINGLELINE | DT_CALCRECT);
            int availW = rc.right - rc.left - SC(28);   // 与 DRAW 侧可用宽度一致 (含滚动条与内边距)
            int lines = s.empty() ? 1 : WrapText(dc, s, availW, nullptr);
            SelectObject(dc, of); ReleaseDC(lb, dc);
            mi->itemHeight = lines * r1.bottom + SC(8);
            if (mi->itemHeight < SC(18)) mi->itemHeight = SC(18);
            return TRUE;
        }
        break;
    }
    // 自绘 LISTBOX 条目: 长文本自动换行, 完成项灰色+删除线
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (c && di && di->CtlType == ODT_LISTBOX && di->itemID != (UINT)-1) {
            int n = (int)SendMessageW(di->hwndItem, LB_GETTEXTLEN, di->itemID, 0);
            std::wstring s;
            if (n > 0) { s.resize(n); SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)&s[0]); }
            bool sel = (di->itemState & ODS_SELECTED) != 0;
            bool done = s.rfind(L"[✓] ", 0) == 0;   // ComposeSubItem 的完成前缀
            FillRect(di->hDC, &di->rcItem, CachedBrush(sel ? RGB(204, 232, 255) : RGB(255, 255, 255)));
            if (!s.empty()) {
                HDC dc = di->hDC;
                HFONT f = done ? fStrike : fUI;
                HFONT of = (HFONT)SelectObject(dc, f);
                SetTextColor(dc, done ? RGB(130, 130, 130) : RGB(15, 15, 15));
                SetBkMode(dc, TRANSPARENT);
                int tx = di->rcItem.left + SC(8);
                int availW = di->rcItem.right - tx - SC(8);
                std::wstring wrapped;
                WrapText(dc, s, availW, &wrapped);
                RECT tr = { tx, di->rcItem.top + SC(4), di->rcItem.right - SC(8), di->rcItem.bottom };
                DrawTextW(dc, wrapped.c_str(), -1, &tr, DT_WORDBREAK);
                SelectObject(dc, of);
            }
            if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) { DlgOnSave(c); return 0; }
        if (id == IDCANCEL) {
            // 同步冲突弹窗"保留本地": 清空指纹并立即上传本地覆盖云端 state.json
            if (c->mode == DLG_BKDIFF && c->isSync) {
                g_lastSyncSig.clear();
                DestroyWindow(h);
                DoSyncPush();
            } else {
                DestroyWindow(h);
            }
            return 0;
        }
        if (c->mode == DLG_EDITTASK) {
            int sel = (int)SendMessage(c->list, LB_GETCURSEL, 0, 0);
            if (id == IDC_LIST && HIWORD(wp) == LBN_DBLCLK && sel >= 0) {
                MTLOG(L"LBN_DBLCLK sel=%d -> OpenSubDialog", sel);
                OpenSubDialog(h, c->taskIdx, sel, c->tmpSubs[sel].text, c->tmpSubs[sel].due);   // 双击子任务 = 编辑弹窗
                return 0;
            }
            if (id == IDC_ADDSUB) {
                OpenSubDialog(h, c->taskIdx, -1, L"", L"");
            } else if (id == IDC_EDITSUB && sel >= 0) {
                OpenSubDialog(h, c->taskIdx, sel, c->tmpSubs[sel].text, c->tmpSubs[sel].due);
            } else if (id == IDC_DELSUB && sel >= 0) {
                c->tmpSubs.erase(c->tmpSubs.begin() + sel);
                RefreshSubList(c);
            }
        }
        if (c->mode == DLG_SETTINGS && id == IDC_REST) {
            OpenDavRestoreDialog(h);   // 属主为设置窗口, 关闭后自动恢复其可用状态
            return 0;
        }
        if (c->mode == DLG_COLORPOP && id == IDC_CHKAUTO && HIWORD(wp) == BN_CLICKED) {
            g_showStart = SendMessage(c->showChk, BM_GETCHECK, 0, 0) != BST_CHECKED;
            SaveAppData();
            return 0;
        }
        if (c->mode == DLG_DAVREST && id == IDC_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(c->list, LB_GETCURSEL, 0, 0);
            SetCursor(LoadCursor(nullptr, IDC_WAIT));
            DavPreviewBackup(c, sel);
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        if (c->mode == DLG_DAVREST && id == IDC_DELB) {
            int sel = (int)SendMessageW(c->list, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= (int)g_davBackups.size()) {
                OpenMsgBox(c->h, APP_NAME, L"请先在列表中选择要删除的备份。", false, nullptr);
                return 0;
            }
            std::wstring msg = L"确定删除备份 " + g_davBackups[sel] + L" ?\n删除后不可恢复。";
            int delSel = sel;
            OpenMsgBox(c->h, L"删除备份", msg, true, [c, delSel](bool ok) {
                if (!ok) return;
                SetCursor(LoadCursor(nullptr, IDC_WAIT));
                DWORD code = 0;
                std::wstring err = WebDavRequest(L"DELETE", DavPath(g_davBackups[delSel]), nullptr, nullptr, &code);
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                if (!err.empty() && code != 404) {
                    OpenMsgBox(c->h, APP_NAME, L"删除失败: " + err, false, nullptr);
                    return;
                }
                SendMessageW(c->list, LB_DELETESTRING, delSel, 0);
                g_davBackups.erase(g_davBackups.begin() + delSel);
                c->previewOk = false; c->prevName.clear();
                SetWindowTextW(c->e1, L"");
                int count = (int)g_davBackups.size();
                if (count == 0) {
                    OpenMsgBox(c->h, APP_NAME, L"最后一个备份已删除。", false, nullptr);
                    DestroyWindow(c->h);
                } else {
                    int ns = (delSel >= count) ? count - 1 : delSel;
                    SendMessageW(c->list, LB_SETCURSEL, ns, 0);
                    DavPreviewBackup(c, ns);
                }
            });
            return 0;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        int w = rc.right;
        FillRect(dc, &rc, g_bgBrush ? g_bgBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
        // 自绘标题栏 (与主窗口风格一致)
        RECT tb = { 0, 0, w, DLG_TOP };
        FillRect(dc, &tb, CachedBrush(DarkBg(12)));
        HPEN pen = CachedPen(RGB(215, 215, 215), 1);
        HPEN op = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, 0, DLG_TOP - 1, nullptr); LineTo(dc, w, DLG_TOP - 1);
        SelectObject(dc, op);
        wchar_t title[128]; GetWindowTextW(h, title, 128);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(25, 25, 25));
        HFONT of = (HFONT)SelectObject(dc, fUI);
        TextOutW(dc, SC(10), (DLG_TOP - SC(16)) / 2, title, (int)wcslen(title));
        SelectObject(dc, of);
        DrawCenterText(dc, c->closeRc, L"×", fUI, RGB(60, 60, 60));
        {   // 自绘 1px 描边
            HPEN pen = CachedPen(RGB(202, 202, 202), 1);
            HPEN op = (HPEN)SelectObject(dc, pen);
            HBRUSH prev = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, rc.right, rc.bottom);
            SelectObject(dc, prev); SelectObject(dc, op);
        }
        // 设置窗口: tab 页栏
        if (c->mode == DLG_SETTINGS) {
            const wchar_t* tabNames[3] = { L"通用", L"WebDAV", L"关于" };
            SetBkMode(dc, TRANSPARENT);
            for (int i = 0; i < 3; ++i) {
                bool cur = c->curTab == i;
                SetTextColor(dc, cur ? RGB(25, 25, 25) : RGB(110, 110, 110));
                HFONT tf = (HFONT)SelectObject(dc, fUI);
                RECT tr = c->tabRc[i];
                DrawTextW(dc, tabNames[i], -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dc, tf);
            }
            // tab 栏底线 + 当前 tab 蓝色下划线
            HPEN tpen = CachedPen(RGB(200, 200, 200), 1);
            HPEN otp = (HPEN)SelectObject(dc, tpen);
            MoveToEx(dc, 0, DLG_TOP + SC(28), nullptr); LineTo(dc, w, DLG_TOP + SC(28));
            SelectObject(dc, otp);
            RECT& cr = c->tabRc[c->curTab];
            HPEN upen = CachedPen(RGB(68, 138, 255), 2);
            HPEN oup = (HPEN)SelectObject(dc, upen);
            MoveToEx(dc, cr.left + SC(4), DLG_TOP + SC(28), nullptr); LineTo(dc, cr.right - SC(4), DLG_TOP + SC(28));
            SelectObject(dc, oup);
        }
        // 颜色气泡: 色块
        if (c->mode == DLG_COLORPOP) {
            for (int i = 0; i < BGCOUNT; ++i) {
                HBRUSH sb = CachedBrush(kBgSwatches[i]);
                HBRUSH osb = (HBRUSH)SelectObject(dc, sb);
                HPEN spen = CachedPen(c->selBg == i ? RGB(68, 138, 255) : RGB(170, 170, 170), c->selBg == i ? 2 : 1);
                HPEN osp = (HPEN)SelectObject(dc, spen);
                Rectangle(dc, c->sw[i].left, c->sw[i].top, c->sw[i].right, c->sw[i].bottom);
                SelectObject(dc, osb); SelectObject(dc, osp);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.y < DLG_TOP) {
            if (PtInRect(&c->closeRc, pt)) { SendMessageW(h, WM_COMMAND, IDCANCEL, 0); return 0; }
            ReleaseCapture();
            SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);   // 拖动标题栏
            return 0;
        }
        if (c->mode == DLG_SETTINGS) {
            // tab 页切换
            for (int i = 0; i < 3; ++i) {
                if (PtInRect(&c->tabRc[i], pt)) {
                    if (c->curTab != i) {
                        c->curTab = i;
                        for (HWND ch : c->pg0) ShowWindow(ch, i == 0 ? SW_SHOW : SW_HIDE);
                        for (HWND ch : c->pg1) ShowWindow(ch, i == 1 ? SW_SHOW : SW_HIDE);
                        for (HWND ch : c->pg2) ShowWindow(ch, i == 2 ? SW_SHOW : SW_HIDE);
                        InvalidateRect(h, nullptr, FALSE);
                    }
                    return 0;
                }
            }
        }
        if (c->mode == DLG_COLORPOP) {
            for (int i = 0; i < BGCOUNT; ++i) {
                if (PtInRect(&c->sw[i], pt)) {   // 点色块立即生效并保存
                    c->selBg = i;
                    g_bg = kBgSwatches[i];
                    UpdateBgBrush();
                    SaveAppData();
                    InvalidateRect(g_hwnd, nullptr, TRUE);
                    return 0;
                }
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_CTLCOLORSTATIC: {   // 标签文字背景与窗口背景一致
        HDC dc = (HDC)wp;
        SetBkColor(dc, g_bg);
        SetTextColor(dc, RGB(25, 25, 25));
        return (LRESULT)(g_bgBrush ? g_bgBrush : GetStockObject(WHITE_BRUSH));
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    case WM_DESTROY: {
        if (c && c->owner && c->mode != DLG_COLORPOP) EnableWindow(c->owner, TRUE);   // 恢复属主窗口 (气泡非模态, 未禁用)
        if (c && c->mode == DLG_BKDIFF) g_syncDialogOpen = false;   // 恢复同步轮询
        if (c && c->mode == DLG_CONFIRM && !c->resultPosted) g_pendMsgCb = nullptr;   // × / 否 关闭: 丢弃挂起回调, 防止下次误触发
        RemovePropW(h, PROP_CTX);
        delete c;
        return 0;
    }
    default:
        return DefWindowProcW(h, m, wp, lp);
    }
    return 0;   // break 出 switch 的消息路径
}

// 打开子任务编辑对话框 (owner 为主窗口或编辑任务对话框)
static void OpenSubDialog(HWND owner, int taskIdx, int subIdx, const std::wstring& text, const std::wstring& due) {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_SUBTASK; c->owner = owner;
    c->taskIdx = taskIdx; c->subIdx = subIdx;
    c->pend1 = text; c->pend2 = due;
    CreateWindowExW(0, L"MTDlg", subIdx < 0 ? L"添加子任务" : L"编辑子任务",
                    WS_POPUP, 0, 0, SC(340), SC(150), owner, nullptr, GetModuleHandleW(nullptr), c);
}

// ---------------- 主窗口操作 ----------------
static void ApplyAlpha() {
    SetLayeredWindowAttributes(g_hwnd, 0, g_alpha, LWA_ALPHA);   // 透明度由 DWM 直接合成, 立即生效
    UpdatePctText();
    // 只有顶栏的百分比数字变化, 只重绘顶栏 (整窗重绘没有必要且更慢);
    // UpdateWindow 同步绘制, 避免点击加减后重绘在消息队列里排队产生延迟感
    RECT rc; GetClientRect(g_hwnd, &rc);
    RECT top = { 0, 0, rc.right, TOPBAR_H };
    InvalidateRect(g_hwnd, &top, FALSE);
    UpdateWindow(g_hwnd);
}
static void ApplyTopmost() {
    SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static bool ApplyHotkey() {
    if (!g_hwnd) return false;
    UnregisterHotKey(g_hwnd, 1);
    return RegisterHotKey(g_hwnd, 1, g_hkMods, g_hkVk) != 0;
}
static void ShowBalloon(const std::wstring& msg) {
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfo, msg.c_str(), 256);
    lstrcpynW(nid.szInfoTitle, APP_NAME, 64);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
static void ToggleVisible() {
    if (IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
    else { ShowWindow(g_hwnd, SW_SHOW); SetForegroundWindow(g_hwnd); }
}
// 通用自绘消息弹窗 (替代系统 MessageBox, 与应用风格一致: g_bg 背景 + 自绘标题栏 + fUI 字体, 居中于 owner 前方)
// yesNo=false: 单个"确定"按钮 (纯提示); yesNo=true: 是/否 (确认); cb 在弹窗确认后回调, × / 否 时不回调
static void OpenMsgBox(HWND owner, const std::wstring& title, const std::wstring& msg, bool yesNo, std::function<void(bool)> cb) {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_CONFIRM; c->owner = owner;
    c->taskIdx = -1;
    c->pend1 = msg;
    c->pendTitle = title;
    c->pendYesNo = yesNo;
    g_pendMsgCb = std::move(cb);
    CreateWindowExW(0, L"MTDlg", title.c_str(),
                    WS_POPUP, 0, 0, SC(300), SC(180), owner, nullptr, GetModuleHandleW(nullptr), c);
}
static void ToggleTask(int rowIdx) {
    const RowRef& rr = g_rows[rowIdx];
    Task& t = g_tasks[rr.t];
    if (rr.s < 0) {
        if (!t.done) {   // 勾选主任务完成: 若有未完成的子任务, 先弹自绘确认框
            int pending = 0;
            for (auto& s : t.subs) if (!s.done) ++pending;
            if (pending > 0) {
                wchar_t msg[256];
                wsprintfW(msg, L"该任务还有 %d 个未完成的子任务。\n\n确认将主任务标记为完成？\n(确认后所有子任务将一并标记为完成)", pending);
                OpenMsgBox(g_hwnd, L"确认", msg, true, [rowIdx](bool ok) {
                    if (!ok || rowIdx < 0 || rowIdx >= (int)g_rows.size()) return;
                    const RowRef& rr2 = g_rows[rowIdx];
                    if (rr2.s < 0 && rr2.t >= 0 && rr2.t < (int)g_tasks.size()) {
                        Task& t2 = g_tasks[rr2.t];
                        t2.done = true;
                        for (auto& s : t2.subs) s.done = true;   // 用户确认: 所有子任务一并完成
                        SaveAppData(); RebuildRows();
                    }
                });   // 确认结果异步回调
                return;
            }
        }
        t.done = !t.done;
    }
    else t.subs[rr.s].done = !t.subs[rr.s].done;
    SaveAppData();
    RebuildRows();
}
static void DeleteRow(int rowIdx) {
    const RowRef& rr = g_rows[rowIdx];
    Task& t = g_tasks[rr.t];
    if (rr.s < 0) g_tasks.erase(g_tasks.begin() + rr.t);
    else t.subs.erase(t.subs.begin() + rr.s);
    SaveAppData();
    RebuildRows();
}

// 打开编辑任务对话框
static void OpenEditTaskDialog(int taskIdx) {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_EDITTASK; c->owner = g_hwnd; c->taskIdx = taskIdx;
    c->pend1 = g_tasks[taskIdx].text;
    c->pend2 = g_tasks[taskIdx].due;
    c->tmpSubs = g_tasks[taskIdx].subs;
    CreateWindowExW(0, L"MTDlg", L"编辑任务",
                    WS_POPUP, 0, 0, SC(400), SC(490), g_hwnd, nullptr, GetModuleHandleW(nullptr), c);
}
static void OpenAddTaskDialog() {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_ADDTASK; c->owner = g_hwnd;
    CreateWindowExW(0, L"MTDlg", L"添加任务",
                    WS_POPUP, 0, 0, SC(340), SC(150), g_hwnd, nullptr, GetModuleHandleW(nullptr), c);
}
static void OpenSettingsDialog() {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_SETTINGS; c->owner = g_hwnd;
    c->pMods = g_hkMods; c->pVk = g_hkVk;
    c->pendChk = g_wdBkMode != 0 || g_wdSync != 0;
    c->pend1 = g_webdavUrl; c->pend2 = g_webdavUser; c->pend3 = g_webdavPass;
    c->pend4 = g_webdavDir; c->pendKeep = g_webdavKeepDays;
    c->pendHours = g_wdBkHours; c->pendBkMode = g_wdBkMode; c->pendSync = g_wdSync != 0;
    CreateWindowExW(0, L"MTDlg", L"设置",
                    WS_POPUP, 0, 0, SC(420), SC(408), g_hwnd, nullptr, GetModuleHandleW(nullptr), c);
}
// 颜色气泡: 锚定在顶栏颜色按钮下方, 非模态, 即点即生效
static void OpenColorPopup() {
    DlgCtx* c = new DlgCtx();
    c->mode = DLG_COLORPOP; c->owner = g_hwnd;
    for (int i = 0; i < BGCOUNT; ++i) if (kBgSwatches[i] == g_bg) c->selBg = i;
    // 颜色按钮的屏幕坐标
    RECT br = {};
    for (int i = 0; i < g_nbtns; ++i) if (g_btns[i].id == B_BG) br = g_btns[i].r;
    POINT pt = { (br.left + br.right) / 2, br.bottom };
    ClientToScreen(g_hwnd, &pt);
    int pw = SC(200), ph = DLG_TOP + SC(108);
    // 工作区范围内钳制 (靠屏幕边缘自动收回)
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = pt.x - pw / 2;
    if (x < wa.left) x = wa.left;
    if (x + pw > wa.right) x = wa.right - pw;
    int y = pt.y + SC(2);
    if (y + ph > wa.bottom) y = pt.y - ph - SC(2);   // 下方放不下则弹到按钮上方
    c->pendX = x; c->pendY = y;
    CreateWindowExW(WS_EX_TOOLWINDOW, L"MTDlg", L"颜色",
                    WS_POPUP, 0, 0, pw, ph, g_hwnd, nullptr, GetModuleHandleW(nullptr), c);
}

// 对话框结果处理 (主窗口接收)
static void HandleDlgResult(DlgResult* r) {
    if (r->ok) {
        switch (r->mode) {
        case DLG_CONFIRM: {   // 自绘消息框: 执行挂起回调 (true=确定/是; × / 否 不进入)
            auto cb = std::move(g_pendMsgCb);
            g_pendMsgCb = nullptr;
            if (cb) cb(r->ok);
            break;
        }
        case DLG_ADDTASK: {
            std::wstring txt = OneLine(Trim(r->f1));
            if (!txt.empty()) {
                Task t; t.text = txt; t.due = Trim(r->f2);
                g_tasks.insert(g_tasks.begin(), std::move(t));
                g_view = 0;
                SaveAppData(); RebuildRows();
            }
            break;
        }
        case DLG_EDITTASK: {
            if (r->taskIdx >= 0 && r->taskIdx < (int)g_tasks.size()) {
                Task& t = g_tasks[r->taskIdx];
                std::wstring txt = OneLine(Trim(r->f1));
                if (!txt.empty()) t.text = txt;
                t.due = Trim(r->f2);
                t.subs = r->subs;
                SaveAppData(); RebuildRows();
            }
            break;
        }
        case DLG_SUBTASK: {   // owner 为主窗口: 直接修改数据
            if (r->taskIdx >= 0 && r->taskIdx < (int)g_tasks.size()) {
                Task& t = g_tasks[r->taskIdx];
                SubTask s; s.text = OneLine(Trim(r->f1)); s.due = Trim(r->f2);
                if (r->subIdx >= 0 && r->subIdx < (int)t.subs.size()) {
                    if (!s.text.empty()) t.subs[r->subIdx] = s;
                } else if (!s.text.empty()) {
                    t.expanded = true;
                    t.subs.push_back(s);
                }
                SaveAppData(); RebuildRows();
            }
            break;
        }
        case DLG_SETTINGS: {
            // WebDAV (原独立配置窗口, 已并入设置)
            g_webdavUrl = Trim(r->f1); g_webdavUser = Trim(r->f2); g_webdavPass = r->f3;
            g_wdBkMode = r->chk ? r->bkMode : 0;
            g_wdSync = (r->chk && r->sync) ? 1 : 0;   // 实时同步独立于备份
            g_wdBkHours = r->hours;
            g_webdavDir = Trim(r->f4);
            // 去掉路径分隔符, 文件夹只保留名字本身
            g_webdavDir.erase(std::remove_if(g_webdavDir.begin(), g_webdavDir.end(),
                             [](wchar_t ch) { return ch == L'/' || ch == L'\\'; }), g_webdavDir.end());
            if (g_webdavDir.empty()) g_webdavDir = L"miniTodo";
            g_webdavKeepDays = r->keep;
            if (g_webdavKeepDays < 1) g_webdavKeepDays = 1;
            if (g_webdavKeepDays > 365) g_webdavKeepDays = 365;
            if (g_wdBkHours < 1) g_wdBkHours = 1;
            if (g_wdBkHours > 24) g_wdBkHours = 24;
            // 快捷键 (仅当用户重新捕获了组合键才应用; 注册失败则回退, 提示冲突)
            if (r->hkMods && r->hkVk) {
                UINT om = g_hkMods, ov = g_hkVk;
                g_hkMods = r->hkMods; g_hkVk = r->hkVk;
                g_hkStr = FormatHotkey(g_hkMods, g_hkVk);
                if (!ApplyHotkey()) {
                    g_hkMods = om; g_hkVk = ov;
                    g_hkStr = FormatHotkey(om, ov);
                    ApplyHotkey();
                    OpenMsgBox(g_hwnd, APP_NAME, L"快捷键注册失败, 可能被其他程序占用, 已恢复原快捷键。", false, nullptr);
                }
            }
            SaveAppData();
            UpdateAutoBkTimer();   // 模式/间隔可能变化, 重建定时器
            break;
        }
        case DLG_DAVREST: {
            std::vector<Task> ts;
            if (!ParseTasksFromJson(r->fbody, ts)) {
                OpenMsgBox(g_hwnd, APP_NAME, L"备份内容解析失败, 未恢复。", false, nullptr);
                break;
            }
            OpenMsgBox(g_hwnd, L"恢复备份", L"将用备份内容覆盖当前全部任务清单, 是否继续?", true,
                       [ts = std::move(ts)](bool ok) mutable {
                if (!ok) return;
                g_tasks = std::move(ts);
                for (auto& t : g_tasks) t.expanded = false;
                g_lastBackupSig = TasksSignature();   // 恢复的内容已存在于备份中, 退出时无需重复备份
                SaveAppData();
                RebuildRows();
                UpdateTrayTip();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                OpenMsgBox(g_hwnd, APP_NAME, L"恢复完成。", false, nullptr);
            });
            break;
        }
        }
    }
    delete r;
}

// ---------------- 托盘 ----------------
static void ShowTrayMenu() {
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, (std::wstring(IsWindowVisible(g_hwnd) ? L"隐藏窗口 (" : L"显示窗口 (") + g_hkStr + L")").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"设置…");
    AppendMenuW(m, MF_STRING | (GetAutoStart() ? MF_CHECKED : 0), 2, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"备份到 WebDAV");
    AppendMenuW(m, MF_STRING, 6, L"恢复 WebDAV 备份…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 9, L"退出");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    switch (cmd) {
    case 1:
        if (IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
        else { ShowWindow(g_hwnd, SW_SHOW); SetForegroundWindow(g_hwnd); }
        break;
    case 2: SetAutoStart(!GetAutoStart()); break;
    case 3: {
        SaveAppData();
        std::string body = SerializeTasksJson(g_tasks);   // 备份只含待办数据
        std::wstring name = BackupFileName();
        std::wstring err = WebDavUploadBackup(body, name);
        if (err.empty()) {
            g_lastBackupSig = TasksSignature();   // 记录指纹, 退出时无变化则不再备份
            RotateBackups();
            OpenMsgBox(g_hwnd, APP_NAME, L"已备份为 " + g_webdavDir + L"/" + name, false, nullptr);
        } else {
            OpenMsgBox(g_hwnd, APP_NAME, L"备份失败: " + err, false, nullptr);
        }
        break;
    }
    case 5: OpenSettingsDialog(); break;
    case 6: OpenDavRestoreDialog(g_hwnd); break;
    case 9: DestroyWindow(g_hwnd); break;
    }
}

// ---------------- 右键菜单 ----------------
static void ListContextMenu(int rowIdx, const POINT& spt) {
    if (rowIdx < 0) return;
    const RowRef& rr = g_rows[rowIdx];
    bool isSub = rr.s >= 0;
    SetForegroundWindow(g_hwnd);
    HMENU m = CreatePopupMenu();
    if (isSub) {
        AppendMenuW(m, MF_STRING, 1, L"编辑子任务");
        AppendMenuW(m, MF_STRING, 2, L"删除子任务");
    } else {
        AppendMenuW(m, MF_STRING, 3, L"添加子任务");
        AppendMenuW(m, MF_STRING, 4, L"编辑任务");
        AppendMenuW(m, MF_STRING, 5, L"删除任务");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, spt.x, spt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 2 || cmd == 5) { DeleteRow(rowIdx); return; }
    if (cmd == 4) { OpenEditTaskDialog(rr.t); return; }
    if (cmd == 3) {
        g_tasks[rr.t].expanded = true;
        SaveAppData(); RebuildRows();
        OpenSubDialog(g_hwnd, rr.t, -1, L"", L"");
        return;
    }
    if (cmd == 1) {
        const Task& t = g_tasks[rr.t];
        OpenSubDialog(g_hwnd, rr.t, rr.s, t.subs[rr.s].text, t.subs[rr.s].due);
    }
}

// ---------------- 主窗口过程 ----------------
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        ApplyAlpha();
        // DWM: 扩展边框取阴影, 同时禁用 DWM 非客户区渲染,
        // 彻底消除失焦时系统画进窗口边缘的边框 (改为自绘 1px 描边)
        HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
        if (dwm) {
            typedef HRESULT(WINAPI * FnE)(HWND, const MARGINS*);
            FnE ext = (FnE)GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
            if (ext) { MARGINS mg = { 0, 0, 1, 0 }; ext(h, &mg); }
            typedef HRESULT(WINAPI * FnA)(HWND, DWORD, LPCVOID, DWORD);
            FnA attr = (FnA)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (attr) {
                DWORD ncrp = 1;   // DWMNCRP_DISABLED
                attr(h, 2, &ncrp, sizeof(ncrp));   // DWMWA_NCRENDERING_POLICY
            }
        }
        // 托盘图标
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = h;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_APP_TRAY;
        g_nid.hIcon = g_icon;
        lstrcpynW(g_nid.szTip, APP_NAME L" - 待办清单", ARRAYSIZE(g_nid.szTip));
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        // 恢复配置 (窗口位置等), 之后按配置注册快捷键
        LoadAppData();
        if (!ApplyHotkey()) {
            // 配置的快捷键被占用时自动尝试备用组合
            static const UINT fbs[3][2] = {
                { MOD_CONTROL | MOD_SHIFT, 'M' }, { MOD_CONTROL | MOD_ALT, 'Q' }, { MOD_CONTROL | MOD_SHIFT | MOD_ALT, 'T' },
            };
            bool ok = false;
            for (int i = 0; i < 3 && !ok; ++i) {
                g_hkMods = fbs[i][0]; g_hkVk = fbs[i][1];
                g_hkStr = FormatHotkey(g_hkMods, g_hkVk);
                ok = ApplyHotkey();
            }
            ShowBalloon(ok ? (L"快捷键被占用, 已临时改用 " + g_hkStr + L", 可在设置中调整")
                           : L"快捷键注册失败, 可在托盘菜单-设置中更换");
        }
        ApplyAlpha();
        ApplyTopmost();
        RebuildRows();
        UpdateAutoBkTimer();   // 定时备份: 按配置启动周期定时器
        SetTimer(h, TIMER_DPICHECK, 1000, nullptr);   // DPI/拓扑自检兜底 (每秒)
        // 启动时: 先同步"上次备份指纹" (本地与云端最新备份相同则不重复上传), 再清理过期备份
        SyncBackupSigOnStart();
        if (!Trim(g_webdavUrl).empty()) RotateBackups();
        return 0;
    }
    case WM_APP_DLG:
        HandleDlgResult((DlgResult*)lp);
        return 0;
    case WM_ACTIVATE:   // 激活时自检: 切换主屏/投影模式后系统未必发 DPICHANGED
        if (LOWORD(wp) != WA_INACTIVE) CheckDisplayFit(h);
        return 0;
    case WM_PAINT: {
        // DPI 安全网: 若拓扑变化未触发其他消息, 借绘制时机纠正
        if (GetWindowDpi(h) != g_dpiNow) CheckDisplayFit(h);
        Paint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SHOWWINDOW:   // 显示前: 校正 DPI 并确保窗口完整落在屏幕工作区内
        if (wp && !IsIconic(h)) FitWindowToScreen(h);
        return 0;
    case WM_DISPLAYCHANGE:   // 显示器拓扑/分辨率变化: 同上
        FitWindowToScreen(h);
        return 0;
    case WM_ENTERSIZEMOVE:   // 开始交互拖动/缩放: 暂停包含校验
        g_inMove = true;
        return 0;
    case WM_EXITSIZEMOVE:    // 拖动结束: 恢复校验
        g_inMove = false;
        return 0;
    case WM_DPICHANGED: {   // 跨屏 DPI 变化: 原样采用系统建议矩形 (官方标准做法, 位置/大小已按目标屏算好, 不会跨屏乒乓)
        UINT ndpi = HIWORD(wp);
        DpiLog(L"WM_DPICHANGED: ndpi=%u g_dpiNow=%u", ndpi, g_dpiNow);
        if (ndpi == 0 || ndpi == g_dpiNow) return 0;   // 同 DPI 的重复消息直接忽略
        g_dpiNow = ndpi;
        g_s = ndpi / 96.0;
        RecalcLayout();
        RECT* prc = (RECT*)lp;
        if (prc && prc->right > prc->left && prc->bottom > prc->top)
            SetWindowPos(h, nullptr, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);   // 触发 WM_SIZE: 重排按钮+滚动条
        g_scroll = 0;
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE: {
        if (IsIconic(h)) return 0;
        RECT rc; GetClientRect(h, &rc);
        g_logW = (int)(rc.right / g_s + 0.5);   // 维护逻辑尺寸 (DPI 变化后用于校验实际尺寸是否同步)
        g_logH = (int)(rc.bottom / g_s + 0.5);
        LayoutButtons(rc.right);
        int listH = rc.bottom - TOPBAR_H - BOTBAR_H;
        int content = RowsContentH();
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_PAGE | SIF_RANGE;
        si.nMax = std::max(0, content - 1);
        si.nPage = (UINT)std::max(1, listH);
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        if (g_scroll > std::max(0, content - listH)) {
            g_scroll = std::max(0, content - listH);
            SetScrollPos(h, SB_VERT, g_scroll, TRUE);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = { SC(360), SC(240) };
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wp) return 0;   // 去掉全部非客户区
        return DefWindowProcW(h, m, wp, lp);
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetWindowRect(h, &rc);
        int x = pt.x - rc.left, y = pt.y - rc.top;
        int w = rc.right - rc.left, hh = rc.bottom - rc.top;
        bool L = x < EDGE, R = x >= w - EDGE, T = y < EDGE, B = y >= hh - EDGE;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
        ScreenToClient(h, &pt);
        if (pt.y < TOPBAR_H && !HitButtonRect(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
        return 0;   // 禁止标题区双击最大化
    case WM_NCPAINT:
        return 0;   // 客户区覆盖整个窗口, 系统经典边框不允许绘制
    case WM_NCUAHDRAWCAPTION:   // 未文档化: 焦点切换时系统重绘经典边框
    case WM_NCUAHDRAWFRAME:
        return 1;
    case WM_NCACTIVATE:
        // lParam 传 -1: 系统更新激活状态但不重绘非客户区,
        // 避免失焦时经典边框画在自绘内容 (底栏标签) 之上
        DefWindowProcW(h, m, wp, -1);
        InvalidateRect(h, nullptr, FALSE);   // 强制重绘, 擦除可能已画入的残留
        return 1;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT rc; GetClientRect(h, &rc);
        // 底栏标签页
        if (y >= rc.bottom - BOTBAR_H) {
            for (int i = 0; i < 2; ++i) {
                if (PtInRect(&g_tabRects[i], { x, y }) && g_view != i) {
                    g_view = i;
                    RebuildRows();
                    return 0;
                }
            }
            return 0;
        }
        int b = HitButton(x, y);
        if (b) {
            switch (b) {
            case B_ADD: OpenAddTaskDialog(); break;
            case B_PIN: g_topmost = !g_topmost; ApplyTopmost(); InvalidateRect(h, nullptr, FALSE); break;
            case B_OPM: g_alpha = AlphaOfPct(std::max(30, PctOfAlpha(g_alpha) - 10)); ApplyAlpha(); break;
            case B_OPP: g_alpha = AlphaOfPct(std::min(100, PctOfAlpha(g_alpha) + 10)); ApplyAlpha(); break;
            case B_SET: OpenSettingsDialog(); break;
            case B_BG: OpenColorPopup(); break;
            case B_CLOSE: ShowWindow(h, SW_HIDE); break;
            }
            return 0;
        }
        int row = HitRow(x, y);
        if (row >= 0) {
            const RowRef& rr = g_rows[row];
            int rh = RowHeight(row);
            int baseH = rr.s < 0 ? ROW_H : SUBROW_H;
            int yy = TOPBAR_H - g_scroll + RowTop(row);
            int cbx = rr.s < 0 ? SC(8) : SC(26);
            RECT cb = { cbx, yy + (baseH - SC(13)) / 2 - 1, cbx + SC(15), yy + (baseH - SC(13)) / 2 + SC(15) };
            if (PtInRect(&cb, { x, y })) { ToggleTask(row); return 0; }
            if (rr.s < 0 && !g_tasks[rr.t].subs.empty()) {
                RECT ar = { SC(20), yy, SC(36), yy + rh };
                if (PtInRect(&ar, { x, y })) {
                    g_tasks[rr.t].expanded = !g_tasks[rr.t].expanded;
                    SaveAppData(); RebuildRows();
                    return 0;
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int row = HitRow(x, y);
        if (row >= 0) {
            const RowRef& rr = g_rows[row];
            if (rr.s < 0) OpenEditTaskDialog(rr.t);
            else {
                const Task& t = g_tasks[rr.t];
                OpenSubDialog(h, rr.t, rr.s, t.subs[rr.s].text, t.subs[rr.s].due);
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT spt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (spt.x == -1 && spt.y == -1) return 0;
        if ((HWND)wp != h) return 0;
        POINT cpt = spt; ScreenToClient(h, &cpt);
        ListContextMenu(HitRow(cpt.x, cpt.y), spt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT rc; GetClientRect(h, &rc);
        int hb = HitButton(x, y);
        int row = (y < rc.bottom - BOTBAR_H) ? HitRow(x, y) : -1;
        int tab = -1;
        if (y >= rc.bottom - BOTBAR_H)
            for (int i = 0; i < 2; ++i) if (PtInRect(&g_tabRects[i], { x, y })) tab = i;
        if (hb != g_hoverBtn || row != g_hoverRow || tab != g_hoverTab) {
            // 只失效悬停状态变化的按钮/行/标签矩形, 避免整窗重绘造成点击与悬停反馈延迟
            auto invBtn = [&](int id) {
                if (!id) return;
                for (int i = 0; i < g_nbtns; ++i) if (g_btns[i].id == id) {
                    RECT r = g_btns[i].r; InflateRect(&r, 1, 1); InvalidateRect(h, &r, FALSE);
                }
            };
            auto invRow = [&](int idx) {
                if (idx < 0 || idx >= (int)g_rows.size()) return;
                RECT r = { 0, TOPBAR_H - g_scroll + RowTop(idx), rc.right,
                           TOPBAR_H - g_scroll + RowTop(idx) + RowHeight(idx) };
                InvalidateRect(h, &r, FALSE);
            };
            if (g_hoverBtn != hb) { invBtn(g_hoverBtn); invBtn(hb); }
            if (g_hoverRow != row) { invRow(g_hoverRow); invRow(row); }
            if (g_hoverTab != tab) {
                for (int i = 0; i < 2; ++i) {
                    RECT r = g_tabRects[i]; InflateRect(&r, 2, 2); InvalidateRect(h, &r, FALSE);
                }
            }
            g_hoverBtn = hb; g_hoverRow = row; g_hoverTab = tab;
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: {
        // 鼠标离开: 只失效此前悬停的元素, 避免整窗重绘
        RECT rc; GetClientRect(h, &rc);
        auto invBtn = [&](int id) {
            if (!id) return;
            for (int i = 0; i < g_nbtns; ++i) if (g_btns[i].id == id) {
                RECT r = g_btns[i].r; InflateRect(&r, 1, 1); InvalidateRect(h, &r, FALSE);
            }
        };
        auto invRow = [&](int idx) {
            if (idx < 0 || idx >= (int)g_rows.size()) return;
            RECT r = { 0, TOPBAR_H - g_scroll + RowTop(idx), rc.right,
                       TOPBAR_H - g_scroll + RowTop(idx) + RowHeight(idx) };
            InvalidateRect(h, &r, FALSE);
        };
        invBtn(g_hoverBtn); invRow(g_hoverRow);
        for (int i = 0; i < 2; ++i) {
            RECT r = g_tabRects[i]; InflateRect(&r, 2, 2); InvalidateRect(h, &r, FALSE);
        }
        g_hoverBtn = 0; g_hoverRow = -1; g_hoverTab = -1;
        return 0;
    }
    case WM_VSCROLL: {
        RECT rc; GetClientRect(h, &rc);
        int listH = rc.bottom - TOPBAR_H - BOTBAR_H;
        int content = RowsContentH();
        int old = g_scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: g_scroll -= ROW_H; break;
        case SB_LINEDOWN: g_scroll += ROW_H; break;
        case SB_PAGEUP: g_scroll -= listH; break;
        case SB_PAGEDOWN: g_scroll += listH; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: g_scroll = HIWORD(wp); break;
        }
        g_scroll = std::max(0, std::min(g_scroll, std::max(0, content - listH)));
        if (g_scroll != old) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        RECT rc; GetClientRect(h, &rc);
        int listH = rc.bottom - TOPBAR_H - BOTBAR_H;
        int content = RowsContentH();
        int old = g_scroll;
        g_scroll -= delta / WHEEL_DELTA * ROW_H * 2;
        g_scroll = std::max(0, std::min(g_scroll, std::max(0, content - listH)));
        if (g_scroll != old) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(h, SW_HIDE);
        return 0;
    case WM_APP_TRAY:
        if (lp == WM_LBUTTONUP) {
            ToggleVisible();
        } else if (lp == WM_RBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;
    case WM_HOTKEY:
        if (wp == 1) ToggleVisible();
        return 0;
    case WM_TIMER:
        // 实时备份: 10 秒防抖到期; 定时备份: 周期到期 (内部都做指纹比较, 无变化不上传)
        if (wp == TIMER_AUTOBK_DEBOUNCE) {
            KillTimer(h, TIMER_AUTOBK_DEBOUNCE);
            DoAutoBk();        // 实时备份 (模式 1)
            DoSyncPush();      // 实时同步推送 (开关开启时)
        }
        else if (wp == TIMER_AUTOBK_TICK) DoAutoBk();
        else if (wp == TIMER_SYNC_POLL) SyncPoll();   // 实时同步: 拉取其他机器的修改
        else if (wp == TIMER_DPICHECK) {   // 显示模式切换可能不触发任何消息, 定时兜底校验
            if (!g_inMove && !IsIconic(h) && IsWindowVisible(h)) {
                // 调试日志: 仅在观测值变化时记录, 供跨屏适配问题排查
                RECT crc; GetClientRect(h, &crc);
                typedef UINT(WINAPI* PFN_DFW)(HWND);
                static PFN_DFW dfw = (PFN_DFW)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
                UINT dpiWin = dfw ? dfw(h) : 0;
                UINT dpiMon = GetMonitorDpi(h);
                static UINT gs_wdpi, gs_mdpi, gs_cw, gs_ch, gs_dpiNow;
                static int gs_logw, gs_logh;
                if (dpiWin != gs_wdpi || dpiMon != gs_mdpi || crc.right != (LONG)gs_cw || crc.bottom != (LONG)gs_ch
                    || g_dpiNow != gs_dpiNow || g_logW != gs_logw || g_logH != gs_logh) {
                    DpiLog(L"tick: dpiWin=%u dpiMon=%u g_dpiNow=%u client=%ldx%ld log=%dx%d",
                           dpiWin, dpiMon, g_dpiNow, crc.right, crc.bottom, g_logW, g_logH);
                    gs_wdpi = dpiWin; gs_mdpi = dpiMon; gs_cw = (UINT)crc.right; gs_ch = (UINT)crc.bottom;
                    gs_dpiNow = g_dpiNow; gs_logw = g_logW; gs_logh = g_logH;
                }
                CheckDisplayFit(h);
            }
        }
        return 0;
    case WM_ENDSESSION:
        if (wp) SaveAppData();
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);   // × 隐藏到托盘, 常驻后台
        return 0;
    case WM_DESTROY: {
        // 自动备份模式下数据已实时/定时上云, 退出不再额外备份
        SaveAppData();
        KillTimer(h, TIMER_AUTOBK_DEBOUNCE);
        KillTimer(h, TIMER_AUTOBK_TICK);
        KillTimer(h, TIMER_DPICHECK);
        KillTimer(h, TIMER_SYNC_POLL);
        UnregisterHotKey(h, 1);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    default:
        if (m == g_msgTaskbarCreated) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
            return 0;
        }
        return DefWindowProcW(h, m, wp, lp);
    }
}

// 按 DPI 重算布局常量并重建字体 (启动及 WM_DPICHANGED 跨屏时调用)
static void RecalcLayout() {
    TOPBAR_H = SC(34); BOTBAR_H = SC(24); ROW_H = SC(26); SUBROW_H = SC(22); EDGE = SC(6); DLG_TOP = SC(30);
    if (fUI) DeleteObject(fUI);
    if (fRow) DeleteObject(fRow);
    if (fSub) DeleteObject(fSub);
    if (fStrike) DeleteObject(fStrike);
    if (fStrikeSub) DeleteObject(fStrikeSub);
    if (fGear) DeleteObject(fGear);
    fUI = MkFont(12, false);
    fRow = MkFont(13, false);
    fSub = MkFont(12, false);
    fStrike = MkFont(13, true);
    fStrikeSub = MkFont(12, true);
    fGear = CreateFontW(-SC(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");   // ⚙ 齿轮字形
}

// 取指定屏幕的真实 DPI (GetDpiForMonitor, 系统重定位后仍准确)
static UINT GetMonitorDpiOf(HMONITOR mon) {
    typedef HRESULT(WINAPI* PFN_GDM)(HMONITOR, int, UINT*, UINT*);
    static PFN_GDM gdm = nullptr;
    if (!gdm) {
        static HMODULE sh = LoadLibraryW(L"Shcore.dll");
        gdm = sh ? (PFN_GDM)GetProcAddress(sh, "GetDpiForMonitor") : nullptr;
    }
    UINT dx = 0, dy = 0;
    if (gdm && mon && SUCCEEDED(gdm(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx) return dx;
    return 0;
}

// 取窗口所在屏幕的真实 DPI (GetDpiForMonitor, 系统重定位后仍准确)
static UINT GetMonitorDpi(HWND h) {
    return GetMonitorDpiOf(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
}

// 取窗口当前生效的 DPI: 优先所在屏幕的真实 DPI (GetDpiForWindow 在系统重定位场景可能返回旧值)
static UINT GetWindowDpi(HWND h) {
    UINT d = GetMonitorDpi(h);
    if (d) return d;
    typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow f = (PFN_GetDpiForWindow)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT w = f ? f(h) : 0;
    return w ? w : (UINT)(g_s * 96 + 0.5);
}

// 应用 DPI 变化: 重算布局/字体并按比例调整窗口尺寸
// (用于隐藏期间/拓扑变化场景, 窗口静止在本屏, 无跨屏乒乓风险)
static void ApplyDpiChange(HWND h, UINT ndpi) {
    RECT orc; GetWindowRect(h, &orc);
    DpiLog(L"ApplyDpiChange: ndpi=%u old=%u rect=(%ld,%ld,%ld,%ld)", ndpi, g_dpiNow, orc.left, orc.top, orc.right, orc.bottom);
    double k = (double)ndpi / g_dpiNow;
    g_dpiNow = ndpi;
    g_s = ndpi / 96.0;
    RecalcLayout();
    SetWindowPos(h, nullptr, orc.left, orc.top,
                 (LONG)((orc.right - orc.left) * k + 0.5),
                 (LONG)((orc.bottom - orc.top) * k + 0.5),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    RECT crc; GetClientRect(h, &crc);
    LayoutButtons(crc.right);   // 显式重排: 系统重定位期间 SetWindowPos 可能未产生 WM_SIZE, 不能依赖它
    g_scroll = 0;
    InvalidateRect(h, nullptr, TRUE);
    RECT nrc; GetWindowRect(h, &nrc);
    DpiLog(L"ApplyDpiChange: after rect=(%ld,%ld,%ld,%ld)", nrc.left, nrc.top, nrc.right, nrc.bottom);
    if (!g_inMove) FitWindowToScreen(h);   // 缩放后窗口可能超出屏幕, 夹回工作区
}

// DPI 自检: DPI 不一致则重缩放; 实际尺寸与逻辑尺寸×缩放系数偏差过大则强制纠正
// (跨屏重定位期间系统可能覆盖我们的尺寸调整, 导致字体缩了但窗口/控件没同步)
static void CheckDisplayFit(HWND h) {
    UINT ndpi = GetWindowDpi(h);
    if (ndpi && ndpi != g_dpiNow) {
        DpiLog(L"CheckDisplayFit: dpi mismatch ndpi=%u g_dpiNow=%u", ndpi, g_dpiNow);
        ApplyDpiChange(h, ndpi);
    }
    RECT crc; GetClientRect(h, &crc);
    int ew = (int)(g_logW * g_s + 0.5), eh = (int)(g_logH * g_s + 0.5);
    if (abs(crc.right - ew) > 2 || abs(crc.bottom - eh) > 2) {
        DpiLog(L"CheckDisplayFit: size mismatch client=%ldx%ld expect=%dx%d (log %dx%d s=%.2f)",
               crc.right, crc.bottom, ew, eh, g_logW, g_logH, g_s);
        SetWindowPos(h, nullptr, 0, 0, ew, eh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT rc2; GetClientRect(h, &rc2);
        LayoutButtons(rc2.right);
        InvalidateRect(h, nullptr, TRUE);
    }
}

// DPI 校正 + 窗口必须完整落在所在屏幕工作区内 (从托盘显示/显示模式切换时调用)
// 交互拖动期间跳过, 不影响用户把窗口拖到两块屏幕的任意位置
static void FitWindowToScreen(HWND h) {
    if (g_inMove) return;
    UINT ndpi = GetWindowDpi(h);
    if (ndpi && ndpi != g_dpiNow) ApplyDpiChange(h, ndpi);
    RECT rc; GetWindowRect(h, &rc);
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    LONG ww = rc.right - rc.left, hh = rc.bottom - rc.top, need = 0;
    LONG maxw = mi.rcWork.right - mi.rcWork.left, maxh = mi.rcWork.bottom - mi.rcWork.top;
    if (ww > maxw) { ww = maxw; need = 1; }          // 窗口比屏幕还大: 缩到工作区
    if (hh > maxh) { hh = maxh; need = 1; }
    LONG nx = rc.left, ny = rc.top;
    if (nx < mi.rcWork.left)        { nx = mi.rcWork.left; need = 1; }          // 超出左/上边
    if (ny < mi.rcWork.top)         { ny = mi.rcWork.top; need = 1; }
    if (nx + ww > mi.rcWork.right)  { nx = mi.rcWork.right - ww; need = 1; }    // 超出右/下边
    if (ny + hh > mi.rcWork.bottom) { ny = mi.rcWork.bottom - hh; need = 1; }
    if (need) SetWindowPos(h, nullptr, nx, ny, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------- 入口 ----------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    MTLOG(L"APP START ver=%ls", APP_VER);   // 启动日志 (诊断日志机制自检)
    // 进程 DPI 感知: 优先 Per-Monitor V2 (跨不同缩放率屏幕时经 WM_DPICHANGED 自动重缩放),
    // 不支持时回退 System Aware (跨屏需重启程序才能匹配)
    typedef BOOL(WINAPI* PFN_SetDpiAwarenessCtx)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_SetDpiAwarenessCtx setCtx = (PFN_SetDpiAwarenessCtx)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    bool pmv2 = setCtx && setCtx((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (!pmv2) {
        if (setCtx) setCtx((HANDLE)-3);   // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
        else {
            HMODULE shc = LoadLibraryW(L"shcore.dll");
            if (shc) {
                typedef HRESULT(WINAPI* PFN_SetProcDpi)(int);
                ((PFN_SetProcDpi)(void*)GetProcAddress(shc, "SetProcessDpiAwareness"))((int)1 /*SYSTEM_DPI_AWARE*/);
            } else SetProcessDPIAware();
        }
    }
    HDC sdc = GetDC(nullptr);
    g_s = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, sdc);
    // 布局常量与字体按 DPI 计算 (必须在所有窗口创建之前)
    g_dpiNow = (UINT)(g_s * 96 + 0.5);
    RecalcLayout();
    // 单实例: 重复启动时唤出已有窗口
    // (v2: 若旧版本残留进程占用旧名互斥体, 新名可正常启动)
    HANDLE mu = CreateMutexW(nullptr, TRUE, L"MiniTodo_Instance_Guard_v2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND ex = FindWindowW(L"MTMain", nullptr);
        if (ex) { ShowWindow(ex, SW_SHOW); SetForegroundWindow(ex); }
        return 0;
    }
    InitCfgPath();

    g_icon = MakeIcon(32);
    // 程序图标: 优先用嵌入资源 (与托盘对勾同款), 加载失败回退到 GDI 绘制
    HICON icBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    HICON icSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (icBig) g_icon = icBig; else icBig = g_icon;
    if (!icSm) icSm = g_icon;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = icBig; wc.hIconSm = icSm;
    wc.lpszClassName = L"MTMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW dc = { sizeof(dc) };
    dc.style = CS_HREDRAW | CS_VREDRAW;
    dc.lpfnWndProc = DlgProc;
    dc.hInstance = hInst;
    dc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    dc.hIcon = icBig; dc.hIconSm = icSm;
    dc.hbrBackground = CreateSolidBrush(RGB(250, 250, 250));   // 与主窗口一致的浅色背景
    dc.lpszClassName = L"MTDlg";
    RegisterClassExW(&dc);

    UpdateBgBrush();

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"MTMain", APP_NAME,
                             WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL,
                             SC(80), SC(60), SC(420), SC(560), nullptr, nullptr, hInst, nullptr);
    if (!h) return 1;
    // 启动显示: 配置为"最小化"时窗口保持隐藏, 只在托盘出现
    if (g_showStart) {
        ShowWindow(h, SW_SHOW);
        UpdateWindow(h);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
