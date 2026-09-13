/* Tests for src/fileio.c against real files in a temporary folder. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "fileio.h"
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

static wchar_t folder[MAX_PATH];

static void PathIn(wchar_t *path, const wchar_t *name)
{
    swprintf(path, MAX_PATH, L"%s%s", folder, name);
}

static void WriteBytes(const wchar_t *path, const unsigned char *data, size_t size)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    if (size > 0) {
        WriteFile(file, data, (DWORD)size, &written, NULL);
    }
    CloseHandle(file);
}

int wmain(void)
{
    GetTempPathW(MAX_PATH, folder);
    wcscat_s(folder, MAX_PATH, L"QuickPadTests\\");
    CreateDirectoryW(folder, NULL);

    wchar_t path[MAX_PATH];
    wchar_t *text = NULL;
    size_t length = 0;
    TextFormat format;
    DWORD error = 0;

    PathIn(path, L"small.txt");
    const unsigned char small[] = { 0xEF, 0xBB, 0xBF, 'a', 0x0D, 0x0A, 0xC4, 0x9F };
    WriteBytes(path, small, sizeof small);
    CHECK(FileLoad(path, &text, &length, &format, &error), "small file loads");
    CHECK(length == 4 && wmemcmp(text, L"a\r\n\x011F", 4) == 0, "small file text");
    CHECK(format.encoding == TEXT_ENCODING_UTF8 && format.byteOrderMark && format.lineEnding == LINE_ENDING_CRLF, "small file format");
    MemFree(text);
    DeleteFileW(path);

    PathIn(path, L"missing.txt");
    CHECK(!FileLoad(path, &text, &length, &format, &error) && error == ERROR_FILE_NOT_FOUND, "missing file");

    PathIn(path, L"empty.txt");
    WriteBytes(path, NULL, 0);
    CHECK(FileLoad(path, &text, &length, &format, &error) && length == 0, "empty file");
    MemFree(text);
    DeleteFileW(path);

    PathIn(path, L"large.txt");
    size_t size = 2 * 1024 * 1024;
    unsigned char *large = MemAlloc(size);
    for (size_t i = 0; i < size; ++i) {
        large[i] = i % 64 == 63 ? '\n' : (unsigned char)('a' + i % 26);
    }
    WriteBytes(path, large, size);
    CHECK(FileLoad(path, &text, &length, &format, &error), "large file loads through a mapping");
    CHECK(length == size && text[0] == L'a' && text[63] == L'\n' && text[size - 1] == L'\n', "large file text");
    CHECK(format.lineEnding == LINE_ENDING_LF, "large file line ending");
    MemFree(text);

    HANDLE writer = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(writer != INVALID_HANDLE_VALUE, "open the file for writing");
    CHECK(FileLoad(path, &text, &length, &format, &error), "file open for writing elsewhere still loads");
    CHECK(length == size, "file open for writing elsewhere text");
    MemFree(text);
    CloseHandle(writer);
    DeleteFileW(path);
    MemFree(large);

    RemoveDirectoryW(folder);
    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
