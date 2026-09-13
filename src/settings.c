#include "settings.h"
#include "quickpad.h"

#define SECTION L"QuickPad"

Settings settings;

static wchar_t *iniPath;

/* The executable's path with its extension replaced by .ini. */
static const wchar_t *IniPath(void)
{
    if (iniPath != NULL) {
        return iniPath;
    }
    for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2) {
        wchar_t *path = MemAlloc((capacity + 4) * sizeof(wchar_t));
        if (path == NULL) {
            return NULL;
        }
        DWORD length = GetModuleFileNameW(NULL, path, capacity);
        if (length > 0 && length < capacity) {
            DWORD dot = length;
            while (dot > 0 && path[dot - 1] != L'.' && path[dot - 1] != L'\\') {
                --dot;
            }
            DWORD end = dot > 0 && path[dot - 1] == L'.' ? dot - 1 : length;
            memcpy(path + end, L".ini", 5 * sizeof(wchar_t));
            iniPath = path;
            return iniPath;
        }
        MemFree(path);
        if (length == 0) {
            return NULL;
        }
    }
    return NULL;
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
}

void SettingsSave(void)
{
    WriteNumber(L"WordWrap", settings.wordWrap);
    WriteNumber(L"PoolSize", settings.poolSize);
    WriteNumber(L"WindowWidth", settings.windowWidth);
    WriteNumber(L"WindowHeight", settings.windowHeight);
}
