#include <windows.h>
#include <set>
#include <tuple>
#include <vector>
#include <algorithm>
#include <climits>
#include <string>
#include <sstream>

#include <unistd.h>
#include <stdlib.h>

#include "win.hh"

#include <d2d1.h>

extern "C" {
#include "winpriv.h"

int cs_mbstowcs(wchar *ws, const char *s, size_t wlen);
}

#define lengthof(array) (sizeof(array) / sizeof(*(array)))

#define CLOSE_BUTTON_PADDING  2
#define ADD_BUTTON_PADDING 2

using std::tuple;
using std::get;

typedef void (*CallbackFn)(void*);
typedef tuple<CallbackFn, void*> Callback;
typedef std::set<Callback> CallbackSet;

static CallbackSet callbacks;
static std::vector<Tab> tabs;
static unsigned int active_tab = 0;

static float g_xscale, g_yscale;

static void init_scale_factors() {
    static ID2D1Factory* d2d_factory = nullptr;
    if (d2d_factory == nullptr) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
    }
    float xdpi, ydpi;
    d2d_factory->ReloadSystemMetrics();
    d2d_factory->GetDesktopDpi(&xdpi, &ydpi);
    g_xscale = xdpi / 96.0f;
    g_yscale = ydpi / 96.0f;
}

Tab::Tab() : terminal(new term), chld(new child) {
    memset(terminal.get(), 0, sizeof(struct term));
    memset(chld.get(), 0, sizeof(struct child));
    info.attention = false;
    info.titles_i = 0;
}
Tab::~Tab() {
    if (terminal)
        term_free(terminal.get());
    if (chld)
        child_free(chld.get());
}
Tab::Tab(Tab&& t) {
    info = t.info;
    terminal = std::move(t.terminal);
    chld = std::move(t.chld);
}
Tab& Tab::operator=(Tab&& t) {
    std::swap(terminal, t.terminal);
    std::swap(chld, t.chld);
    std::swap(info, t.info);
    return *this;
}

extern "C" {

void win_set_timer(CallbackFn cb, void* data, uint ticks) {
    auto result = callbacks.insert(std::make_tuple(cb, data));
    CallbackSet::iterator iter = result.first;
    SetTimer(wnd, reinterpret_cast<UINT_PTR>(&*iter), ticks, NULL);
}

void win_process_timer_message(WPARAM message) {
    void* pointer = reinterpret_cast<void*>(message);
    auto callback = *reinterpret_cast<Callback*>(pointer);
    callbacks.erase(callback);
    KillTimer(wnd, message);

    // call the callback
    get<0>(callback)( get<1>(callback) );
}

static void invalidate_tabs() {
    win_invalidate_all();
}

term* win_active_terminal() {
    return tabs.at(active_tab).terminal.get();
}

int win_tab_count() { return tabs.size(); }
int win_active_tab() { return active_tab; }

static void update_window_state() {
    win_update_menus();
    if (cfg.title_settable)
      SetWindowTextW(wnd, win_tab_get_title(active_tab));
    win_adapt_term_size();
}

static void set_active_tab(unsigned int index) {
    active_tab = index;
    Tab* active = &tabs.at(active_tab);
    for (auto& tab : tabs) {
        term_set_focus(tab.terminal.get(), &tab == active);
    }
    active->info.attention = false;
    update_window_state();
    win_invalidate_all();
}

static unsigned int rel_index(int change) {
    return (int(active_tab) + change + tabs.size()) % tabs.size();
}

void win_tab_change(int change) {
    set_active_tab(rel_index(change));
}
void win_tab_move(int amount) {
    auto new_idx = rel_index(amount);
    std::swap(tabs[active_tab], tabs[new_idx]);
    set_active_tab(new_idx);
}

static Tab& tab_by_term(struct term* term) {
    auto match = find_if(tabs.begin(), tabs.end(), [=](Tab& tab) {
            return tab.terminal.get() == term; });
    return *match;
}

static char* g_home;
static char* g_cmd;
static char** g_argv;
static void newtab(
        unsigned short rows, unsigned short cols,
        unsigned short width, unsigned short height, const char* cwd, char* title) {
    tabs.push_back(Tab());
    Tab& tab = tabs.back();
    tab.terminal->child = tab.chld.get();
    term_reset(tab.terminal.get());
    term_resize(tab.terminal.get(), rows, cols);
    tab.chld->cmd = g_cmd;
    tab.chld->home = g_home;
    struct winsize wsz{rows, cols, width, height};
    child_create(tab.chld.get(), tab.terminal.get(), g_argv, &wsz, cwd);
    wchar * ws;
    if (title) {
      int size = cs_mbstowcs(NULL, title, 0) + 1;
      ws = (wchar *)malloc(size * sizeof(wchar));  // includes terminating NUL
      cs_mbstowcs(ws, title, size);
    }
    else {
      int size = cs_mbstowcs(NULL, g_cmd, 0) + 1;
      ws = (wchar *)malloc(size * sizeof(wchar));  // includes terminating NUL
      cs_mbstowcs(ws, g_cmd, size);
    }
    win_tab_set_title(tab.terminal.get(), ws);
    free(ws);
}

static void set_tab_bar_visibility(bool b);

void win_tab_set_argv(char** argv) {
    g_argv = argv;
}

void win_tab_init(char* home, char* cmd, char** argv, int width, int height, char* title) {
    g_home = home;
    g_cmd = cmd;
    g_argv = argv;
    newtab(cfg.rows, cfg.cols, width, height, nullptr, title);
    set_tab_bar_visibility(tabs.size() > 1);
}
void win_tab_create() {
    auto& t = *tabs[active_tab].terminal;
    std::stringstream cwd_path;
    cwd_path << "/proc/" << t.child->pid << "/cwd";
    char* cwd = realpath(cwd_path.str().c_str(), 0);
    newtab(t.rows, t.cols, t.cols * font_width, t.rows * font_height, cwd, nullptr);
    free(cwd);
    set_active_tab(tabs.size() - 1);
    set_tab_bar_visibility(tabs.size() > 1);
}

void win_tab_close() {
    child_terminate(win_active_terminal()->child);
}
    
void win_tab_clean() {
    bool invalidate = false;
    for (;;) {
        auto it = std::find_if(tabs.begin(), tabs.end(), [](Tab& x) {
                return x.chld->pid == 0; });
        if (it == tabs.end()) break;
        invalidate = true;
        for (;;) {
          auto cb = std::find_if(callbacks.begin(), callbacks.end(), [&it](Callback x) {
            return ((term *)(get<1>(x)) == (*it).terminal.get()); });
          if (cb == callbacks.end()) break;
          KillTimer(wnd, reinterpret_cast<UINT_PTR>(&*cb));
          callbacks.erase(cb);
        }
        tabs.erase(it);
    }
    if (invalidate && tabs.size() > 0) {
        if (active_tab >= tabs.size())
            set_active_tab(tabs.size() - 1);
        else
            set_active_tab(active_tab);
        set_tab_bar_visibility(tabs.size() > 1);
        win_invalidate_all();
    }
}

void win_tab_attention(struct term* term) {
    tab_by_term(term).info.attention = true;
    invalidate_tabs();
}

void win_tab_set_title(struct term* term, wchar_t* title) {
    auto& tab = tab_by_term(term);
    if (tab.info.titles[tab.info.titles_i] != title) {
        tab.info.titles[tab.info.titles_i] = title;
        invalidate_tabs();
    }
    if (term == win_active_terminal()) {
      win_set_title((wchar *)tab.info.titles[tab.info.titles_i].data());
    }
}

wchar_t* win_tab_get_title(unsigned int idx) {
    return (wchar_t *)tabs[idx].info.titles[tabs[idx].info.titles_i].c_str();
}

void win_tab_title_push(struct term* term) {
  Tab& tab = tab_by_term(term);
  if (tab.info.titles_i == lengthof(tab.info.titles))
    tab.info.titles_i = 0;
  else
    tab.info.titles_i++;
}
  
wchar_t* win_tab_title_pop(struct term* term) {
  Tab& tab = tab_by_term(term);
  if (!tab.info.titles_i)
    tab.info.titles_i = lengthof(tab.info.titles);
  else
    tab.info.titles_i--;
  return win_tab_get_title(active_tab);
}

/*
 * Title stack (implemented as fixed-size circular buffer)
 */
void
win_tab_save_title(struct term* term)
{
  win_tab_title_push(term);
}

void
win_tab_restore_title(struct term* term)
{
  win_tab_set_title(term, win_tab_title_pop(term));
}

bool win_should_die() { return tabs.size() == 0; }

static int tabheight() {
    init_scale_factors();
    return 23 * g_yscale;
}

static bool tab_bar_visible = false;
static void fix_window_size() {
    // doesn't work fully when you put fullscreen and then show or hide
    // tab bar, but it's not too terrible (just looks little off) so I
    // don't care. Maybe fix it later?
    if (win_is_fullscreen) {
        win_adapt_term_size();
    } else {
        auto& t = *tabs[active_tab].terminal;
        win_set_chars(t.rows, t.cols);
    }
}
static void set_tab_bar_visibility(bool b) {
    if (b == tab_bar_visible) return;

    tab_bar_visible = b;
    g_render_tab_height = win_tab_height();
    fix_window_size();
    win_invalidate_all();
}
int win_tab_height() { return tab_bar_visible ? tabheight() : 0; }

static int tab_font_size() {
    return 14 * g_yscale;
}

static HGDIOBJ new_tab_font() {
    return CreateFont(tab_font_size(),0,0,0,FW_NORMAL,0,0,0,1,0,0,CLEARTYPE_QUALITY,0,0);
}

static HGDIOBJ new_active_tab_font() {
    return CreateFont(tab_font_size(),0,0,0,FW_BOLD,0,0,0,1,0,0,CLEARTYPE_QUALITY,0,0);
}

// paint a tab to dc (where dc draws to buffer)
static void paint_tab(HDC dc, int width, int tabheight, const Tab& tab) {
    // box lines
    MoveToEx(dc, 0, tabheight, nullptr);
    LineTo(dc, 0, 0);
    LineTo(dc, width, 0);

    // title
    const auto & str = tab.info.titles[tab.info.titles_i];
    SIZE str_size;
    GetTextExtentPoint32W(dc, str.data(), str.size(), &str_size);
    
    RECT text_rect = {0, 0, width, tabheight};
    UINT text_fmt = DT_VCENTER | DT_SINGLELINE;
    const int close_button_size = tab_font_size() + 2 * CLOSE_BUTTON_PADDING * g_xscale;
    if ((str_size.cx + close_button_size) > width) {
        text_rect.left += 5 * g_xscale;
        text_rect.right = width - close_button_size;
        text_fmt |= DT_RIGHT;
    }
    else {
        text_fmt |= DT_CENTER;
    }
    DrawTextW(dc, str.data(), str.size(), &text_rect, text_fmt);
    
    // close button
    RECT close_rect = {int(width - CLOSE_BUTTON_PADDING * g_xscale - tab_font_size()),
                       0,
                       int(width - CLOSE_BUTTON_PADDING * g_xscale),
                       tabheight};

    DrawTextW(dc, L"x", 1, &close_rect, DT_CENTER | DT_VCENTER);
}

// Wrap GDI object for automatic release
struct SelectWObj {
    HDC tdc;
    HGDIOBJ old;
    SelectWObj(HDC dc, HGDIOBJ obj) { tdc = dc; old = SelectObject(dc, obj); }
    ~SelectWObj() { DeleteObject(SelectObject(tdc, old)); }
};

static int tab_paint_width = 0;
void win_paint_tabs(HDC dc, int width) {
    if (!tab_bar_visible) return;

    const int loc_tabheight = 18 * g_yscale;
        
    // the sides of drawable area are not visible, so we really should draw to
    // coordinates 1..(width-2)
    width = width - 2 * PADDING - ADD_BUTTON_PADDING - loc_tabheight * 0.5;

    const auto bg = cfg.tab_bg_colour;
    const auto fg = cfg.tab_fg_colour;
    const auto active_bg = cfg.tab_active_bg_colour;
    const auto attention_bg = cfg.tab_attention_bg_colour;

    const unsigned int preferred_width = 200 * g_xscale;
    const int tabwidth = (width / tabs.size()) > preferred_width ? preferred_width : width / tabs.size();

    tab_paint_width = tabwidth;
    RECT tabrect;
    SetRect(&tabrect, 0, 0, tabwidth, loc_tabheight+1);

    HDC bufdc = CreateCompatibleDC(dc);
    SetBkMode(bufdc, TRANSPARENT);
    SetTextColor(bufdc, fg);
    //SetTextAlign(bufdc, TA_CENTER);
    {
        auto brush = CreateSolidBrush(bg);
        auto obrush = SelectWObj(bufdc, brush);
        auto open = SelectWObj(bufdc, CreatePen(PS_SOLID, 0, fg));
        auto obuf = SelectWObj(bufdc,
                CreateCompatibleBitmap(dc, tabwidth, tabheight()));

        auto ofont = SelectWObj(bufdc, new_tab_font());

        for (size_t i = 0; i < tabs.size(); i++) {
            bool  active = i == active_tab;
            if (active) {
                auto activebrush = CreateSolidBrush(active_bg);
                FillRect(bufdc, &tabrect, activebrush);
                DeleteObject(activebrush);
            } else if (tabs[i].info.attention) {
                auto activebrush = CreateSolidBrush(attention_bg);
                FillRect(bufdc, &tabrect, activebrush);
                DeleteObject(activebrush);
            } else {
                FillRect(bufdc, &tabrect, brush);
            }

            if (active) {
                auto _f = SelectWObj(bufdc, new_active_tab_font());
                paint_tab(bufdc, tabwidth, loc_tabheight, tabs[i]);
            } else {
                MoveToEx(bufdc, 0, loc_tabheight, nullptr);
                LineTo(bufdc, tabwidth, loc_tabheight);
                paint_tab(bufdc, tabwidth, loc_tabheight, tabs[i]);
            }

            BitBlt(dc, i*tabwidth+PADDING, PADDING, tabwidth, tabheight(),
                    bufdc, 0, 0, SRCCOPY);
        }

        if (true || ((int)tabs.size() * tabwidth < width)/*always true*/) {
            SetRect(&tabrect, 0, 0, width - (tabs.size() * tabwidth), loc_tabheight+1);
            auto obrush = SelectWObj(bufdc, brush);
            auto obuf = SelectWObj(bufdc, CreateCompatibleBitmap(dc, width - (tabs.size() * tabwidth), tabheight()));
            FillRect(bufdc, &tabrect, brush);
            MoveToEx(bufdc, 0, 0, nullptr);
            LineTo(bufdc, 0, loc_tabheight);
            LineTo(bufdc, width - (tabs.size() * tabwidth), loc_tabheight);

            // add button
            const int size = loc_tabheight * 0.5;
            const int margin = (loc_tabheight - size)/2;
            MoveToEx(bufdc, ADD_BUTTON_PADDING * g_xscale + margin + size/2, margin, nullptr);
            LineTo(bufdc, ADD_BUTTON_PADDING * g_xscale+ margin + size/2, margin + size);
            MoveToEx(bufdc, ADD_BUTTON_PADDING * g_xscale+ margin, margin + size/2, nullptr);
            LineTo(bufdc, ADD_BUTTON_PADDING * g_xscale + margin + size, margin + size/2);
                
            //TextOutW(dc, tab_font_size() / 2 + CLOSE_BUTTON_PADDING * g_xscale, (loc_tabheight - tab_font_size()) / 2, L"+", 1);
            BitBlt(dc, tabs.size()*tabwidth+PADDING, PADDING, width - (tabs.size() * tabwidth), tabheight(),
                    bufdc, 0, 0, SRCCOPY);
        }
    }
    DeleteDC(bufdc);
}

void win_for_each_term(void (*cb)(struct term* term)) {
    for (Tab& tab : tabs)
        cb(tab.terminal.get());
}

void win_tab_mouse_click(bool down, int x) {
    unsigned int tab = x / tab_paint_width;
    if (tab >= tabs.size()) {
        if (!down) {
            bool click_add = x < (tab_paint_width * tabs.size() + (ADD_BUTTON_PADDING + 18)* g_xscale);
            if (click_add)
                win_tab_create();
        }
        return;
    }

    set_active_tab(tab);
    
    if (down) {
        bool click_close = x > (tab_paint_width * (tab + 1) - tab_font_size() - CLOSE_BUTTON_PADDING * g_xscale);
        if (click_close)
            win_tab_close();
    }
}
    
}

std::vector<Tab>& win_tabs() {
    return tabs;
}

static void lambda_callback(void* data) {
    auto callback = static_cast<std::function<void()>*>(data);
    (*callback)();
    delete callback;
}
void win_callback(unsigned int ticks, std::function<void()> callback) {
    win_set_timer(lambda_callback, new std::function<void()>(callback), ticks);
}
