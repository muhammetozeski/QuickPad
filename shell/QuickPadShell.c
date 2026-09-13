/*
 * QuickPadShell.dll: the open command Explorer runs for files associated with QuickPad.
 *
 * The association's open verb names this class with DelegateExecute. Explorer loads the DLL into its
 * own process and executes the command with the selected files, and the command hands the paths to
 * the running QuickPad host with WM_COPYDATA. Opening a file therefore starts no process and needs
 * no COM activation. The call comes from Explorer, which owns the foreground, so the host is also
 * allowed to bring its window to the front. When no host is running, QuickPad.exe from this DLL's
 * folder is started with the paths.
 *
 * The DLL runs inside Explorer: it uses only kernel32, user32 and ole32, which Explorer has loaded,
 * and keeps every failure to itself.
 */
#define COBJMACROS
#define CONST_VTABLE

#include "../src/host.h"
#include "../src/shellid.h"

#include <shobjidl.h>

#define SEND_TIMEOUT 5000
#define COMMAND_LINE_CAPACITY 32767

static const CLSID openCommandClass = QUICKPAD_OPEN_COMMAND_CLSID;
static LONG liveObjects;
static LONG serverLocks;

typedef struct OpenCommand {
    IExecuteCommand execute;
    IObjectWithSelection selection;
    LONG references;
    IShellItemArray *items;
} OpenCommand;

static void *Allocate(size_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void Free(void *block)
{
    if (block != NULL) {
        HeapFree(GetProcessHeap(), 0, block);
    }
}

/* QuickPad.exe in the folder this DLL was loaded from. */
static wchar_t *ExecutablePath(void)
{
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&openCommandClass, &module)) {
        return NULL;
    }
    static const wchar_t executableName[] = L"QuickPad.exe";
    wchar_t *path = Allocate((32768 + ARRAYSIZE(executableName)) * sizeof(wchar_t));
    DWORD length = path != NULL ? GetModuleFileNameW(module, path, 32768) : 0;
    if (length == 0 || length >= 32768) {
        Free(path);
        return NULL;
    }
    while (length > 0 && path[length - 1] != L'\\') {
        --length;
    }
    memcpy(path + length, executableName, sizeof executableName);
    return path;
}

static BOOL SendPath(HWND host, const wchar_t *path)
{
    COPYDATASTRUCT data = { HOST_COPY_OPEN, (DWORD)(((size_t)lstrlenW(path) + 1) * sizeof(wchar_t)), (PVOID)path };
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG | SMTO_BLOCK, SEND_TIMEOUT, &result) != 0
        && result == TRUE;
}

static void Append(wchar_t *buffer, size_t *length, const wchar_t *text)
{
    size_t count = (size_t)lstrlenW(text);
    memcpy(buffer + *length, text, count * sizeof(wchar_t));
    *length += count;
    buffer[*length] = 0;
}

/* Starts QuickPad.exe with the paths, in as many launches as the command line limit needs. */
static void StartQuickPad(wchar_t **paths, DWORD count)
{
    wchar_t *executable = ExecutablePath();
    wchar_t *commandLine = Allocate((COMMAND_LINE_CAPACITY + 1) * sizeof(wchar_t));
    if (executable != NULL && commandLine != NULL) {
        size_t executableLength = (size_t)lstrlenW(executable);
        DWORD next = 0;
        while (next < count) {
            size_t length = 0;
            Append(commandLine, &length, L"\"");
            Append(commandLine, &length, executable);
            Append(commandLine, &length, L"\"");
            DWORD first = next;
            while (next < count) {
                size_t pathLength = (size_t)lstrlenW(paths[next]);
                if (next > first && length + pathLength + 3 > COMMAND_LINE_CAPACITY) {
                    break;
                }
                if (executableLength + pathLength + 6 > COMMAND_LINE_CAPACITY) {
                    ++next;
                    continue;
                }
                Append(commandLine, &length, L" \"");
                Append(commandLine, &length, paths[next]);
                Append(commandLine, &length, L"\"");
                ++next;
            }
            STARTUPINFOW startup = { sizeof startup };
            PROCESS_INFORMATION process;
            if (CreateProcessW(executable, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
                AllowSetForegroundWindow(process.dwProcessId);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }
    }
    Free(commandLine);
    Free(executable);
}

/* ---- The command object -------------------------------------------------------------------- */

static OpenCommand *FromExecute(IExecuteCommand *pointer)
{
    return (OpenCommand *)pointer;
}

static OpenCommand *FromSelection(IObjectWithSelection *pointer)
{
    return (OpenCommand *)((char *)pointer - offsetof(OpenCommand, selection));
}

static HRESULT QueryCommand(OpenCommand *command, REFIID interfaceId, void **object)
{
    if (InlineIsEqualGUID(interfaceId, &IID_IUnknown) || InlineIsEqualGUID(interfaceId, &IID_IExecuteCommand)) {
        *object = &command->execute;
    } else if (InlineIsEqualGUID(interfaceId, &IID_IObjectWithSelection)) {
        *object = &command->selection;
    } else {
        *object = NULL;
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&command->references);
    return S_OK;
}

static ULONG ReleaseCommand(OpenCommand *command)
{
    LONG references = InterlockedDecrement(&command->references);
    if (references == 0) {
        if (command->items != NULL) {
            IShellItemArray_Release(command->items);
        }
        Free(command);
        InterlockedDecrement(&liveObjects);
    }
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE ExecuteQueryInterface(IExecuteCommand *self, REFIID interfaceId, void **object)
{
    return QueryCommand(FromExecute(self), interfaceId, object);
}

static ULONG STDMETHODCALLTYPE ExecuteAddRef(IExecuteCommand *self)
{
    return (ULONG)InterlockedIncrement(&FromExecute(self)->references);
}

static ULONG STDMETHODCALLTYPE ExecuteRelease(IExecuteCommand *self)
{
    return ReleaseCommand(FromExecute(self));
}

static HRESULT STDMETHODCALLTYPE SetKeyState(IExecuteCommand *self, DWORD keyState)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(keyState);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SetParameters(IExecuteCommand *self, LPCWSTR parameters)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(parameters);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SetPosition(IExecuteCommand *self, POINT point)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(point);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SetShowWindow(IExecuteCommand *self, int show)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(show);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SetNoShowUI(IExecuteCommand *self, BOOL noShowUI)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(noShowUI);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SetDirectory(IExecuteCommand *self, LPCWSTR directory)
{
    UNREFERENCED_PARAMETER(self);
    UNREFERENCED_PARAMETER(directory);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Execute(IExecuteCommand *self)
{
    OpenCommand *command = FromExecute(self);
    DWORD count = 0;
    if (command->items == NULL || FAILED(IShellItemArray_GetCount(command->items, &count)) || count == 0) {
        return S_OK;
    }
    wchar_t **paths = Allocate(count * sizeof(wchar_t *));
    if (paths == NULL) {
        return E_OUTOFMEMORY;
    }

    DWORD gathered = 0;
    for (DWORD i = 0; i < count; ++i) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IShellItemArray_GetItemAt(command->items, i, &item))) {
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &paths[gathered]))) {
                ++gathered;
            }
            IShellItem_Release(item);
        }
    }

    DWORD sent = 0;
    HWND host = FindWindowW(HOST_WINDOW_CLASS, NULL);
    if (host != NULL) {
        DWORD processId = 0;
        GetWindowThreadProcessId(host, &processId);
        AllowSetForegroundWindow(processId);
        while (sent < gathered && SendPath(host, paths[sent])) {
            ++sent;
        }
    }
    if (sent < gathered) {
        StartQuickPad(paths + sent, gathered - sent);
    }

    for (DWORD i = 0; i < gathered; ++i) {
        CoTaskMemFree(paths[i]);
    }
    Free(paths);
    return S_OK;
}

static const IExecuteCommandVtbl executeVtbl = {
    ExecuteQueryInterface,
    ExecuteAddRef,
    ExecuteRelease,
    SetKeyState,
    SetParameters,
    SetPosition,
    SetShowWindow,
    SetNoShowUI,
    SetDirectory,
    Execute,
};

static HRESULT STDMETHODCALLTYPE SelectionQueryInterface(IObjectWithSelection *self, REFIID interfaceId, void **object)
{
    return QueryCommand(FromSelection(self), interfaceId, object);
}

static ULONG STDMETHODCALLTYPE SelectionAddRef(IObjectWithSelection *self)
{
    return (ULONG)InterlockedIncrement(&FromSelection(self)->references);
}

static ULONG STDMETHODCALLTYPE SelectionRelease(IObjectWithSelection *self)
{
    return ReleaseCommand(FromSelection(self));
}

static HRESULT STDMETHODCALLTYPE SetSelection(IObjectWithSelection *self, IShellItemArray *items)
{
    OpenCommand *command = FromSelection(self);
    if (items != NULL) {
        IShellItemArray_AddRef(items);
    }
    if (command->items != NULL) {
        IShellItemArray_Release(command->items);
    }
    command->items = items;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE GetSelection(IObjectWithSelection *self, REFIID interfaceId, void **object)
{
    OpenCommand *command = FromSelection(self);
    if (command->items == NULL) {
        *object = NULL;
        return E_FAIL;
    }
    return IShellItemArray_QueryInterface(command->items, interfaceId, object);
}

static const IObjectWithSelectionVtbl selectionVtbl = {
    SelectionQueryInterface,
    SelectionAddRef,
    SelectionRelease,
    SetSelection,
    GetSelection,
};

/* ---- Class factory and exports ------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE FactoryQueryInterface(IClassFactory *self, REFIID interfaceId, void **object)
{
    if (InlineIsEqualGUID(interfaceId, &IID_IUnknown) || InlineIsEqualGUID(interfaceId, &IID_IClassFactory)) {
        *object = self;
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

/* The factory is a static object; the DLL stays loaded while objects or locks exist. */
static ULONG STDMETHODCALLTYPE FactoryAddRef(IClassFactory *self)
{
    UNREFERENCED_PARAMETER(self);
    return 2;
}

static ULONG STDMETHODCALLTYPE FactoryRelease(IClassFactory *self)
{
    UNREFERENCED_PARAMETER(self);
    return 1;
}

static HRESULT STDMETHODCALLTYPE CreateInstance(IClassFactory *self, IUnknown *outer, REFIID interfaceId, void **object)
{
    UNREFERENCED_PARAMETER(self);
    *object = NULL;
    if (outer != NULL) {
        return CLASS_E_NOAGGREGATION;
    }
    OpenCommand *command = Allocate(sizeof *command);
    if (command == NULL) {
        return E_OUTOFMEMORY;
    }
    InterlockedIncrement(&liveObjects);
    command->execute.lpVtbl = &executeVtbl;
    command->selection.lpVtbl = &selectionVtbl;
    command->references = 1;
    HRESULT result = QueryCommand(command, interfaceId, object);
    ReleaseCommand(command);
    return result;
}

static HRESULT STDMETHODCALLTYPE LockServer(IClassFactory *self, BOOL lock)
{
    UNREFERENCED_PARAMETER(self);
    if (lock) {
        InterlockedIncrement(&serverLocks);
    } else {
        InterlockedDecrement(&serverLocks);
    }
    return S_OK;
}

static const IClassFactoryVtbl factoryVtbl = {
    FactoryQueryInterface,
    FactoryAddRef,
    FactoryRelease,
    CreateInstance,
    LockServer,
};

static IClassFactory factory = { &factoryVtbl };

/* Exported as DllGetClassObject. */
HRESULT WINAPI ShellGetClassObject(REFCLSID classId, REFIID interfaceId, void **object)
{
    if (!InlineIsEqualGUID(classId, &openCommandClass)) {
        *object = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return FactoryQueryInterface(&factory, interfaceId, object);
}

/* Exported as DllCanUnloadNow. */
HRESULT WINAPI ShellCanUnloadNow(void)
{
    return liveObjects == 0 && serverLocks == 0 ? S_OK : S_FALSE;
}
