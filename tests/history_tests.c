/* Unit tests for src/history.c: merged steps, undo, redo and the saved state. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "history.h"
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

static BOOL TextIs(Document *document, const wchar_t *expected)
{
    const wchar_t *text = DocumentText(document);
    return text != NULL && wcscmp(text, expected) == 0;
}

static void Type(History *history, Document *document, size_t position, const wchar_t *text)
{
    for (size_t i = 0; text[i] != 0; ++i) {
        HistoryReplace(history, document, position + i, position + i, text + i, 1, EDIT_TYPING, position + i, position + i);
    }
}

int wmain(void)
{
    Document document;
    History history;
    DocumentInitialize(&document);
    HistoryInitialize(&history);
    size_t anchor = 0;
    size_t caret = 0;

    CHECK(!HistoryIsModified(&history), "new document is not modified");
    Type(&history, &document, 0, L"hello");
    CHECK(TextIs(&document, L"hello"), "typing");
    CHECK(history.count == 1, "typing merges into one step");
    CHECK(HistoryIsModified(&history), "typing modifies");

    CHECK(HistoryUndo(&history, &document, &anchor, &caret), "undo typing");
    CHECK(TextIs(&document, L""), "undo removes all typed characters");
    CHECK(anchor == 0 && caret == 0, "undo restores the caret");
    CHECK(!HistoryIsModified(&history), "undo back to the saved state");
    CHECK(HistoryRedo(&history, &document, &anchor, &caret), "redo typing");
    CHECK(TextIs(&document, L"hello") && caret == 5, "redo puts text and caret back");

    HistoryBreakMerge(&history);
    Type(&history, &document, 5, L" world");
    CHECK(history.count == 2, "a break starts a new step");

    HistoryMarkSaved(&history);
    CHECK(!HistoryIsModified(&history), "saved");
    HistoryReplace(&history, &document, 11, 11, L"!", 1, EDIT_TYPING, 11, 11);
    CHECK(history.count == 3, "typing after a save does not merge into the saved step");
    CHECK(HistoryIsModified(&history), "typing after save modifies");
    HistoryUndo(&history, &document, &anchor, &caret);
    CHECK(!HistoryIsModified(&history), "undo to the saved step");

    HistoryReplace(&history, &document, 10, 11, NULL, 0, EDIT_BACKSPACE, 11, 11);
    HistoryReplace(&history, &document, 9, 10, NULL, 0, EDIT_BACKSPACE, 10, 10);
    HistoryReplace(&history, &document, 8, 9, NULL, 0, EDIT_BACKSPACE, 9, 9);
    CHECK(TextIs(&document, L"hello wo"), "backspaces");
    CHECK(history.count == 3, "backspaces merge");
    HistoryUndo(&history, &document, &anchor, &caret);
    CHECK(TextIs(&document, L"hello world") && caret == 11, "undo backspaces");
    CHECK(HistoryCanRedo(&history), "redo is available after undo");

    HistoryReplace(&history, &document, 0, 1, NULL, 0, EDIT_DELETE, 0, 0);
    CHECK(!HistoryCanRedo(&history), "an edit drops redo");
    HistoryReplace(&history, &document, 0, 1, NULL, 0, EDIT_DELETE, 0, 0);
    CHECK(TextIs(&document, L"llo world") && history.count == 3, "deletes merge");
    CHECK(HistoryIsModified(&history), "deletes modify");
    HistoryUndo(&history, &document, &anchor, &caret);
    CHECK(TextIs(&document, L"hello world"), "undo deletes");
    CHECK(!HistoryIsModified(&history), "undo returns to the saved text");

    HistoryReplace(&history, &document, 11, 11, L"?", 1, EDIT_OTHER, 11, 11);
    HistoryMarkSaved(&history);
    HistoryUndo(&history, &document, &anchor, &caret);
    HistoryReplace(&history, &document, 0, 0, L">", 1, EDIT_OTHER, 0, 0);
    HistoryUndo(&history, &document, &anchor, &caret);
    CHECK(TextIs(&document, L"hello world"), "undo an edit made after undoing past the save");
    CHECK(HistoryIsModified(&history), "a saved state dropped from redo cannot be reached again");

    HistoryReplace(&history, &document, 0, 5, L"bye\nnow", 7, EDIT_OTHER, 0, 5);
    CHECK(TextIs(&document, L"bye\nnow world") && DocumentLineCount(&document) == 2, "replace a range");
    HistoryUndo(&history, &document, &anchor, &caret);
    CHECK(TextIs(&document, L"hello world") && anchor == 0 && caret == 5, "undo a replacement restores the selection");
    HistoryRedo(&history, &document, &anchor, &caret);
    CHECK(TextIs(&document, L"bye\nnow world") && caret == 7, "redo a replacement");

    HistoryClear(&history);
    CHECK(!HistoryCanUndo(&history) && !HistoryIsModified(&history), "clear");

    HistoryRelease(&history);
    DocumentRelease(&document);
    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
