#include "dashcdg/win32_timing_boost.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>

/*
 * Do NOT link avrt.lib / import AVRT.dll: AVRT exists only on Vista+.
 * Windows XP / 2000 have no AVRT.dll — static imports prevent the EXE from starting.
 * MMCSS entry points are resolved via LoadLibraryW(L"avrt.dll") when present.
 */
typedef HANDLE(WINAPI *dashcdg_pfn_AvSetMmThreadCharacteristicsW)(LPCWSTR TaskName, LPDWORD TaskIndex);
typedef BOOL(WINAPI *dashcdg_pfn_AvSetMmThreadPriority)(HANDLE AvrtHandle, int Priority);
typedef BOOL(WINAPI *dashcdg_pfn_AvRevertMmThreadCharacteristics)(HANDLE AvrtHandle);

/* Match MinGW/MSVC avrt.h: AVRT_PRIORITY_HIGH = 0 */
#define DASHCDG_AVRT_PRIORITY_HIGH 0

static HMODULE dashcdg_avrt_module(void) {
    static HMODULE mod;
    static int tried;

    if (!tried) {
        tried = 1;
        mod = LoadLibraryW(L"avrt.dll");
    }
    return mod;
}

static void dashcdg_avrt_resolve(
        dashcdg_pfn_AvSetMmThreadCharacteristicsW *out_set,
        dashcdg_pfn_AvSetMmThreadPriority *out_pri,
        dashcdg_pfn_AvRevertMmThreadCharacteristics *out_rev
) {
    HMODULE avrt = dashcdg_avrt_module();

    if (out_set != NULL) {
        *out_set = NULL;
    }
    if (out_pri != NULL) {
        *out_pri = NULL;
    }
    if (out_rev != NULL) {
        *out_rev = NULL;
    }
    if (avrt == NULL) {
        return;
    }
    if (out_set != NULL) {
        *out_set = (dashcdg_pfn_AvSetMmThreadCharacteristicsW)(INT_PTR) GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
    }
    if (out_pri != NULL) {
        *out_pri = (dashcdg_pfn_AvSetMmThreadPriority)(INT_PTR) GetProcAddress(avrt, "AvSetMmThreadPriority");
    }
    if (out_rev != NULL) {
        *out_rev = (dashcdg_pfn_AvRevertMmThreadCharacteristics)(INT_PTR) GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
    }
}

static int g_time_period_on;

static void dashcdg_win32_time_period_atexit(void) {
    if (g_time_period_on) {
        (void) timeEndPeriod(1);
        g_time_period_on = 0;
    }
}

void dashcdg_win32_process_timing_enable(void) {
    if (g_time_period_on) {
        return;
    }
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        g_time_period_on = 1;
        (void) atexit(dashcdg_win32_time_period_atexit);
    }
}

void dashcdg_win32_process_timing_disable(void) {
    dashcdg_win32_time_period_atexit();
}

void dashcdg_win32_thread_timing_boost_begin(struct dashcdg_win32_mmcss_handle *out) {
    dashcdg_pfn_AvSetMmThreadCharacteristicsW fn_set = NULL;
    dashcdg_pfn_AvSetMmThreadPriority fn_pri = NULL;

    DWORD task_index = 0;
    HANDLE task;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    dashcdg_avrt_resolve(&fn_set, &fn_pri, NULL);
    if (fn_set != NULL && fn_pri != NULL) {
        task = fn_set(L"Pro Audio", &task_index);
        if (task != NULL && task != INVALID_HANDLE_VALUE) {
            (void) fn_pri(task, DASHCDG_AVRT_PRIORITY_HIGH);
            out->task_handle = (void *) task;
            out->active = 1;
            return;
        }
    }

    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) == 0) {
        (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    out->active = 1;
}

void dashcdg_win32_thread_timing_boost_end(struct dashcdg_win32_mmcss_handle *in) {
    dashcdg_pfn_AvRevertMmThreadCharacteristics fn_rev = NULL;

    if (in == NULL || !in->active) {
        return;
    }
    if (in->task_handle != NULL) {
        dashcdg_avrt_resolve(NULL, NULL, &fn_rev);
        if (fn_rev != NULL) {
            (void) fn_rev((HANDLE) in->task_handle);
        }
        in->task_handle = NULL;
    }
    in->active = 0;
}

#else

void dashcdg_win32_process_timing_enable(void) {}

void dashcdg_win32_process_timing_disable(void) {}

void dashcdg_win32_thread_timing_boost_begin(struct dashcdg_win32_mmcss_handle *out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

void dashcdg_win32_thread_timing_boost_end(struct dashcdg_win32_mmcss_handle *in) {
    (void) in;
}

#endif
