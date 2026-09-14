#define COBJMACROS

#include "editor.h"
#include "blocks.h"
#include "fileio.h"
#include "quickpad.h"
#include "resource.h"
#include "settings.h"
#include "startup.h"
#include "strings.h"
#include "textview.h"
#include "theme.h"
#include "trace.h"
#include "workers.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#define EDITOR_CLASS L"QuickPadEditor"
#define STATUS_CLASS L"QuickPadStatus"
#define STATUS_BACKGROUND RGB(32, 32, 32)
#define STATUS_TEXT RGB(200, 200, 200)
#define VIEW_ID 1
#define ENCODING_GROUP 100
#define ENCODING_COMBO 101

/* Posted to a shown window after it appears: its taskbar button is added once the queue is empty. */
#define WM_EDITOR_ADD_TAB (WM_APP + 0x50)

/* Windows waiting in the pool sit here, cloaked, so they never cover anything or take input. */
#define PARK_POSITION (-32000)

/* The pool is refilled only after windows have stopped opening and closing for this long. */
#define POOL_QUIET_PERIOD 5000

enum EncodingChoice {
    CHOICE_UTF8,
    CHOICE_UTF8_WITH_MARK,
    CHOICE_UTF16LE,
    CHOICE_UTF16BE,
    CHOICE_ANSI,
};

static const wchar_t *const encodingNames[] = {
    L"UTF-8",
    L"UTF-8 with BOM",
    L"UTF-16 LE",
    L"UTF-16 BE",
    L"ANSI",
};

typedef struct Editor {
    struct Editor *next;
    HWND window;
    HWND view;
    wchar_t *path;
    TextFormat format;
    BOOL formatChanged;      /* line ending or encoding changed from the menu since the last save */
    BOOL modifiedShown;
    BOOL shown;
    BOOL pooled;
    BOOL dirty;              /* closed and hidden, still to be cleared and parked for the pool */
    BOOL raised;             /* placed above every window while still cloaked, so showing it needs no z-order change */
    BOOL topmost;
    BOOL fullScreen;
    LONG_PTR savedStyle;
    WINDOWPLACEMENT savedPlacement;
    HWND status;             /* created only while the status bar is on */
    wchar_t statusText[128];
} Editor;

static HINSTANCE instanceHandle;
static ATOM editorAtom;
static HMENU menuTemplate;
static HACCEL accelerators;
static HFONT editorFont;
static Editor *editors;
static BOOL comReady;
static BOOL residentProcess;
static HWND ownerWindow;
static ULONGLONG lastWindowChange;

/* One Find or Replace dialog serves every window; its search text and options carry over. */
static HWND findDialog;
static FINDREPLACEW findData;
static wchar_t findWhat[256];
static wchar_t replaceWith[256];
static UINT findMessage;

/* ---- Small helpers ------------------------------------------------------------------------- */

static HWND DialogOwner(const Editor *editor)
{
    return editor != NULL && editor->shown ? editor->window : NULL;
}

static void ShowFileError(const Editor *editor, const wchar_t *path, DWORD error)
{
    wchar_t *system = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, (LPWSTR)&system, 0, NULL);
    wchar_t *message = StringJoin(path, L"\n\n", system != NULL ? system : L"");
    MessageBoxW(DialogOwner(editor), message != NULL ? message : path, QP_APP_NAME, MB_ICONERROR);
    MemFree(message);
    if (system != NULL) {
        LocalFree(system);
    }
}

static BOOL IsDirty(Editor *editor)
{
    return editor->formatChanged || TextViewIsModified(editor->view);
}

static void UpdateTitle(Editor *editor)
{
    BOOL modified = IsDirty(editor);
    wchar_t *title = StringJoin(modified ? L"*" : L"", editor->path != NULL ? PathFileName(editor->path) : L"Untitled",
        L" - " QP_APP_NAME);
    if (title != NULL) {
        SetWindowTextW(editor->window, title);
        MemFree(title);
    }
    editor->modifiedShown = modified;
}

static void SetPath(Editor *editor, const wchar_t *path)
{
    MemFree(editor->path);
    editor->path = path != NULL ? StringCopy(path) : NULL;
}

static BOOL EnsureCom(void)
{
    if (!comReady) {
        comReady = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    }
    return comReady;
}

/* ---- Editor windows ------------------------------------------------------------------------ */

static Editor *FindByPath(const wchar_t *path)
{
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        if (editor->path != NULL && CompareStringOrdinal(editor->path, -1, path, -1, TRUE) == CSTR_EQUAL) {
            return editor;
        }
    }
    return NULL;
}

static void SetCloaked(HWND window, BOOL cloaked)
{
    DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloaked, sizeof cloaked);
}

/*
 * Windows gives visible top-level windows a taskbar button even when they are cloaked, unless they
 * have an owner. Pooled windows are owned by the host window; a window that opens loses its owner and
 * gets its button through this.
 */
static ITaskbarList *Taskbar(void)
{
    static ITaskbarList *taskbar;
    if (taskbar == NULL && EnsureCom()
        && SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList, (void **)&taskbar))
        && FAILED(ITaskbarList_HrInit(taskbar))) {
        ITaskbarList_Release(taskbar);
        taskbar = NULL;
    }
    return taskbar;
}

static void SetTaskbarButton(HWND window, BOOL present)
{
    ITaskbarList *taskbar = Taskbar();
    if (taskbar != NULL) {
        if (present) {
            ITaskbarList_AddTab(taskbar, window);
        } else {
            ITaskbarList_DeleteTab(taskbar, window);
        }
    }
}

/* Windows in the pool, counting closed ones still to be cleared and parked. */
static int PoolCount(void)
{
    int count = 0;
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        count += editor->pooled || editor->dirty;
    }
    return count;
}

static RECT WorkArea(void)
{
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    MONITORINFO info = { sizeof info };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &info);
    return info.rcWork;
}

static SIZE WindowSize(const RECT *work)
{
    SIZE size;
    int workWidth = work->right - work->left;
    int workHeight = work->bottom - work->top;
    size.cx = settings.windowWidth > 0 ? settings.windowWidth : workWidth * 3 / 5;
    size.cy = settings.windowHeight > 0 ? settings.windowHeight : workHeight * 2 / 3;
    size.cx = size.cx < workWidth ? size.cx : workWidth;
    size.cy = size.cy < workHeight ? size.cy : workHeight;
    return size;
}

/* Centered on the monitor under the mouse, each further window a caption height lower and to the right. */
static RECT NewWindowFrame(void)
{
    RECT work = WorkArea();
    SIZE size = WindowSize(&work);
    int step = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    int offset = (EditorShownCount() % 8) * step;
    int left = work.left + (work.right - work.left - size.cx) / 2 + offset;
    int top = work.top + (work.bottom - work.top - size.cy) / 2 + offset;
    left = left + size.cx > work.right ? work.right - size.cx : left;
    top = top + size.cy > work.bottom ? work.bottom - size.cy : top;
    left = left < work.left ? work.left : left;
    top = top < work.top ? work.top : top;
    RECT frame = { left, top, left + size.cx, top + size.cy };
    return frame;
}

/* Shows an editor off screen without activating it, still cloaked, and draws it so it is ready to appear. */
static void ParkEditor(Editor *editor)
{
    RECT work = WorkArea();
    SIZE size = WindowSize(&work);
    WINDOWPLACEMENT placement = { sizeof placement };
    GetWindowPlacement(editor->window, &placement);
    placement.flags = 0;
    placement.showCmd = SW_SHOWNOACTIVATE;
    SetRect(&placement.rcNormalPosition, PARK_POSITION, PARK_POSITION, PARK_POSITION + size.cx, PARK_POSITION + size.cy);
    SetWindowLongPtrW(editor->window, GWLP_HWNDPARENT, (LONG_PTR)ownerWindow);
    SetWindowPlacement(editor->window, &placement);
    RedrawWindow(editor->window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
    editor->pooled = TRUE;
}

/*
 * A window gets only the titles of its menus; each menu is filled from the one shared template the
 * first time it opens, so creating a window does not build every menu item.
 */
static HMENU CreateMenuBar(void)
{
    HMENU bar = CreateMenu();
    int count = menuTemplate != NULL ? GetMenuItemCount(menuTemplate) : 0;
    for (int i = 0; bar != NULL && i < count; ++i) {
        wchar_t title[64];
        HMENU popup = CreatePopupMenu();
        if (popup != NULL && GetMenuStringW(menuTemplate, (UINT)i, title, ARRAYSIZE(title), MF_BYPOSITION) > 0) {
            AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)popup, title);
        }
    }
    return bar;
}

static void CopyMenuItems(HMENU source, HMENU target)
{
    int count = GetMenuItemCount(source);
    for (int i = 0; i < count; ++i) {
        wchar_t text[128];
        MENUITEMINFOW info = { sizeof info };
        info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING | MIIM_SUBMENU;
        info.dwTypeData = text;
        info.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(source, (UINT)i, TRUE, &info)) {
            continue;
        }
        if ((info.fType & MFT_SEPARATOR) != 0) {
            info.fMask = MIIM_FTYPE;
        } else if (info.hSubMenu != NULL) {
            HMENU submenu = CreatePopupMenu();
            if (submenu == NULL) {
                continue;
            }
            CopyMenuItems(info.hSubMenu, submenu);
            info.hSubMenu = submenu;
        }
        info.dwTypeData = text;
        InsertMenuItemW(target, (UINT)i, TRUE, &info);
    }
}

static Editor *CreateEditor(void)
{
    Editor *editor = MemAllocZero(sizeof *editor);
    if (editor == NULL) {
        return NULL;
    }
    editor->format = TextDefaultFormat();

    RECT work = WorkArea();
    SIZE size = WindowSize(&work);
    HMENU menu = CreateMenuBar();
    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, EDITOR_CLASS, L"Untitled - " QP_APP_NAME, WS_OVERLAPPEDWINDOW,
        PARK_POSITION, PARK_POSITION, size.cx, size.cy, ownerWindow, menu, instanceHandle, editor);
    if (window == NULL) {
        if (menu != NULL) {
            DestroyMenu(menu);
        }
        MemFree(editor);
        return NULL;
    }
    editor->next = editors;
    editors = editor;
    SetCloaked(window, TRUE);
    ParkEditor(editor);
    return editor;
}

/* Turns a closed window back into an empty pooled one. */
static void ResetEditor(Editor *editor)
{
    TRACE("reset");
    editor->dirty = FALSE;
    TextViewClear(editor->view);
    TRACE("TextViewClear");
    SetPath(editor, NULL);
    editor->format = TextDefaultFormat();
    editor->formatChanged = FALSE;
    UpdateTitle(editor);
    ParkEditor(editor);
    TRACE("ParkEditor");
    TRACE_DUMP("reset");
}

/* A drawn editor from the pool, a closed one made ready again, or a new one when there is neither. */
static Editor *TakeEditor(void)
{
    Editor *dirty = NULL;
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        if (editor->pooled) {
            return editor;
        }
        if (editor->dirty && dirty == NULL) {
            dirty = editor;
        }
    }
    if (dirty != NULL) {
        ResetEditor(dirty);
        return dirty;
    }
    return CreateEditor();
}

static void ShowEditor(Editor *editor)
{
    HWND window = editor->window;
    if (editor->shown) {
        if (IsIconic(window)) {
            ShowWindow(window, SW_RESTORE);
        }
        SetForegroundWindow(window);
        return;
    }

    /*
     * Everything happens while the window is still cloaked, so it appears complete in one frame.
     * The next pooled window usually already waits at this frame and only needs raising.
     */
    RECT frame = NewWindowFrame();
    RECT current;
    GetWindowRect(window, &current);
    UINT keepFrame = EqualRect(&current, &frame) ? SWP_NOMOVE | SWP_NOSIZE : 0;
    TRACE(keepFrame != 0 ? "frame kept" : "frame moves");
    editor->pooled = FALSE;
    editor->shown = TRUE;
    lastWindowChange = GetTickCount64();
    SetWindowLongPtrW(window, GWLP_HWNDPARENT, 0);
    TRACE("owner cleared");
    /*
     * The window usually waits at this frame above every other window already (see EditorIdle); a
     * cloaked window there is neither seen nor hit by the mouse. Otherwise it is put there now.
     */
    if (keepFrame == 0 || !editor->raised) {
        SetWindowPos(window, HWND_TOPMOST, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
            SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_DEFERERASE | keepFrame);
        editor->raised = TRUE;
        TRACE("SetWindowPos");
    }
    RedrawWindow(window, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
    TRACE("RedrawWindow");
    SetCloaked(window, FALSE);
    TRACE("uncloak");
    /* The rest of the file is decoded on the worker threads from here on, while the window is already on screen. */
    TextViewStartLoad(editor->view);
    SetForegroundWindow(window);
    TRACE("SetForegroundWindow");
    /* Leaving the top band and the taskbar button, a call into Explorer, wait until every file of this launch is on screen. */
    PostMessageW(window, WM_EDITOR_ADD_TAB, 0, 0);
    TRACE_DUMP("open");
}

/* Returns a closed editor to the pool, or destroys it when the pool is full. */
static void CloseEditor(Editor *editor)
{
    HWND window = editor->window;
    lastWindowChange = GetTickCount64();
    if (editor->fullScreen) {
        SetWindowLongPtrW(window, GWL_STYLE, editor->savedStyle);
        SetWindowPlacement(window, &editor->savedPlacement);
        editor->fullScreen = FALSE;
    }
    if (editor->topmost || editor->raised) {
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        editor->topmost = FALSE;
        editor->raised = FALSE;
    }
    if (findDialog != NULL && findData.hwndOwner == window) {
        DestroyWindow(findDialog);
        findDialog = NULL;
    }
    WINDOWPLACEMENT placement = { sizeof placement };
    if (editor->shown && GetWindowPlacement(window, &placement)) {
        int width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
        int height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
        if (width != settings.windowWidth || height != settings.windowHeight) {
            settings.windowWidth = width;
            settings.windowHeight = height;
            SettingsSave();
        }
    }

    editor->shown = FALSE;
    TRACE("close");
    if (!residentProcess || PoolCount() >= settings.poolSize) {
        DestroyWindow(window);
        TRACE("DestroyWindow");
        TRACE_DUMP("close (destroyed)");
        return;
    }

    /* The window disappears now; clearing its text and parking it for the pool waits for idle time. */
    SetCloaked(window, TRUE);
    TRACE("cloak");
    SetTaskbarButton(window, FALSE);
    TRACE("DeleteTab");
    ShowWindow(window, SW_HIDE);
    TRACE("hide");
    editor->dirty = TRUE;
    TRACE_DUMP("close");
}

/* Loads a file into an editor. A missing file can become a new, empty document with that name. */
static BOOL LoadInto(Editor *editor, const wchar_t *path)
{
    TextLoad *load = NULL;
    DWORD error = 0;

    /* The load writes the detected format into the editor and decodes the rest while the window shows. */
    TRACE("LoadInto");
    BOOL begun = FileBeginLoad(path, &editor->format, editor->view, WM_TEXTVIEW_LOAD_DONE, &load, &error);
    TRACE("FileBeginLoad");
    if (!begun) {
        if (error != ERROR_FILE_NOT_FOUND) {
            ShowFileError(editor, path, error);
            return FALSE;
        }
        wchar_t *question = StringJoin(L"Cannot find the file\n", path, L"\n\nDo you want to create a new file?");
        int answer = MessageBoxW(DialogOwner(editor), question != NULL ? question : path, QP_APP_NAME,
            MB_YESNO | MB_ICONQUESTION);
        MemFree(question);
        if (answer != IDYES) {
            return FALSE;
        }
        TextViewClear(editor->view);
        editor->format = TextDefaultFormat();
    } else {
        TextViewSetLoad(editor->view, load);
        TRACE("TextViewSetLoad");
        if (editor->shown) {
            TextViewStartLoad(editor->view);
        }
    }

    SetPath(editor, path);
    editor->formatChanged = FALSE;
    UpdateTitle(editor);
    TRACE("UpdateTitle");
    return TRUE;
}

/* Opens a file chosen in this window: into it when it is an untouched new document, otherwise in its own window. */
static void OpenFromEditor(Editor *editor, const wchar_t *path)
{
    size_t length = 0;
    TextViewGetText(editor->view, &length);
    if (editor->path == NULL && !TextViewIsModified(editor->view) && length == 0 && FindByPath(path) == NULL) {
        LoadInto(editor, path);
    } else {
        EditorOpenFile(path);
    }
}

static void ShowOpenDialog(Editor *editor)
{
    IFileOpenDialog *dialog = NULL;
    if (!EnsureCom() || FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
            &IID_IFileOpenDialog, (void **)&dialog))) {
        return;
    }

    COMDLG_FILTERSPEC filters[] = {
        { L"All Files (*.*)", L"*.*" },
        { L"Text Documents (*.txt)", L"*.txt" },
    };
    IFileOpenDialog_SetFileTypes(dialog, ARRAYSIZE(filters), filters);
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(dialog, &options);
    IFileOpenDialog_SetOptions(dialog, options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);

    IShellItemArray *items = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dialog, editor->window)) && SUCCEEDED(IFileOpenDialog_GetResults(dialog, &items))) {
        DWORD count = 0;
        IShellItemArray_GetCount(items, &count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem *item = NULL;
            if (FAILED(IShellItemArray_GetItemAt(items, i, &item))) {
                continue;
            }
            wchar_t *path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                OpenFromEditor(editor, path);
                CoTaskMemFree(path);
            }
            IShellItem_Release(item);
        }
        IShellItemArray_Release(items);
    }
    IFileOpenDialog_Release(dialog);
}

static DWORD ChoiceFromFormat(TextFormat format)
{
    switch (format.encoding) {
    case TEXT_ENCODING_UTF16LE:
        return CHOICE_UTF16LE;
    case TEXT_ENCODING_UTF16BE:
        return CHOICE_UTF16BE;
    case TEXT_ENCODING_ANSI:
        return CHOICE_ANSI;
    default:
        return format.byteOrderMark ? CHOICE_UTF8_WITH_MARK : CHOICE_UTF8;
    }
}

/* Applies the encoding picked in Save As; picking the current encoding keeps the file's own byte order mark state. */
static TextFormat FormatFromChoice(TextFormat format, DWORD choice)
{
    if (choice == ChoiceFromFormat(format)) {
        return format;
    }
    format.byteOrderMark = choice != CHOICE_UTF8 && choice != CHOICE_ANSI;
    format.encoding = choice == CHOICE_UTF16LE ? TEXT_ENCODING_UTF16LE
        : choice == CHOICE_UTF16BE ? TEXT_ENCODING_UTF16BE
        : choice == CHOICE_ANSI ? TEXT_ENCODING_ANSI
        : TEXT_ENCODING_UTF8;
    return format;
}

static BOOL SaveTo(Editor *editor, const wchar_t *path, TextFormat format)
{
    size_t length = 0;
    const wchar_t *text = TextViewGetText(editor->view, &length);
    size_t size = 0;
    BOOL lossy = FALSE;
    unsigned char *bytes = text != NULL ? TextEncode(text, length, format, &size, &lossy) : NULL;
    if (bytes != NULL && lossy) {
        int answer = MessageBoxW(editor->window,
            L"This document contains characters the ANSI encoding cannot store. They would be saved as question marks.\n\n"
            L"Do you want to save it as UTF-8 instead?",
            QP_APP_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
        if (answer == IDCANCEL) {
            MemFree(bytes);
            return FALSE;
        }
        if (answer == IDYES) {
            MemFree(bytes);
            format.encoding = TEXT_ENCODING_UTF8;
            format.byteOrderMark = FALSE;
            bytes = TextEncode(text, length, format, &size, &lossy);
        }
    }
    if (bytes == NULL) {
        ShowFileError(editor, path, ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    DWORD error = 0;
    BOOL written = FileWrite(path, bytes, size, &error);
    MemFree(bytes);
    if (!written) {
        ShowFileError(editor, path, error);
        return FALSE;
    }

    if (editor->path == NULL || CompareStringOrdinal(editor->path, -1, path, -1, FALSE) != CSTR_EQUAL) {
        SetPath(editor, path);
    }
    editor->format = format;
    editor->formatChanged = FALSE;
    TextViewMarkSaved(editor->view);
    UpdateTitle(editor);
    return TRUE;
}

static BOOL SaveAs(Editor *editor)
{
    IFileSaveDialog *dialog = NULL;
    if (!EnsureCom() || FAILED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
            &IID_IFileSaveDialog, (void **)&dialog))) {
        return FALSE;
    }

    COMDLG_FILTERSPEC filters[] = {
        { L"Text Documents (*.txt)", L"*.txt" },
        { L"All Files (*.*)", L"*.*" },
    };
    IFileSaveDialog_SetFileTypes(dialog, ARRAYSIZE(filters), filters);
    IFileSaveDialog_SetDefaultExtension(dialog, L"txt");
    FILEOPENDIALOGOPTIONS options = 0;
    IFileSaveDialog_GetOptions(dialog, &options);
    IFileSaveDialog_SetOptions(dialog, options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN);

    if (editor->path != NULL) {
        wchar_t *folder = StringCopy(editor->path);
        if (folder != NULL) {
            wchar_t *name = (wchar_t *)PathFileName(folder);
            if (name > folder) {
                name[-1] = 0;
                IShellItem *folderItem = NULL;
                if (SUCCEEDED(SHCreateItemFromParsingName(folder, NULL, &IID_IShellItem, (void **)&folderItem))) {
                    IFileSaveDialog_SetFolder(dialog, folderItem);
                    IShellItem_Release(folderItem);
                }
            }
            MemFree(folder);
        }
        IFileSaveDialog_SetFileName(dialog, PathFileName(editor->path));
    } else {
        IFileSaveDialog_SetFileName(dialog, L"Untitled.txt");
    }

    IFileDialogCustomize *customize = NULL;
    if (SUCCEEDED(IFileSaveDialog_QueryInterface(dialog, &IID_IFileDialogCustomize, (void **)&customize))) {
        IFileDialogCustomize_StartVisualGroup(customize, ENCODING_GROUP, L"&Encoding:");
        IFileDialogCustomize_AddComboBox(customize, ENCODING_COMBO);
        for (DWORD i = 0; i < ARRAYSIZE(encodingNames); ++i) {
            IFileDialogCustomize_AddControlItem(customize, ENCODING_COMBO, i, encodingNames[i]);
        }
        IFileDialogCustomize_SetSelectedControlItem(customize, ENCODING_COMBO, ChoiceFromFormat(editor->format));
        IFileDialogCustomize_EndVisualGroup(customize);
    }

    BOOL saved = FALSE;
    IShellItem *item = NULL;
    if (SUCCEEDED(IFileSaveDialog_Show(dialog, editor->window)) && SUCCEEDED(IFileSaveDialog_GetResult(dialog, &item))) {
        wchar_t *path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            DWORD choice = ChoiceFromFormat(editor->format);
            if (customize != NULL) {
                IFileDialogCustomize_GetSelectedControlItem(customize, ENCODING_COMBO, &choice);
            }
            saved = SaveTo(editor, path, FormatFromChoice(editor->format, choice));
            CoTaskMemFree(path);
        }
        IShellItem_Release(item);
    }
    if (customize != NULL) {
        IFileDialogCustomize_Release(customize);
    }
    IFileSaveDialog_Release(dialog);
    return saved;
}

static BOOL Save(Editor *editor)
{
    return editor->path != NULL ? SaveTo(editor, editor->path, editor->format) : SaveAs(editor);
}

/* Offers to save unsaved changes; FALSE when the user cancels. */
static BOOL ConfirmDiscard(Editor *editor)
{
    if (!IsDirty(editor)) {
        return TRUE;
    }
    SetForegroundWindow(editor->window);
    wchar_t *question = StringJoin(L"Do you want to save changes to ", editor->path != NULL ? editor->path : L"Untitled", L"?");
    int answer = MessageBoxW(editor->window, question != NULL ? question : L"Do you want to save changes?", QP_APP_NAME,
        MB_YESNOCANCEL | MB_ICONWARNING);
    MemFree(question);
    if (answer == IDYES) {
        return Save(editor);
    }
    return answer == IDNO;
}

static void UpdateMenu(Editor *editor, HMENU menu)
{
    size_t start = 0;
    size_t end = 0;
    TextViewGetSelection(editor->view, &start, &end);
    UINT selection = start != end ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(menu, IDM_EDIT_UNDO, TextViewCanUndo(editor->view) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_EDIT_REDO, TextViewCanRedo(editor->view) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(menu, IDM_EDIT_CUT, selection);
    EnableMenuItem(menu, IDM_EDIT_COPY, selection);
    EnableMenuItem(menu, IDM_EDIT_DELETE, selection);
    EnableMenuItem(menu, IDM_EDIT_PASTE, TextViewCanPaste(editor->view) ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(menu, IDM_FORMAT_WORD_WRAP, settings.wordWrap ? MF_CHECKED : MF_UNCHECKED);
    UINT hasPath = editor->path != NULL ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(menu, IDM_FILE_RELOAD, hasPath);
    EnableMenuItem(menu, IDM_FILE_OPEN_FOLDER, hasPath);
    EnableMenuItem(menu, IDM_FILE_COPY_PATH, hasPath);
    EnableMenuItem(menu, IDM_EDIT_UPPERCASE, selection);
    EnableMenuItem(menu, IDM_EDIT_LOWERCASE, selection);
    CheckMenuItem(menu, IDM_FORMAT_AUTO_INDENT, settings.autoIndent ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem(menu, IDM_FORMAT_TAB_2, IDM_FORMAT_TAB_8,
        settings.tabSize == 2 ? IDM_FORMAT_TAB_2 : settings.tabSize == 4 ? IDM_FORMAT_TAB_4 : IDM_FORMAT_TAB_8, MF_BYCOMMAND);
    CheckMenuRadioItem(menu, IDM_FORMAT_CRLF, IDM_FORMAT_CR, IDM_FORMAT_CRLF + (UINT)editor->format.lineEnding, MF_BYCOMMAND);
    CheckMenuRadioItem(menu, IDM_FORMAT_UTF8, IDM_FORMAT_ANSI, IDM_FORMAT_UTF8 + ChoiceFromFormat(editor->format), MF_BYCOMMAND);
    CheckMenuItem(menu, IDM_VIEW_STATUS_BAR, settings.statusBar ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_VIEW_ALWAYS_ON_TOP, editor->topmost ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, IDM_VIEW_FULL_SCREEN, editor->fullScreen ? MF_CHECKED : MF_UNCHECKED);
    if (GetMenuState(menu, IDM_START_WITH_WINDOWS, MF_BYCOMMAND) != (UINT)-1) {
        CheckMenuItem(menu, IDM_START_WITH_WINDOWS, StartupIsEnabled() ? MF_CHECKED : MF_UNCHECKED);
    }
}

static void ToggleWordWrap(void)
{
    settings.wordWrap = !settings.wordWrap;
    SettingsSave();
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        TextViewSetWordWrap(editor->view, settings.wordWrap);
    }
}

/* ---- Find, replace and go to --------------------------------------------------------------- */

static UINT_PTR CALLBACK FindDialogHook(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    INT_PTR themed = 0;
    if (ThemeDialogMessage(dialog, message, wParam, lParam, &themed)) {
        return (UINT_PTR)themed;
    }
    if (message == WM_INITDIALOG) {
        ThemePrepareDialog(dialog);
        return TRUE;
    }
    return FALSE;
}

static SearchOptions FindOptions(void)
{
    SearchOptions options = { (findData.Flags & FR_MATCHCASE) != 0, (findData.Flags & FR_WHOLEWORD) != 0 };
    return options;
}

static void ShowNotFound(Editor *editor)
{
    wchar_t *message = StringJoin(L"Cannot find \"", findWhat, L"\"");
    MessageBoxW(findDialog != NULL ? findDialog : editor->window, message != NULL ? message : findWhat, QP_APP_NAME,
        MB_ICONINFORMATION);
    MemFree(message);
}

static void ShowFindDialog(Editor *editor, BOOL replace)
{
    if (findDialog != NULL) {
        DestroyWindow(findDialog);
        findDialog = NULL;
    }

    /* A selection on one line becomes the text to find. */
    size_t start = 0;
    size_t end = 0;
    TextViewGetSelection(editor->view, &start, &end);
    if (end > start && end - start < ARRAYSIZE(findWhat)) {
        size_t length = 0;
        const wchar_t *text = TextViewGetText(editor->view, &length);
        BOOL oneLine = text != NULL;
        for (size_t i = start; oneLine && i < end; ++i) {
            oneLine = text[i] != L'\n';
        }
        if (oneLine) {
            memcpy(findWhat, text + start, (end - start) * sizeof(wchar_t));
            findWhat[end - start] = 0;
        }
    }

    findData.lStructSize = sizeof findData;
    findData.hwndOwner = editor->window;
    findData.lpstrFindWhat = findWhat;
    findData.wFindWhatLen = ARRAYSIZE(findWhat);
    findData.lpstrReplaceWith = replaceWith;
    findData.wReplaceWithLen = ARRAYSIZE(replaceWith);
    findData.Flags = (findData.Flags & (FR_MATCHCASE | FR_WHOLEWORD)) | FR_DOWN | FR_ENABLEHOOK;
    findData.lpfnHook = FindDialogHook;
    findDialog = replace ? ReplaceTextW(&findData) : FindTextW(&findData);
}

static void FindNext(Editor *editor, BOOL down)
{
    if (findWhat[0] == 0) {
        ShowFindDialog(editor, FALSE);
    } else if (!TextViewFind(editor->view, findWhat, FindOptions(), down)) {
        ShowNotFound(editor);
    }
}

static void HandleFindMessage(Editor *editor, const FINDREPLACEW *data)
{
    if ((data->Flags & FR_DIALOGTERM) != 0) {
        findDialog = NULL;
    } else if ((data->Flags & FR_FINDNEXT) != 0) {
        FindNext(editor, (data->Flags & FR_DOWN) != 0);
    } else if ((data->Flags & FR_REPLACE) != 0) {
        if (!TextViewReplace(editor->view, findWhat, replaceWith, FindOptions())) {
            ShowNotFound(editor);
        }
    } else if ((data->Flags & FR_REPLACEALL) != 0) {
        if (TextViewReplaceAll(editor->view, findWhat, replaceWith, FindOptions()) == 0) {
            ShowNotFound(editor);
        }
    }
}

static INT_PTR CALLBACK GoToProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    INT_PTR themed = 0;
    if (ThemeDialogMessage(dialog, message, wParam, lParam, &themed)) {
        return themed;
    }

    Editor *editor = (Editor *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        editor = (Editor *)lParam;
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        ThemePrepareDialog(dialog);
        SendDlgItemMessageW(dialog, IDC_GOTO_LINE, EM_LIMITTEXT, 10, 0);
        SetDlgItemInt(dialog, IDC_GOTO_LINE, (UINT)(TextViewCaretLine(editor->view) + 1), FALSE);
        SendDlgItemMessageW(dialog, IDC_GOTO_LINE, EM_SETSEL, 0, -1);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            BOOL valid = FALSE;
            UINT line = GetDlgItemInt(dialog, IDC_GOTO_LINE, &valid, FALSE);
            if (!valid || line == 0 || line > TextViewLineCount(editor->view)) {
                MessageBoxW(dialog, L"The line number is beyond the total number of lines.", L"Go To Line", MB_ICONWARNING);
                HWND edit = GetDlgItem(dialog, IDC_GOTO_LINE);
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
                return TRUE;
            }
            EndDialog(dialog, IDOK);
            TextViewGoToLine(editor->view, line - 1);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- Status bar, text size and window commands --------------------------------------------- */

static HFONT CreateEditorFont(void);

static HFONT statusFont;
static int statusHeight;
static BOOL statusClassReady;

static LRESULT CALLBACK StatusProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message != WM_PAINT) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    const Editor *editor = (const Editor *)GetWindowLongPtrW(window, GWLP_USERDATA);
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(window, &paint);
    RECT client;
    GetClientRect(window, &client);
    SetBkColor(dc, STATUS_BACKGROUND);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &client, NULL, 0, NULL);
    if (editor != NULL) {
        HGDIOBJ previous = SelectObject(dc, statusFont);
        SetTextColor(dc, STATUS_TEXT);
        SetBkMode(dc, TRANSPARENT);
        client.right -= GetSystemMetrics(SM_CXVSCROLL);
        DrawTextW(dc, editor->statusText, -1, &client, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    EndPaint(window, &paint);
    return 0;
}

/* The status bar class and font are made the first time a status bar is turned on. */
static BOOL PrepareStatusBar(void)
{
    if (statusClassReady) {
        return TRUE;
    }
    if (statusFont == NULL) {
        NONCLIENTMETRICSW metrics = { sizeof metrics };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
        statusFont = CreateFontIndirectW(&metrics.lfStatusFont);
        HDC screen = GetDC(NULL);
        HGDIOBJ previous = SelectObject(screen, statusFont);
        TEXTMETRICW text;
        GetTextMetricsW(screen, &text);
        SelectObject(screen, previous);
        ReleaseDC(NULL, screen);
        statusHeight = text.tmHeight + text.tmHeight / 2;
    }
    WNDCLASSEXW windowClass = { sizeof windowClass };
    windowClass.lpfnWndProc = StatusProc;
    windowClass.hInstance = instanceHandle;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.lpszClassName = STATUS_CLASS;
    statusClassReady = statusFont != NULL && RegisterClassExW(&windowClass) != 0;
    return statusClassReady;
}

static const wchar_t *LineEndingName(LineEnding ending)
{
    return ending == LINE_ENDING_LF ? L"Unix (LF)" : ending == LINE_ENDING_CR ? L"Macintosh (CR)" : L"Windows (CRLF)";
}

static void UpdateStatus(Editor *editor)
{
    if (editor->status == NULL) {
        return;
    }
    size_t caret = TextViewCaretPosition(editor->view);
    size_t line = TextViewLineFromPosition(editor->view, caret);
    size_t column = caret - TextViewLineStart(editor->view, line) + 1;
    wsprintfW(editor->statusText, L"Ln %lu, Col %lu        %d%%        %s        %s", (unsigned long)(line + 1),
        (unsigned long)column, settings.zoom, LineEndingName(editor->format.lineEnding), encodingNames[ChoiceFromFormat(editor->format)]);
    InvalidateRect(editor->status, NULL, FALSE);
}

static void LayoutEditor(Editor *editor)
{
    RECT client;
    GetClientRect(editor->window, &client);
    int height = editor->status != NULL ? statusHeight : 0;
    int viewHeight = client.bottom - height > 0 ? client.bottom - height : 0;
    MoveWindow(editor->view, 0, 0, client.right, viewHeight, TRUE);
    if (editor->status != NULL) {
        MoveWindow(editor->status, 0, viewHeight, client.right, height, TRUE);
    }
}

static void ShowStatusBar(Editor *editor, BOOL show)
{
    if (show && editor->status == NULL && PrepareStatusBar()) {
        editor->status = CreateWindowExW(0, STATUS_CLASS, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, editor->window, NULL,
            instanceHandle, NULL);
        if (editor->status != NULL) {
            SetWindowLongPtrW(editor->status, GWLP_USERDATA, (LONG_PTR)editor);
        }
    } else if (!show && editor->status != NULL) {
        DestroyWindow(editor->status);
        editor->status = NULL;
    }
    TextViewNotifySelection(editor->view, editor->status != NULL);
    UpdateStatus(editor);
    LayoutEditor(editor);
}

static void ToggleStatusBar(void)
{
    settings.statusBar = !settings.statusBar;
    SettingsSave();
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        ShowStatusBar(editor, settings.statusBar);
    }
}

/* Every editor shares one font; a new size or face replaces it in all of them. */
static void ApplyEditorFont(void)
{
    HFONT font = CreateEditorFont();
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        TextViewSetFont(editor->view, font);
        UpdateStatus(editor);
    }
    HFONT previous = editorFont;
    editorFont = font;
    if (previous != NULL && previous != (HFONT)GetStockObject(ANSI_FIXED_FONT)) {
        DeleteObject(previous);
    }
}

/* steps 0 restores 100 percent; each step is ten percent. */
static void Zoom(int steps)
{
    int zoom = steps == 0 ? 100 : settings.zoom + steps * 10;
    zoom = zoom < SETTINGS_ZOOM_MIN ? SETTINGS_ZOOM_MIN : zoom > SETTINGS_ZOOM_MAX ? SETTINGS_ZOOM_MAX : zoom;
    if (zoom != settings.zoom) {
        settings.zoom = zoom;
        SettingsSave();
        ApplyEditorFont();
    }
}

static void ChooseEditorFont(Editor *editor)
{
    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    LOGFONTW font = { 0 };
    font.lfHeight = -MulDiv(settings.fontSize, dpi, 72);
    font.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(font.lfFaceName, settings.fontName, LF_FACESIZE);

    CHOOSEFONTW choose = { sizeof choose };
    choose.hwndOwner = editor->window;
    choose.lpLogFont = &font;
    choose.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_ENABLEHOOK;
    choose.lpfnHook = FindDialogHook;
    if (!ChooseFontW(&choose)) {
        return;
    }
    int size = choose.iPointSize / 10;
    settings.fontSize = size < SETTINGS_FONT_SIZE_MIN ? SETTINGS_FONT_SIZE_MIN : size > SETTINGS_FONT_SIZE_MAX ? SETTINGS_FONT_SIZE_MAX : size;
    lstrcpynW(settings.fontName, font.lfFaceName, LF_FACESIZE);
    SettingsSave();
    ApplyEditorFont();
}

static void SetTabSize(int cells)
{
    settings.tabSize = cells;
    SettingsSave();
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        TextViewSetTabSize(editor->view, cells);
    }
}

static void ToggleAutoIndent(void)
{
    settings.autoIndent = !settings.autoIndent;
    SettingsSave();
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        TextViewSetAutoIndent(editor->view, settings.autoIndent);
    }
}

/* The line ending and encoding picked here are used by the next save. */
static void ChangeFormat(Editor *editor, TextFormat format)
{
    if (format.encoding == editor->format.encoding && format.byteOrderMark == editor->format.byteOrderMark
        && format.lineEnding == editor->format.lineEnding) {
        return;
    }
    editor->format = format;
    editor->formatChanged = TRUE;
    UpdateTitle(editor);
    UpdateStatus(editor);
}

static void ToggleTopmost(Editor *editor)
{
    editor->topmost = !editor->topmost;
    SetWindowPos(editor->window, editor->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ToggleFullScreen(Editor *editor)
{
    HWND window = editor->window;
    if (editor->fullScreen) {
        SetWindowLongPtrW(window, GWL_STYLE, editor->savedStyle);
        SetWindowPlacement(window, &editor->savedPlacement);
        SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        editor->fullScreen = FALSE;
        return;
    }
    MONITORINFO monitor = { sizeof monitor };
    editor->savedPlacement.length = sizeof editor->savedPlacement;
    if (!GetWindowPlacement(window, &editor->savedPlacement)
        || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    editor->savedStyle = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, editor->savedStyle & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
    SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left,
        monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    editor->fullScreen = TRUE;
}

/* ---- Line and text commands ---------------------------------------------------------------- */

/* The lines the selection touches; a selection ending at the start of a line leaves that line out. */
static void SelectedLines(Editor *editor, size_t *first, size_t *last)
{
    size_t start = 0;
    size_t end = 0;
    TextViewGetSelection(editor->view, &start, &end);
    *first = TextViewLineFromPosition(editor->view, start);
    *last = TextViewLineFromPosition(editor->view, end);
    if (*last > *first && end == TextViewLineStart(editor->view, *last)) {
        --*last;
    }
}

/* Replaces [start, end) with the pieces joined, then selects [anchor, caret]. */
static void ReplaceWithPieces(Editor *editor, size_t start, size_t end, const wchar_t *first, size_t firstLength,
    const wchar_t *second, size_t secondLength, size_t anchor, size_t caret)
{
    wchar_t *text = MemAlloc((firstLength + secondLength + 1) * sizeof(wchar_t));
    if (text == NULL) {
        MessageBeep(MB_ICONERROR);
        return;
    }
    memcpy(text, first, firstLength * sizeof(wchar_t));
    memcpy(text + firstLength, second, secondLength * sizeof(wchar_t));
    if (TextViewReplaceRange(editor->view, start, end, text, firstLength + secondLength)) {
        TextViewSetSelection(editor->view, anchor, caret);
    }
    MemFree(text);
}

static void DuplicateLines(Editor *editor)
{
    size_t first = 0;
    size_t last = 0;
    SelectedLines(editor, &first, &last);
    size_t start = 0;
    size_t end = 0;
    TextViewGetSelection(editor->view, &start, &end);
    size_t blockStart = TextViewLineStart(editor->view, first);
    size_t blockEnd = TextViewLineEnd(editor->view, last);
    size_t length = 0;
    const wchar_t *text = TextViewGetText(editor->view, &length);
    if (text == NULL) {
        return;
    }
    size_t shift = blockEnd - blockStart + 1;
    ReplaceWithPieces(editor, blockEnd, blockEnd, L"\n", 1, text + blockStart, blockEnd - blockStart, start + shift, end + shift);
}

static void DeleteLines(Editor *editor)
{
    size_t first = 0;
    size_t last = 0;
    SelectedLines(editor, &first, &last);
    size_t from = TextViewLineStart(editor->view, first);
    size_t to = 0;
    if (last + 1 < TextViewLineCount(editor->view)) {
        to = TextViewLineStart(editor->view, last + 1);
    } else {
        to = TextViewLineEnd(editor->view, last);
        from = first > 0 ? TextViewLineEnd(editor->view, first - 1) : from;
    }
    TextViewReplaceRange(editor->view, from, to, NULL, 0);
}

static void MoveLines(Editor *editor, BOOL down)
{
    size_t first = 0;
    size_t last = 0;
    SelectedLines(editor, &first, &last);
    if ((!down && first == 0) || (down && last + 1 >= TextViewLineCount(editor->view))) {
        return;
    }
    size_t start = 0;
    size_t end = 0;
    TextViewGetSelection(editor->view, &start, &end);
    size_t length = 0;
    const wchar_t *text = TextViewGetText(editor->view, &length);
    if (text == NULL) {
        return;
    }
    size_t blockStart = TextViewLineStart(editor->view, first);
    size_t blockEnd = TextViewLineEnd(editor->view, last);
    if (down) {
        size_t nextStart = blockEnd + 1;
        size_t nextEnd = TextViewLineEnd(editor->view, last + 1);
        size_t shift = nextEnd - nextStart + 1;
        wchar_t *next = MemAlloc((nextEnd - nextStart + 1) * sizeof(wchar_t));
        if (next != NULL) {
            memcpy(next, text + nextStart, (nextEnd - nextStart) * sizeof(wchar_t));
            next[nextEnd - nextStart] = L'\n';
            ReplaceWithPieces(editor, blockStart, nextEnd, next, nextEnd - nextStart + 1, text + blockStart,
                blockEnd - blockStart, start + shift, end + shift);
            MemFree(next);
        }
    } else {
        size_t previousStart = TextViewLineStart(editor->view, first - 1);
        size_t shift = blockStart - previousStart;
        wchar_t *block = MemAlloc((blockEnd - blockStart + 1) * sizeof(wchar_t));
        if (block != NULL) {
            memcpy(block, text + blockStart, (blockEnd - blockStart) * sizeof(wchar_t));
            block[blockEnd - blockStart] = L'\n';
            ReplaceWithPieces(editor, previousStart, blockEnd, block, blockEnd - blockStart + 1, text + previousStart,
                blockStart - 1 - previousStart, start - shift, end - shift);
            MemFree(block);
        }
    }
}

static void JoinLines(Editor *editor)
{
    size_t first = 0;
    size_t last = 0;
    SelectedLines(editor, &first, &last);
    if (first == last) {
        if (last + 1 >= TextViewLineCount(editor->view)) {
            return;
        }
        ++last;
    }
    size_t from = TextViewLineStart(editor->view, first);
    size_t to = TextViewLineEnd(editor->view, last);
    size_t length = 0;
    const wchar_t *text = TextViewGetText(editor->view, &length);
    wchar_t *joined = text != NULL ? MemAlloc((to - from + 1) * sizeof(wchar_t)) : NULL;
    if (joined == NULL) {
        return;
    }
    for (size_t i = from; i < to; ++i) {
        joined[i - from] = text[i] == L'\n' ? L' ' : text[i];
    }
    TextViewReplaceRange(editor->view, from, to, joined, to - from);
    MemFree(joined);
}

static void ChangeCase(Editor *editor, BOOL upper)
{
    size_t start = 0;
    size_t end = 0;
    TextViewGetSelection(editor->view, &start, &end);
    size_t length = 0;
    const wchar_t *text = TextViewGetText(editor->view, &length);
    if (text == NULL || start == end || end - start > 0x7FFFFFFF) {
        return;
    }
    DWORD flags = LCMAP_LINGUISTIC_CASING | (upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE);
    int needed = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, text + start, (int)(end - start), NULL, 0, NULL, NULL, 0);
    wchar_t *mapped = needed > 0 ? MemAlloc((size_t)needed * sizeof(wchar_t)) : NULL;
    if (mapped == NULL) {
        return;
    }
    int written = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, flags, text + start, (int)(end - start), mapped, needed, NULL, NULL, 0);
    if (written > 0 && TextViewReplaceRange(editor->view, start, end, mapped, (size_t)written)) {
        TextViewSetSelection(editor->view, start, start + (size_t)written);
    }
    MemFree(mapped);
}

/* Removes spaces and tabs before every line break and at the end; only the changed span is replaced. */
static void TrimTrailingWhitespace(Editor *editor)
{
    size_t length = 0;
    const wchar_t *text = TextViewGetText(editor->view, &length);
    wchar_t *trimmed = text != NULL ? MemAlloc((length + 1) * sizeof(wchar_t)) : NULL;
    if (trimmed == NULL) {
        return;
    }
    size_t out = 0;
    size_t keep = 0;
    for (size_t i = 0; i < length; ++i) {
        wchar_t ch = text[i];
        if (ch == L'\n') {
            out = keep;
            trimmed[out++] = ch;
            keep = out;
        } else {
            trimmed[out++] = ch;
            if (ch != L' ' && ch != L'\t') {
                keep = out;
            }
        }
    }
    out = keep;
    if (out != length) {
        size_t head = 0;
        while (head < out && text[head] == trimmed[head]) {
            ++head;
        }
        size_t tail = 0;
        while (tail < out - head && text[length - 1 - tail] == trimmed[out - 1 - tail]) {
            ++tail;
        }
        TextViewReplaceRange(editor->view, head, length - tail, trimmed + head, out - head - tail);
    }
    MemFree(trimmed);
}

static void InsertTimeDate(Editor *editor)
{
    wchar_t time[64];
    wchar_t date[64];
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, time, ARRAYSIZE(time)) == 0) {
        time[0] = 0;
    }
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, NULL, NULL, date, ARRAYSIZE(date), NULL) == 0) {
        date[0] = 0;
    }
    wchar_t *stamp = StringJoin(time, L" ", date);
    if (stamp != NULL) {
        size_t start = 0;
        size_t end = 0;
        TextViewGetSelection(editor->view, &start, &end);
        TextViewReplaceRange(editor->view, start, end, stamp, (size_t)lstrlenW(stamp));
        MemFree(stamp);
    }
}

/* ---- File commands ------------------------------------------------------------------------- */

static void ReloadFromDisk(Editor *editor)
{
    if (editor->path == NULL) {
        return;
    }
    if (IsDirty(editor) && MessageBoxW(editor->window, L"Discard the changes and load the file again from disk?", QP_APP_NAME,
            MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    size_t caret = TextViewCaretPosition(editor->view);
    wchar_t *path = StringCopy(editor->path);
    if (path != NULL && LoadInto(editor, path)) {
        TextViewSetSelection(editor->view, caret, caret);
    }
    MemFree(path);
}

static void OpenContainingFolder(Editor *editor)
{
    wchar_t windows[MAX_PATH];
    UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (editor->path == NULL || length == 0 || length >= ARRAYSIZE(windows)) {
        return;
    }
    wchar_t *explorer = StringJoin(windows, L"\\explorer.exe", L"");
    wchar_t *commandLine = StringJoin(L"explorer.exe /select,\"", editor->path, L"\"");
    STARTUPINFOW startup = { sizeof startup };
    PROCESS_INFORMATION process;
    if (explorer != NULL && commandLine != NULL
        && CreateProcessW(explorer, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        AllowSetForegroundWindow(process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    MemFree(commandLine);
    MemFree(explorer);
}

static void CopyPath(Editor *editor)
{
    if (editor->path == NULL) {
        return;
    }
    size_t size = ((size_t)lstrlenW(editor->path) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    wchar_t *copy = memory != NULL ? GlobalLock(memory) : NULL;
    if (copy == NULL) {
        if (memory != NULL) {
            GlobalFree(memory);
        }
        return;
    }
    memcpy(copy, editor->path, size);
    GlobalUnlock(memory);
    if (OpenClipboard(editor->window)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
            GlobalFree(memory);
        }
        CloseClipboard();
    } else {
        GlobalFree(memory);
    }
}

static void OpenDroppedFiles(Editor *editor, HDROP drop)
{
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, NULL, 0);
        wchar_t *path = MemAlloc(((size_t)length + 1) * sizeof(wchar_t));
        if (path != NULL && DragQueryFileW(drop, i, path, length + 1) != 0 && (GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            OpenFromEditor(editor, path);
        }
        MemFree(path);
    }
    DragFinish(drop);
}

static void HandleCommand(Editor *editor, int command)
{
    HWND view = editor->view;
    switch (command) {
    case IDM_FILE_NEW:
        EditorOpenNew();
        break;
    case IDM_FILE_OPEN:
        ShowOpenDialog(editor);
        break;
    case IDM_FILE_SAVE:
        Save(editor);
        break;
    case IDM_FILE_SAVE_AS:
        SaveAs(editor);
        break;
    case IDM_FILE_CLOSE:
        SendMessageW(editor->window, WM_CLOSE, 0, 0);
        break;
    case IDM_EDIT_UNDO:
        TextViewUndo(view);
        break;
    case IDM_EDIT_REDO:
        TextViewRedo(view);
        break;
    case IDM_EDIT_CUT:
        TextViewCut(view);
        break;
    case IDM_EDIT_COPY:
        TextViewCopy(view);
        break;
    case IDM_EDIT_PASTE:
        TextViewPaste(view);
        break;
    case IDM_EDIT_DELETE:
        TextViewDeleteSelection(view);
        break;
    case IDM_EDIT_SELECT_ALL:
        TextViewSelectAll(view);
        break;
    case IDM_EDIT_FIND:
        ShowFindDialog(editor, FALSE);
        break;
    case IDM_EDIT_FIND_NEXT:
        FindNext(editor, TRUE);
        break;
    case IDM_EDIT_FIND_PREVIOUS:
        FindNext(editor, FALSE);
        break;
    case IDM_EDIT_REPLACE:
        ShowFindDialog(editor, TRUE);
        break;
    case IDM_EDIT_GOTO:
        DialogBoxParamW(instanceHandle, MAKEINTRESOURCEW(IDD_GOTO), editor->window, GoToProc, (LPARAM)editor);
        break;
    case IDM_FORMAT_WORD_WRAP:
        ToggleWordWrap();
        break;
    case IDM_START_WITH_WINDOWS:
        StartupToggle(editor->window);
        break;
    case IDM_FILE_RELOAD:
        ReloadFromDisk(editor);
        break;
    case IDM_FILE_OPEN_FOLDER:
        OpenContainingFolder(editor);
        break;
    case IDM_FILE_COPY_PATH:
        CopyPath(editor);
        break;
    case IDM_EDIT_TIME_DATE:
        InsertTimeDate(editor);
        break;
    case IDM_EDIT_DUPLICATE_LINE:
        DuplicateLines(editor);
        break;
    case IDM_EDIT_DELETE_LINE:
        DeleteLines(editor);
        break;
    case IDM_EDIT_MOVE_LINE_UP:
        MoveLines(editor, FALSE);
        break;
    case IDM_EDIT_MOVE_LINE_DOWN:
        MoveLines(editor, TRUE);
        break;
    case IDM_EDIT_JOIN_LINES:
        JoinLines(editor);
        break;
    case IDM_EDIT_UPPERCASE:
        ChangeCase(editor, TRUE);
        break;
    case IDM_EDIT_LOWERCASE:
        ChangeCase(editor, FALSE);
        break;
    case IDM_EDIT_TRIM_WHITESPACE:
        TrimTrailingWhitespace(editor);
        break;
    case IDM_FORMAT_AUTO_INDENT:
        ToggleAutoIndent();
        break;
    case IDM_FORMAT_FONT:
        ChooseEditorFont(editor);
        break;
    case IDM_FORMAT_TAB_2:
        SetTabSize(2);
        break;
    case IDM_FORMAT_TAB_4:
        SetTabSize(4);
        break;
    case IDM_FORMAT_TAB_8:
        SetTabSize(8);
        break;
    case IDM_FORMAT_CRLF:
    case IDM_FORMAT_LF:
    case IDM_FORMAT_CR: {
        TextFormat format = editor->format;
        format.lineEnding = (LineEnding)(command - IDM_FORMAT_CRLF);
        ChangeFormat(editor, format);
        break;
    }
    case IDM_FORMAT_UTF8:
    case IDM_FORMAT_UTF8_BOM:
    case IDM_FORMAT_UTF16LE:
    case IDM_FORMAT_UTF16BE:
    case IDM_FORMAT_ANSI:
        ChangeFormat(editor, FormatFromChoice(editor->format, (DWORD)(command - IDM_FORMAT_UTF8)));
        break;
    case IDM_VIEW_ZOOM_IN:
        Zoom(1);
        break;
    case IDM_VIEW_ZOOM_OUT:
        Zoom(-1);
        break;
    case IDM_VIEW_ZOOM_RESET:
        Zoom(0);
        break;
    case IDM_VIEW_STATUS_BAR:
        ToggleStatusBar();
        break;
    case IDM_VIEW_ALWAYS_ON_TOP:
        ToggleTopmost(editor);
        break;
    case IDM_VIEW_FULL_SCREEN:
        ToggleFullScreen(editor);
        break;
    }
}

static LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Editor *editor = (Editor *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (editor == NULL) {
        if (message == WM_NCCREATE) {
            editor = ((CREATESTRUCTW *)lParam)->lpCreateParams;
            editor->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)editor);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT themed = 0;
    if (ThemeMenuBarMessage(window, message, wParam, lParam, &themed)) {
        return themed;
    }
    if (message == findMessage && findMessage != 0) {
        HandleFindMessage(editor, (const FINDREPLACEW *)lParam);
        return 0;
    }

    switch (message) {
    case WM_CREATE: {
        /* Windows animates windows as they appear; editors appear at once instead. */
        BOOL disableTransitions = TRUE;
        DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof disableTransitions);
        ThemePrepareWindow(window);
        editor->view = TextViewCreate(window, VIEW_ID, instanceHandle);
        if (editor->view == NULL) {
            return -1;
        }
        ThemePrepareScrollBars(editor->view);
        TextViewSetColors(editor->view, ThemeTextColors());
        TextViewSetFont(editor->view, editorFont);
        TextViewSetWordWrap(editor->view, settings.wordWrap);
        TextViewSetTabSize(editor->view, settings.tabSize);
        TextViewSetAutoIndent(editor->view, settings.autoIndent);
        if (settings.statusBar) {
            ShowStatusBar(editor, TRUE);
        }
        return 0;
    }

    case WM_SIZE:
        LayoutEditor(editor);
        return 0;

    case WM_DROPFILES:
        OpenDroppedFiles(editor, (HDROP)wParam);
        return 0;

    case WM_MOUSEWHEEL:
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
            Zoom(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1);
            return 0;
        }
        break;

    case WM_SETFOCUS:
        SetFocus(editor->view);
        return 0;

    case WM_EDITOR_ADD_TAB:
        if (editor->shown) {
            if (editor->raised && !editor->topmost) {
                /* Active by now, so it stays on top of the other windows without the topmost flag. */
                SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            editor->raised = FALSE;
            SetTaskbarButton(window, TRUE);
        }
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == TEXTVIEW_SELECTION_CHANGED && (HWND)lParam == editor->view) {
            UpdateStatus(editor);
        } else if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == editor->view) {
            if (IsDirty(editor) != editor->modifiedShown) {
                UpdateTitle(editor);
            }
        } else {
            HandleCommand(editor, LOWORD(wParam));
        }
        return 0;

    case WM_INITMENUPOPUP: {
        HMENU popup = (HMENU)wParam;
        HMENU bar = GetMenu(window);
        int position = LOWORD(lParam);
        if (!HIWORD(lParam) && bar != NULL && GetMenuItemCount(popup) == 0 && GetSubMenu(bar, position) == popup) {
            CopyMenuItems(GetSubMenu(menuTemplate, position), popup);
        }
        UpdateMenu(editor, popup);
        return 0;
    }

    case WM_CLOSE:
        /* Only opened windows close; anything asking a pooled window to close is ignored. */
        if (editor->shown && ConfirmDiscard(editor)) {
            CloseEditor(editor);
        }
        return 0;

    case WM_QUERYENDSESSION:
        return ConfirmDiscard(editor);

    case WM_NCDESTROY: {
        Editor **link = &editors;
        while (*link != NULL && *link != editor) {
            link = &(*link)->next;
        }
        if (*link != NULL) {
            *link = editor->next;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        BOOL wasShown = editor->shown;
        MemFree(editor->path);
        MemFree(editor);
        if (!residentProcess && wasShown && EditorShownCount() == 0) {
            PostQuitMessage(0);
        }
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/* Draws the characters most text uses once, so GDI has their glyphs cached before the first file opens. */
static void WarmGlyphCache(void)
{
    wchar_t sample[128];
    int length = 0;
    for (wchar_t ch = 0x20; ch < 0x7F; ++ch) {
        sample[length++] = ch;
    }
    for (const wchar_t *extra = L"\x00E7\x011F\x0131\x00F6\x015F\x00FC\x00C7\x011E\x0130\x00D6\x015E\x00DC"; *extra != 0; ++extra) {
        sample[length++] = *extra;
    }

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 16, 16);
    ReleaseDC(NULL, screen);
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    HGDIOBJ previousFont = SelectObject(dc, editorFont);
    ExtTextOutW(dc, 0, 0, 0, NULL, sample, (UINT)length, NULL);
    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static int CALLBACK NoteFontFamily(const LOGFONTW *font, const TEXTMETRICW *metrics, DWORD type, LPARAM found)
{
    UNREFERENCED_PARAMETER(font);
    UNREFERENCED_PARAMETER(metrics);
    UNREFERENCED_PARAMETER(type);
    *(BOOL *)found = TRUE;
    return 0;
}

static BOOL FontInstalled(HDC dc, const wchar_t *face)
{
    LOGFONTW font = { 0 };
    font.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(font.lfFaceName, face, LF_FACESIZE);
    BOOL found = FALSE;
    EnumFontFamiliesExW(dc, &font, NoteFontFamily, (LPARAM)&found, 0);
    return found;
}

/*
 * The font from the settings at the zoomed size. Where it is not installed, Comic Sans MS, then
 * Courier New, which has shipped with every Windows version; the stock fixed font covers a system
 * without any of them.
 */
static HFONT CreateEditorFont(void)
{
    const wchar_t *const faces[] = { settings.fontName, SETTINGS_FONT_NAME_DEFAULT, L"Courier New" };
    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    int height = -MulDiv(settings.fontSize * settings.zoom, dpi, 72 * 100);
    height = height < 0 ? height : -1;
    HFONT font = NULL;
    for (size_t i = 0; i < ARRAYSIZE(faces) && font == NULL; ++i) {
        if (FontInstalled(screen, faces[i])) {
            font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faces[i]);
        }
    }
    ReleaseDC(NULL, screen);
    return font != NULL ? font : (HFONT)GetStockObject(ANSI_FIXED_FONT);
}

/* ---- Public functions ---------------------------------------------------------------------- */

BOOL EditorInitialize(HINSTANCE instance)
{
    instanceHandle = instance;
    if (!TextViewRegisterClass(instance)) {
        return FALSE;
    }
    ThemeInitialize();
    SettingsLoad();
    BlockSetBudget((size_t)settings.readyMemoryMB << 20);
    WorkersInitialize();

    WNDCLASSEXW windowClass = { sizeof windowClass };
    windowClass.lpfnWndProc = EditorProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    windowClass.hIconSm = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.lpszClassName = EDITOR_CLASS;
    editorAtom = RegisterClassExW(&windowClass);
    if (editorAtom == 0) {
        return FALSE;
    }

    accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCELERATORS));
    menuTemplate = LoadMenuW(instance, MAKEINTRESOURCEW(IDR_MENU));
    findMessage = RegisterWindowMessageW(FINDMSGSTRINGW);
    editorFont = CreateEditorFont();
    WarmGlyphCache();
    return TRUE;
}

BOOL EditorOpenFile(const wchar_t *path)
{
    TRACE("EditorOpenFile");
    Editor *existing = FindByPath(path);
    if (existing != NULL) {
        ShowEditor(existing);
        return TRUE;
    }

    /* A pooled editor that fails to load stays in the pool. */
    Editor *editor = TakeEditor();
    TRACE("TakeEditor");
    if (editor == NULL || !LoadInto(editor, path)) {
        return FALSE;
    }
    ShowEditor(editor);
    return TRUE;
}

BOOL EditorOpenNew(void)
{
    Editor *editor = TakeEditor();
    if (editor == NULL) {
        return FALSE;
    }
    ShowEditor(editor);
    return TRUE;
}

BOOL EditorIdle(DWORD *wait)
{
    *wait = INFINITE;
    if (!residentProcess) {
        return FALSE;
    }

    /* Closed windows are cleared and parked first; a pool already full loses them instead. */
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        if (editor->dirty) {
            if (PoolCount() > settings.poolSize) {
                editor->dirty = FALSE;
                DestroyWindow(editor->window);
            } else {
                ResetEditor(editor);
            }
            return TRUE;
        }
    }
    if (BlockPrepare()) {
        return TRUE;
    }

    ULONGLONG now = GetTickCount64();
    if (lastWindowChange != 0 && now < lastWindowChange + POOL_QUIET_PERIOD) {
        *wait = (DWORD)(lastWindowChange + POOL_QUIET_PERIOD - now);
        return FALSE;
    }

    Editor *next = NULL;
    int pooled = 0;
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        if (editor->pooled) {
            next = next != NULL ? next : editor;
            ++pooled;
        }
    }
    if (pooled > settings.poolSize) {
        DestroyWindow(next->window);
        return TRUE;
    }
    if (pooled < settings.poolSize) {
        return CreateEditor() != NULL;
    }

    /* The window TakeEditor hands out next waits, cloaked, where the next window will open, above every other window. */
    if (next != NULL) {
        RECT frame = NewWindowFrame();
        RECT current;
        GetWindowRect(next->window, &current);
        BOOL moves = !EqualRect(&current, &frame);
        if (moves || !next->raised) {
            SetWindowPos(next->window, HWND_TOPMOST, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                SWP_NOACTIVATE | (moves ? 0 : SWP_NOMOVE | SWP_NOSIZE));
            next->raised = TRUE;
            /* The first drawing after a window moved or changed place in the z-order is slow; it happens here, not when a file opens. */
            RedrawWindow(next->window, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            return TRUE;
        }
    }
    return FALSE;
}

int EditorPoolSize(void)
{
    return settings.poolSize;
}

void EditorSetPoolSize(int size)
{
    settings.poolSize = size < 0 ? 0 : size > SETTINGS_POOL_SIZE_MAX ? SETTINGS_POOL_SIZE_MAX : size;
    SettingsSave();
}

void EditorSetHost(HWND host)
{
    residentProcess = host != NULL;
    ownerWindow = host;
}

int EditorShownCount(void)
{
    int count = 0;
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        count += editor->shown;
    }
    return count;
}

BOOL EditorCloseAll(void)
{
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        if (editor->shown && !ConfirmDiscard(editor)) {
            return FALSE;
        }
    }
    while (editors != NULL) {
        HWND window = editors->window;
        editors->shown = FALSE;
        DestroyWindow(window);
    }
    return TRUE;
}

BOOL EditorTranslateMessage(MSG *message)
{
    if (findDialog != NULL && IsDialogMessageW(findDialog, message)) {
        return TRUE;
    }
    HWND root = message->hwnd != NULL ? GetAncestor(message->hwnd, GA_ROOT) : NULL;
    return root != NULL && (ATOM)GetClassLongPtrW(root, GCW_ATOM) == editorAtom
        && TranslateAcceleratorW(root, accelerators, message);
}
