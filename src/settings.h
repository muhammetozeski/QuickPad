#pragma once

#include <windows.h>

/* Settings kept in QuickPad.ini next to the executable. */
typedef struct Settings {
    BOOL wordWrap;
    int poolSize;
    int windowWidth;
    int windowHeight;
    BOOL startupAsked;
} Settings;

#define SETTINGS_POOL_SIZE_DEFAULT 30
#define SETTINGS_POOL_SIZE_MAX 100

extern Settings settings;

void SettingsLoad(void);
void SettingsSave(void);
