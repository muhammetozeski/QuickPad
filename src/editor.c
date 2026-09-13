#define COBJMACROS

#include "editor.h"
#include "fileio.h"
#include "quickpad.h"
#include "resource.h"
#include "settings.h"
#include "textview.h"
#include "theme.h"

#include <shobjidl.h>

#define EDITOR_CLASS L"QuickPadEditor"
#define VIEW_ID 1
#define ENCODING_GROUP 100
#define ENCODING_COMBO 101

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
} Editor;

static HINSTANCE instanceHandle;
static ATOM editorAtom;
static HACCEL accelerators;
static HFONT editorFont;
static Editor *editors;
static BOOL comReady;

/* ---- Small helpers ------------------------------------------------------------------------- */

static wchar_t *CopyString(const wchar_t *text)
{
    size_t length = (size_t)lstrlenW(text);
    wchar_t *copy = MemAlloc((length + 1) * sizeof(wchar_t));
    if (copy != NULL) {
        memcpy(copy, text, (length + 1) * sizeof(wchar_t));
    }
    return copy;
}

/* Joins up to three strings into a MemAlloc block. */
static wchar_t *Concat(const wchar_t *first, const wchar_t *second, const wchar_t *third)
{
    size_t a = (size_t)lstrlenW(first);
    size_t b = (size_t)lstrlenW(second);
    size_t c = (size_t)lstrlenW(third);
    wchar_t *joined = MemAlloc((a + b + c + 1) * sizeof(wchar_t));
    if (joined != NULL) {
        memcpy(joined, first, a * sizeof(wchar_t));
        memcpy(joined + a, second, b * sizeof(wchar_t));
        memcpy(joined + a + b, third, (c + 1) * sizeof(wchar_t));
    }
    return joined;
}

static const wchar_t *FileName(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *p = path; *p != 0; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    return name;
}

static HWND DialogOwner(const Editor *editor)
{
    return editor != NULL && IsWindowVisible(editor->window) ? editor->window : NULL;
}

static void ShowFileError(const Editor *editor, const wchar_t *path, DWORD error)
{
    wchar_t *system = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, (LPWSTR)&system, 0, NULL);
    wchar_t *message = Concat(path, L"\n\n", system != NULL ? system : L"");
    MessageBoxW(DialogOwner(editor), message != NULL ? message : path, QP_APP_NAME, MB_ICONERROR);
    MemFree(message);
    if (system != NULL) {
        LocalFree(system);
    }
}

static void UpdateTitle(Editor *editor)
{
    BOOL modified = TextViewIsModified(editor->view);
    wchar_t *title = Concat(modified ? L"*" : L"", editor->path != NULL ? FileName(editor->path) : L"Untitled",
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
    editor->path = path != NULL ? CopyString(path) : NULL;
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

static Editor *CreateEditor(void)
{
    Editor *editor = MemAllocZero(sizeof *editor);
    if (editor == NULL) {
        return NULL;
    }
    editor->format = TextDefaultFormat();

    HMENU menu = LoadMenuW(instanceHandle, MAKEINTRESOURCEW(IDR_MENU));
    HWND window = CreateWindowExW(0, EDITOR_CLASS, L"Untitled - " QP_APP_NAME, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, menu, instanceHandle, editor);
    if (window == NULL) {
        if (menu != NULL) {
            DestroyMenu(menu);
        }
        MemFree(editor);
        return NULL;
    }
    editor->next = editors;
    editors = editor;
    return editor;
}

static void ShowEditor(Editor *editor)
{
    editor->shown = TRUE;
    ShowWindow(editor->window, IsIconic(editor->window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(editor->window);
    RedrawWindow(editor->window, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

static void CloseEditor(Editor *editor)
{
    DestroyWindow(editor->window);
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
        wchar_t *question = Concat(L"Cannot find the file\n", path, L"\n\nDo you want to create a new file?");
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
        wchar_t *folder = CopyString(editor->path);
        if (folder != NULL) {
            wchar_t *name = (wchar_t *)FileName(folder);
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
        IFileSaveDialog_SetFileName(dialog, FileName(editor->path));
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
    wchar_t *question = Concat(L"Do you want to save changes to ", editor->path != NULL ? editor->path : L"Untitled", L"?");
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
    case WM_CREATE:
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
        if (ConfirmDiscard(editor)) {
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
        if (editors == NULL && wasShown) {
            PostQuitMessage(0);
        }
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
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
    return editorFont != NULL;
}

BOOL EditorOpenFile(const wchar_t *path)
{
    Editor *existing = FindByPath(path);
    if (existing != NULL) {
        ShowEditor(existing);
        return TRUE;
    }

    Editor *editor = CreateEditor();
    if (editor == NULL) {
        return FALSE;
    }
    if (!LoadInto(editor, path)) {
        CloseEditor(editor);
        return FALSE;
    }
    ShowEditor(editor);
    return TRUE;
}

BOOL EditorOpenNew(void)
{
    Editor *editor = CreateEditor();
    if (editor == NULL) {
        return FALSE;
    }
    ShowEditor(editor);
    return TRUE;
}

BOOL EditorTranslateMessage(MSG *message)
{
    HWND root = message->hwnd != NULL ? GetAncestor(message->hwnd, GA_ROOT) : NULL;
    return root != NULL && (ATOM)GetClassLongPtrW(root, GCW_ATOM) == editorAtom
        && TranslateAcceleratorW(root, accelerators, message);
}
