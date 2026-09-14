#include "settings.h"
#include "quickpad.h"
#include "strings.h"

#define SECTION L"QuickPad"
#define SECTION_CAPACITY 4096

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

/* The value of key in a section read with GetPrivateProfileSectionW, or NULL. */
static const wchar_t *FindValue(const wchar_t *section, const wchar_t *key)
{
    int keyLength = lstrlenW(key);
    for (const wchar_t *entry = section; *entry != 0; entry += lstrlenW(entry) + 1) {
        if (CompareStringOrdinal(entry, keyLength, key, keyLength, TRUE) == CSTR_EQUAL && entry[keyLength] == L'=') {
            return entry + keyLength + 1;
        }
    }
    return NULL;
}

static int ReadNumber(const wchar_t *section, const wchar_t *key, int fallback, int minimum, int maximum)
{
    const wchar_t *text = FindValue(section, key);
    if (text == NULL) {
        return fallback;
    }
    while (*text == L' ' || *text == L'\t') {
        ++text;
    }
    BOOL negative = *text == L'-';
    text += negative;
    if (*text < L'0' || *text > L'9') {
        return fallback;
    }
    long long value = 0;
    while (*text >= L'0' && *text <= L'9' && value < 0x7FFFFFFF) {
        value = value * 10 + (*text++ - L'0');
    }
    value = negative ? -value : value;
    return value < minimum ? minimum : value > maximum ? maximum : (int)value;
}

/* Everything is read in one call; a missing file or key keeps the default. */
void SettingsLoad(void)
{
    static wchar_t section[SECTION_CAPACITY];
    const wchar_t *path = IniPath();
    section[0] = 0;
    section[1] = 0;
    if (path != NULL) {
        GetPrivateProfileSectionW(SECTION, section, SECTION_CAPACITY, path);
    }

    settings.wordWrap = ReadNumber(section, L"WordWrap", 0, 0, 1);
    settings.poolSize = ReadNumber(section, L"PoolSize", SETTINGS_POOL_SIZE_DEFAULT, 0, SETTINGS_POOL_SIZE_MAX);
    settings.windowWidth = ReadNumber(section, L"WindowWidth", 0, 0, 32767);
    settings.windowHeight = ReadNumber(section, L"WindowHeight", 0, 0, 32767);
    settings.startupAsked = ReadNumber(section, L"StartupAsked", 0, 0, 1);
    settings.fontSize = ReadNumber(section, L"FontSize", SETTINGS_FONT_SIZE_DEFAULT, SETTINGS_FONT_SIZE_MIN, SETTINGS_FONT_SIZE_MAX);
    settings.zoom = ReadNumber(section, L"Zoom", 100, SETTINGS_ZOOM_MIN, SETTINGS_ZOOM_MAX);
    settings.tabSize = ReadNumber(section, L"TabSize", SETTINGS_TAB_SIZE_DEFAULT, 1, 16);
    settings.autoIndent = ReadNumber(section, L"AutoIndent", 0, 0, 1);
    settings.statusBar = ReadNumber(section, L"StatusBar", 0, 0, 1);
    settings.readyMemoryMB = ReadNumber(section, L"ReadyMemoryMB", SETTINGS_READY_MEMORY_DEFAULT, 0, SETTINGS_READY_MEMORY_MAX);
    settings.gpu = ReadNumber(section, L"Gpu", 1, 0, 1);
    const wchar_t *fontName = FindValue(section, L"FontName");
    lstrcpynW(settings.fontName, fontName != NULL && *fontName != 0 ? fontName : SETTINGS_FONT_NAME_DEFAULT,
        ARRAYSIZE(settings.fontName));
}

/* Appends key=value and its terminator to a section being built. */
static void AddEntry(wchar_t *section, size_t *length, const wchar_t *key, const wchar_t *value)
{
    wchar_t *entry = StringJoin(key, L"=", value);
    if (entry == NULL) {
        return;
    }
    size_t entryLength = (size_t)lstrlenW(entry) + 1;
    if (*length + entryLength + 1 <= SECTION_CAPACITY) {
        memcpy(section + *length, entry, entryLength * sizeof(wchar_t));
        *length += entryLength;
    }
    MemFree(entry);
}

static void AddNumber(wchar_t *section, size_t *length, const wchar_t *key, int value)
{
    wchar_t text[16];
    wsprintfW(text, L"%d", value);
    AddEntry(section, length, key, text);
}

/* Writes the whole section in one call. */
void SettingsSave(void)
{
    const wchar_t *path = IniPath();
    wchar_t *section = MemAlloc(SECTION_CAPACITY * sizeof(wchar_t));
    if (path == NULL || section == NULL) {
        MemFree(section);
        return;
    }
    size_t length = 0;
    AddNumber(section, &length, L"WordWrap", settings.wordWrap);
    AddNumber(section, &length, L"PoolSize", settings.poolSize);
    AddNumber(section, &length, L"WindowWidth", settings.windowWidth);
    AddNumber(section, &length, L"WindowHeight", settings.windowHeight);
    AddNumber(section, &length, L"StartupAsked", settings.startupAsked);
    AddEntry(section, &length, L"FontName", settings.fontName);
    AddNumber(section, &length, L"FontSize", settings.fontSize);
    AddNumber(section, &length, L"Zoom", settings.zoom);
    AddNumber(section, &length, L"TabSize", settings.tabSize);
    AddNumber(section, &length, L"AutoIndent", settings.autoIndent);
    AddNumber(section, &length, L"StatusBar", settings.statusBar);
    AddNumber(section, &length, L"ReadyMemoryMB", settings.readyMemoryMB);
    AddNumber(section, &length, L"Gpu", settings.gpu);
    section[length] = 0;
    WritePrivateProfileSectionW(SECTION, section, path);
    MemFree(section);
}
