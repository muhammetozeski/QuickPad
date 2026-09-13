#define COBJMACROS

#include "editor.h"
#include "fileio.h"
#include "quickpad.h"
#include "resource.h"
#include "settings.h"
#include "strings.h"
#include "textview.h"
#include "theme.h"

#include <dwmapi.h>
#include <shobjidl.h>

#define EDITOR_CLASS L"QuickPadEditor"
#define VIEW_ID 1
#define ENCODING_GROUP 100
#define ENCODING_COMBO 101

/* Windows waiting in the pool sit here, cloaked, so they never cover anything or take input. */
#define PARK_POSITION (-32000)

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
    BOOL modifiedShown;
    BOOL shown;
    BOOL pooled;
} Editor;

static HINSTANCE instanceHandle;
static ATOM editorAtom;
static HACCEL accelerators;
static HFONT editorFont;
static Editor *editors;
static BOOL comReady;
static BOOL residentProcess;
static HWND ownerWindow;

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

static void UpdateTitle(Editor *editor)
{
    BOOL modified = TextViewIsModified(editor->view);
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

static int PoolCount(void)
{
    int count = 0;
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        count += editor->pooled;
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

static Editor *CreateEditor(void)
{
    Editor *editor = MemAllocZero(sizeof *editor);
    if (editor == NULL) {
        return NULL;
    }
    editor->format = TextDefaultFormat();

    RECT work = WorkArea();
    SIZE size = WindowSize(&work);
    HMENU menu = LoadMenuW(instanceHandle, MAKEINTRESOURCEW(IDR_MENU));
    HWND window = CreateWindowExW(0, EDITOR_CLASS, L"Untitled - " QP_APP_NAME, WS_OVERLAPPEDWINDOW,
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

/* A drawn editor from the pool, or a new one when the pool is empty. */
static Editor *TakeEditor(void)
{
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        if (editor->pooled) {
            return editor;
        }
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

    /* Everything happens while the window is still cloaked, so it appears complete in one frame. */
    RECT frame = NewWindowFrame();
    editor->pooled = FALSE;
    editor->shown = TRUE;
    SetWindowLongPtrW(window, GWLP_HWNDPARENT, 0);
    SetWindowPos(window, HWND_TOP, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top, SWP_NOACTIVATE);
    RedrawWindow(window, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
    SetCloaked(window, FALSE);
    SetForegroundWindow(window);
    SetTaskbarButton(window, TRUE);
}

/* Returns a closed editor to the pool, or destroys it when the pool is full. */
static void CloseEditor(Editor *editor)
{
    HWND window = editor->window;
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
    if (!residentProcess || PoolCount() >= settings.poolSize) {
        DestroyWindow(window);
        return;
    }

    SetCloaked(window, TRUE);
    SetTaskbarButton(window, FALSE);
    ShowWindow(window, SW_HIDE);
    TextViewClear(editor->view);
    SetPath(editor, NULL);
    editor->format = TextDefaultFormat();
    UpdateTitle(editor);
    ParkEditor(editor);
}

/* Loads a file into an editor. A missing file can become a new, empty document with that name. */
static BOOL LoadInto(Editor *editor, const wchar_t *path)
{
    wchar_t *text = NULL;
    size_t length = 0;
    TextFormat format = TextDefaultFormat();
    DWORD error = 0;

    if (!FileLoad(path, &text, &length, &format, &error)) {
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
    } else if (!TextViewSetText(editor->view, text, length)) {
        MemFree(text);
        ShowFileError(editor, path, ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    SetPath(editor, path);
    editor->format = format;
    UpdateTitle(editor);
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
    if (!TextViewIsModified(editor->view)) {
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
}

static void ToggleWordWrap(void)
{
    settings.wordWrap = !settings.wordWrap;
    SettingsSave();
    for (Editor *editor = editors; editor != NULL; editor = editor->next) {
        TextViewSetWordWrap(editor->view, settings.wordWrap);
    }
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
    case IDM_FORMAT_WORD_WRAP:
        ToggleWordWrap();
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
        return 0;
    }

    case WM_SIZE:
        MoveWindow(editor->view, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;

    case WM_SETFOCUS:
        SetFocus(editor->view);
        return 0;

    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == editor->view) {
            if (TextViewIsModified(editor->view) != editor->modifiedShown) {
                UpdateTitle(editor);
            }
        } else {
            HandleCommand(editor, LOWORD(wParam));
        }
        return 0;

    case WM_INITMENUPOPUP:
        UpdateMenu(editor, (HMENU)wParam);
        return 0;

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

/* ---- Public functions ---------------------------------------------------------------------- */

BOOL EditorInitialize(HINSTANCE instance)
{
    instanceHandle = instance;
    if (!TextViewRegisterClass(instance)) {
        return FALSE;
    }
    ThemeInitialize();
    SettingsLoad();

    WNDCLASSEXW windowClass = { sizeof windowClass };
    windowClass.lpfnWndProc = EditorProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.lpszClassName = EDITOR_CLASS;
    editorAtom = RegisterClassExW(&windowClass);
    if (editorAtom == 0) {
        return FALSE;
    }

    accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_ACCELERATORS));
    HDC screen = GetDC(NULL);
    int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    editorFont = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (editorFont == NULL) {
        return FALSE;
    }
    WarmGlyphCache();
    return TRUE;
}

BOOL EditorOpenFile(const wchar_t *path)
{
    Editor *existing = FindByPath(path);
    if (existing != NULL) {
        ShowEditor(existing);
        return TRUE;
    }

    /* A pooled editor that fails to load stays in the pool. */
    Editor *editor = TakeEditor();
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

BOOL EditorIdle(void)
{
    if (!residentProcess) {
        return FALSE;
    }
    int pooled = PoolCount();
    if (pooled > settings.poolSize) {
        for (Editor *editor = editors; editor != NULL; editor = editor->next) {
            if (editor->pooled) {
                DestroyWindow(editor->window);
                return TRUE;
            }
        }
    }
    return pooled < settings.poolSize && CreateEditor() != NULL;
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
    HWND root = message->hwnd != NULL ? GetAncestor(message->hwnd, GA_ROOT) : NULL;
    return root != NULL && (ATOM)GetClassLongPtrW(root, GCW_ATOM) == editorAtom
        && TranslateAcceleratorW(root, accelerators, message);
}
