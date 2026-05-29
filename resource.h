#pragma once

// ---- Icons ----------------------------------------------------------------
// WiFi: 5 signal levels x 2 connectivity states (internet / limited) + off
#define IDI_WIFI_0              101
#define IDI_WIFI_1              102
#define IDI_WIFI_2              103
#define IDI_WIFI_3              104
#define IDI_WIFI_4              105

#define IDI_WIFI_0_LIMITED      111
#define IDI_WIFI_1_LIMITED      112
#define IDI_WIFI_2_LIMITED      113
#define IDI_WIFI_3_LIMITED      114
#define IDI_WIFI_4_LIMITED      115

#define IDI_WIFI_OFF            120

// Ethernet: connected / limited / unplugged
#define IDI_ETHERNET            130
#define IDI_ETHERNET_LIMITED    131
#define IDI_ETHERNET_UNPLUGGED  132

// Fallback when no interfaces are up at all
#define IDI_DISCONNECTED        140

// ---- Dark-mode icon variants ----------------------------------------------
// Shown when the taskbar is dark (light/white glyphs).
// INVARIANT: each dark ID == its light counterpart + 1000
#define IDI_WIFI_0_DARK              1101
#define IDI_WIFI_1_DARK              1102
#define IDI_WIFI_2_DARK              1103
#define IDI_WIFI_3_DARK              1104
#define IDI_WIFI_4_DARK              1105

#define IDI_WIFI_0_LIMITED_DARK      1111
#define IDI_WIFI_1_LIMITED_DARK      1112
#define IDI_WIFI_2_LIMITED_DARK      1113
#define IDI_WIFI_3_LIMITED_DARK      1114
#define IDI_WIFI_4_LIMITED_DARK      1115

#define IDI_WIFI_OFF_DARK            1120

#define IDI_ETHERNET_DARK            1130
#define IDI_ETHERNET_LIMITED_DARK    1131
#define IDI_ETHERNET_UNPLUGGED_DARK  1132

#define IDI_DISCONNECTED_DARK        1140

// ---- Context menu commands -----------------------------------------------
#define IDM_OPEN_NETWORK_SETTINGS  201
#define IDM_OPEN_NCPA              202
#define IDM_REFRESH                203
#define IDM_EXIT                   204

// Icon-theme override (Auto / Light / Dark). Must stay contiguous and in this
// order — the code maps ThemePref onto them by arithmetic.
#define IDM_THEME_AUTO             210
#define IDM_THEME_LIGHT            211
#define IDM_THEME_DARK             212
