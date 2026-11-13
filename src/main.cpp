#include <Windows.h>
#include <Psapi.h>

#include <cstdint>
#include <cstring>

#include "MinHook.h"

#pragma comment(lib, "Psapi.lib")

// ============================================================
// Shared state
// ============================================================

struct SharedImeState
{
    volatile LONG ready;

    // Semantic state exposed to indicator:
    //
    //   0 = Microsoft Pinyin English/alphanumeric mode
    //   1 = Microsoft Pinyin Chinese/native mode
    //
    volatile LONG mode;

    // 1 = hook installed successfully
    // negative = initialization error
    volatile LONG hookStatus;

    volatile LONG updates;
};

static HANDLE g_mapping = nullptr;
static SharedImeState* g_shared = nullptr;

static HANDLE g_changeEvent = nullptr;

// ============================================================
// InputService hook
// ============================================================

using OnConversionModeSetFn =
    HRESULT(WINAPI*)(void* This, DWORD mode);

static OnConversionModeSetFn
    Original_OnConversionModeSet = nullptr;

// ============================================================
// Shared state
// ============================================================

static void SetHookStatus(LONG status)
{
    if (g_shared)
    {
        InterlockedExchange(
            &g_shared->hookStatus,
            status
        );
    }
}

static void PublishMode(DWORD rawMode)
{
    if (!g_shared)
        return;

    // InputService writer discovered by reverse engineering:
    //
    //   inc dword ptr [rcx+80]
    //   mov eax, edx
    //   mov dword ptr [rcx+C8], edx
    //
    // Therefore EDX itself is the conversion-mode DWORD.
    //
    // Observed:
    //
    //   English:
    //       rawMode = 0x00000000
    //
    //   Chinese:
    //       rawMode = 0x00000401
    //
    // Bit 0 = native mode.
    constexpr DWORD kNativeMode = 0x00000001;

    const LONG semanticMode =
        (rawMode & kNativeMode)
            ? 1
            : 0;

    InterlockedExchange(
        &g_shared->mode,
        semanticMode
    );

    InterlockedExchange(
        &g_shared->ready,
        1
    );

    InterlockedIncrement(
        &g_shared->updates
    );

    if (g_changeEvent)
    {
        SetEvent(
            g_changeEvent
        );
    }
}

// ============================================================
// Hook
// ============================================================

static HRESULT WINAPI Hook_OnConversionModeSet(
    void* This,
    DWORD mode)
{
    const HRESULT hr =
        Original_OnConversionModeSet(
            This,
            mode
        );

    PublishMode(mode);

    return hr;
}

// ============================================================
// Pattern scanner
// ============================================================

static BYTE* FindPattern(
    BYTE* base,
    SIZE_T size,
    const BYTE* pattern,
    SIZE_T patternSize)
{
    if (!base ||
        !pattern ||
        patternSize == 0 ||
        size < patternSize)
    {
        return nullptr;
    }

    const SIZE_T last =
        size - patternSize;

    for (SIZE_T i = 0; i <= last; ++i)
    {
        if (std::memcmp(
                base + i,
                pattern,
                patternSize) == 0)
        {
            return base + i;
        }
    }

    return nullptr;
}

// ============================================================
// IPC
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

    // DLL is the authoritative producer.
    //
    // Reset even if the indicator kept the mapping alive
    // across a ctfmon restart.
    InterlockedExchange(
        &g_shared->ready,
        0
    );

    InterlockedExchange(
        &g_shared->mode,
        -1
    );

    InterlockedExchange(
        &g_shared->hookStatus,
        0
    );

    InterlockedExchange(
        &g_shared->updates,
        0
    );

    g_changeEvent =
        CreateEventW(
            nullptr,
            FALSE,  // auto-reset
            FALSE,
            L"Local\\IMESteady.ModeChanged"
        );

    return g_changeEvent != nullptr;
}

// ============================================================
// Initialization worker
// ============================================================

static DWORD WINAPI WorkerThread(
    LPVOID)
{
    if (!InitializeSharedObjects())
        return 1;

    HMODULE inputService =
        GetModuleHandleW(
            L"InputService.dll"
        );

    if (!inputService)
    {
        SetHookStatus(-1);
        return 2;
    }

    MODULEINFO info{};

    if (!GetModuleInformation(
            GetCurrentProcess(),
            inputService,
            &info,
            sizeof(info)))
    {
        SetHookStatus(-2);
        return 3;
    }

    // Function entry discovered in x64dbg:
    //
    // FF 81 80 00 00 00
    //     inc dword ptr [rcx+80]
    //
    // 8B C2
    //     mov eax,edx
    //
    static constexpr BYTE pattern[] =
    {
        0xFF, 0x81,
        0x80, 0x00, 0x00, 0x00,
        0x8B, 0xC2
    };

    BYTE* target =
        FindPattern(
            reinterpret_cast<BYTE*>(
                info.lpBaseOfDll
            ),
            static_cast<SIZE_T>(
                info.SizeOfImage
            ),
            pattern,
            sizeof(pattern)
        );

    if (!target)
    {
        SetHookStatus(-3);
        return 4;
    }

    const MH_STATUS initResult =
        MH_Initialize();

    if (initResult != MH_OK &&
        initResult != MH_ERROR_ALREADY_INITIALIZED)
    {
        SetHookStatus(-4);
        return 5;
    }

    const MH_STATUS createResult =
        MH_CreateHook(
            target,
            &Hook_OnConversionModeSet,
            reinterpret_cast<void**>(
                &Original_OnConversionModeSet
            )
        );

    if (createResult != MH_OK)
    {
        SetHookStatus(-5);
        return 6;
    }

    const MH_STATUS enableResult =
        MH_EnableHook(target);

    if (enableResult != MH_OK)
    {
        SetHookStatus(-6);
        return 7;
    }

    SetHookStatus(1);

    // No polling loop.
    //
    // From now on MinHook invokes us only when
    // InputService actually changes conversion mode.
    return 0;
}

// ============================================================
// DLL entry
// ============================================================

BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID)
{
    if (reason ==
        DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(
            instance
        );

        HANDLE thread =
            CreateThread(
                nullptr,
                0,
                WorkerThread,
                nullptr,
                0,
                nullptr
            );

        if (thread)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}