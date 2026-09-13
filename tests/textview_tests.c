/*
 * Tests for src/textview.c. The control lives in a hidden window on this thread; keys are sent as
 * messages with the modifier state set through SetKeyboardState, which GetKeyState reads back.
 */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "textview.h"
#include "quickpad.h"

static int failures;
static int checks;

#define CHECK(condition, name)                                                  \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            wprintf(L"FAIL %hs: %hs (line %d)\n", name, #condition, __LINE__);  \
        }                                                                       \
    } while (0)

static HWND view;
static int charWidth;
static int lineHeight;
static int margin;

static void SetModifiers(BOOL shift, BOOL control)
{
    BYTE state[256];
    GetKeyboardState(state);
    state[VK_SHIFT] = shift ? 0x80 : 0;
    state[VK_CONTROL] = control ? 0x80 : 0;
    state[VK_MENU] = 0;
    SetKeyboardState(state);
}

static void Key(WPARAM key, BOOL shift, BOOL control)
{
    SetModifiers(shift, control);
    SendMessageW(view, WM_KEYDOWN, key, 0);
    SetModifiers(FALSE, FALSE);
}

static void Type(const wchar_t *text)
{
    for (size_t i = 0; text[i] != 0; ++i) {
        SendMessageW(view, WM_CHAR, text[i], 0);
    }
}

static void Load(const wchar_t *text)
{
    size_t length = wcslen(text);
    wchar_t *copy = MemAlloc((length + 1) * sizeof(wchar_t));
    memcpy(copy, text, length * sizeof(wchar_t));
    TextViewSetText(view, copy, length);
}

static BOOL TextIs(const wchar_t *expected)
{
    size_t length = 0;
    const wchar_t *text = TextViewGetText(view, &length);
    return length == wcslen(expected) && wmemcmp(text, expected, length) == 0;
}

static BOOL SelectionIs(size_t start, size_t end)
{
    size_t selectionStart = 0;
    size_t selectionEnd = 0;
    TextViewGetSelection(view, &selectionStart, &selectionEnd);
    return selectionStart == start && selectionEnd == end;
}

static void Click(int x, int y, BOOL shift)
{
    LPARAM point = MAKELPARAM(x, y);
    SendMessageW(view, WM_LBUTTONDOWN, shift ? MK_SHIFT : 0, point);
    SendMessageW(view, WM_LBUTTONUP, 0, point);
}

static void TestNavigation(void)
{
    Load(L"hello world\nsecond line\n\nlast");
    Key(VK_RIGHT, FALSE, FALSE);
    Key(VK_RIGHT, FALSE, FALSE);
    Key(VK_RIGHT, FALSE, FALSE);
    CHECK(SelectionIs(3, 3), "right arrow");
    Key(VK_END, FALSE, FALSE);
    CHECK(SelectionIs(11, 11), "end");
    Key(VK_RIGHT, FALSE, FALSE);
    CHECK(SelectionIs(12, 12), "right arrow crosses the line break");
    Key(VK_UP, FALSE, FALSE);
    CHECK(SelectionIs(0, 0), "up keeps the column");
    Key(VK_RIGHT, FALSE, TRUE);
    CHECK(SelectionIs(6, 6), "word right skips the word and the space");
    Key(VK_RIGHT, FALSE, TRUE);
    CHECK(SelectionIs(11, 11), "word right stops at the line end");
    Key(VK_RIGHT, FALSE, TRUE);
    CHECK(SelectionIs(12, 12), "word right crosses the line break");
    Key(VK_LEFT, FALSE, TRUE);
    CHECK(SelectionIs(11, 11), "word left crosses the line break");
    Key(VK_LEFT, FALSE, TRUE);
    CHECK(SelectionIs(6, 6), "word left");
    Key(VK_END, FALSE, TRUE);
    CHECK(SelectionIs(29, 29), "control end");
    Key(VK_HOME, TRUE, TRUE);
    CHECK(SelectionIs(0, 29), "shift control home selects everything");
    Key(VK_LEFT, FALSE, FALSE);
    CHECK(SelectionIs(0, 0), "left collapses to the selection start");

    Load(L"abcdef\nab\nabcdef");
    for (int i = 0; i < 5; ++i) {
        Key(VK_RIGHT, FALSE, FALSE);
    }
    Key(VK_DOWN, FALSE, FALSE);
    CHECK(SelectionIs(9, 9), "down onto a shorter line goes to its end");
    Key(VK_DOWN, FALSE, FALSE);
    CHECK(SelectionIs(15, 15), "down again returns to the remembered column");
    Key(VK_UP, TRUE, FALSE);
    CHECK(SelectionIs(9, 15), "shift up selects");

    Load(L"a\xD83D\xDE00" L"b");
    Key(VK_RIGHT, FALSE, FALSE);
    Key(VK_RIGHT, FALSE, FALSE);
    CHECK(SelectionIs(3, 3), "right arrow steps over a surrogate pair");
    Key(VK_BACK, FALSE, FALSE);
    CHECK(TextIs(L"ab") && SelectionIs(1, 1), "backspace removes a whole surrogate pair");
}

static void TestEditing(void)
{
    Load(L"");
    Type(L"hi there");
    CHECK(TextIs(L"hi there"), "typing");
    CHECK(TextViewIsModified(view), "typing modifies");
    TextViewUndo(view);
    CHECK(TextIs(L"") && !TextViewIsModified(view), "one undo removes the typed word");
    TextViewRedo(view);
    CHECK(TextIs(L"hi there") && SelectionIs(8, 8), "redo");

    Key(VK_BACK, FALSE, FALSE);
    Key(VK_BACK, FALSE, FALSE);
    Key(VK_BACK, FALSE, FALSE);
    CHECK(TextIs(L"hi th"), "backspace");
    TextViewUndo(view);
    CHECK(TextIs(L"hi there"), "one undo restores the backspaces");

    Type(L"\r");
    CHECK(TextIs(L"hi there\n") && TextViewLineCount(view) == 2, "enter inserts a line break");
    Key(VK_BACK, FALSE, TRUE);
    CHECK(TextIs(L"hi there"), "control backspace at a line start removes the break");
    Key(VK_BACK, FALSE, TRUE);
    CHECK(TextIs(L"hi "), "control backspace removes a word");

    Load(L"hello world");
    TextViewSetSelection(view, 0, 5);
    Type(L"bye");
    CHECK(TextIs(L"bye world"), "typing replaces the selection");
    TextViewUndo(view);
    CHECK(TextIs(L"hello world") && SelectionIs(0, 5), "undo restores the replaced selection");

    Load(L"abc def");
    Key(VK_DELETE, FALSE, FALSE);
    CHECK(TextIs(L"bc def"), "delete");
    Key(VK_DELETE, FALSE, TRUE);
    CHECK(TextIs(L"def"), "control delete removes a word and its space");

    Load(L"tab");
    Type(L"\t");
    CHECK(TextIs(L"\ttab"), "tab is typed");
    SendMessageW(view, WM_CHAR, 0x01, 0);
    SendMessageW(view, WM_CHAR, 0x1B, 0);
    CHECK(TextIs(L"\ttab"), "control characters are ignored");

    TextViewMarkSaved(view);
    CHECK(!TextViewIsModified(view), "saved");
}

static void TestSearch(void)
{
    SearchOptions options = { FALSE, FALSE };
    Load(L"one two one");
    CHECK(TextViewFind(view, L"one", options, TRUE) && SelectionIs(0, 3), "find first");
    CHECK(TextViewFind(view, L"one", options, TRUE) && SelectionIs(8, 11), "find next");
    CHECK(TextViewFind(view, L"one", options, TRUE) && SelectionIs(0, 3), "find wraps around");
    CHECK(TextViewFind(view, L"one", options, FALSE) && SelectionIs(8, 11), "find previous wraps around");
    CHECK(!TextViewFind(view, L"three", options, TRUE), "missing text");
    CHECK(TextViewReplaceAll(view, L"one", L"1", options) == 2 && TextIs(L"1 two 1"), "replace all");
    TextViewUndo(view);
    CHECK(TextIs(L"one two one"), "replace all is one undo step");

    Load(L"aXbXc");
    CHECK(TextViewReplace(view, L"X", L"-", options) && SelectionIs(1, 2) && TextIs(L"aXbXc"), "replace first finds");
    CHECK(TextViewReplace(view, L"X", L"-", options) && TextIs(L"a-bXc") && SelectionIs(3, 4), "replace then find next");

    Load(L"a\nb\nc");
    TextViewGoToLine(view, 2);
    CHECK(SelectionIs(4, 4) && TextViewCaretLine(view) == 2, "go to line");
    TextViewGoToLine(view, 99);
    CHECK(TextViewCaretLine(view) == 2, "go to line past the end");
}

static void TestMouse(void)
{
    Load(L"first line\nsecond line\nthird");
    Click(margin + 3 * charWidth + 1, lineHeight + 2, FALSE);
    CHECK(SelectionIs(14, 14), "click places the caret");
    Click(margin + 6 * charWidth + charWidth / 2 + 1, 2 * lineHeight + 1, TRUE);
    CHECK(SelectionIs(14, 28), "shift click past the end of the last line extends the selection");
    SendMessageW(view, WM_LBUTTONDBLCLK, 0, MAKELPARAM(margin + 2 * charWidth, 1));
    CHECK(SelectionIs(0, 5), "double click selects a word");
    Click(margin + 500 * charWidth, 1, FALSE);
    CHECK(SelectionIs(10, 10), "click past the end of a line");
}

static void TestWordWrap(void)
{
    RECT client;
    GetClientRect(view, &client);
    size_t columns = (size_t)((client.right - margin * 2) / charWidth);

    wchar_t text[512];
    size_t length = 0;
    for (size_t i = 0; i < columns - 1; ++i) {
        text[length++] = L'a';
    }
    text[length++] = L' ';
    for (size_t i = 0; i < 5; ++i) {
        text[length++] = L'b';
    }
    text[length] = 0;
    size_t secondRow = columns;

    TextViewSetWordWrap(view, TRUE);
    Load(text);
    Key(VK_DOWN, FALSE, FALSE);
    CHECK(SelectionIs(secondRow, secondRow), "down moves to the wrapped row");
    Key(VK_END, FALSE, FALSE);
    CHECK(SelectionIs(length, length), "end of the last row");
    Key(VK_HOME, FALSE, FALSE);
    CHECK(SelectionIs(secondRow, secondRow), "home of a wrapped row");
    Key(VK_UP, FALSE, FALSE);
    CHECK(SelectionIs(0, 0), "up to the first row");
    Key(VK_END, FALSE, FALSE);
    CHECK(SelectionIs(secondRow, secondRow), "end of the first row stops at the wrap");
    Type(L"x");
    CHECK(text[secondRow] == L'b' && TextViewGetText(view, &length)[secondRow] == L'x', "typing at the end of the first row");
    Click(margin + 1, lineHeight + 1, FALSE);
    CHECK(SelectionIs(secondRow, secondRow), "click on the second row");
    TextViewSetWordWrap(view, FALSE);
}

static void TestPaint(void)
{
    Load(L"select me");
    TextViewSetSelection(view, 0, 6);
    RECT client;
    GetClientRect(view, &client);

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    ReleaseDC(NULL, screen);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    SendMessageW(view, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);

    COLORREF window = GetSysColor(COLOR_WINDOW);
    COLORREF inside = GetPixel(dc, margin + 1, 1);
    COLORREF outside = GetPixel(dc, margin + 8 * charWidth + 1, 1);
    COLORREF below = GetPixel(dc, 5, lineHeight * 3);
    CHECK(inside != window, "selected cells are highlighted");
    CHECK(outside == window, "cells after the selection use the window color");
    CHECK(below == window, "rows below the text are cleared");

    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

static void TestLargeText(void)
{
    size_t lines = 200000;
    size_t lineLength = 50;
    size_t length = lines * (lineLength + 2);
    wchar_t *text = MemAlloc((length + 1) * sizeof(wchar_t));
    size_t position = 0;
    for (size_t line = 0; line < lines; ++line) {
        for (size_t i = 0; i < lineLength; ++i) {
            text[position++] = (wchar_t)(L'a' + (line + i) % 26);
        }
        text[position++] = L'\r';
        text[position++] = L'\n';
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER before;
    LARGE_INTEGER after;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    CHECK(TextViewSetText(view, text, length), "large text is taken");
    QueryPerformanceCounter(&after);
    wprintf(L"SetText of %zu characters: %.1f ms\n", length, (double)(after.QuadPart - before.QuadPart) * 1000.0 / (double)frequency.QuadPart);
    CHECK(TextViewLineCount(view) == lines + 1, "large text line count");

    Key(VK_END, FALSE, TRUE);
    CHECK(TextViewCaretLine(view) == lines, "control end on large text");
    Type(L"z");
    Key(VK_HOME, FALSE, TRUE);
    Type(L"y");
    size_t textLength = 0;
    const wchar_t *content = TextViewGetText(view, &textLength);
    CHECK(textLength == lines * (lineLength + 1) + 2 && content[0] == L'y' && content[textLength - 1] == L'z', "edits at both ends");
}

int wmain(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    TextViewRegisterClass(instance);

    WNDCLASSW parentClass = { 0 };
    parentClass.lpfnWndProc = DefWindowProcW;
    parentClass.hInstance = instance;
    parentClass.lpszClassName = L"TextViewTestParent";
    RegisterClassW(&parentClass);
    HWND parent = CreateWindowExW(0, parentClass.lpszClassName, L"test", WS_OVERLAPPEDWINDOW, 0, 0, 900, 700, NULL, NULL, instance, NULL);
    view = TextViewCreate(parent, 1, instance);
    MoveWindow(view, 0, 0, 800, 600, FALSE);

    HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    TextViewSetFont(view, font);

    HDC dc = GetDC(view);
    HGDIOBJ previous = SelectObject(dc, font);
    TEXTMETRICW metrics;
    GetTextMetricsW(dc, &metrics);
    SIZE digit;
    GetTextExtentPoint32W(dc, L"0", 1, &digit);
    SelectObject(dc, previous);
    ReleaseDC(view, dc);
    charWidth = digit.cx;
    lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
    margin = charWidth / 2 > 2 ? charWidth / 2 : 2;

    TestNavigation();
    TestEditing();
    TestSearch();
    TestMouse();
    TestWordWrap();
    TestPaint();
    TestLargeText();

    DestroyWindow(parent);
    DeleteObject(font);
    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
