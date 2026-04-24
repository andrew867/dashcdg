/*
 * Short playful subtitles for badge LVGL screens (titles stay literal elsewhere).
 * ASCII only: embedded font glyph coverage is limited.
 */
#include "esp_random.h"

#include "badge_ui_flair.h"

static const char *const k_movie[] = {
    "\"We're gonna need a bigger subnet.\"",
    "\"Follow the SSID.\"",
    "\"I am your router.\"",
    "\"Resistance is futile - try WPA2 anyway.\"",
    "\"The truth is out there... behind NAT.\"",
    "\"I'll be back... after DHCP renews.\"",
    "\"This is the way... to the settings menu.\"",
    "\"I can do this all day... on battery.\"",
    "\"That's not a moon - that's multicast.\"",
    "\"Hello, IT. Have you tried power-cycling?\"",
    "\"Turn it off and on again.\"",
    "\"We're in the pipe, five by five.\"",
    "\"Never tell me the odds... of packet loss.\"",
};

static const char *const k_wifi[] = {
    "DHCP: drama-free handshakes since forever.",
    "Scan like you mean it.",
    "Your AP called. It wants RSSI.",
    "PSK goes in the vault, not the group chat.",
    "IPv4 club: still accepting members.",
    "Forget saved creds? Bold move, Cotton.",
    "Airwaves: the original cloud.",
};

static const char *const k_touch[] = {
    "Teach the glass where your finger lives.",
    "Four corners, zero drama (we hope).",
    "Stylus yoga: stretch to each edge.",
    "Bad cal = taps in another zip code.",
    "Precision > vibes, but vibes help.",
    "XPT2046 wants coordinates, not opinions.",
};

static const char *const k_display[] = {
    "Pixels, photons, and power naps.",
    "Turn down for what? For battery.",
    "RGB status: your desk's mood ring.",
    "Backlight: the dimmer the plot twist.",
    "Sleep mode: the panel's intermission.",
};

static const char *const k_home[] = {
    "Pick a module. Try not to brick the vibe.",
    "Multicast karaoke meets pocket propaganda.",
    "Sterling uplink optional; swagger default.",
    "Badge OS: slightly too online.",
    "RX first, explanations never.",
};

static const char *pick(const char *const *tab, unsigned n)
{
    if (!n || !tab) {
        return "";
    }
    return tab[esp_random() % n];
}

const char *dashcdg_ui_flair_movie_tagline(void)
{
    return pick(k_movie, (unsigned)(sizeof(k_movie) / sizeof(k_movie[0])));
}

const char *dashcdg_ui_flair_wifi_sub(void)
{
    return pick(k_wifi, (unsigned)(sizeof(k_wifi) / sizeof(k_wifi[0])));
}

const char *dashcdg_ui_flair_touch_cal_sub(void)
{
    return pick(k_touch, (unsigned)(sizeof(k_touch) / sizeof(k_touch[0])));
}

const char *dashcdg_ui_flair_display_sub(void)
{
    return pick(k_display, (unsigned)(sizeof(k_display) / sizeof(k_display[0])));
}

const char *dashcdg_ui_flair_home_sub(void)
{
    return pick(k_home, (unsigned)(sizeof(k_home) / sizeof(k_home[0])));
}
