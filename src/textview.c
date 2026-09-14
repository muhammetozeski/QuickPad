#include "textview.h"
#include "document.h"
#include "history.h"
#include "layout.h"
#include "quickpad.h"
#include "trace.h"

#include <imm.h>
#include <limits.h>
#include <windowsx.h>

#define AUTOSCROLL_TIMER 1
#define AUTOSCROLL_INTERVAL 40
#define VISIBLE_GLYPH_SLACK 256

/* A painted row longer than this beyond the window has the rest of its width estimated from its length. */
#define MEASURED_REMAINDER 4096

/* Character widths of one font, shared by every view that uses it. */
typedef struct FontMetrics {
    struct FontMetrics *next;
    HFONT font;
    int users;
    LayoutMetrics layout;
} FontMetrics;

static FontMetrics *fontMetrics;

enum {
    MENU_UNDO = 1,
    MENU_CUT,
    MENU_COPY,
    MENU_PASTE,
    MENU_DELETE,
    MENU_SELECT_ALL,
};

typedef struct TextView {
    HWND window;
    Document document;
    History history;
    size_t anchor;
    size_t caret;
    BOOL caretTrailing;      /* caret on a wrap boundary is drawn at the end of the upper row */
    long long desiredX;      /* x kept across vertical moves, -1 when not set */
    HFONT font;
    const TextViewColors *palette;
    int lineHeight;
    int charWidth;
    int margin;
    int clientWidth;
    int clientHeight;
    BOOL wordWrap;
    long long wrapWidth;
    size_t topLine;
    size_t topRow;
    long long scrollX;
    long long widestRow;     /* widest row painted since the text was set; the horizontal scroll range */
    FontMetrics *shared;
    LayoutMetrics *metrics;
    int tabCells;
    BOOL autoIndent;
    BOOL notifySelection;
    BOOL focused;
    BOOL selecting;
    int wheelDelta;
    int horizontalWheelDelta;
    wchar_t *lineCopy;
    size_t lineCopyCapacity;
    size_t *rows;
    size_t rowsCapacity;
    wchar_t *glyphs;
    size_t glyphsCapacity;
    int *advances;
    size_t advancesCapacity;
} TextView;

typedef struct Place {
    size_t line;
    size_t row;
    long long x;
} Place;

typedef struct Colors {
    COLORREF text;
    COLORREF background;
    COLORREF selectedText;
    COLORREF selectedBackground;
} Colors;

/* ---- Helpers ------------------------------------------------------------------------------- */

static TextView *ViewFrom(HWND window)
{
    return (TextView *)GetWindowLongPtrW(window, 0);
}

/* Widths of the glyphs the font has; characters it lacks keep the cell widths layout.c gives them. */
static void MeasureWithFont(void *context, wchar_t first, int *widths)
{
    wchar_t characters[256];
    WORD glyphs[256];
    INT measured[256];
    for (int i = 0; i < 256; ++i) {
        characters[i] = (wchar_t)(first + i);
    }
    HDC dc = GetDC(NULL);
    HGDIOBJ previous = SelectObject(dc, (HFONT)context);
    if (GetCharWidth32W(dc, first, (UINT)first + 255, measured)
        && GetGlyphIndicesW(dc, characters, 256, glyphs, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
        for (int i = 0; i < 256; ++i) {
            if (glyphs[i] != 0xFFFF) {
                widths[i] = measured[i];
            }
        }
    }
    SelectObject(dc, previous);
    ReleaseDC(NULL, dc);
}

static FontMetrics *AcquireMetrics(HFONT font, int cellWidth)
{
    for (FontMetrics *entry = fontMetrics; entry != NULL; entry = entry->next) {
        if (entry->font == font && entry->layout.cellWidth == cellWidth) {
            ++entry->users;
            return entry;
        }
    }
    FontMetrics *entry = MemAllocZero(sizeof *entry);
    if (entry == NULL) {
        return NULL;
    }
    entry->font = font;
    entry->users = 1;
    LayoutMetricsInitialize(&entry->layout, cellWidth, MeasureWithFont, font);
    entry->next = fontMetrics;
    fontMetrics = entry;
    return entry;
}

static void ReleaseMetrics(FontMetrics *entry)
{
    if (entry == NULL || --entry->users > 0) {
        return;
    }
    FontMetrics **link = &fontMetrics;
    while (*link != NULL && *link != entry) {
        link = &(*link)->next;
    }
    if (*link != NULL) {
        *link = entry->next;
    }
    LayoutMetricsRelease(&entry->layout);
    MemFree(entry);
}

/* Tells the parent the caret or selection moved, when it asked for that. */
static void NotifySelection(TextView *view)
{
    if (view->notifySelection) {
        HWND parent = GetParent(view->window);
        if (parent != NULL) {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(view->window), TEXTVIEW_SELECTION_CHANGED), (LPARAM)view->window);
        }
    }
}

/* Forward declarations for the load completion. */
static void Redraw(TextView *view);

/*
 * Waits for the rest of a file still being decoded, then shows the whole text. Called before every
 * operation that could touch text beyond the part decoded so far.
 */
static void EnsureLoaded(TextView *view)
{
    if (view->document.load != NULL) {
        TRACE("EnsureLoaded");
        DocumentFinishLoad(&view->document);
        TRACE("load finished");
        Redraw(view);
        NotifySelection(view);
    }
}

/* Grows a scratch block; its old contents are not kept. */
static BOOL GrowScratch(void **block, size_t *capacity, size_t needed, size_t elementSize)
{
    if (needed <= *capacity) {
        return TRUE;
    }
    size_t grown = needed + needed / 2 + 64;
    void *memory = MemAlloc(grown * elementSize);
    if (memory == NULL) {
        return FALSE;
    }
    MemFree(*block);
    *block = memory;
    *capacity = grown;
    return TRUE;
}

static int FullRows(const TextView *view)
{
    int rows = view->clientHeight / view->lineHeight;
    return rows < 1 ? 1 : rows;
}

static int PartialRows(const TextView *view)
{
    int rows = (view->clientHeight + view->lineHeight - 1) / view->lineHeight;
    return rows < 1 ? 1 : rows;
}

static int CaretWidth(void)
{
    DWORD width = 1;
    SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &width, 0);
    return width < 1 ? 1 : (int)width;
}

static void Selection(const TextView *view, size_t *start, size_t *end)
{
    *start = view->anchor < view->caret ? view->anchor : view->caret;
    *end = view->anchor < view->caret ? view->caret : view->anchor;
}

/* The characters of a line without its break; valid until the next call. */
static const wchar_t *LineText(TextView *view, size_t line, size_t *length)
{
    const Document *document = &view->document;
    size_t start = DocumentLineStart(document, line);
    size_t count = DocumentLineEnd(document, line) - start;
    *length = count;

    const wchar_t *text = DocumentPeek(document, start, count);
    if (text != NULL) {
        return text;
    }
    if (!GrowScratch((void **)&view->lineCopy, &view->lineCopyCapacity, count + 1, sizeof(wchar_t))) {
        *length = 0;
        return L"";
    }
    DocumentCopy(document, start, count, view->lineCopy);
    return view->lineCopy;
}

/* Fills view->rows with the row starts of a line and returns how many rows it has. */
static size_t LineRows(TextView *view, const wchar_t *text, size_t length)
{
    if (!view->wordWrap || !GrowScratch((void **)&view->rows, &view->rowsCapacity, length + 1, sizeof(size_t))) {
        view->rows[0] = 0;
        return 1;
    }
    return LayoutWrapLine(view->metrics, text, length, view->wrapWidth, view->rows);
}

static size_t RowOfOffset(const TextView *view, size_t rowCount, size_t offset, BOOL trailing)
{
    size_t row = rowCount - 1;
    while (row > 0 && view->rows[row] > offset) {
        --row;
    }
    if (trailing && row > 0 && view->rows[row] == offset) {
        --row;
    }
    return row;
}

static Place Locate(TextView *view, size_t position, BOOL trailing)
{
    Place place;
    place.line = DocumentLineFromPosition(&view->document, position);
    size_t length = 0;
    const wchar_t *text = LineText(view, place.line, &length);
    size_t offset = position - DocumentLineStart(&view->document, place.line);
    size_t rowCount = LineRows(view, text, length);
    place.row = RowOfOffset(view, rowCount, offset, trailing);
    place.x = LayoutAdvance(view->metrics, text, view->rows[place.row], offset, 0);
    return place;
}

/* Moves line and row by delta rows, stopping at the first and last row; returns the rows moved. */
static long long MoveRows(TextView *view, size_t *line, size_t *row, long long delta)
{
    size_t lastLine = DocumentLineCount(&view->document) - 1;
    if (!view->wordWrap) {
        size_t before = *line;
        if (delta < 0) {
            size_t step = (size_t)(-delta);
            *line = step > *line ? 0 : *line - step;
        } else {
            size_t step = (size_t)delta;
            *line = step > lastLine - *line ? lastLine : *line + step;
        }
        *row = 0;
        return (long long)*line - (long long)before;
    }

    long long moved = 0;
    while (delta > 0) {
        size_t length = 0;
        const wchar_t *text = LineText(view, *line, &length);
        size_t rowCount = LineRows(view, text, length);
        if (*row >= rowCount) {
            *row = rowCount - 1;
        }
        size_t below = rowCount - 1 - *row;
        if ((size_t)delta <= below) {
            *row += (size_t)delta;
            moved += delta;
            break;
        }
        if (*line == lastLine) {
            *row += below;
            moved += (long long)below;
            break;
        }
        moved += (long long)below + 1;
        delta -= (long long)below + 1;
        ++*line;
        *row = 0;
    }
    while (delta < 0) {
        if ((size_t)(-delta) <= *row) {
            *row -= (size_t)(-delta);
            moved += delta;
            break;
        }
        if (*line == 0) {
            moved -= (long long)*row;
            *row = 0;
            break;
        }
        moved -= (long long)*row + 1;
        delta += (long long)*row + 1;
        --*line;
        size_t length = 0;
        const wchar_t *text = LineText(view, *line, &length);
        *row = LineRows(view, text, length) - 1;
    }
    return moved;
}

/* Rows from one place down to another, at most limit; -1 when the second place is above the first. */
static long long RowsBetween(TextView *view, size_t fromLine, size_t fromRow, size_t toLine, size_t toRow, long long limit)
{
    if (toLine < fromLine || (toLine == fromLine && toRow < fromRow)) {
        return -1;
    }
    if (!view->wordWrap) {
        size_t distance = toLine - fromLine;
        return distance > (size_t)limit ? limit : (long long)distance;
    }

    long long distance = 0;
    size_t row = fromRow;
    for (size_t line = fromLine; line < toLine; ++line) {
        size_t length = 0;
        const wchar_t *text = LineText(view, line, &length);
        size_t rowCount = LineRows(view, text, length);
        if (row < rowCount) {
            distance += (long long)(rowCount - row);
        }
        if (distance >= limit) {
            return limit;
        }
        row = 0;
    }
    distance += (long long)(toRow - row);
    return distance > limit ? limit : distance;
}

static void UpdateWrapWidth(TextView *view)
{
    if (!view->wordWrap) {
        view->wrapWidth = 0;
        return;
    }
    int width = view->clientWidth - view->margin * 2;
    view->wrapWidth = width > view->charWidth ? width : view->charWidth;
}

/* ---- Scrolling ----------------------------------------------------------------------------- */

static void ClampVertical(TextView *view)
{
    size_t lineCount = DocumentLineCount(&view->document);
    if (view->topLine >= lineCount) {
        view->topLine = lineCount - 1;
        view->topRow = 0;
    }

    size_t length = 0;
    const wchar_t *text = NULL;
    if (view->wordWrap) {
        text = LineText(view, view->topLine, &length);
        size_t rowCount = LineRows(view, text, length);
        if (view->topRow >= rowCount) {
            view->topRow = rowCount - 1;
        }
    } else {
        view->topRow = 0;
    }

    size_t lastLine = lineCount - 1;
    size_t lastRow = 0;
    if (view->wordWrap) {
        text = LineText(view, lastLine, &length);
        lastRow = LineRows(view, text, length) - 1;
    }
    MoveRows(view, &lastLine, &lastRow, -(long long)(FullRows(view) - 1));
    if (view->topLine > lastLine || (view->topLine == lastLine && view->topRow > lastRow)) {
        view->topLine = lastLine;
        view->topRow = lastRow;
    }
}

static long long ContentWidth(TextView *view)
{
    return view->widestRow + view->charWidth + view->margin * 2;
}

static void ClampHorizontal(TextView *view)
{
    if (view->wordWrap) {
        view->scrollX = 0;
        return;
    }
    long long maximum = ContentWidth(view) - view->clientWidth;
    if (view->scrollX > maximum) {
        view->scrollX = maximum;
    }
    if (view->scrollX < 0) {
        view->scrollX = 0;
    }
}

static void ScrollToCaret(TextView *view)
{
    Place place = Locate(view, view->caret, view->caretTrailing);
    long long visible = FullRows(view);
    long long distance = RowsBetween(view, view->topLine, view->topRow, place.line, place.row, visible);
    if (distance < 0) {
        view->topLine = place.line;
        view->topRow = place.row;
    } else if (distance >= visible) {
        size_t line = place.line;
        size_t row = place.row;
        MoveRows(view, &line, &row, -(visible - 1));
        view->topLine = line;
        view->topRow = row;
    }

    if (!view->wordWrap) {
        long long screenX = view->margin + place.x - view->scrollX;
        if (screenX < view->margin) {
            long long target = place.x - view->clientWidth / 4;
            view->scrollX = target < 0 ? 0 : target;
        } else if (screenX > view->clientWidth - view->charWidth) {
            view->scrollX = place.x + view->margin - view->clientWidth + view->clientWidth / 4;
        }
    }
}

static void UpdateScrollBars(TextView *view)
{
    /* While a file is still being decoded the line count is not final; the bars are set once it is. */
    if (view->document.load != NULL) {
        return;
    }
    size_t lineCount = DocumentLineCount(&view->document);
    SCROLLINFO info = { sizeof info };
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    info.nMin = 0;
    info.nMax = lineCount - 1 > (size_t)INT_MAX ? INT_MAX : (int)(lineCount - 1);
    info.nPage = (UINT)FullRows(view);
    info.nPos = view->topLine > (size_t)INT_MAX ? INT_MAX : (int)view->topLine;
    SetScrollInfo(view->window, SB_VERT, &info, TRUE);

    if (view->wordWrap) {
        ShowScrollBar(view->window, SB_HORZ, FALSE);
        return;
    }
    long long content = ContentWidth(view);
    if (content < view->scrollX + view->clientWidth) {
        content = view->scrollX + view->clientWidth;
    }
    info.nMax = content - 1 > INT_MAX ? INT_MAX : (int)(content - 1);
    info.nPage = (UINT)(view->clientWidth > 0 ? view->clientWidth : 1);
    info.nPos = view->scrollX > INT_MAX ? INT_MAX : (int)view->scrollX;
    ShowScrollBar(view->window, SB_HORZ, TRUE);
    SetScrollInfo(view->window, SB_HORZ, &info, TRUE);
}

static void UpdateCaret(TextView *view)
{
    if (!view->focused) {
        return;
    }
    Place place = Locate(view, view->caret, view->caretTrailing);
    int rows = PartialRows(view);
    long long distance = RowsBetween(view, view->topLine, view->topRow, place.line, place.row, rows + 1);
    long long x = view->margin + place.x - (view->wordWrap ? 0 : view->scrollX);
    if (distance < 0 || distance > rows || x < -view->charWidth || x > (long long)view->clientWidth + view->charWidth) {
        SetCaretPos(-32000, -32000);
    } else {
        SetCaretPos((int)x, (int)distance * view->lineHeight);
    }
}

/* Repaints after scrolling without moving the view to the caret. */
static void Redraw(TextView *view)
{
    ClampVertical(view);
    ClampHorizontal(view);
    UpdateScrollBars(view);
    InvalidateRect(view->window, NULL, FALSE);
    UpdateCaret(view);
}

/* Repaints after the caret or the text changed, scrolling the caret into view. */
static void RevealCaret(TextView *view)
{
    ClampVertical(view);
    ClampHorizontal(view);
    ScrollToCaret(view);
    ClampVertical(view);
    UpdateScrollBars(view);
    InvalidateRect(view->window, NULL, FALSE);
    UpdateCaret(view);
}

static void ScrollRows(TextView *view, long long delta)
{
    size_t line = view->topLine;
    size_t row = view->topRow;
    MoveRows(view, &line, &row, delta);
    view->topLine = line;
    view->topRow = row;
    Redraw(view);
}

static void ScrollToX(TextView *view, long long x)
{
    view->scrollX = x;
    Redraw(view);
}

/* ---- Caret movement ------------------------------------------------------------------------ */

static void MoveCaret(TextView *view, size_t position, BOOL extend, BOOL trailing, BOOL keepDesiredX)
{
    size_t length = DocumentLength(&view->document);
    view->caret = position > length ? length : position;
    if (!extend) {
        view->anchor = view->caret;
    }
    view->caretTrailing = trailing;
    if (!keepDesiredX) {
        view->desiredX = -1;
    }
    HistoryBreakMerge(&view->history);
    RevealCaret(view);
    NotifySelection(view);
}

static size_t PreviousPosition(const TextView *view, size_t position)
{
    if (position == 0) {
        return 0;
    }
    --position;
    if (position > 0 && IS_LOW_SURROGATE(DocumentCharAt(&view->document, position))
        && IS_HIGH_SURROGATE(DocumentCharAt(&view->document, position - 1))) {
        --position;
    }
    return position;
}

static size_t NextPosition(const TextView *view, size_t position)
{
    size_t length = DocumentLength(&view->document);
    if (position >= length) {
        return length;
    }
    ++position;
    if (position < length && IS_LOW_SURROGATE(DocumentCharAt(&view->document, position))
        && IS_HIGH_SURROGATE(DocumentCharAt(&view->document, position - 1))) {
        ++position;
    }
    return position;
}

/* 0 line break, 1 blank, 2 word character, 3 anything else. */
static int CharacterClass(wchar_t ch)
{
    if (ch == L'\n') {
        return 0;
    }
    if (ch == L' ' || ch == L'\t') {
        return 1;
    }
    return ch == L'_' || IsCharAlphaNumericW(ch) ? 2 : 3;
}

static size_t WordRight(const TextView *view, size_t position)
{
    const Document *document = &view->document;
    size_t length = DocumentLength(document);
    if (position >= length) {
        return length;
    }
    int kind = CharacterClass(DocumentCharAt(document, position));
    if (kind == 0) {
        return position + 1;
    }
    if (kind != 1) {
        while (position < length && CharacterClass(DocumentCharAt(document, position)) == kind) {
            ++position;
        }
    }
    while (position < length && CharacterClass(DocumentCharAt(document, position)) == 1) {
        ++position;
    }
    return position;
}

static size_t WordLeft(const TextView *view, size_t position)
{
    const Document *document = &view->document;
    if (position == 0) {
        return 0;
    }
    if (CharacterClass(DocumentCharAt(document, position - 1)) == 0) {
        return position - 1;
    }
    while (position > 0 && CharacterClass(DocumentCharAt(document, position - 1)) == 1) {
        --position;
    }
    if (position == 0) {
        return 0;
    }
    int kind = CharacterClass(DocumentCharAt(document, position - 1));
    if (kind == 0) {
        return position;
    }
    while (position > 0 && CharacterClass(DocumentCharAt(document, position - 1)) == kind) {
        --position;
    }
    return position;
}

static void SelectWordAt(TextView *view, size_t position)
{
    const Document *document = &view->document;
    size_t length = DocumentLength(document);
    if (length == 0) {
        return;
    }
    size_t probe = position < length ? position : length - 1;
    if (CharacterClass(DocumentCharAt(document, probe)) == 0) {
        if (probe == 0 || CharacterClass(DocumentCharAt(document, probe - 1)) == 0) {
            MoveCaret(view, position, FALSE, FALSE, FALSE);
            return;
        }
        --probe;
    }
    int kind = CharacterClass(DocumentCharAt(document, probe));
    size_t start = probe;
    size_t end = probe + 1;
    while (start > 0 && CharacterClass(DocumentCharAt(document, start - 1)) == kind) {
        --start;
    }
    while (end < length && CharacterClass(DocumentCharAt(document, end)) == kind) {
        ++end;
    }
    view->anchor = start;
    MoveCaret(view, end, TRUE, FALSE, FALSE);
}

static void KeyHome(TextView *view, BOOL extend)
{
    Place place = Locate(view, view->caret, view->caretTrailing);
    MoveCaret(view, DocumentLineStart(&view->document, place.line) + view->rows[place.row], extend, FALSE, FALSE);
}

static void KeyEnd(TextView *view, BOOL extend)
{
    Place place = Locate(view, view->caret, view->caretTrailing);
    size_t length = 0;
    const wchar_t *text = LineText(view, place.line, &length);
    size_t rowCount = LineRows(view, text, length);
    BOOL wrapped = place.row + 1 < rowCount;
    size_t rowEnd = wrapped ? view->rows[place.row + 1] : length;
    MoveCaret(view, DocumentLineStart(&view->document, place.line) + rowEnd, extend, wrapped, FALSE);
}

static void KeyVertical(TextView *view, long long delta, BOOL extend)
{
    Place place = Locate(view, view->caret, view->caretTrailing);
    if (view->desiredX < 0) {
        view->desiredX = place.x;
    }
    size_t line = place.line;
    size_t row = place.row;
    if (MoveRows(view, &line, &row, delta) == 0) {
        MoveCaret(view, view->caret, extend, view->caretTrailing, TRUE);
        return;
    }

    size_t length = 0;
    const wchar_t *text = LineText(view, line, &length);
    size_t rowCount = LineRows(view, text, length);
    size_t rowEnd = row + 1 < rowCount ? view->rows[row + 1] : length;
    size_t offset = LayoutOffsetAtX(view->metrics, text, view->rows[row], rowEnd, view->desiredX);
    BOOL trailing = offset == rowEnd && row + 1 < rowCount;
    MoveCaret(view, DocumentLineStart(&view->document, line) + offset, extend, trailing, TRUE);
}

static void KeyPage(TextView *view, int direction, BOOL extend)
{
    long long page = FullRows(view);
    size_t line = view->topLine;
    size_t row = view->topRow;
    MoveRows(view, &line, &row, direction * page);
    view->topLine = line;
    view->topRow = row;
    KeyVertical(view, direction * page, extend);
}

static size_t PositionFromPoint(TextView *view, int x, int y, BOOL *trailing)
{
    long long rowDelta = y >= 0 ? y / view->lineHeight : -((-(long long)y + view->lineHeight - 1) / view->lineHeight);
    size_t line = view->topLine;
    size_t row = view->topRow;
    MoveRows(view, &line, &row, rowDelta);

    size_t length = 0;
    const wchar_t *text = LineText(view, line, &length);
    size_t rowCount = LineRows(view, text, length);
    if (row >= rowCount) {
        row = rowCount - 1;
    }
    size_t rowEnd = row + 1 < rowCount ? view->rows[row + 1] : length;
    long long textX = (long long)x - view->margin + (view->wordWrap ? 0 : view->scrollX);
    size_t offset = LayoutOffsetAtX(view->metrics, text, view->rows[row], rowEnd, textX);
    *trailing = offset == rowEnd && row + 1 < rowCount;
    return DocumentLineStart(&view->document, line) + offset;
}

/* ---- Editing ------------------------------------------------------------------------------- */

static void NotifyChange(TextView *view)
{
    HWND parent = GetParent(view->window);
    if (parent != NULL) {
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(view->window), EN_CHANGE), (LPARAM)view->window);
    }
}

static BOOL ReplaceRange(TextView *view, size_t start, size_t end, const wchar_t *text, size_t length, EditKind kind)
{
    if (!HistoryReplace(&view->history, &view->document, start, end, text, length, kind, view->anchor, view->caret)) {
        MessageBeep(MB_ICONERROR);
        return FALSE;
    }
    view->anchor = view->caret = start + length;
    view->caretTrailing = FALSE;
    view->desiredX = -1;
    RevealCaret(view);
    NotifyChange(view);
    NotifySelection(view);
    return TRUE;
}

static void TypeCharacter(TextView *view, wchar_t ch)
{
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    ReplaceRange(view, start, end, &ch, 1, ch == L'\n' ? EDIT_OTHER : EDIT_TYPING);
}

/* A line break followed by the spaces and tabs the current line starts with, up to 256 of them. */
static void BreakLineWithIndent(TextView *view)
{
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    const Document *document = &view->document;
    size_t lineStart = DocumentLineStart(document, DocumentLineFromPosition(document, start));
    wchar_t text[257];
    size_t length = 0;
    text[length++] = L'\n';
    while (lineStart + length - 1 < start && length < ARRAYSIZE(text)) {
        wchar_t ch = DocumentCharAt(document, lineStart + length - 1);
        if (ch != L' ' && ch != L'\t') {
            break;
        }
        text[length++] = ch;
    }
    ReplaceRange(view, start, end, text, length, EDIT_OTHER);
}

static void Backspace(TextView *view, BOOL word)
{
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    if (start != end) {
        ReplaceRange(view, start, end, NULL, 0, EDIT_OTHER);
    } else if (word) {
        ReplaceRange(view, WordLeft(view, view->caret), view->caret, NULL, 0, EDIT_OTHER);
    } else if (view->caret > 0) {
        ReplaceRange(view, PreviousPosition(view, view->caret), view->caret, NULL, 0, EDIT_BACKSPACE);
    }
}

static void DeleteForward(TextView *view, BOOL word)
{
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    if (start != end) {
        ReplaceRange(view, start, end, NULL, 0, EDIT_OTHER);
    } else if (word) {
        ReplaceRange(view, view->caret, WordRight(view, view->caret), NULL, 0, EDIT_OTHER);
    } else if (view->caret < DocumentLength(&view->document)) {
        ReplaceRange(view, view->caret, NextPosition(view, view->caret), NULL, 0, EDIT_DELETE);
    }
}

static BOOL KeyDown(TextView *view, WPARAM key)
{
    BOOL shift = GetKeyState(VK_SHIFT) < 0;
    BOOL control = GetKeyState(VK_CONTROL) < 0;
    BOOL alt = GetKeyState(VK_MENU) < 0;
    BOOL shortcut = control && !alt;
    size_t start;
    size_t end;
    Selection(view, &start, &end);

    switch (key) {
    case VK_LEFT:
        if (start != end && !shift && !control) {
            MoveCaret(view, start, FALSE, FALSE, FALSE);
        } else {
            MoveCaret(view, control ? WordLeft(view, view->caret) : PreviousPosition(view, view->caret), shift, FALSE, FALSE);
        }
        return TRUE;
    case VK_RIGHT:
        if (start != end && !shift && !control) {
            MoveCaret(view, end, FALSE, FALSE, FALSE);
        } else {
            MoveCaret(view, control ? WordRight(view, view->caret) : NextPosition(view, view->caret), shift, FALSE, FALSE);
        }
        return TRUE;
    case VK_UP:
        if (control) {
            ScrollRows(view, -1);
        } else {
            KeyVertical(view, -1, shift);
        }
        return TRUE;
    case VK_DOWN:
        if (control) {
            ScrollRows(view, 1);
        } else {
            KeyVertical(view, 1, shift);
        }
        return TRUE;
    case VK_PRIOR:
        KeyPage(view, -1, shift);
        return TRUE;
    case VK_NEXT:
        KeyPage(view, 1, shift);
        return TRUE;
    case VK_HOME:
        if (control) {
            MoveCaret(view, 0, shift, FALSE, FALSE);
        } else {
            KeyHome(view, shift);
        }
        return TRUE;
    case VK_END:
        if (control) {
            MoveCaret(view, DocumentLength(&view->document), shift, FALSE, FALSE);
        } else {
            KeyEnd(view, shift);
        }
        return TRUE;
    case VK_BACK:
        Backspace(view, control);
        return TRUE;
    case VK_DELETE:
        if (shift && !control) {
            TextViewCut(view->window);
        } else {
            DeleteForward(view, control);
        }
        return TRUE;
    case VK_INSERT:
        if (control && !shift) {
            TextViewCopy(view->window);
        } else if (shift && !control) {
            TextViewPaste(view->window);
        }
        return TRUE;
    case 'A':
        if (shortcut && !shift) {
            TextViewSelectAll(view->window);
            return TRUE;
        }
        break;
    case 'C':
        if (shortcut && !shift) {
            TextViewCopy(view->window);
            return TRUE;
        }
        break;
    case 'X':
        if (shortcut && !shift) {
            TextViewCut(view->window);
            return TRUE;
        }
        break;
    case 'V':
        if (shortcut && !shift) {
            TextViewPaste(view->window);
            return TRUE;
        }
        break;
    case 'Z':
        if (shortcut) {
            if (shift) {
                TextViewRedo(view->window);
            } else {
                TextViewUndo(view->window);
            }
            return TRUE;
        }
        break;
    case 'Y':
        if (shortcut && !shift) {
            TextViewRedo(view->window);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- Painting ------------------------------------------------------------------------------ */

static COLORREF Blend(COLORREF front, COLORREF back, int weight)
{
    int r = (GetRValue(front) * weight + GetRValue(back) * (255 - weight)) / 255;
    int g = (GetGValue(front) * weight + GetGValue(back) * (255 - weight)) / 255;
    int b = (GetBValue(front) * weight + GetBValue(back) * (255 - weight)) / 255;
    return RGB(r, g, b);
}

static Colors CurrentColors(const TextView *view)
{
    Colors colors;
    if (view->palette != NULL) {
        colors.text = view->palette->text;
        colors.background = view->palette->background;
        colors.selectedText = view->focused ? view->palette->selectedText : view->palette->text;
        colors.selectedBackground = view->focused ? view->palette->selectedBackground : view->palette->unfocusedSelectedBackground;
        return colors;
    }
    colors.text = GetSysColor(COLOR_WINDOWTEXT);
    colors.background = GetSysColor(COLOR_WINDOW);
    if (view->focused) {
        colors.selectedText = GetSysColor(COLOR_HIGHLIGHTTEXT);
        colors.selectedBackground = GetSysColor(COLOR_HIGHLIGHT);
    } else {
        colors.selectedText = colors.text;
        colors.selectedBackground = Blend(GetSysColor(COLOR_HIGHLIGHT), colors.background, 96);
    }
    return colors;
}

static void FillBox(HDC dc, COLORREF color, long long left, int top, long long right, int bottom)
{
    if (right <= left) {
        return;
    }
    RECT box = { (LONG)left, top, (LONG)right, bottom };
    SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &box, NULL, 0, NULL);
}

static void PaintRow(TextView *view, HDC dc, const wchar_t *text, size_t rowStart, size_t rowEnd, size_t lineStart,
    BOOL breakAfter, int y, size_t selectionStart, size_t selectionEnd, const Colors *colors, size_t glyphLimit)
{
    int charWidth = view->charWidth;
    int bottom = y + view->lineHeight;
    long long origin = view->margin - (view->wordWrap ? 0 : view->scrollX);
    long long x = origin;
    if (x > 0) {
        FillBox(dc, colors->background, 0, y, x < view->clientWidth ? x : view->clientWidth, bottom);
    }

    size_t i = rowStart;
    while (i < rowEnd) {
        int width = LayoutCharWidth(view->metrics, text[i], x - origin);
        if (width > 0 && x + width > 0) {
            break;
        }
        x += width;
        ++i;
    }

    size_t first = i;
    size_t count = 0;
    while (i < rowEnd && count < glyphLimit) {
        wchar_t ch = text[i];
        int width = LayoutCharWidth(view->metrics, ch, x - origin);
        if (x >= view->clientWidth && width > 0) {
            break;
        }
        view->glyphs[count] = ch == L'\t' ? L' ' : ch;
        view->advances[count] = width;
        ++count;
        x += width;
        ++i;
    }

    if (!view->wordWrap) {
        long long rowWidth = x - origin;
        size_t remaining = rowEnd - i;
        rowWidth = remaining <= MEASURED_REMAINDER ? LayoutAdvance(view->metrics, text, i, rowEnd, rowWidth)
                                                   : rowWidth + (long long)remaining * charWidth;
        if (rowWidth > view->widestRow) {
            view->widestRow = rowWidth;
        }
    }

    long long runX = x;
    for (size_t k = 0; k < count; k++) {
        runX -= view->advances[k];
    }
    size_t k = 0;
    while (k < count) {
        size_t position = lineStart + first + k;
        BOOL selected = position >= selectionStart && position < selectionEnd;
        size_t next = k;
        int width = 0;
        while (next < count) {
            size_t nextPosition = lineStart + first + next;
            if ((nextPosition >= selectionStart && nextPosition < selectionEnd) != selected) {
                break;
            }
            width += view->advances[next];
            ++next;
        }
        RECT box = { (LONG)runX, y, (LONG)(runX + width), bottom };
        SetTextColor(dc, selected ? colors->selectedText : colors->text);
        SetBkColor(dc, selected ? colors->selectedBackground : colors->background);
        ExtTextOutW(dc, (int)runX, y, ETO_OPAQUE | ETO_CLIPPED, &box, view->glyphs + k, (UINT)(next - k), view->advances + k);
        runX += width;
        k = next;
    }

    long long end = runX;
    if (breakAfter && i == rowEnd) {
        size_t breakPosition = lineStart + rowEnd;
        if (breakPosition >= selectionStart && breakPosition < selectionEnd) {
            FillBox(dc, colors->selectedBackground, end > 0 ? end : 0, y, end + charWidth, bottom);
            end += charWidth;
        }
    }
    if (end < view->clientWidth) {
        FillBox(dc, colors->background, end > 0 ? end : 0, y, view->clientWidth, bottom);
    }
}

static void PaintArea(TextView *view, HDC dc, const RECT *area)
{
    HGDIOBJ previousFont = SelectObject(dc, view->font);
    UINT previousAlign = SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    SetBkMode(dc, OPAQUE);
    Colors colors = CurrentColors(view);
    size_t selectionStart;
    size_t selectionEnd;
    Selection(view, &selectionStart, &selectionEnd);

    /* A glyph is at least a pixel wide, so a row never needs more glyphs than the window has pixels. */
    size_t visibleGlyphs = (size_t)(view->clientWidth > 0 ? view->clientWidth : 0) + VISIBLE_GLYPH_SLACK;
    size_t lineCount = DocumentLineCount(&view->document);
    int y = 0;
    size_t line = view->topLine;
    size_t row = view->topRow;
    while (y < area->bottom && line < lineCount) {
        size_t length = 0;
        const wchar_t *text = LineText(view, line, &length);
        size_t rowCount = LineRows(view, text, length);
        size_t glyphLimit = length + 1 < visibleGlyphs ? length + 1 : visibleGlyphs;
        if (!GrowScratch((void **)&view->glyphs, &view->glyphsCapacity, glyphLimit, sizeof(wchar_t))
            || !GrowScratch((void **)&view->advances, &view->advancesCapacity, glyphLimit, sizeof(int))) {
            break;
        }
        size_t lineStart = DocumentLineStart(&view->document, line);
        for (; row < rowCount && y < area->bottom; ++row, y += view->lineHeight) {
            if (y + view->lineHeight <= area->top) {
                continue;
            }
            size_t rowEnd = row + 1 < rowCount ? view->rows[row + 1] : length;
            PaintRow(view, dc, text, view->rows[row], rowEnd, lineStart, row + 1 == rowCount && line + 1 < lineCount,
                y, selectionStart, selectionEnd, &colors, glyphLimit);
        }
        row = 0;
        ++line;
    }
    if (y < area->bottom) {
        FillBox(dc, colors.background, 0, y, view->clientWidth > area->right ? view->clientWidth : area->right, area->bottom);
    }

    SetTextAlign(dc, previousAlign);
    SelectObject(dc, previousFont);
}

/* ---- Input helpers ------------------------------------------------------------------------- */

static void ApplyFont(TextView *view, HFONT font)
{
    view->font = font;
    HDC dc = GetDC(view->window);
    HGDIOBJ previous = SelectObject(dc, font);
    TEXTMETRICW metrics;
    GetTextMetricsW(dc, &metrics);
    /*
     * The cell is the average width of letters and digits, rounded up. It sizes the margin, tab stops,
     * scroll steps and the characters the font has no glyph for; text uses the widths of its glyphs.
     */
    static const wchar_t sample[] = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int sampleLength = ARRAYSIZE(sample) - 1;
    SIZE extent = { 0 };
    GetTextExtentPoint32W(dc, sample, sampleLength, &extent);
    SelectObject(dc, previous);
    ReleaseDC(view->window, dc);

    view->lineHeight = metrics.tmHeight + metrics.tmExternalLeading > 0 ? metrics.tmHeight + metrics.tmExternalLeading : 1;
    view->charWidth = extent.cx > 0 ? (extent.cx + sampleLength - 1) / sampleLength : 1;
    view->margin = view->charWidth / 2 > 2 ? view->charWidth / 2 : 2;

    FontMetrics *shared = AcquireMetrics(font, view->charWidth);
    if (shared != NULL) {
        ReleaseMetrics(view->shared);
        view->shared = shared;
        view->metrics = &shared->layout;
    }
    LayoutSetTabCells(view->metrics, view->tabCells);
    view->widestRow = 0;
    UpdateWrapWidth(view);
    if (view->focused) {
        DestroyCaret();
        CreateCaret(view->window, NULL, CaretWidth(), view->lineHeight);
        ShowCaret(view->window);
    }
    RevealCaret(view);
}

static void ExtendSelectionToPoint(TextView *view, int x, int y)
{
    BOOL trailing = FALSE;
    size_t position = PositionFromPoint(view, x, y, &trailing);
    MoveCaret(view, position, TRUE, trailing, FALSE);
}

static void ShowContextMenu(TextView *view, LPARAM lParam)
{
    POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (lParam == -1) {
        GetCaretPos(&point);
        point.y += view->lineHeight;
        ClientToScreen(view->window, &point);
    }

    size_t start;
    size_t end;
    Selection(view, &start, &end);
    UINT selectionState = start != end ? MF_ENABLED : MF_GRAYED;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (TextViewCanUndo(view->window) ? MF_ENABLED : MF_GRAYED), MENU_UNDO, L"&Undo");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | selectionState, MENU_CUT, L"Cu&t");
    AppendMenuW(menu, MF_STRING | selectionState, MENU_COPY, L"&Copy");
    AppendMenuW(menu, MF_STRING | (TextViewCanPaste(view->window) ? MF_ENABLED : MF_GRAYED), MENU_PASTE, L"&Paste");
    AppendMenuW(menu, MF_STRING | selectionState, MENU_DELETE, L"&Delete");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_SELECT_ALL, L"Select &All");
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, view->window, NULL);
    DestroyMenu(menu);

    switch (command) {
    case MENU_UNDO:
        TextViewUndo(view->window);
        break;
    case MENU_CUT:
        TextViewCut(view->window);
        break;
    case MENU_COPY:
        TextViewCopy(view->window);
        break;
    case MENU_PASTE:
        TextViewPaste(view->window);
        break;
    case MENU_DELETE:
        TextViewDeleteSelection(view->window);
        break;
    case MENU_SELECT_ALL:
        TextViewSelectAll(view->window);
        break;
    }
}

static void PlaceImeWindow(TextView *view)
{
    HIMC context = ImmGetContext(view->window);
    if (context == NULL) {
        return;
    }
    COMPOSITIONFORM form = { CFS_POINT };
    GetCaretPos(&form.ptCurrentPos);
    ImmSetCompositionWindow(context, &form);
    LOGFONTW font;
    if (GetObjectW(view->font, sizeof font, &font) != 0) {
        ImmSetCompositionFontW(context, &font);
    }
    ImmReleaseContext(view->window, context);
}

static void HandleScrollBar(TextView *view, int bar, WPARAM wParam)
{
    SCROLLINFO info = { sizeof info, SIF_TRACKPOS };
    if (bar == SB_VERT) {
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            ScrollRows(view, -1);
            break;
        case SB_LINEDOWN:
            ScrollRows(view, 1);
            break;
        case SB_PAGEUP:
            ScrollRows(view, -FullRows(view));
            break;
        case SB_PAGEDOWN:
            ScrollRows(view, FullRows(view));
            break;
        case SB_TOP:
            view->topLine = 0;
            view->topRow = 0;
            Redraw(view);
            break;
        case SB_BOTTOM:
            view->topLine = DocumentLineCount(&view->document) - 1;
            view->topRow = 0;
            Redraw(view);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            GetScrollInfo(view->window, SB_VERT, &info);
            view->topLine = (size_t)(info.nTrackPos < 0 ? 0 : info.nTrackPos);
            view->topRow = 0;
            Redraw(view);
            break;
        }
        return;
    }

    switch (LOWORD(wParam)) {
    case SB_LINELEFT:
        ScrollToX(view, view->scrollX - view->charWidth);
        break;
    case SB_LINERIGHT:
        ScrollToX(view, view->scrollX + view->charWidth);
        break;
    case SB_PAGELEFT:
        ScrollToX(view, view->scrollX - view->clientWidth);
        break;
    case SB_PAGERIGHT:
        ScrollToX(view, view->scrollX + view->clientWidth);
        break;
    case SB_LEFT:
        ScrollToX(view, 0);
        break;
    case SB_RIGHT:
        ScrollToX(view, ContentWidth(view));
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        GetScrollInfo(view->window, SB_HORZ, &info);
        ScrollToX(view, info.nTrackPos);
        break;
    }
}

/* ---- Window procedure ---------------------------------------------------------------------- */

static void DestroyView(TextView *view)
{
    ReleaseMetrics(view->shared);
    DocumentRelease(&view->document);
    HistoryRelease(&view->history);
    MemFree(view->lineCopy);
    MemFree(view->rows);
    MemFree(view->glyphs);
    MemFree(view->advances);
    MemFree(view);
}

static LRESULT CALLBACK TextViewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    TextView *view = ViewFrom(window);
    if (view == NULL) {
        if (message == WM_NCCREATE) {
            view = MemAllocZero(sizeof *view);
            if (view == NULL) {
                return FALSE;
            }
            view->window = window;
            view->rowsCapacity = 256;
            view->rows = MemAlloc(view->rowsCapacity * sizeof(size_t));
            if (view->rows == NULL || !DocumentInitialize(&view->document)) {
                MemFree(view->rows);
                MemFree(view);
                return FALSE;
            }
            HistoryInitialize(&view->history);
            view->desiredX = -1;
            view->tabCells = LAYOUT_DEFAULT_TAB_CELLS;
            view->lineHeight = 1;
            view->charWidth = 1;
            SetWindowLongPtrW(window, 0, (LONG_PTR)view);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (view->document.load != NULL) {
        /* Painting and the window plumbing only read what is decoded; input and text requests wait for the rest. */
        switch (message) {
        case WM_KEYDOWN:
        case WM_CHAR:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_CONTEXTMENU:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_VSCROLL:
        case WM_HSCROLL:
        case WM_TIMER:
        case WM_GETTEXT:
        case WM_GETTEXTLENGTH:
        case WM_IME_STARTCOMPOSITION:
        case EM_GETSEL:
        case EM_SETSEL:
        case WM_TEXTVIEW_LOAD_DONE:
            EnsureLoaded(view);
            break;
        case WM_MOUSEMOVE:
            if (view->selecting) {
                EnsureLoaded(view);
            }
            break;
        }
    }

    switch (message) {
    case WM_CREATE:
        ApplyFont(view, (HFONT)GetStockObject(SYSTEM_FIXED_FONT));
        return 0;

    case WM_TEXTVIEW_LOAD_DONE:
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, 0, 0);
        DestroyView(view);
        return 0;

    case WM_SIZE:
        view->clientWidth = LOWORD(lParam);
        view->clientHeight = HIWORD(lParam);
        UpdateWrapWidth(view);
        Redraw(view);
        return 0;

    case WM_PAINT: {
        long long widest = view->widestRow;
        PAINTSTRUCT paint;
        TRACE("WM_PAINT");
        HDC dc = BeginPaint(window, &paint);
        PaintArea(view, dc, &paint.rcPaint);
        EndPaint(window, &paint);
        TRACE("painted");
        if (view->widestRow != widest && !view->wordWrap) {
            UpdateScrollBars(view);
        }
        return 0;
    }

    case WM_PRINTCLIENT: {
        RECT area;
        GetClientRect(window, &area);
        PaintArea(view, (HDC)wParam, &area);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SETFOCUS:
        view->focused = TRUE;
        CreateCaret(window, NULL, CaretWidth(), view->lineHeight);
        UpdateCaret(view);
        ShowCaret(window);
        /* Focus only changes the colors of selected text. */
        if (view->anchor != view->caret) {
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;

    case WM_KILLFOCUS:
        view->focused = FALSE;
        HideCaret(window);
        DestroyCaret();
        if (view->anchor != view->caret) {
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, IDC_IBEAM));
            return TRUE;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;

    case WM_KEYDOWN:
        if (KeyDown(view, wParam)) {
            return 0;
        }
        break;

    case WM_CHAR: {
        wchar_t ch = (wchar_t)wParam;
        if (ch == L'\r') {
            ch = L'\n';
        }
        if (ch == L'\n' && view->autoIndent) {
            BreakLineWithIndent(view);
        } else if ((ch >= 0x20 && ch != 0x7F) || ch == L'\n' || ch == L'\t') {
            TypeCharacter(view, ch);
        }
        return 0;
    }

    case WM_IME_STARTCOMPOSITION:
        PlaceImeWindow(view);
        break;

    case WM_LBUTTONDOWN: {
        SetFocus(window);
        BOOL trailing = FALSE;
        size_t position = PositionFromPoint(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &trailing);
        MoveCaret(view, position, (wParam & MK_SHIFT) != 0, trailing, FALSE);
        view->selecting = TRUE;
        SetCapture(window);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        BOOL trailing = FALSE;
        SelectWordAt(view, PositionFromPoint(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &trailing));
        return 0;
    }

    case WM_MOUSEMOVE:
        if (view->selecting) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            ExtendSelectionToPoint(view, x, y);
            if (x < 0 || y < 0 || x >= view->clientWidth || y >= view->clientHeight) {
                SetTimer(window, AUTOSCROLL_TIMER, AUTOSCROLL_INTERVAL, NULL);
            } else {
                KillTimer(window, AUTOSCROLL_TIMER);
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == AUTOSCROLL_TIMER && view->selecting) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            ExtendSelectionToPoint(view, point.x, point.y);
        }
        return 0;

    case WM_LBUTTONUP:
        if (view->selecting) {
            view->selecting = FALSE;
            KillTimer(window, AUTOSCROLL_TIMER);
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        view->selecting = FALSE;
        KillTimer(window, AUTOSCROLL_TIMER);
        return 0;

    case WM_RBUTTONDOWN: {
        SetFocus(window);
        BOOL trailing = FALSE;
        size_t position = PositionFromPoint(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &trailing);
        size_t start;
        size_t end;
        Selection(view, &start, &end);
        if (position < start || position > end || start == end) {
            MoveCaret(view, position, FALSE, trailing, FALSE);
        }
        return 0;
    }

    case WM_CONTEXTMENU:
        ShowContextMenu(view, lParam);
        return 0;

    case WM_MOUSEWHEEL: {
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
            /* Ctrl with the wheel is for the parent, which changes the text size. */
            break;
        }
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        view->wheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
        long long rowsPerNotch = lines == WHEEL_PAGESCROLL ? FullRows(view) : (long long)lines;
        long long rows = (long long)view->wheelDelta * rowsPerNotch / WHEEL_DELTA;
        if (rows != 0) {
            view->wheelDelta -= (int)(rows * WHEEL_DELTA / rowsPerNotch);
            ScrollRows(view, -rows);
        } else if (rowsPerNotch == 0) {
            view->wheelDelta = 0;
        }
        return 0;
    }

    case WM_MOUSEHWHEEL: {
        UINT characters = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &characters, 0);
        view->horizontalWheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
        long long columns = (long long)view->horizontalWheelDelta * (long long)characters / WHEEL_DELTA;
        if (columns != 0) {
            view->horizontalWheelDelta -= (int)(columns * WHEEL_DELTA / (long long)characters);
            ScrollToX(view, view->scrollX + columns * view->charWidth);
        } else if (characters == 0) {
            view->horizontalWheelDelta = 0;
        }
        return 0;
    }

    case WM_VSCROLL:
        HandleScrollBar(view, SB_VERT, wParam);
        return 0;

    case WM_HSCROLL:
        HandleScrollBar(view, SB_HORZ, wParam);
        return 0;

    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        InvalidateRect(window, NULL, FALSE);
        break;

    case WM_GETTEXTLENGTH: {
        size_t length = DocumentLength(&view->document) + DocumentLineCount(&view->document) - 1;
        return length > (size_t)INT_MAX ? INT_MAX : (LRESULT)length;
    }

    case WM_GETTEXT: {
        size_t capacity = (size_t)wParam;
        wchar_t *out = (wchar_t *)lParam;
        const wchar_t *text = DocumentText(&view->document);
        if (capacity == 0 || out == NULL || text == NULL) {
            return 0;
        }
        size_t length = DocumentLength(&view->document);
        size_t written = 0;
        for (size_t i = 0; i < length; ++i) {
            size_t needed = text[i] == L'\n' ? 2 : 1;
            if (written + needed >= capacity) {
                break;
            }
            if (text[i] == L'\n') {
                out[written++] = L'\r';
            }
            out[written++] = text[i];
        }
        out[written] = 0;
        return (LRESULT)written;
    }

    case EM_GETSEL: {
        size_t start;
        size_t end;
        Selection(view, &start, &end);
        if (wParam != 0) {
            *(DWORD *)wParam = (DWORD)start;
        }
        if (lParam != 0) {
            *(DWORD *)lParam = (DWORD)end;
        }
        return MAKELRESULT(start > 0xFFFF ? 0xFFFF : start, end > 0xFFFF ? 0xFFFF : end);
    }

    case EM_SETSEL: {
        size_t length = DocumentLength(&view->document);
        size_t anchor = (INT_PTR)wParam < 0 ? length : (size_t)wParam;
        size_t caret = (INT_PTR)lParam < 0 ? length : (size_t)lParam;
        TextViewSetSelection(window, anchor, caret);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/* ---- Public functions ---------------------------------------------------------------------- */

BOOL TextViewRegisterClass(HINSTANCE instance)
{
    WNDCLASSEXW windowClass = { sizeof windowClass };
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = TextViewProc;
    windowClass.cbWndExtra = sizeof(LONG_PTR);
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    windowClass.lpszClassName = TEXTVIEW_CLASS;
    return RegisterClassExW(&windowClass) != 0;
}

HWND TextViewCreate(HWND parent, int id, HINSTANCE instance)
{
    return CreateWindowExW(0, TEXTVIEW_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, instance, NULL);
}

static void ResetView(TextView *view)
{
    HistoryClear(&view->history);
    view->anchor = 0;
    view->caret = 0;
    view->caretTrailing = FALSE;
    view->desiredX = -1;
    view->topLine = 0;
    view->topRow = 0;
    view->scrollX = 0;
    view->widestRow = 0;
    RevealCaret(view);
    NotifySelection(view);
}

BOOL TextViewSetText(HWND window, wchar_t *text, size_t length)
{
    TextView *view = ViewFrom(window);
    if (!DocumentAdopt(&view->document, text, length)) {
        return FALSE;
    }
    ResetView(view);
    return TRUE;
}

void TextViewSetLoad(HWND window, TextLoad *load)
{
    TextView *view = ViewFrom(window);
    DocumentAdoptLoad(&view->document, load);
    ResetView(view);
}

void TextViewStartLoad(HWND window)
{
    DocumentStartLoad(&ViewFrom(window)->document);
}

BOOL TextViewIsLoading(HWND window)
{
    return ViewFrom(window)->document.load != NULL;
}

void TextViewClear(HWND window)
{
    TextView *view = ViewFrom(window);
    Document empty;
    if (DocumentInitialize(&empty)) {
        DocumentRelease(&view->document);
        view->document = empty;
    } else {
        DocumentDelete(&view->document, 0, DocumentLength(&view->document));
    }
    ResetView(view);
}

const wchar_t *TextViewGetText(HWND window, size_t *length)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    *length = DocumentLength(&view->document);
    return DocumentText(&view->document);
}

BOOL TextViewIsModified(HWND window)
{
    return HistoryIsModified(&ViewFrom(window)->history);
}

void TextViewMarkSaved(HWND window)
{
    HistoryMarkSaved(&ViewFrom(window)->history);
}

void TextViewSetFont(HWND window, HFONT font)
{
    ApplyFont(ViewFrom(window), font);
}

void TextViewSetColors(HWND window, const TextViewColors *colors)
{
    ViewFrom(window)->palette = colors;
    InvalidateRect(window, NULL, FALSE);
}

void TextViewSetWordWrap(HWND window, BOOL wrap)
{
    TextView *view = ViewFrom(window);
    view->wordWrap = wrap;
    view->topRow = 0;
    view->scrollX = 0;
    view->caretTrailing = FALSE;
    view->desiredX = -1;
    UpdateWrapWidth(view);
    ShowScrollBar(window, SB_HORZ, !wrap);
    RevealCaret(view);
}

void TextViewGetSelection(HWND window, size_t *start, size_t *end)
{
    Selection(ViewFrom(window), start, end);
}

void TextViewSetSelection(HWND window, size_t anchor, size_t caret)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t length = DocumentLength(&view->document);
    view->anchor = anchor > length ? length : anchor;
    MoveCaret(view, caret, TRUE, FALSE, FALSE);
}

size_t TextViewLineCount(HWND window)
{
    return DocumentLineCount(&ViewFrom(window)->document);
}

size_t TextViewCaretLine(HWND window)
{
    TextView *view = ViewFrom(window);
    return DocumentLineFromPosition(&view->document, view->caret);
}

void TextViewGoToLine(HWND window, size_t line)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t lineCount = DocumentLineCount(&view->document);
    MoveCaret(view, DocumentLineStart(&view->document, line < lineCount ? line : lineCount - 1), FALSE, FALSE, FALSE);
}

BOOL TextViewCanUndo(HWND window)
{
    return HistoryCanUndo(&ViewFrom(window)->history);
}

BOOL TextViewCanRedo(HWND window)
{
    return HistoryCanRedo(&ViewFrom(window)->history);
}

BOOL TextViewCanPaste(HWND window)
{
    UNREFERENCED_PARAMETER(window);
    return IsClipboardFormatAvailable(CF_UNICODETEXT);
}

static void AfterHistoryStep(TextView *view, size_t anchor, size_t caret)
{
    view->anchor = anchor;
    view->caret = caret;
    view->caretTrailing = FALSE;
    view->desiredX = -1;
    RevealCaret(view);
    NotifyChange(view);
    NotifySelection(view);
}

void TextViewUndo(HWND window)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t anchor = 0;
    size_t caret = 0;
    if (HistoryUndo(&view->history, &view->document, &anchor, &caret)) {
        AfterHistoryStep(view, anchor, caret);
    }
}

void TextViewRedo(HWND window)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t anchor = 0;
    size_t caret = 0;
    if (HistoryRedo(&view->history, &view->document, &anchor, &caret)) {
        AfterHistoryStep(view, anchor, caret);
    }
}

void TextViewCopy(HWND window)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    const wchar_t *text = start != end ? DocumentText(&view->document) : NULL;
    if (text == NULL) {
        return;
    }

    size_t breaks = 0;
    for (size_t i = start; i < end; ++i) {
        breaks += text[i] == L'\n';
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (end - start + breaks + 1) * sizeof(wchar_t));
    wchar_t *out = memory != NULL ? GlobalLock(memory) : NULL;
    if (out == NULL) {
        if (memory != NULL) {
            GlobalFree(memory);
        }
        return;
    }
    size_t written = 0;
    for (size_t i = start; i < end; ++i) {
        if (text[i] == L'\n') {
            out[written++] = L'\r';
        }
        out[written++] = text[i];
    }
    out[written] = 0;
    GlobalUnlock(memory);

    if (OpenClipboard(window)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
            GlobalFree(memory);
        }
        CloseClipboard();
    } else {
        GlobalFree(memory);
    }
}

void TextViewCut(HWND window)
{
    TextViewCopy(window);
    TextViewDeleteSelection(window);
}

void TextViewPaste(HWND window)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(window)) {
        return;
    }
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *source = data != NULL ? GlobalLock(data) : NULL;
    if (source != NULL) {
        size_t available = GlobalSize(data) / sizeof(wchar_t);
        size_t length = 0;
        while (length < available && source[length] != 0) {
            ++length;
        }
        wchar_t *copy = MemAlloc((length + 1) * sizeof(wchar_t));
        if (copy != NULL) {
            memcpy(copy, source, length * sizeof(wchar_t));
            length = DocumentNormalizeLineBreaks(copy, length);
        }
        GlobalUnlock(data);
        CloseClipboard();
        if (copy != NULL) {
            size_t start;
            size_t end;
            Selection(view, &start, &end);
            ReplaceRange(view, start, end, copy, length, EDIT_OTHER);
            MemFree(copy);
        }
        return;
    }
    CloseClipboard();
}

void TextViewDeleteSelection(HWND window)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    if (start != end) {
        ReplaceRange(view, start, end, NULL, 0, EDIT_OTHER);
    }
}

void TextViewSelectAll(HWND window)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    view->anchor = 0;
    view->caret = DocumentLength(&view->document);
    view->caretTrailing = FALSE;
    view->desiredX = -1;
    HistoryBreakMerge(&view->history);
    Redraw(view);
    NotifySelection(view);
}

BOOL TextViewFind(HWND window, const wchar_t *pattern, SearchOptions options, BOOL down)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t length = DocumentLength(&view->document);
    const wchar_t *text = DocumentText(&view->document);
    size_t patternLength = (size_t)lstrlenW(pattern);
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    size_t found = 0;
    if (text == NULL || !SearchFind(text, length, down ? end : start, pattern, patternLength, options, down, &found)) {
        return FALSE;
    }
    view->anchor = found;
    MoveCaret(view, found + patternLength, TRUE, FALSE, FALSE);
    return TRUE;
}

BOOL TextViewReplace(HWND window, const wchar_t *pattern, const wchar_t *with, SearchOptions options)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t patternLength = (size_t)lstrlenW(pattern);
    size_t start;
    size_t end;
    Selection(view, &start, &end);
    const wchar_t *text = DocumentText(&view->document);
    if (text != NULL && patternLength > 0 && end - start == patternLength
        && CompareStringOrdinal(text + start, (int)patternLength, pattern, (int)patternLength, !options.matchCase) == CSTR_EQUAL
        && (!options.wholeWord || SearchIsWholeWord(text, DocumentLength(&view->document), start, patternLength))) {
        ReplaceRange(view, start, end, with, (size_t)lstrlenW(with), EDIT_OTHER);
    }
    return TextViewFind(window, pattern, options, TRUE);
}

BOOL TextViewReplaceRange(HWND window, size_t start, size_t end, const wchar_t *text, size_t length)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t documentLength = DocumentLength(&view->document);
    end = end < documentLength ? end : documentLength;
    start = start < end ? start : end;
    return ReplaceRange(view, start, end, text, length, EDIT_OTHER);
}

size_t TextViewLineStart(HWND window, size_t line)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    return DocumentLineStart(&view->document, line);
}

size_t TextViewLineEnd(HWND window, size_t line)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    return DocumentLineEnd(&view->document, line);
}

size_t TextViewLineFromPosition(HWND window, size_t position)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    return DocumentLineFromPosition(&view->document, position);
}

size_t TextViewCaretPosition(HWND window)
{
    return ViewFrom(window)->caret;
}

void TextViewSetTabSize(HWND window, int cells)
{
    TextView *view = ViewFrom(window);
    view->tabCells = cells > 0 ? cells : LAYOUT_DEFAULT_TAB_CELLS;
    LayoutSetTabCells(view->metrics, view->tabCells);
    view->widestRow = 0;
    view->desiredX = -1;
    RevealCaret(view);
}

void TextViewSetAutoIndent(HWND window, BOOL autoIndent)
{
    ViewFrom(window)->autoIndent = autoIndent;
}

void TextViewNotifySelection(HWND window, BOOL notify)
{
    ViewFrom(window)->notifySelection = notify;
}

size_t TextViewReplaceAll(HWND window, const wchar_t *pattern, const wchar_t *with, SearchOptions options)
{
    TextView *view = ViewFrom(window);
    EnsureLoaded(view);
    size_t length = DocumentLength(&view->document);
    const wchar_t *text = DocumentText(&view->document);
    if (text == NULL) {
        return 0;
    }
    wchar_t *replacement = NULL;
    size_t replacementLength = 0;
    size_t rangeStart = 0;
    size_t rangeEnd = 0;
    size_t count = SearchReplaceAll(text, length, pattern, (size_t)lstrlenW(pattern), with, (size_t)lstrlenW(with),
        options, &replacement, &replacementLength, &rangeStart, &rangeEnd);
    if (count > 0) {
        ReplaceRange(view, rangeStart, rangeEnd, replacement, replacementLength, EDIT_OTHER);
        MemFree(replacement);
    }
    return count;
}
