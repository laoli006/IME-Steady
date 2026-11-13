#define NOMINMAX

#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Shell32.lib")

// ============================================================
// Messages
// ============================================================

constexpr UINT WM_TRAYICON =
    WM_APP + 1;

constexpr UINT WM_IME_CHANGED =
    WM_APP + 2;

constexpr UINT WM_LAYOUT_CHANGED =
    WM_APP + 3;

constexpr UINT WM_FOREGROUND_CHANGED =
    WM_APP + 4;

constexpr UINT ID_TRAY_EXIT =
    1001;

// ============================================================
// Shared state
// ============================================================

struct SharedImeState
{
    volatile LONG ready;
    volatile LONG mode;
    volatile LONG hookStatus;
    volatile LONG updates;
};

static HANDLE g_mapping = nullptr;
static SharedImeState* g_shared = nullptr;

static HANDLE g_imeEvent = nullptr;
static HANDLE g_layoutEvent = nullptr;
static HANDLE g_stopEvent = nullptr;

static HANDLE g_eventThread = nullptr;

// ============================================================
// Window / tray
// ============================================================

static HWND g_hwnd = nullptr;

static NOTIFYICONDATAW g_nid{};

static bool g_trayAdded = false;

static UINT g_taskbarCreated = 0;

// ============================================================
// Hooks
// ============================================================

static HWINEVENTHOOK
    g_foregroundHook = nullptr;

static HMODULE
    g_langHookModule = nullptr;

static HHOOK
    g_callWndHook = nullptr;

// ============================================================
// Indicator state
// ============================================================

enum class IndicatorState
{
    Unknown,
    EnglishUS,
    PinyinChinese,
    PinyinEnglish
};

static IndicatorState g_lastState =
    IndicatorState::Unknown;

static UINT g_lastIconSize = 0;

// ============================================================
// Shared objects
// ============================================================

static bool InitializeSharedObjects()
{
    g_mapping =
        CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(SharedImeState),
            L"Local\\IMESteady.State"
        );

    if (!g_mapping)
        return false;

    const bool newMapping =
        GetLastError() !=
        ERROR_ALREADY_EXISTS;

    g_shared =
        reinterpret_cast<SharedImeState*>(
            MapViewOfFile(
                g_mapping,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                sizeof(SharedImeState)
            )
        );

    if (!g_shared)
        return false;

    if (newMapping)
    {
        g_shared->ready = 0;
        g_shared->mode = -1;
        g_shared->hookStatus = 0;
        g_shared->updates = 0;
    }

    g_imeEvent =
        CreateEventW(
            nullptr,
            FALSE,
            FALSE,
            L"Local\\IMESteady.ModeChanged"
        );

    g_layoutEvent =
        CreateEventW(
            nullptr,
            FALSE,
            FALSE,
            L"Local\\IMESteady.LayoutChanged"
        );

    g_stopEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr
        );

    return
        g_imeEvent &&
        g_layoutEvent &&
        g_stopEvent;
}

// ============================================================
// Current foreground HKL
// ============================================================

static LANGID GetForegroundLanguage()
{
    HWND foreground =
        GetForegroundWindow();

    if (!foreground)
        return 0;

    DWORD threadId =
        GetWindowThreadProcessId(
            foreground,
            nullptr
        );

    if (!threadId)
        return 0;

    HKL layout =
        GetKeyboardLayout(
            threadId
        );

    return LOWORD(
        reinterpret_cast<ULONG_PTR>(
            layout
        )
    );
}

static IndicatorState GetIndicatorState()
{
    const LANGID lang =
        GetForegroundLanguage();

    if (lang == 0x0409)
    {
        return
            IndicatorState::EnglishUS;
    }

    if (lang == 0x0804)
    {
        if (!g_shared)
            return IndicatorState::Unknown;

        const LONG ready =
            InterlockedCompareExchange(
                &g_shared->ready,
                0,
                0
            );

        if (!ready)
            return IndicatorState::Unknown;

        const LONG mode =
            InterlockedCompareExchange(
                &g_shared->mode,
                0,
                0
            );

        if (mode == 1)
            return IndicatorState::PinyinChinese;

        if (mode == 0)
            return IndicatorState::PinyinEnglish;
    }

    return IndicatorState::Unknown;
}

// ============================================================
// DPI
// ============================================================

static UINT GetTrayDpi()
{
    HWND taskbar =
        FindWindowW(
            L"Shell_TrayWnd",
            nullptr
        );

    if (!taskbar)
        return 96;

    UINT dpi =
        GetDpiForWindow(
            taskbar
        );

    return dpi ? dpi : 96;
}

static UINT GetTrayIconSize()
{
    const UINT dpi =
        GetTrayDpi();

    int size =
        GetSystemMetricsForDpi(
            SM_CXSMICON,
            dpi
        );

    if (size <= 0)
    {
        size =
            MulDiv(
                16,
                static_cast<int>(dpi),
                96
            );
    }

    return static_cast<UINT>(
        std::max(size, 16)
    );
}

// ============================================================
// Text
// ============================================================

static const wchar_t*
GetStateText(
    IndicatorState state)
{
    switch (state)
    {
    case IndicatorState::EnglishUS:
        return L"EN";

    case IndicatorState::PinyinChinese:
        return L"\u4E2D";

    case IndicatorState::PinyinEnglish:
        return L"\u82F1";

    default:
        return L"?";
    }
}

// ============================================================
// Icon renderer
// ============================================================

static HICON CreateTextIcon(
    IndicatorState state,
    UINT size)
{
    // --------------------------------------------------------
    // Grayscale source surface
    // --------------------------------------------------------

    BITMAPINFO srcInfo{};

    srcInfo.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);

    srcInfo.bmiHeader.biWidth =
        static_cast<LONG>(size);

    srcInfo.bmiHeader.biHeight =
        -static_cast<LONG>(size);

    srcInfo.bmiHeader.biPlanes = 1;
    srcInfo.bmiHeader.biBitCount = 32;
    srcInfo.bmiHeader.biCompression = BI_RGB;

    void* srcBits = nullptr;

    HDC screen =
        GetDC(nullptr);

    HBITMAP srcBitmap =
        CreateDIBSection(
            screen,
            &srcInfo,
            DIB_RGB_COLORS,
            &srcBits,
            nullptr,
            0
        );

    ReleaseDC(
        nullptr,
        screen
    );

    if (!srcBitmap ||
        !srcBits)
    {
        if (srcBitmap)
            DeleteObject(srcBitmap);

        return nullptr;
    }

    const SIZE_T pixelCount =
        static_cast<SIZE_T>(size) *
        static_cast<SIZE_T>(size);

    std::memset(
        srcBits,
        0,
        pixelCount *
            sizeof(std::uint32_t)
    );

    HDC dc =
        CreateCompatibleDC(nullptr);

    if (!dc)
    {
        DeleteObject(srcBitmap);
        return nullptr;
    }

    HGDIOBJ oldBitmap =
        SelectObject(
            dc,
            srcBitmap
        );

    RECT rect
    {
        0,
        0,
        static_cast<LONG>(size),
        static_cast<LONG>(size)
    };

    FillRect(
        dc,
        &rect,
        static_cast<HBRUSH>(
            GetStockObject(
                BLACK_BRUSH
            )
        )
    );

    SetBkMode(
        dc,
        TRANSPARENT
    );

    SetTextColor(
        dc,
        RGB(255, 255, 255)
    );

    // --------------------------------------------------------
    // Font
    // --------------------------------------------------------

    const UINT dpi =
        GetTrayDpi();

    int logicalHeight = 13;
    const wchar_t* fontName =
        L"Microsoft YaHei UI";

    switch (state)
    {
    case IndicatorState::EnglishUS:

        logicalHeight = 12;
        fontName = L"Segoe UI";
        break;

    case IndicatorState::PinyinChinese:
    case IndicatorState::PinyinEnglish:

        logicalHeight = 13;
        fontName =
            L"Microsoft YaHei UI";
        break;

    default:

        logicalHeight = 11;
        fontName = L"Segoe UI";
        break;
    }

    const int fontHeight =
        -MulDiv(
            logicalHeight,
            static_cast<int>(dpi),
            96
        );

    HFONT font =
        CreateFontW(
            fontHeight,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            fontName
        );

    if (!font)
    {
        SelectObject(
            dc,
            oldBitmap
        );

        DeleteDC(dc);
        DeleteObject(srcBitmap);

        return nullptr;
    }

    HGDIOBJ oldFont =
        SelectObject(
            dc,
            font
        );

    DrawTextW(
        dc,
        GetStateText(state),
        -1,
        &rect,
        DT_CENTER |
            DT_VCENTER |
            DT_SINGLELINE |
            DT_NOPREFIX
    );

    SelectObject(
        dc,
        oldFont
    );

    DeleteObject(font);

    SelectObject(
        dc,
        oldBitmap
    );

    DeleteDC(dc);

    // --------------------------------------------------------
    // ARGB output surface
    // --------------------------------------------------------

    BITMAPV5HEADER dstInfo{};

    dstInfo.bV5Size =
        sizeof(BITMAPV5HEADER);

    dstInfo.bV5Width =
        static_cast<LONG>(size);

    dstInfo.bV5Height =
        -static_cast<LONG>(size);

    dstInfo.bV5Planes = 1;
    dstInfo.bV5BitCount = 32;

    dstInfo.bV5Compression =
        BI_BITFIELDS;

    dstInfo.bV5RedMask =
        0x00FF0000;

    dstInfo.bV5GreenMask =
        0x0000FF00;

    dstInfo.bV5BlueMask =
        0x000000FF;

    dstInfo.bV5AlphaMask =
        0xFF000000;

    void* dstBits = nullptr;

    screen =
        GetDC(nullptr);

    HBITMAP dstBitmap =
        CreateDIBSection(
            screen,
            reinterpret_cast<BITMAPINFO*>(
                &dstInfo
            ),
            DIB_RGB_COLORS,
            &dstBits,
            nullptr,
            0
        );

    ReleaseDC(
        nullptr,
        screen
    );

    if (!dstBitmap ||
        !dstBits)
    {
        DeleteObject(srcBitmap);

        if (dstBitmap)
            DeleteObject(dstBitmap);

        return nullptr;
    }

    auto* src =
        reinterpret_cast<std::uint32_t*>(
            srcBits
        );

    auto* dst =
        reinterpret_cast<std::uint32_t*>(
            dstBits
        );

    for (SIZE_T i = 0;
         i < pixelCount;
         ++i)
    {
        const std::uint32_t p =
            src[i];

        const BYTE b =
            static_cast<BYTE>(
                p & 0xFF
            );

        const BYTE g =
            static_cast<BYTE>(
                (p >> 8) & 0xFF
            );

        const BYTE r =
            static_cast<BYTE>(
                (p >> 16) & 0xFF
            );

        const BYTE alpha =
            static_cast<BYTE>(
                (
                    static_cast<unsigned>(r) +
                    static_cast<unsigned>(g) +
                    static_cast<unsigned>(b)
                ) / 3
            );

        if (alpha == 0)
        {
            dst[i] = 0;
        }
        else
        {
            dst[i] =
                (
                    static_cast<std::uint32_t>(
                        alpha
                    ) << 24
                ) |
                0x00FFFFFF;
        }
    }

    HBITMAP monoMask =
        CreateBitmap(
            size,
            size,
            1,
            1,
            nullptr
        );

    if (!monoMask)
    {
        DeleteObject(dstBitmap);
        DeleteObject(srcBitmap);
        return nullptr;
    }

    HDC maskDC =
        CreateCompatibleDC(nullptr);

    if (maskDC)
    {
        HGDIOBJ oldMask =
            SelectObject(
                maskDC,
                monoMask
            );

        PatBlt(
            maskDC,
            0,
            0,
            size,
            size,
            BLACKNESS
        );

        SelectObject(
            maskDC,
            oldMask
        );

        DeleteDC(maskDC);
    }

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = dstBitmap;
    iconInfo.hbmMask = monoMask;

    HICON icon =
        CreateIconIndirect(
            &iconInfo
        );

    DeleteObject(monoMask);
    DeleteObject(dstBitmap);
    DeleteObject(srcBitmap);

    return icon;
}

// ============================================================
// Tray
// ============================================================

static void AddTrayIcon()
{
    const IndicatorState state =
        GetIndicatorState();

    const UINT iconSize =
        GetTrayIconSize();

    HICON icon =
        CreateTextIcon(
            state,
            iconSize
        );

    ZeroMemory(
        &g_nid,
        sizeof(g_nid)
    );

    g_nid.cbSize =
        sizeof(g_nid);

    g_nid.hWnd =
        g_hwnd;

    g_nid.uID = 1;

    g_nid.uFlags =
        NIF_MESSAGE |
        NIF_ICON |
        NIF_TIP |
        NIF_SHOWTIP;

    g_nid.uCallbackMessage =
        WM_TRAYICON;

    g_nid.hIcon =
        icon;

    wcscpy_s(
        g_nid.szTip,
        L"IME Steady"
    );

    if (Shell_NotifyIconW(
            NIM_ADD,
            &g_nid))
    {
        g_trayAdded = true;

        g_nid.uVersion =
            NOTIFYICON_VERSION_4;

        Shell_NotifyIconW(
            NIM_SETVERSION,
            &g_nid
        );
    }

    if (icon)
        DestroyIcon(icon);

    g_lastState = state;
    g_lastIconSize = iconSize;
}

static void UpdateTrayIcon(
    bool force = false)
{
    const IndicatorState state =
        GetIndicatorState();

    const UINT iconSize =
        GetTrayIconSize();

    if (!force &&
        state == g_lastState &&
        iconSize == g_lastIconSize)
    {
        return;
    }

    if (!g_trayAdded)
    {
        AddTrayIcon();
        return;
    }

    HICON icon =
        CreateTextIcon(
            state,
            iconSize
        );

    if (!icon)
        return;

    g_nid.uFlags =
        NIF_ICON;

    g_nid.hIcon =
        icon;

    Shell_NotifyIconW(
        NIM_MODIFY,
        &g_nid
    );

    DestroyIcon(icon);

    g_lastState = state;
    g_lastIconSize = iconSize;
}

static void RemoveTrayIcon()
{
    if (!g_trayAdded)
        return;

    Shell_NotifyIconW(
        NIM_DELETE,
        &g_nid
    );

    g_trayAdded = false;
}

// ============================================================
// IPC event listener
//
// Pure blocking wait.
// No timer.
// No polling.
// ============================================================

static DWORD WINAPI EventThreadProc(
    LPVOID)
{
    HANDLE handles[3]
    {
        g_imeEvent,
        g_layoutEvent,
        g_stopEvent
    };

    while (true)
    {
        const DWORD result =
            WaitForMultipleObjects(
                3,
                handles,
                FALSE,
                INFINITE
            );

        if (result ==
            WAIT_OBJECT_0)
        {
            PostMessageW(
                g_hwnd,
                WM_IME_CHANGED,
                0,
                0
            );
        }
        else if (result ==
                 WAIT_OBJECT_0 + 1)
        {
            PostMessageW(
                g_hwnd,
                WM_LAYOUT_CHANGED,
                0,
                0
            );
        }
        else
        {
            break;
        }
    }

    return 0;
}

// ============================================================
// Foreground window change
// ============================================================

static void CALLBACK ForegroundEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND,
    LONG,
    LONG,
    DWORD,
    DWORD)
{
    if (event !=
        EVENT_SYSTEM_FOREGROUND)
    {
        return;
    }

    if (g_hwnd)
    {
        PostMessageW(
            g_hwnd,
            WM_FOREGROUND_CHANGED,
            0,
            0
        );
    }
}

// ============================================================
// Install WM_INPUTLANGCHANGE hook
// ============================================================

static bool InstallLanguageHook()
{
    wchar_t path[MAX_PATH]{};

    DWORD length =
        GetModuleFileNameW(
            nullptr,
            path,
            MAX_PATH
        );

    if (!length ||
        length >= MAX_PATH)
    {
        return false;
    }

    wchar_t* slash =
        wcsrchr(
            path,
            L'\\'
        );

    if (!slash)
        return false;

    *(slash + 1) = L'\0';

    wcscat_s(
        path,
        L"langhook64.dll"
    );

    g_langHookModule =
        LoadLibraryW(path);

    if (!g_langHookModule)
        return false;

    HOOKPROC proc =
        reinterpret_cast<HOOKPROC>(
            GetProcAddress(
                g_langHookModule,
                "ImeSteadyCallWndProc"
            )
        );

    if (!proc)
        return false;

    g_callWndHook =
        SetWindowsHookExW(
            WH_CALLWNDPROC,
            proc,
            g_langHookModule,
            0
        );

    return g_callWndHook != nullptr;
}

// ============================================================
// Context menu
// ============================================================

static void ShowTrayMenu()
{
    POINT pt{};

    GetCursorPos(
        &pt
    );

    HMENU menu =
        CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING,
        ID_TRAY_EXIT,
        L"Exit"
    );

    SetForegroundWindow(
        g_hwnd
    );

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON |
            TPM_BOTTOMALIGN |
            TPM_LEFTALIGN,
        pt.x,
        pt.y,
        0,
        g_hwnd,
        nullptr
    );

    DestroyMenu(menu);
}

// ============================================================
// Window proc
// ============================================================

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message ==
        g_taskbarCreated)
    {
        g_trayAdded = false;

        AddTrayIcon();

        return 0;
    }

    switch (message)
    {
    case WM_IME_CHANGED:

        UpdateTrayIcon();
        return 0;

    case WM_LAYOUT_CHANGED:

        // WM_INPUTLANGCHANGE is already post-change,
        // so one direct read is enough.
        UpdateTrayIcon(true);
        return 0;

    case WM_FOREGROUND_CHANGED:

        UpdateTrayIcon(true);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED:

        UpdateTrayIcon(true);
        return 0;

    case WM_COMMAND:

        if (LOWORD(wParam) ==
            ID_TRAY_EXIT)
        {
            DestroyWindow(hwnd);
            return 0;
        }

        break;

    case WM_TRAYICON:
    {
        const UINT event =
            LOWORD(lParam);

        if (event ==
                WM_CONTEXTMENU ||
            event ==
                WM_RBUTTONUP)
        {
            ShowTrayMenu();
            return 0;
        }

        break;
    }

    case WM_DESTROY:

        if (g_stopEvent)
            SetEvent(g_stopEvent);

        RemoveTrayIcon();

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}

// ============================================================
// WinMain
// ============================================================

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    );

    if (!InitializeSharedObjects())
        return 1;

    g_taskbarCreated =
        RegisterWindowMessageW(
            L"TaskbarCreated"
        );

    constexpr wchar_t className[] =
        L"IMESteadyWindow";

    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.lpfnWndProc =
        WindowProc;

    wc.hInstance =
        instance;

    wc.lpszClassName =
        className;

    if (!RegisterClassExW(&wc))
        return 2;

    g_hwnd =
        CreateWindowExW(
            WS_EX_TOOLWINDOW |
                WS_EX_NOACTIVATE,

            className,
            L"IME Steady",

            WS_POPUP,

            0,
            0,
            0,
            0,

            nullptr,
            nullptr,
            instance,
            nullptr
        );

    if (!g_hwnd)
        return 3;

    // Exact post-layout-change notification.
    if (!InstallLanguageHook())
    {
        MessageBoxW(
            nullptr,
            L"Failed to install language hook.",
            L"IME Steady",
            MB_OK |
            MB_ICONERROR
        );

        return 4;
    }

    // Foreground thread may have its own HKL.
    g_foregroundHook =
        SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            ForegroundEventProc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT |
                WINEVENT_SKIPOWNPROCESS
        );

    AddTrayIcon();

    g_eventThread =
        CreateThread(
            nullptr,
            0,
            EventThreadProc,
            nullptr,
            0,
            nullptr
        );

    MSG msg{};

    while (GetMessageW(
               &msg,
               nullptr,
               0,
               0) > 0)
    {
        TranslateMessage(
            &msg
        );

        DispatchMessageW(
            &msg
        );
    }

    // ========================================================
    // Cleanup
    // ========================================================

    if (g_callWndHook)
    {
        UnhookWindowsHookEx(
            g_callWndHook
        );

        g_callWndHook = nullptr;
    }

    if (g_foregroundHook)
    {
        UnhookWinEvent(
            g_foregroundHook
        );

        g_foregroundHook = nullptr;
    }

    if (g_stopEvent)
    {
        SetEvent(
            g_stopEvent
        );
    }

    if (g_eventThread)
    {
        WaitForSingleObject(
            g_eventThread,
            INFINITE
        );

        CloseHandle(
            g_eventThread
        );

        g_eventThread = nullptr;
    }

    if (g_langHookModule)
    {
        FreeLibrary(
            g_langHookModule
        );

        g_langHookModule = nullptr;
    }

    if (g_shared)
    {
        UnmapViewOfFile(
            g_shared
        );

        g_shared = nullptr;
    }

    if (g_mapping)
    {
        CloseHandle(
            g_mapping
        );

        g_mapping = nullptr;
    }

    if (g_imeEvent)
    {
        CloseHandle(
            g_imeEvent
        );

        g_imeEvent = nullptr;
    }

    if (g_layoutEvent)
    {
        CloseHandle(
            g_layoutEvent
        );

        g_layoutEvent = nullptr;
    }

    if (g_stopEvent)
    {
        CloseHandle(
            g_stopEvent
        );

        g_stopEvent = nullptr;
    }

    return 0;
}