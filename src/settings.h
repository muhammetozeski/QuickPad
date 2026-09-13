#pragma once

#include <windows.h>

/* Settings kept in QuickPad.ini next to the executable. */
typedef struct Settings {
    BOOL wordWrap;
    int poolSize;
    int windowWidth;
    int windowHeight;
    BOOL startupAsked;
    wchar_t fontName[LF_FACESIZE];
    int fontSize;
    int zoom;
    int tabSize;
    BOOL autoIndent;
    BOOL statusBar;
} Settings;

#define SETTINGS_POOL_SIZE_DEFAULT 30
#define SETTINGS_POOL_SIZE_MAX 100
#define SETTINGS_FONT_NAME_DEFAULT L"Comic Sans MS"
#define SETTINGS_FONT_SIZE_DEFAULT 11
#define SETTINGS_FONT_SIZE_MIN 4
#define SETTINGS_FONT_SIZE_MAX 96
#define SETTINGS_ZOOM_MIN 10
#define SETTINGS_ZOOM_MAX 500
#define SETTINGS_TAB_SIZE_DEFAULT 8

extern Settings settings;

void SettingsLoad(void);
void SettingsSave(void);
