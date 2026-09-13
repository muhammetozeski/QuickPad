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

    PathIn(path, L"written.txt");
    const unsigned char first[] = { 'o', 'n', 'e' };
    const unsigned char second[] = { 't', 'w', 'o', '!', '\r', '\n' };
    CHECK(FileWrite(path, first, sizeof first, &error), "write a new file");
    CHECK(FileLoad(path, &text, &length, &format, &error) && length == 3 && wmemcmp(text, L"one", 3) == 0, "new file content");
    MemFree(text);
    SetFileAttributesW(path, FILE_ATTRIBUTE_HIDDEN);
    CHECK(FileWrite(path, second, sizeof second, &error), "replace an existing hidden file");
    CHECK(FileLoad(path, &text, &length, &format, &error) && length == 6 && wmemcmp(text, L"two!\r\n", 6) == 0, "replaced content");
    MemFree(text);
    CHECK((GetFileAttributesW(path) & FILE_ATTRIBUTE_HIDDEN) != 0, "replacing keeps the hidden attribute");
    wchar_t leftover[MAX_PATH];
    PathIn(leftover, L"written.txt.QuickPad-save.tmp");
    CHECK(GetFileAttributesW(leftover) == INVALID_FILE_ATTRIBUTES, "no temporary file is left behind");
    SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY);
    CHECK(!FileWrite(path, first, sizeof first, &error) && error == ERROR_ACCESS_DENIED, "read-only file is refused");
    CHECK(GetFileAttributesW(leftover) == INVALID_FILE_ATTRIBUTES, "no temporary file after a refusal");
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path);

    RemoveDirectoryW(folder);
    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
