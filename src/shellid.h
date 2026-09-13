#pragma once

/*
 * The open command in QuickPadShell.dll. File associations name it with DelegateExecute, and
 * QuickPad.exe --register points the class at the DLL.
 */
#define QUICKPAD_OPEN_COMMAND_CLSID \
    { 0x0DAFC6E1, 0xFF1B, 0x4AA1, { 0x9E, 0x0F, 0xBF, 0x6D, 0x3F, 0xAC, 0xA6, 0xDB } }
#define QUICKPAD_OPEN_COMMAND_CLSID_TEXT L"{0DAFC6E1-FF1B-4AA1-9E0F-BF6D3FACA6DB}"

#define QUICKPAD_SHELL_DLL_NAME L"QuickPadShell.dll"
