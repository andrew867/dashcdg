#ifndef DASHCDG_WIN32_TIMING_BOOST_H
#define DASHCDG_WIN32_TIMING_BOOST_H

/*
 * Windows-only: MMCSS ("Pro Audio") via runtime LoadLibrary(avrt.dll) when available,
 * fallback thread priority otherwise; WinMM timeBeginPeriod(1) process-wide (XP+).
 * Do not static-link AVRT — AVRT.dll does not exist on XP/2000.
 */

struct dashcdg_win32_mmcss_handle {
    void *task_handle; /* HANDLE when active */
    int active;
};

void dashcdg_win32_process_timing_enable(void);
void dashcdg_win32_process_timing_disable(void);

void dashcdg_win32_thread_timing_boost_begin(struct dashcdg_win32_mmcss_handle *out);
void dashcdg_win32_thread_timing_boost_end(struct dashcdg_win32_mmcss_handle *in);

#endif
