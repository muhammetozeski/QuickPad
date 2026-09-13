#include "fileio.h"
#include "quickpad.h"

/* Larger files are mapped instead of copied into a buffer first. */
#define MAP_THRESHOLD (1024 * 1024)
#define MAX_FILE_SIZE 0x7FFFFFF0
#define READ_CHUNK (1u << 30)

BOOL FileLoad(const wchar_t *path, wchar_t **text, size_t *length, TextFormat *format, DWORD *error)
{
    *text = NULL;
    *length = 0;

    /* Mapping is only safe while nobody else can shorten the file, which this share mode ensures. */
    BOOL mappable = TRUE;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        mappable = FALSE;
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }
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
        } else {
            size_t done = 0;
            while (done < size) {
                DWORD chunk = size - done > READ_CHUNK ? READ_CHUNK : (DWORD)(size - done);
                DWORD read = 0;
                if (!ReadFile(file, buffer + done, chunk, &read, NULL)) {
                    *error = GetLastError();
                    MemFree(buffer);
                    buffer = NULL;
                    break;
                }
                if (read == 0) {
                    break;
                }
                done += read;
            }
            size = done;
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
