#include "dashcdg/win32_gdi_view.h"

#include "dashcdg/cdg_raster.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "dashcdg/common.h"
#include "dashcdg/media_clock.h"

struct dashcdg_win32_gdi_view {
    HWND hwnd;
    int quit;
    dashcdg_win32_gdi_key_cb on_key;
    void *key_user;
    uint64_t last_present_ms;
    uint8_t bgra_scratch[DASHCDG_CDG_RGBA_BYTES];
    BITMAPINFO bmi;
};

#define DASHCDG_WIN32_GDI_MAX_FPS 50U
#define DASHCDG_WIN32_GDI_FRAME_INTERVAL_MS (1000U / DASHCDG_WIN32_GDI_MAX_FPS)

static const wchar_t DASHCDG_GDI_CLASS_NAME[] = L"DashCdgRxGdiView";

static void dashcdg_win32_rgba_to_bgra(const uint8_t *rgba, uint8_t *bgra, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        size_t o = i * 4U;

        bgra[o + 0U] = rgba[o + 2U];
        bgra[o + 1U] = rgba[o + 1U];
        bgra[o + 2U] = rgba[o + 0U];
        bgra[o + 3U] = rgba[o + 3U];
    }
}

static LRESULT CALLBACK dashcdg_win32_gdi_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    struct dashcdg_win32_gdi_view *view = (struct dashcdg_win32_gdi_view *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW *cs = (CREATESTRUCTW *) lparam;

            view = (struct dashcdg_win32_gdi_view *) cs->lpCreateParams;
            if (view != NULL) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) view);
            }
            return 0;
        }
        case WM_DESTROY:
            if (view != NULL) {
                view->quit = 1;
            }
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (view != NULL && view->on_key != NULL) {
                /*
                 * Bit 30: previous key state (1 = key was already down). Windows sends WM_KEYDOWN
                 * repeatedly while held; forwarding those repeats toggles RX debug keys (e.g. D =
                 * decode-drop) multiple times per physical press, often leaving decode disabled with
                 * counters still climbing (video path only).
                 */
                if ((lparam & (LPARAM) (1UL << 30)) == 0) {
                    view->on_key(view->key_user, (unsigned) wparam, 1);
                }
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int dashcdg_win32_gdi_register_class_once(HINSTANCE inst) {
    static int registered;
    WNDCLASSW wc;

    if (registered) {
        return 1;
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dashcdg_win32_gdi_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = DASHCDG_GDI_CLASS_NAME;
    if (RegisterClassW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 0;
    }
    registered = 1;
    return 1;
}

int dashcdg_win32_gdi_view_create(
        struct dashcdg_win32_gdi_view **out,
        const char *title,
        int window_pixel_w,
        int window_pixel_h,
        dashcdg_win32_gdi_key_cb on_key,
        void *key_user
) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    struct dashcdg_win32_gdi_view *view;
    HWND hwnd;
    RECT r;
    DWORD style;
    int w;
    int h;
    wchar_t wtitle[256];
    size_t conv;

    if (out == NULL || title == NULL || window_pixel_w <= 0 || window_pixel_h <= 0) {
        return 0;
    }

    *out = NULL;
    if (!dashcdg_win32_gdi_register_class_once(inst)) {
        return 0;
    }

    view = (struct dashcdg_win32_gdi_view *) calloc(1, sizeof(*view));
    if (view == NULL) {
        return 0;
    }

    view->on_key = on_key;
    view->key_user = key_user;
    memset(&view->bmi, 0, sizeof(view->bmi));
    view->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    view->bmi.bmiHeader.biWidth = DASHCDG_VISIBLE_WIDTH;
    view->bmi.bmiHeader.biHeight = -(LONG) DASHCDG_VISIBLE_HEIGHT;
    view->bmi.bmiHeader.biPlanes = 1;
    view->bmi.bmiHeader.biBitCount = 32;
    view->bmi.bmiHeader.biCompression = BI_RGB;

    conv = (size_t) MultiByteToWideChar(
            CP_UTF8,
            0,
            title,
            -1,
            wtitle,
            (int) (sizeof(wtitle) / sizeof(wtitle[0]))
    );
    if (conv == 0U) {
        wtitle[0] = L'd';
        wtitle[1] = L'a';
        wtitle[2] = L's';
        wtitle[3] = L'h';
        wtitle[4] = L'c';
        wtitle[5] = L'd';
        wtitle[6] = L'g';
        wtitle[7] = L'\0';
    }

    style = WS_OVERLAPPEDWINDOW;
    r.left = 0;
    r.top = 0;
    r.right = window_pixel_w;
    r.bottom = window_pixel_h;
    AdjustWindowRect(&r, style, FALSE);
    w = (int) (r.right - r.left);
    h = (int) (r.bottom - r.top);

    hwnd = CreateWindowExW(
            0U,
            DASHCDG_GDI_CLASS_NAME,
            wtitle,
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            w,
            h,
            NULL,
            NULL,
            inst,
            view
    );
    if (hwnd == NULL) {
        free(view);
        return 0;
    }

    view->hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    *out = view;
    return 1;
}

void dashcdg_win32_gdi_view_destroy(struct dashcdg_win32_gdi_view *view) {
    if (view == NULL) {
        return;
    }
    if (view->hwnd != NULL) {
        DestroyWindow(view->hwnd);
        view->hwnd = NULL;
    }
    free(view);
}

int dashcdg_win32_gdi_view_poll(struct dashcdg_win32_gdi_view *view) {
    MSG msg;

    if (view == NULL) {
        return 0;
    }

    while (PeekMessageW(&msg, NULL, 0U, 0U, PM_REMOVE) != 0) {
        if (msg.message == WM_QUIT) {
            view->quit = 1;
            return 0;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return view->quit ? 0 : 1;
}

static int dashcdg_win32_gdi_view_present_bgra_internal(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *bgra,
        size_t bgra_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b
) {
    HDC hdc;
    RECT cr;
    int cw;
    int ch;
    uint64_t now_ms;

    if (view == NULL || bgra == NULL || bgra_bytes != DASHCDG_CDG_RGBA_BYTES || view->hwnd == NULL) {
        return 0;
    }
    now_ms = dashcdg_clock_now_ms();
    if (view->last_present_ms != 0U &&
            now_ms - view->last_present_ms < (uint64_t) DASHCDG_WIN32_GDI_FRAME_INTERVAL_MS) {
        return 1;
    }
    view->last_present_ms = now_ms;
    ValidateRect(view->hwnd, NULL);

    hdc = GetDC(view->hwnd);
    if (hdc == NULL) {
        return 0;
    }
    if (GetClientRect(view->hwnd, &cr) == 0) {
        ReleaseDC(view->hwnd, hdc);
        return 0;
    }
    cw = (int) (cr.right - cr.left);
    ch = (int) (cr.bottom - cr.top);
    if (cw <= 0 || ch <= 0) {
        ReleaseDC(view->hwnd, hdc);
        return 1;
    }

    StretchDIBits(
            hdc,
            0,
            0,
            cw,
            ch,
            0,
            0,
            DASHCDG_VISIBLE_WIDTH,
            DASHCDG_VISIBLE_HEIGHT,
            bgra,
            &view->bmi,
            DIB_RGB_COLORS,
            SRCCOPY
    );

    if (show_hud && hud_line_a != NULL) {
        int len_a;
        int len_b;
        size_t i;

        len_a = 0;
        for (i = 0; i < 512U && hud_line_a[i] != '\0'; ++i) {
            len_a++;
        }
        len_b = 0;
        if (hud_line_b != NULL) {
            for (i = 0; i < 512U && hud_line_b[i] != '\0'; ++i) {
                len_b++;
            }
        }
        HFONT font = (HFONT) GetStockObject(ANSI_FIXED_FONT);
        HFONT old = (HFONT) SelectObject(hdc, font);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(0, 0, 0));
        SetTextColor(hdc, RGB(120, 240, 120));
        TextOutA(hdc, 8, 8, hud_line_a, len_a);
        if (hud_line_b != NULL) {
            TextOutA(hdc, 8, 24, hud_line_b, len_b);
        }
        SelectObject(hdc, old);
    }

    ReleaseDC(view->hwnd, hdc);
    return 1;
}

int dashcdg_win32_gdi_view_present_rgba(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *rgba,
        size_t rgba_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b
) {
    if (view == NULL || rgba == NULL || rgba_bytes != DASHCDG_CDG_RGBA_BYTES) {
        return 0;
    }
    if (view->hwnd == NULL) {
        return 0;
    }
    {
        uint64_t now_ms = dashcdg_clock_now_ms();

        if (view->last_present_ms != 0U &&
                now_ms - view->last_present_ms < (uint64_t) DASHCDG_WIN32_GDI_FRAME_INTERVAL_MS) {
            return 1;
        }
    }

    dashcdg_win32_rgba_to_bgra(
            rgba,
            view->bgra_scratch,
            (size_t) DASHCDG_VISIBLE_WIDTH * (size_t) DASHCDG_VISIBLE_HEIGHT
    );
    return dashcdg_win32_gdi_view_present_bgra_internal(
            view,
            view->bgra_scratch,
            DASHCDG_CDG_RGBA_BYTES,
            show_hud,
            hud_line_a,
            hud_line_b
    );
}

int dashcdg_win32_gdi_view_present_bgra(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *bgra,
        size_t bgra_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b
) {
    return dashcdg_win32_gdi_view_present_bgra_internal(
            view,
            bgra,
            bgra_bytes,
            show_hud,
            hud_line_a,
            hud_line_b
    );
}

#else

struct dashcdg_win32_gdi_view {
    int unused;
};

int dashcdg_win32_gdi_view_create(
        struct dashcdg_win32_gdi_view **out,
        const char *title,
        int window_pixel_w,
        int window_pixel_h,
        dashcdg_win32_gdi_key_cb on_key,
        void *key_user
) {
    (void) title;
    (void) window_pixel_w;
    (void) window_pixel_h;
    (void) on_key;
    (void) key_user;
    if (out != NULL) {
        *out = NULL;
    }
    return 0;
}

void dashcdg_win32_gdi_view_destroy(struct dashcdg_win32_gdi_view *view) {
    (void) view;
}

int dashcdg_win32_gdi_view_poll(struct dashcdg_win32_gdi_view *view) {
    (void) view;
    return 0;
}

int dashcdg_win32_gdi_view_present_rgba(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *rgba,
        size_t rgba_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b
) {
    (void) view;
    (void) rgba;
    (void) rgba_bytes;
    (void) show_hud;
    (void) hud_line_a;
    (void) hud_line_b;
    return 0;
}

int dashcdg_win32_gdi_view_present_bgra(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *bgra,
        size_t bgra_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b
) {
    (void) view;
    (void) bgra;
    (void) bgra_bytes;
    (void) show_hud;
    (void) hud_line_a;
    (void) hud_line_b;
    return 0;
}

#endif
