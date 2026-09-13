#include "register.h"
#include "quickpad.h"
#include "resource.h"
#include "shellid.h"
#include "strings.h"

#include <shlobj.h>

#define PROGRAM_ID L"QuickPad.Document"
#define CLASSES L"Software\\Classes\\"
#define CLASS_KEY CLASSES L"CLSID\\" QUICKPAD_OPEN_COMMAND_CLSID_TEXT

static const wchar_t *const extensions[] = {
    L".txt", L".log", L".ini", L".cfg", L".conf", L".md", L".csv", L".json", L".xml", L".yaml", L".yml", L".nfo", L".srt",
};

static BOOL SetString(const wchar_t *key, const wchar_t *name, const wchar_t *value)
{
    if (key == NULL || value == NULL) {
        return FALSE;
    }
    DWORD size = (DWORD)(((size_t)lstrlenW(value) + 1) * sizeof(wchar_t));
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value, size) == ERROR_SUCCESS;
}

/* TRUE when the file holds exactly these bytes. */
static BOOL FileMatches(const wchar_t *path, const void *data, DWORD size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    BOOL matches = FALSE;
    LARGE_INTEGER fileSize;
    unsigned char *contents = NULL;
    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == size && (contents = MemAlloc(size)) != NULL) {
        DWORD read = 0;
        if (ReadFile(file, contents, size, &read, NULL) && read == size) {
            const unsigned char *expected = data;
            matches = TRUE;
            for (DWORD i = 0; i < size && matches; ++i) {
                matches = contents[i] == expected[i];
            }
        }
    }
    MemFree(contents);
    CloseHandle(file);
    return matches;
}

/*
 * Writes the shell extension embedded in this executable next to it. Explorer keeps a loaded DLL
 * open, so an older copy in use is renamed out of the way first.
 */
static wchar_t *WriteShellExtension(const wchar_t *executable)
{
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_SHELL_EXTENSION), RT_RCDATA);
    HGLOBAL loaded = resource != NULL ? LoadResource(NULL, resource) : NULL;
    const void *data = loaded != NULL ? LockResource(loaded) : NULL;
    DWORD size = resource != NULL ? SizeofResource(NULL, resource) : 0;
    if (data == NULL || size == 0) {
        return NULL;
    }

    wchar_t *folder = StringCopy(executable);
    if (folder == NULL) {
        return NULL;
    }
    ((wchar_t *)PathFileName(folder))[0] = 0;
    wchar_t *path = StringJoin(folder, QUICKPAD_SHELL_DLL_NAME, L"");
    wchar_t *old = StringJoin(folder, QUICKPAD_SHELL_DLL_NAME, L".old");
    MemFree(folder);
    if (path == NULL || old == NULL || FileMatches(path, data, size)) {
        MemFree(old);
        return path;
    }

    DeleteFileW(old);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE && MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING)) {
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    MemFree(old);

    DWORD written = 0;
    BOOL complete = file != INVALID_HANDLE_VALUE && WriteFile(file, data, size, &written, NULL) && written == size;
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    if (!complete) {
        MemFree(path);
        return NULL;
    }
    return path;
}

int RegisterFileTypes(void)
{
    wchar_t *executable = PathOfExecutable();
    wchar_t *extension = executable != NULL ? WriteShellExtension(executable) : NULL;
    if (extension == NULL) {
        MemFree(executable);
        return 1;
    }
    wchar_t *command = StringJoin(L"\"", executable, L"\" \"%1\"");
    wchar_t *icon = StringJoin(executable, L",0", L"");

    BOOL ok = TRUE;
    ok &= SetString(CLASS_KEY, NULL, L"QuickPad Open Command");
    ok &= SetString(CLASS_KEY L"\\InprocServer32", NULL, extension);
    ok &= SetString(CLASS_KEY L"\\InprocServer32", L"ThreadingModel", L"Apartment");

    ok &= SetString(CLASSES PROGRAM_ID, NULL, L"Text Document");
    ok &= SetString(CLASSES PROGRAM_ID L"\\DefaultIcon", NULL, icon);
    ok &= SetString(CLASSES PROGRAM_ID L"\\shell\\open", L"MultiSelectModel", L"Player");
    ok &= SetString(CLASSES PROGRAM_ID L"\\shell\\open\\command", NULL, command);
    ok &= SetString(CLASSES PROGRAM_ID L"\\shell\\open\\command", L"DelegateExecute", QUICKPAD_OPEN_COMMAND_CLSID_TEXT);

    ok &= SetString(CLASSES L"Applications\\QuickPad.exe", L"FriendlyAppName", QP_APP_NAME);
    ok &= SetString(CLASSES L"Applications\\QuickPad.exe\\DefaultIcon", NULL, icon);
    ok &= SetString(CLASSES L"Applications\\QuickPad.exe\\shell\\open", L"MultiSelectModel", L"Player");
    ok &= SetString(CLASSES L"Applications\\QuickPad.exe\\shell\\open\\command", NULL, command);
    ok &= SetString(CLASSES L"Applications\\QuickPad.exe\\shell\\open\\command", L"DelegateExecute", QUICKPAD_OPEN_COMMAND_CLSID_TEXT);

    ok &= SetString(L"Software\\QuickPad\\Capabilities", L"ApplicationName", QP_APP_NAME);
    ok &= SetString(L"Software\\QuickPad\\Capabilities", L"ApplicationDescription", L"A plain text editor that opens files at once.");
    ok &= SetString(L"Software\\RegisteredApplications", QP_APP_NAME, L"Software\\QuickPad\\Capabilities");

    for (size_t i = 0; i < ARRAYSIZE(extensions); ++i) {
        ok &= SetString(CLASSES L"Applications\\QuickPad.exe\\SupportedTypes", extensions[i], L"");
        wchar_t *openWith = StringJoin(CLASSES, extensions[i], L"\\OpenWithProgids");
        ok &= SetString(openWith, PROGRAM_ID, L"");
        MemFree(openWith);
        ok &= SetString(L"Software\\QuickPad\\Capabilities\\FileAssociations", extensions[i], PROGRAM_ID);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    MemFree(icon);
    MemFree(command);
    MemFree(extension);
    MemFree(executable);
    return ok ? 0 : 1;
}
