#pragma once

#define CONSTANTS_WINDOW_NAME "Kaentake"
#define CONSTANTS_DLL_NAME    "Kaentake.dll"
#define CONSTANTS_CONFIG_NAME "config.ini"

#define CONSTANTS_CENTER_STATUSBAR FALSE

#define CONSTANTS_DEFAULT_HOST     "127.0.0.1"
#define CONSTANTS_USE_COMMAND_LINE TRUE
#define CONSTANTS_USE_CONFIG_FILE  TRUE

extern char* g_sServerHost;
extern long g_nServerPort;

// Caps used by addiesKaentakeCompile.cpp
#define CONSTANTS_DAMAGE_CAP 999999.0
#define CONSTANTS_SPEED_CAP  250

// Declared in resolution.cpp
int get_screen_width();
int get_screen_height();