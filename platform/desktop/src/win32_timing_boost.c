#include "dashcdg/win32_timing_boost.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>

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
    DWORD task_index = 0;
    HANDLE task;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (task != NULL) {
        (void) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        out->task_handle = (void *) task;
        out->active = 1;
        return;
    }
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) == 0) {
        (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    out->active = 1;
}

void dashcdg_win32_thread_timing_boost_end(struct dashcdg_win32_mmcss_handle *in) {
    if (in == NULL || !in->active) {
        return;
    }
    if (in->task_handle != NULL) {
        (void) AvRevertMmThreadCharacteristics((HANDLE) in->task_handle);
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
