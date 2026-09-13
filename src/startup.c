#include "startup.h"
#include "quickpad.h"
#include "settings.h"
#include "strings.h"

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* The command the Run value holds for this executable. */
static wchar_t *StartupCommand(void)
{
    wchar_t *executable = PathOfExecutable();
    wchar_t *command = executable != NULL ? StringJoin(L"\"", executable, L"\" --background") : NULL;
    MemFree(executable);
    return command;
}

BOOL StartupIsEnabled(void)
{
    wchar_t *command = StartupCommand();
    if (command == NULL) {
        return FALSE;
    }
    DWORD size = 0;
    BOOL enabled = FALSE;
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, QP_APP_NAME, RRF_RT_REG_SZ, NULL, NULL, &size) == ERROR_SUCCESS) {
        wchar_t *value = MemAlloc(size + sizeof(wchar_t));
        if (value != NULL && RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, QP_APP_NAME, RRF_RT_REG_SZ, NULL, value, &size) == ERROR_SUCCESS) {
            enabled = CompareStringOrdinal(value, -1, command, -1, TRUE) == CSTR_EQUAL;
        }
        MemFree(value);
    }
    MemFree(command);
    return enabled;
}

static LSTATUS SetStartup(BOOL enabled)
{
    if (!enabled) {
        LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, QP_APP_NAME);
        return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
    }
    wchar_t *command = StartupCommand();
    if (command == NULL) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    DWORD size = (DWORD)(((size_t)lstrlenW(command) + 1) * sizeof(wchar_t));
    LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, QP_APP_NAME, REG_SZ, command, size);
    MemFree(command);
    return status;
}

static void Apply(HWND owner, BOOL enabled)
{
    LSTATUS status = SetStartup(enabled);
    if (status == ERROR_SUCCESS) {
        return;
    }
    wchar_t *system = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD)status, 0, (LPWSTR)&system, 0, NULL);
    wchar_t *message = StringJoin(enabled ? L"QuickPad could not be added to the programs that start with Windows.\n\n"
                                          : L"QuickPad could not be removed from the programs that start with Windows.\n\n",
        system != NULL ? system : L"", L"");
    MessageBoxW(owner, message != NULL ? message : L"The startup setting could not be changed.", QP_APP_NAME, MB_ICONERROR);
    MemFree(message);
    if (system != NULL) {
        LocalFree(system);
    }
}

void StartupToggle(HWND owner)
{
    Apply(owner, !StartupIsEnabled());
}

void StartupAskOnce(void)
{
    if (settings.startupAsked) {
        return;
    }
    settings.startupAsked = TRUE;
    SettingsSave();
    if (StartupIsEnabled()) {
        return;
    }
    int answer = MessageBoxW(NULL,
        L"Start QuickPad with Windows?\n\n"
        L"QuickPad then waits in the background with its windows drawn, so files open at once. "
        L"You can change this later under Settings > Start with Windows or in the notification area menu.",
        QP_APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (answer == IDYES) {
        Apply(NULL, TRUE);
    }
}
