#include <Windows.h>

static HANDLE g_layoutEvent = nullptr;

static HANDLE GetLayoutEvent()
{
    if (!g_layoutEvent)
    {
        g_layoutEvent =
            OpenEventW(
                EVENT_MODIFY_STATE,
                FALSE,
                L"Local\\IMESteady.LayoutChanged"
            );
    }

    return g_layoutEvent;
}

extern "C"
__declspec(dllexport)
LRESULT CALLBACK ImeSteadyCallWndProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const auto* info =
            reinterpret_cast<const CWPSTRUCT*>(
                lParam
            );

        // This message is delivered AFTER the input
        // language of the affected application changed.
        if (info &&
            info->message == WM_INPUTLANGCHANGE)
        {
            HANDLE event =
                GetLayoutEvent();

            if (event)
                SetEvent(event);
        }
    }

    return CallNextHookEx(
        nullptr,
        nCode,
        wParam,
        lParam
    );
}

BOOL WINAPI DllMain(
    HINSTANCE,
    DWORD reason,
    LPVOID)
{
    if (reason == DLL_PROCESS_DETACH)
    {
        if (g_layoutEvent)
        {
            CloseHandle(
                g_layoutEvent
            );

            g_layoutEvent = nullptr;
        }
    }

    return TRUE;
}