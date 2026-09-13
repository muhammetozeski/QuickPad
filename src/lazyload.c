/*
 * Libraries that only the editor needs are bound on first call instead of at process start, so a
 * launch that only hands a file to the running instance never loads them.
 *
 * Headers that declare a function with dllimport make the compiler call it through a pointer named
 * __imp_<function>; LAZY_POINTER defines that pointer, starting at a stub that binds the function.
 * Headers without dllimport make the compiler call the function by name; LAZY_FUNCTION defines a
 * function with that name which binds on first call. The returns argument is "return", or empty
 * for functions without a result.
 */
#include "quickpad.h"

#include <imm.h>
#include <objbase.h>

static FARPROC Bind(const wchar_t *library, const char *function)
{
    HMODULE module = LoadLibraryExW(library, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    FARPROC address = module != NULL ? GetProcAddress(module, function) : NULL;
    if (address == NULL) {
        MessageBoxW(NULL, L"A Windows system library could not be loaded.", QP_APP_NAME, MB_ICONERROR);
        ExitProcess(1);
    }
    return address;
}

#define LAZY_FUNCTION(library, result, returns, name, parameters, arguments)           \
    static result (WINAPI *Bound_##name) parameters;                                    \
    result WINAPI name parameters                                                       \
    {                                                                                    \
        if (Bound_##name == NULL) {                                                     \
            Bound_##name = (result (WINAPI *) parameters)Bind(library, #name);          \
        }                                                                                \
        returns Bound_##name arguments;                                                 \
    }

#define LAZY_POINTER(library, result, returns, name, parameters, arguments)            \
    static result WINAPI Bind_##name parameters;                                        \
    result (WINAPI *__imp_##name) parameters = Bind_##name;                             \
    static result WINAPI Bind_##name parameters                                         \
    {                                                                                    \
        __imp_##name = (result (WINAPI *) parameters)Bind(library, #name);              \
        returns __imp_##name arguments;                                                 \
    }

LAZY_FUNCTION(L"imm32.dll", HIMC, return, ImmGetContext, (HWND window), (window))
LAZY_FUNCTION(L"imm32.dll", BOOL, return, ImmReleaseContext, (HWND window, HIMC context), (window, context))
LAZY_FUNCTION(L"imm32.dll", BOOL, return, ImmSetCompositionWindow, (HIMC context, LPCOMPOSITIONFORM form), (context, form))
LAZY_FUNCTION(L"imm32.dll", BOOL, return, ImmSetCompositionFontW, (HIMC context, LPLOGFONTW font), (context, font))

LAZY_POINTER(L"ole32.dll", HRESULT, return, CoInitializeEx, (LPVOID reserved, DWORD model), (reserved, model))
LAZY_POINTER(L"ole32.dll", HRESULT, return, CoCreateInstance,
    (REFCLSID classId, LPUNKNOWN outer, DWORD context, REFIID interfaceId, LPVOID *object),
    (classId, outer, context, interfaceId, object))
LAZY_POINTER(L"ole32.dll", void, , CoTaskMemFree, (LPVOID block), (block))
