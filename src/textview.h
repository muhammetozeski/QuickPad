#pragma once

#include <windows.h>

#include "search.h"
#include "textload.h"

/*
 * A plain text editing control. It keeps the text in a Document and draws only the rows that are
 * on screen, so the cost of opening a file is decoding its first part; the rest is decoded by the
 * worker threads while the window is already showing it.
 * The parent receives WM_COMMAND with EN_CHANGE whenever the text changes.
 */
#define TEXTVIEW_CLASS L"QuickPadTextView"

/* WM_COMMAND notification code; see TextViewNotifySelection. */
#define TEXTVIEW_SELECTION_CHANGED 0x0701

/* Posted to the view by the worker that decodes the last part of a load; see TextViewSetLoad. */
#define WM_TEXTVIEW_LOAD_DONE (WM_APP + 0x40)

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

/*
 * Takes over a load begun with the view as its notify window and WM_TEXTVIEW_LOAD_DONE as its
 * message. The view shows what is decoded at once and finishes the load before anything else
 * touches the text.
 */
void TextViewSetLoad(HWND view, TextLoad *load);

/* Hands the rest of the load to the worker threads, once the window is on screen. */
void TextViewStartLoad(HWND view);

/* TRUE while a load is still being decoded. */
BOOL TextViewIsLoading(HWND view);

void TextViewClear(HWND view);

/* The whole text with L'\n' line breaks, null-terminated, valid until the text changes. */
const wchar_t *TextViewGetText(HWND view, size_t *length);

BOOL TextViewIsModified(HWND view);
void TextViewMarkSaved(HWND view);

/*
 * The font stays owned by the caller and must outlive the views that use it. Proportional fonts are
 * laid out with the width of each glyph.
 */
void TextViewSetFont(HWND view, HFONT font);
/* Tab stops every cells average character widths; the views of one font share the setting. */
void TextViewSetTabSize(HWND view, int cells);
/* Enter repeats the spaces and tabs the current line starts with. */
void TextViewSetAutoIndent(HWND view, BOOL autoIndent);
/* Sends the parent WM_COMMAND with TEXTVIEW_SELECTION_CHANGED whenever the caret or selection moves. */
void TextViewNotifySelection(HWND view, BOOL notify);
void TextViewSetWordWrap(HWND view, BOOL wrap);

void TextViewGetSelection(HWND view, size_t *start, size_t *end);
void TextViewSetSelection(HWND view, size_t anchor, size_t caret);
size_t TextViewLineCount(HWND view);
size_t TextViewCaretLine(HWND view);
size_t TextViewCaretPosition(HWND view);
size_t TextViewLineStart(HWND view, size_t line);
/* Position of the line's line break, or the text length for the last line. */
size_t TextViewLineEnd(HWND view, size_t line);
size_t TextViewLineFromPosition(HWND view, size_t position);
/* Replaces [start, end) with text (L'\n' line breaks) as one undo step and puts the caret after it. */
BOOL TextViewReplaceRange(HWND view, size_t start, size_t end, const wchar_t *text, size_t length);
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
