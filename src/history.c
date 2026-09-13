#include "history.h"
#include "quickpad.h"

#define HISTORY_NO_SAVED_STATE ((size_t)-1)

static void FreeEntry(HistoryEntry *entry)
{
    MemFree(entry->removed);
    MemFree(entry->inserted);
}

static void DropRedo(History *history)
{
    for (size_t i = history->count; i < history->total; ++i) {
        FreeEntry(&history->entries[i]);
    }
    history->total = history->count;
    if (history->savedCount != HISTORY_NO_SAVED_STATE && history->savedCount > history->count) {
        history->savedCount = HISTORY_NO_SAVED_STATE;
    }
}

/* Makes room for length more characters at the end of a text block. */
static BOOL Reserve(wchar_t **text, size_t *capacity, size_t used, size_t length)
{
    if (used + length <= *capacity) {
        return TRUE;
    }
    size_t grown = (used + length) * 2 + 16;
    wchar_t *block = MemAlloc(grown * sizeof(wchar_t));
    if (block == NULL) {
        return FALSE;
    }
    if (used > 0) {
        memcpy(block, *text, used * sizeof(wchar_t));
    }
    MemFree(*text);
    *text = block;
    *capacity = grown;
    return TRUE;
}

void HistoryInitialize(History *history)
{
    History empty = { 0 };
    *history = empty;
}

void HistoryClear(History *history)
{
    history->count = 0;
    DropRedo(history);
    history->savedCount = 0;
    history->mergeBlocked = FALSE;
}

void HistoryRelease(History *history)
{
    HistoryClear(history);
    MemFree(history->entries);
    HistoryInitialize(history);
}

/* Continues the last step when this edit extends it in the same way. */
static BOOL TryMerge(History *history, Document *document, size_t start, size_t end, const wchar_t *text,
    size_t length, EditKind kind)
{
    if (kind == EDIT_OTHER || history->mergeBlocked || history->count == 0 || history->count != history->total
        || history->savedCount == history->count) {
        return FALSE;
    }

    HistoryEntry *last = &history->entries[history->count - 1];
    if (last->kind != kind) {
        return FALSE;
    }

    size_t removedLength = end - start;
    if (kind == EDIT_TYPING) {
        if (removedLength != 0 || last->position + last->insertedLength != start
            || !Reserve(&last->inserted, &last->insertedCapacity, last->insertedLength, length)
            || !DocumentInsert(document, start, text, length)) {
            return FALSE;
        }
        memcpy(last->inserted + last->insertedLength, text, length * sizeof(wchar_t));
        last->insertedLength += length;
        return TRUE;
    }

    if (length != 0 || last->insertedLength != 0) {
        return FALSE;
    }
    if (kind == EDIT_BACKSPACE && end == last->position) {
        if (!Reserve(&last->removed, &last->removedCapacity, last->removedLength, removedLength)) {
            return FALSE;
        }
        memmove(last->removed + removedLength, last->removed, last->removedLength * sizeof(wchar_t));
        DocumentCopy(document, start, removedLength, last->removed);
        last->removedLength += removedLength;
        last->position = start;
        DocumentDelete(document, start, removedLength);
        return TRUE;
    }
    if (kind == EDIT_DELETE && start == last->position) {
        if (!Reserve(&last->removed, &last->removedCapacity, last->removedLength, removedLength)) {
            return FALSE;
        }
        DocumentCopy(document, start, removedLength, last->removed + last->removedLength);
        last->removedLength += removedLength;
        DocumentDelete(document, start, removedLength);
        return TRUE;
    }
    return FALSE;
}

BOOL HistoryReplace(History *history, Document *document, size_t start, size_t end, const wchar_t *text,
    size_t length, EditKind kind, size_t anchorBefore, size_t caretBefore)
{
    if (start == end && length == 0) {
        return TRUE;
    }
    if (TryMerge(history, document, start, end, text, length, kind)) {
        history->mergeBlocked = FALSE;
        return TRUE;
    }

    HistoryEntry entry = { 0 };
    entry.position = start;
    entry.kind = kind;
    entry.anchorBefore = anchorBefore;
    entry.caretBefore = caretBefore;
    entry.removedLength = end - start;
    entry.insertedLength = length;
    if (!Reserve(&entry.removed, &entry.removedCapacity, 0, entry.removedLength)
        || !Reserve(&entry.inserted, &entry.insertedCapacity, 0, length)) {
        FreeEntry(&entry);
        return FALSE;
    }
    DocumentCopy(document, start, entry.removedLength, entry.removed);
    if (length > 0) {
        memcpy(entry.inserted, text, length * sizeof(wchar_t));
    }

    DropRedo(history);
    if (history->count == history->capacity) {
        size_t capacity = history->capacity * 2 + 64;
        HistoryEntry *entries = MemAlloc(capacity * sizeof(HistoryEntry));
        if (entries == NULL) {
            FreeEntry(&entry);
            return FALSE;
        }
        if (history->count > 0) {
            memcpy(entries, history->entries, history->count * sizeof(HistoryEntry));
        }
        MemFree(history->entries);
        history->entries = entries;
        history->capacity = capacity;
    }

    DocumentDelete(document, start, entry.removedLength);
    if (!DocumentInsert(document, start, text, length)) {
        DocumentInsert(document, start, entry.removed, entry.removedLength);
        FreeEntry(&entry);
        return FALSE;
    }

    history->entries[history->count++] = entry;
    history->total = history->count;
    history->mergeBlocked = FALSE;
    return TRUE;
}

BOOL HistoryCanUndo(const History *history)
{
    return history->count > 0;
}

BOOL HistoryCanRedo(const History *history)
{
    return history->count < history->total;
}

BOOL HistoryUndo(History *history, Document *document, size_t *anchor, size_t *caret)
{
    if (history->count == 0) {
        return FALSE;
    }
    HistoryEntry *entry = &history->entries[history->count - 1];
    DocumentDelete(document, entry->position, entry->insertedLength);
    if (!DocumentInsert(document, entry->position, entry->removed, entry->removedLength)) {
        DocumentInsert(document, entry->position, entry->inserted, entry->insertedLength);
        return FALSE;
    }
    --history->count;
    history->mergeBlocked = TRUE;
    *anchor = entry->anchorBefore;
    *caret = entry->caretBefore;
    return TRUE;
}

BOOL HistoryRedo(History *history, Document *document, size_t *anchor, size_t *caret)
{
    if (history->count == history->total) {
        return FALSE;
    }
    HistoryEntry *entry = &history->entries[history->count];
    DocumentDelete(document, entry->position, entry->removedLength);
    if (!DocumentInsert(document, entry->position, entry->inserted, entry->insertedLength)) {
        DocumentInsert(document, entry->position, entry->removed, entry->removedLength);
        return FALSE;
    }
    ++history->count;
    history->mergeBlocked = TRUE;
    *anchor = *caret = entry->position + entry->insertedLength;
    return TRUE;
}

void HistoryBreakMerge(History *history)
{
    history->mergeBlocked = TRUE;
}

void HistoryMarkSaved(History *history)
{
    history->savedCount = history->count;
    history->mergeBlocked = TRUE;
}

BOOL HistoryIsModified(const History *history)
{
    return history->count != history->savedCount;
}
