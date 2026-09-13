#pragma once

#include "document.h"

/* What produced an edit; consecutive typing, backspaces or deletes are undone as one step. */
typedef enum EditKind {
    EDIT_OTHER,
    EDIT_TYPING,
    EDIT_BACKSPACE,
    EDIT_DELETE,
} EditKind;

/* One undoable step: the text at position was removed and the inserted text put in its place. */
typedef struct HistoryEntry {
    size_t position;
    wchar_t *removed;
    size_t removedLength;
    size_t removedCapacity;
    wchar_t *inserted;
    size_t insertedLength;
    size_t insertedCapacity;
    size_t anchorBefore;
    size_t caretBefore;
    EditKind kind;
} HistoryEntry;

typedef struct History {
    HistoryEntry *entries;
    size_t count;
    size_t total;
    size_t capacity;
    size_t savedCount;
    BOOL mergeBlocked;
} History;

void HistoryInitialize(History *history);
void HistoryRelease(History *history);

/* Forgets every step; the current text counts as saved. */
void HistoryClear(History *history);

/*
 * Replaces [start, end) of the document with text (L'\n' line breaks) and records the step.
 * anchorBefore and caretBefore are restored by undo. Returns FALSE and leaves the document
 * unchanged when memory runs out.
 */
BOOL HistoryReplace(History *history, Document *document, size_t start, size_t end, const wchar_t *text,
    size_t length, EditKind kind, size_t anchorBefore, size_t caretBefore);

BOOL HistoryCanUndo(const History *history);
BOOL HistoryCanRedo(const History *history);
BOOL HistoryUndo(History *history, Document *document, size_t *anchor, size_t *caret);
BOOL HistoryRedo(History *history, Document *document, size_t *anchor, size_t *caret);

/* The next edit starts a new step even if it could continue the last one. */
void HistoryBreakMerge(History *history);

void HistoryMarkSaved(History *history);
BOOL HistoryIsModified(const History *history);
