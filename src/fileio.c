#include "fileio.h"
#include "blocks.h"
#include "quickpad.h"

/* Larger files are mapped instead of copied into a buffer first. */
#define MAP_THRESHOLD (1024 * 1024)
/* Larger files are loaded through a ready block; see FileBeginLoad. */
#define LOAD_BLOCK_THRESHOLD (64 * 1024)
#define MAX_FILE_SIZE 0x7FFFFFF0
#define READ_CHUNK (1u << 30)

/* What a load reads from: the open file and the buffer its bytes go to, a ready block for larger files. */
typedef struct FileSource {
    HANDLE file;
    unsigned char *buffer;
    size_t capacity;
    BOOL block;
} FileSource;

static void ReleaseSource(void *context)
{
    FileSource *source = context;
    if (source->block) {
        BlockReturn(source->buffer, source->capacity);
    } else {
        MemFree(source->buffer);
    }
    if (source->file != INVALID_HANDLE_VALUE) {
        CloseHandle(source->file);
    }
    MemFree(source);
}

/* Reads length bytes at offset in the file into destination; the file must be open without FILE_FLAG_OVERLAPPED. */
static BOOL ReadAt(HANDLE file, unsigned char *destination, size_t offset, size_t length)
{
    size_t done = 0;
    while (done < length) {
        OVERLAPPED position = { 0 };
        position.Offset = (DWORD)(offset + done);
        position.OffsetHigh = (DWORD)((unsigned long long)(offset + done) >> 32);
        DWORD chunk = length - done > READ_CHUNK ? READ_CHUNK : (DWORD)(length - done);
        DWORD read = 0;
        if (!ReadFile(file, destination + done, chunk, &read, &position)) {
            return FALSE;
        }
        if (read == 0) {
            SetLastError(ERROR_HANDLE_EOF);
            return FALSE;
        }
        done += read;
    }
    return TRUE;
}

static BOOL ReadSource(void *context, size_t offset, size_t length)
{
    FileSource *source = context;
    return ReadAt(source->file, source->buffer + offset, offset, length);
}

/* Opens for reading; a file another program holds open for writing is still read, but not mapped. */
static HANDLE OpenForReading(const wchar_t *path, BOOL *mappable)
{
    *mappable = TRUE;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        *mappable = FALSE;
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }
    return file;
}

static BOOL ReadAll(HANDLE file, unsigned char *buffer, size_t *size, DWORD *error)
{
    size_t done = 0;
    while (done < *size) {
        DWORD chunk = *size - done > READ_CHUNK ? READ_CHUNK : (DWORD)(*size - done);
        DWORD read = 0;
        if (!ReadFile(file, buffer + done, chunk, &read, NULL)) {
            *error = GetLastError();
            return FALSE;
        }
        if (read == 0) {
            break;
        }
        done += read;
    }
    *size = done;
    return TRUE;
}

BOOL FileBeginLoad(const wchar_t *path, TextFormat *format, HWND notifyWindow, UINT notifyMessage, TextLoad **load, DWORD *error)
{
    *load = NULL;
    BOOL mappable = TRUE;
    HANDLE file = OpenForReading(path, &mappable);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return FALSE;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file, &fileSize)) {
        *error = GetLastError();
        CloseHandle(file);
        return FALSE;
    }
    if (fileSize.QuadPart > MAX_FILE_SIZE) {
        *error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return FALSE;
    }

    FileSource *source = MemAllocZero(sizeof *source);
    if (source == NULL) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(file);
        return FALSE;
    }
    source->file = file;
    size_t size = (size_t)fileSize.QuadPart;

    /* Larger files go into a block whose pages are already mapped, so neither reading nor decoding waits for that. */
    if (size >= LOAD_BLOCK_THRESHOLD) {
        source->buffer = BlockTake(size, &source->capacity);
        source->block = TRUE;
    } else {
        source->buffer = MemAlloc(size > 0 ? size : 1);
        source->capacity = size;
    }
    if (source->buffer == NULL) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        ReleaseSource(source);
        return FALSE;
    }

    /* The first part is read here; the rest is read by a worker once the load is started. */
    size_t available = size < TEXTLOAD_FIRST_BYTES ? size : TEXTLOAD_FIRST_BYTES;
    if (available > 0 && !ReadAt(file, source->buffer, 0, available)) {
        *error = GetLastError();
        ReleaseSource(source);
        return FALSE;
    }

    *load = TextLoadBegin(source->buffer, available, size, ReadSource, ReleaseSource, source, format, notifyWindow, notifyMessage);
    if (*load == NULL) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        ReleaseSource(source);
        return FALSE;
    }
    return TRUE;
}

BOOL FileLoad(const wchar_t *path, wchar_t **text, size_t *length, TextFormat *format, DWORD *error)
{
    *text = NULL;
    *length = 0;

    /* Mapping is only safe while nobody else can shorten the file, which the share mode ensures. */
    BOOL mappable = TRUE;
    HANDLE file = OpenForReading(path, &mappable);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return FALSE;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file, &fileSize)) {
        *error = GetLastError();
        CloseHandle(file);
        return FALSE;
    }
    if (fileSize.QuadPart > MAX_FILE_SIZE) {
        *error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return FALSE;
    }

    size_t size = (size_t)fileSize.QuadPart;
    const unsigned char *data = (const unsigned char *)"";
    HANDLE mapping = NULL;
    const unsigned char *view = NULL;
    unsigned char *buffer = NULL;

    if (size >= MAP_THRESHOLD && mappable) {
        mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
        view = mapping != NULL ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
        data = view;
    }
    if (size > 0 && view == NULL) {
        buffer = MemAlloc(size);
        if (buffer == NULL) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        } else if (!ReadAll(file, buffer, &size, error)) {
            MemFree(buffer);
            buffer = NULL;
        }
        data = buffer;
    }

    if (data != NULL) {
        *text = TextDecode(data, size, format, length);
        if (*text == NULL) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
    }

    if (view != NULL) {
        UnmapViewOfFile(view);
    }
    if (mapping != NULL) {
        CloseHandle(mapping);
    }
    MemFree(buffer);
    CloseHandle(file);
    return *text != NULL;
}

static BOOL WriteAll(HANDLE file, const unsigned char *data, size_t size, DWORD *error)
{
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > READ_CHUNK ? READ_CHUNK : (DWORD)(size - done);
        DWORD written = 0;
        if (!WriteFile(file, data + done, chunk, &written, NULL)) {
            *error = GetLastError();
            return FALSE;
        }
        done += written;
    }
    return TRUE;
}

static BOOL WriteInPlace(const wchar_t *path, const unsigned char *data, size_t size, DWORD *error)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return FALSE;
    }
    BOOL written = WriteAll(file, data, size, error);
    if (written && !SetEndOfFile(file)) {
        *error = GetLastError();
        written = FALSE;
    }
    CloseHandle(file);
    return written;
}

BOOL FileWrite(const wchar_t *path, const unsigned char *data, size_t size, DWORD *error)
{
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return WriteInPlace(path, data, size, error);
    }

    static const wchar_t suffix[] = L".QuickPad-save.tmp";
    size_t pathLength = (size_t)lstrlenW(path);
    wchar_t *temporary = MemAlloc((pathLength + ARRAYSIZE(suffix)) * sizeof(wchar_t));
    if (temporary == NULL) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return FALSE;
    }
    memcpy(temporary, path, pathLength * sizeof(wchar_t));
    memcpy(temporary + pathLength, suffix, sizeof suffix);

    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        MemFree(temporary);
        return WriteInPlace(path, data, size, error);
    }
    BOOL written = WriteAll(file, data, size, error);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary);
        MemFree(temporary);
        return FALSE;
    }

    BOOL swapped = attributes == INVALID_FILE_ATTRIBUTES
        ? MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH)
        : ReplaceFileW(path, temporary, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, NULL, NULL);
    if (!swapped) {
        DeleteFileW(temporary);
        swapped = WriteInPlace(path, data, size, error);
    }
    MemFree(temporary);
    return swapped;
}
