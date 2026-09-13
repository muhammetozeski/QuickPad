#include "settings.h"
#include "quickpad.h"
#include "strings.h"

#define SECTION L"QuickPad"

Settings settings;

static wchar_t *iniPath;

/* The executable's path with its extension replaced by .ini. */
static const wchar_t *IniPath(void)
{
    if (iniPath != NULL) {
        return iniPath;
    }
    wchar_t *path = PathOfExecutable();
    if (path == NULL) {
        return NULL;
    }
    size_t length = (size_t)lstrlenW(path);
    const wchar_t *name = PathFileName(path);
    size_t end = length;
    for (size_t i = length; i > (size_t)(name - path); --i) {
        if (path[i - 1] == L'.') {
            end = i - 1;
            break;
        }
    }
    iniPath = MemAlloc((end + 5) * sizeof(wchar_t));
    if (iniPath != NULL) {
        memcpy(iniPath, path, end * sizeof(wchar_t));
        memcpy(iniPath + end, L".ini", 5 * sizeof(wchar_t));
    }
    MemFree(path);
    return iniPath;
}

static int ReadNumber(const wchar_t *key, int fallback, int minimum, int maximum)
{
    const wchar_t *path = IniPath();
    int value = path != NULL ? (int)GetPrivateProfileIntW(SECTION, key, fallback, path) : fallback;
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

static void WriteNumber(const wchar_t *key, int value)
{
    const wchar_t *path = IniPath();
    if (path != NULL) {
        wchar_t text[16];
        wsprintfW(text, L"%d", value);
        WritePrivateProfileStringW(SECTION, key, text, path);
    }
}

void SettingsLoad(void)
{
    settings.wordWrap = ReadNumber(L"WordWrap", 0, 0, 1);
    settings.poolSize = ReadNumber(L"PoolSize", SETTINGS_POOL_SIZE_DEFAULT, 0, SETTINGS_POOL_SIZE_MAX);
    settings.windowWidth = ReadNumber(L"WindowWidth", 0, 0, 32767);
    settings.windowHeight = ReadNumber(L"WindowHeight", 0, 0, 32767);
    settings.startupAsked = ReadNumber(L"StartupAsked", 0, 0, 1);
}

void SettingsSave(void)
{
    WriteNumber(L"WordWrap", settings.wordWrap);
    WriteNumber(L"PoolSize", settings.poolSize);
    WriteNumber(L"WindowWidth", settings.windowWidth);
    WriteNumber(L"WindowHeight", settings.windowHeight);
    WriteNumber(L"StartupAsked", settings.startupAsked);
}
