#ifndef DASHCDG_WIN32_TIMING_BOOST_H
#define DASHCDG_WIN32_TIMING_BOOST_H

/*
 * Windows-only: MMCSS ("Pro Audio") + optional thread priority for real-time-ish
 * streaming threads, plus 1 ms system timer resolution (timeBeginPeriod) so
 * Sleep() in the TX/RX loops is less granular under load (IDE foreground, etc.).
 * See docs/specs/v5-multistream-adaptation-architecture.md (enterprise timing section).
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
