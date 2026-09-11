#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#define ID_TRAY 1001
#define ID_TIMER 1002
#define WM_TRAY (WM_APP + 1)
#define IDM_SETTINGS 2001
#define IDM_LOCK 2002
#define IDM_CLICK 2003
#define IDM_QUIT 2004
#define IDM_FONT_INCREASE 2005
#define IDM_FONT_DECREASE 2006

static HWND g_hwnd;
static BOOL g_locked = FALSE, g_clickThrough = TRUE, g_dragging = FALSE;
static int g_fontSize = 16;
static POINT g_dragStart, g_windowStart;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarCreated;

static void GetConfigPath(wchar_t *path)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash;
    if (!n || n >= MAX_PATH)
    {
        lstrcpyW(path, L"TimeWidget.ini");
        return;
    }
    slash = wcsrchr(exe, L'\\');
    if (slash)
        *(slash + 1) = L'\0';
    else
        exe[0] = L'\0';
    /* Required for old MinGW/MSVCRT: swprintf(buffer, format, ...). */
    swprintf(path, L"%sTimeWidget.ini", exe);
}

static void SavePosition(void)
{
    wchar_t path[MAX_PATH], text[32];
    RECT r;
    if (!g_hwnd || !GetWindowRect(g_hwnd, &r))
        return;
    GetConfigPath(path);
    swprintf(text, L"%ld", (long)r.left);
    WritePrivateProfileStringW(L"Position", L"X", text, path);
    swprintf(text, L"%ld", (long)r.top);
    WritePrivateProfileStringW(L"Position", L"Y", text, path);
    WritePrivateProfileStringW(L"State", L"Locked", g_locked ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"State", L"ClickThrough", g_clickThrough ? L"1" : L"0", path);
    swprintf(text, L"%d", g_fontSize);
    WritePrivateProfileStringW(L"State", L"FontSize", text, path);
}

static void LoadSettingsAndPosition(void)
{
    wchar_t path[MAX_PATH];
    GetConfigPath(path);
    g_locked = GetPrivateProfileIntW(L"State", L"Locked", 0, path) != 0;
    g_clickThrough = GetPrivateProfileIntW(L"State", L"ClickThrough", 1, path) != 0;
    g_fontSize = (int)GetPrivateProfileIntW(L"State", L"FontSize", 16, path);
    if (g_fontSize < 10) g_fontSize = 10;
    if (g_fontSize > 48) g_fontSize = 48;
    SetWindowPos(g_hwnd, HWND_TOPMOST,
                 (int)GetPrivateProfileIntW(L"Position", L"X", 50, path),
                 (int)GetPrivateProfileIntW(L"Position", L"Y", 50, path),
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ApplyClickThrough(void)
{
    LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (g_clickThrough)
        style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else
        style &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, style);
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void AddTrayIcon(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Time Widget");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void RemoveTrayIcon(void)
{
    if (g_nid.cbSize)
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT p;
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    GetCursorPos(&p);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_STRING | (g_fontSize >= 48 ? MF_GRAYED : 0), IDM_FONT_INCREASE, L"Increase Font Size");
    AppendMenuW(menu, MF_STRING | (g_fontSize <= 10 ? MF_GRAYED : 0), IDM_FONT_DECREASE, L"Decrease Font Size");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_locked ? MF_CHECKED : 0), IDM_LOCK, L"Lock Position");
    AppendMenuW(menu, MF_STRING | (g_clickThrough ? MF_CHECKED : 0), IDM_CLICK, L"Click Through");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* Uses per-pixel alpha: zero-alpha background, 80%-opaque clock, including black outline. */
static void RenderClock(void)
{
    const int width = g_fontSize * 4 + 20, height = g_fontSize + 20;
    BITMAPINFO bi;
    HDC screen, mem;
    HBITMAP bmp, oldBmp;
    HFONT font, oldFont;
    void *bits;
    DWORD *pixels;
    SYSTEMTIME st;
    wchar_t time[16];
    SIZE size = {width, height};
    POINT src = {0, 0};
    BLENDFUNCTION blend;
    int i;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!mem || !bmp)
    {
        if (bmp)
            DeleteObject(bmp);
        if (mem)
            DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return;
    }
    oldBmp = (HBITMAP)SelectObject(mem, bmp);
    ZeroMemory(bits, width * height * 4);
    GetLocalTime(&st);
    swprintf(time, L"%02u:%02u", (unsigned int)st.wHour, (unsigned int)st.wMinute);
    SetBkMode(mem, TRANSPARENT);
    font = CreateFontW(-g_fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    oldFont = (HFONT)SelectObject(mem, font);
    SetTextAlign(mem, TA_CENTER | TA_TOP);
    SetTextColor(mem, RGB(1, 1, 1));
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            if (dx || dy)
                TextOutW(mem, width / 2 + dx, 7 + dy, time, lstrlenW(time));
    SetTextColor(mem, RGB(180, 150, 20));
    TextOutW(mem, width / 2, 7, time, lstrlenW(time));
    SelectObject(mem, oldFont);
    DeleteObject(font);
    pixels = (DWORD *)bits;
    for (i = 0; i < width * height; ++i)
        if (pixels[i] & 0x00FFFFFFUL)
            pixels[i] = ((pixels[i] & 0x00FFFFFFUL) == 0x00010101UL) ? 0xCC000000UL : (pixels[i] | 0xCC000000UL);
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, screen, NULL, &size, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated)
    {
        AddTrayIcon();
        return 0;
    }
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, ID_TIMER, 1000, NULL);
        return 0;
    case WM_TIMER:
        RenderClock();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        RenderClock();
        return 0;
    }
    case WM_NCHITTEST:
        return g_clickThrough ? HTTRANSPARENT : HTCLIENT;
    case WM_LBUTTONDOWN:
        if (!g_locked && !g_clickThrough)
        {
            RECT r;
            GetCursorPos(&g_dragStart);
            GetWindowRect(hwnd, &r);
            g_windowStart.x = r.left;
            g_windowStart.y = r.top;
            g_dragging = TRUE;
            SetCapture(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging)
        {
            POINT p;
            GetCursorPos(&p);
            SetWindowPos(hwnd, HWND_TOPMOST, g_windowStart.x + p.x - g_dragStart.x, g_windowStart.y + p.y - g_dragStart.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging)
        {
            g_dragging = FALSE;
            ReleaseCapture();
            SavePosition();
        }
        return 0;
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU || lp == WM_LBUTTONDBLCLK)
            ShowTrayMenu(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_SETTINGS:
            MessageBoxW(hwnd, L"To move the widget, uncheck Click Through and Lock Position, then drag the clock. Its position is saved automatically.", L"Time Widget Settings", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_LOCK:
            g_locked = !g_locked;
            SavePosition();
            break;
        case IDM_CLICK:
            g_clickThrough = !g_clickThrough;
            ApplyClickThrough();
            SavePosition();
            break;
        case IDM_FONT_INCREASE:
            if (g_fontSize < 48) { g_fontSize += 2; RenderClock(); SavePosition(); }
            break;
        case IDM_FONT_DECREASE:
            if (g_fontSize > 10) { g_fontSize -= 2; RenderClock(); SavePosition(); }
            break;
        case IDM_QUIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        SavePosition();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* WinMain (rather than wWinMain) deliberately matches the requested -mwindows
   MinGW link command, which does not pass -municode. */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    WNDCLASSW wc;
    MSG msg;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"TimeWidgetClass";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc))
        return 1;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                             L"Time Widget", WS_POPUP, 50, 50, 108, 32, NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;
    LoadSettingsAndPosition();
    ApplyClickThrough();
    AddTrayIcon();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    RenderClock();
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
