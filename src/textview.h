#pragma once

#include <windows.h>

#include "search.h"

/*
 * A plain text editing control. It keeps the text in a Document and draws only the rows that are
 * on screen, so the cost of opening a file is reading it and finding its line breaks.
 * The parent receives WM_COMMAND with EN_CHANGE whenever the text changes.
 */
#define TEXTVIEW_CLASS L"QuickPadTextView"

typedef struct TextViewColors {
    COLORREF text;
    COLORREF background;
    COLORREF selectedText;
    COLORREF selectedBackground;
    COLORREF unfocusedSelectedBackground;
} TextViewColors;

BOOL TextViewRegisterClass(HINSTANCE instance);
HWND TextViewCreate(HWND parent, int id, HINSTANCE instance);

/* colors must stay valid while the view uses them; NULL returns to the system colors. */
void TextViewSetColors(HWND view, const TextViewColors *colors);

/* Takes over text, a MemAlloc block; clears undo and puts the caret at the start. */
BOOL TextViewSetText(HWND view, wchar_t *text, size_t length);
void TextViewClear(HWND view);

/* The whole text with L'\n' line breaks, null-terminated, valid until the text changes. */
const wchar_t *TextViewGetText(HWND view, size_t *length);

BOOL TextViewIsModified(HWND view);
void TextViewMarkSaved(HWND view);

/*
 * The font stays owned by the caller. Text is laid out in equal cells; with a proportional font a
 * cell is the average width of its letters and digits.
 */
void TextViewSetFont(HWND view, HFONT font);
void TextViewSetWordWrap(HWND view, BOOL wrap);

void TextViewGetSelection(HWND view, size_t *start, size_t *end);
void TextViewSetSelection(HWND view, size_t anchor, size_t caret);
size_t TextViewLineCount(HWND view);
size_t TextViewCaretLine(HWND view);
void TextViewGoToLine(HWND view, size_t line);

BOOL TextViewCanUndo(HWND view);
BOOL TextViewCanRedo(HWND view);
BOOL TextViewCanPaste(HWND view);
void TextViewUndo(HWND view);
void TextViewRedo(HWND view);
void TextViewCut(HWND view);
void TextViewCopy(HWND view);
void TextViewPaste(HWND view);
void TextViewDeleteSelection(HWND view);
void TextViewSelectAll(HWND view);

/* Selects the next or previous match, wrapping around; FALSE when there is none. */
BOOL TextViewFind(HWND view, const wchar_t *pattern, SearchOptions options, BOOL down);
/* Replaces the selection when it is a match, then selects the next match. */
BOOL TextViewReplace(HWND view, const wchar_t *pattern, const wchar_t *with, SearchOptions options);
size_t TextViewReplaceAll(HWND view, const wchar_t *pattern, const wchar_t *with, SearchOptions options);
