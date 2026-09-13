/* Unit tests for src/document.c. Random edits are applied to the document and to a plain array side by side. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "document.h"
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

static unsigned long long randomState = 0x9E3779B97F4A7C15ull;

static size_t Random(size_t bound)
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 7;
    randomState ^= randomState << 17;
    return bound == 0 ? 0 : (size_t)(randomState % bound);
}

typedef struct Model {
    wchar_t *text;
    size_t length;
    size_t capacity;
} Model;

static void ModelInsert(Model *model, size_t position, const wchar_t *text, size_t count)
{
    memmove(model->text + position + count, model->text + position, (model->length - position) * sizeof(wchar_t));
    memcpy(model->text + position, text, count * sizeof(wchar_t));
    model->length += count;
}

static void ModelDelete(Model *model, size_t start, size_t count)
{
    memmove(model->text + start, model->text + start + count, (model->length - start - count) * sizeof(wchar_t));
    model->length -= count;
}

/* Compares the whole content and the line index against the model. */
static BOOL Matches(Document *document, const Model *model)
{
    if (DocumentLength(document) != model->length) {
        return FALSE;
    }
    for (size_t i = 0; i < model->length; ++i) {
        if (DocumentCharAt(document, i) != model->text[i]) {
            return FALSE;
        }
    }

    size_t line = 0;
    if (DocumentLineStart(document, 0) != 0) {
        return FALSE;
    }
    for (size_t i = 0; i < model->length; ++i) {
        if (model->text[i] == L'\n') {
            ++line;
            if (line >= DocumentLineCount(document) || DocumentLineStart(document, line) != i + 1) {
                return FALSE;
            }
        }
    }
    return line + 1 == DocumentLineCount(document);
}

static void TestRandomEdits(void)
{
    Document document;
    CHECK(DocumentInitialize(&document), "initialize");

    Model model = { NULL, 0, 200000 };
    model.text = MemAlloc(model.capacity * sizeof(wchar_t));
    const wchar_t alphabet[] = L"ab\ncd\n\nefg ";
    wchar_t chunk[64];

    for (int step = 0; step < 20000; ++step) {
        if (Random(3) != 0 || model.length == 0) {
            size_t count = 1 + Random(Random(8) == 0 ? 60 : 4);
            if (model.length + count >= model.capacity) {
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                chunk[i] = alphabet[Random(sizeof alphabet / sizeof alphabet[0] - 1)];
            }
            size_t position = Random(model.length + 1);
            CHECK(DocumentInsert(&document, position, chunk, count), "insert");
            ModelInsert(&model, position, chunk, count);
        } else {
            size_t start = Random(model.length);
            size_t count = 1 + Random(model.length - start < 40 ? model.length - start : 40);
            if (count > model.length - start) {
                count = model.length - start;
            }
            DocumentDelete(&document, start, count);
            ModelDelete(&model, start, count);
        }

        if (step % 250 == 0) {
            CHECK(Matches(&document, &model), "random edits keep text and lines");
        }
        if (step % 1000 == 0 && model.length > 0) {
            size_t position = Random(model.length);
            size_t line = DocumentLineFromPosition(&document, position);
            CHECK(DocumentLineStart(&document, line) <= position, "line from position starts before it");
            CHECK(DocumentLineEnd(&document, line) >= position, "line from position ends after it");
        }
    }
    CHECK(Matches(&document, &model), "random edits final state");

    const wchar_t *text = DocumentText(&document);
    CHECK(text != NULL && text[model.length] == 0 && wmemcmp(text, model.text, model.length) == 0, "text is contiguous");
    CHECK(Matches(&document, &model), "text keeps content");

    MemFree(model.text);
    DocumentRelease(&document);
}

static void TestNormalize(void)
{
    wchar_t text[] = L"a\r\nb\rc\nd\r\r\n\r";
    size_t length = DocumentNormalizeLineBreaks(text, wcslen(text));
    CHECK(length == 10 && wmemcmp(text, L"a\nb\nc\nd\n\n\n", 10) == 0, "crlf and cr become lf");

    wchar_t plain[] = L"no breaks here, just a line longer than a block";
    size_t plainLength = wcslen(plain);
    CHECK(DocumentNormalizeLineBreaks(plain, plainLength) == plainLength, "text without cr is unchanged");
}

static void TestAdopt(void)
{
    const wchar_t source[] = L"first\r\nsecond\r\n\r\nfourth line with more text\rfifth";
    size_t length = wcslen(source);
    wchar_t *text = MemAlloc((length + 2) * sizeof(wchar_t));
    memcpy(text, source, length * sizeof(wchar_t));

    Document document;
    DocumentInitialize(&document);
    CHECK(DocumentAdopt(&document, text, length), "adopt");
    CHECK(DocumentLineCount(&document) == 5, "adopt counts lines");
    CHECK(DocumentLineStart(&document, 1) == 6, "second line start");
    CHECK(DocumentLineEnd(&document, 1) == 12, "second line end");
    CHECK(DocumentLineStart(&document, 2) == 13 && DocumentLineEnd(&document, 2) == 13, "empty third line");
    CHECK(DocumentLineEnd(&document, 4) == DocumentLength(&document), "last line ends at length");
    CHECK(DocumentLineFromPosition(&document, 12) == 1, "line break belongs to its line");
    CHECK(DocumentLineFromPosition(&document, 13) == 2, "position after break is next line");

    CHECK(DocumentInsert(&document, 0, L"x", 1), "insert into adopted text grows the gap");
    CHECK(DocumentCharAt(&document, 0) == L'x' && DocumentCharAt(&document, 1) == L'f', "insert at start");
    CHECK(DocumentLineStart(&document, 1) == 7, "line starts move after insert");

    DocumentDelete(&document, 0, DocumentLength(&document));
    CHECK(DocumentLength(&document) == 0 && DocumentLineCount(&document) == 1, "delete everything");
    DocumentRelease(&document);
}

int wmain(void)
{
    TestNormalize();
    TestAdopt();
    TestRandomEdits();
    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
