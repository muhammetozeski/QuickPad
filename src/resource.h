#pragma once

#define IDI_APP                  1

#define IDR_MENU                 100
#define IDR_ACCELERATORS         101

#define IDM_FILE_NEW             40001
#define IDM_FILE_OPEN            40002
#define IDM_FILE_SAVE            40003
#define IDM_FILE_SAVE_AS         40004
#define IDM_FILE_CLOSE           40005
#define IDM_FILE_RELOAD          40006
#define IDM_FILE_OPEN_FOLDER     40007
#define IDM_FILE_COPY_PATH       40008

#define IDM_EDIT_UNDO            40010
#define IDM_EDIT_REDO            40011
#define IDM_EDIT_CUT             40012
#define IDM_EDIT_COPY            40013
#define IDM_EDIT_PASTE           40014
#define IDM_EDIT_DELETE          40015
#define IDM_EDIT_FIND            40016
#define IDM_EDIT_FIND_NEXT       40017
#define IDM_EDIT_FIND_PREVIOUS   40018
#define IDM_EDIT_REPLACE         40019
#define IDM_EDIT_GOTO            40020
#define IDM_EDIT_SELECT_ALL      40021
#define IDM_EDIT_TIME_DATE       40022
#define IDM_EDIT_DUPLICATE_LINE  40023
#define IDM_EDIT_DELETE_LINE     40024
#define IDM_EDIT_MOVE_LINE_UP    40025
#define IDM_EDIT_MOVE_LINE_DOWN  40026
#define IDM_EDIT_JOIN_LINES      40027
#define IDM_EDIT_UPPERCASE       40028
#define IDM_EDIT_LOWERCASE       40029
#define IDM_EDIT_TRIM_WHITESPACE 40051

#define IDM_FORMAT_WORD_WRAP     40030
#define IDM_FORMAT_AUTO_INDENT   40031
#define IDM_FORMAT_FONT          40032
#define IDM_FORMAT_TAB_2         40033
#define IDM_FORMAT_TAB_4         40034
#define IDM_FORMAT_TAB_8         40035
#define IDM_FORMAT_CRLF          40036
#define IDM_FORMAT_LF            40037
#define IDM_FORMAT_CR            40038

/* In the order of the encoding choices in editor.c. */
#define IDM_FORMAT_UTF8          40061
#define IDM_FORMAT_UTF8_BOM      40062
#define IDM_FORMAT_UTF16LE       40063
#define IDM_FORMAT_UTF16BE       40064
#define IDM_FORMAT_ANSI          40065

#define IDM_VIEW_ZOOM_IN         40070
#define IDM_VIEW_ZOOM_OUT        40071
#define IDM_VIEW_ZOOM_RESET      40072
#define IDM_VIEW_STATUS_BAR      40073
#define IDM_VIEW_ALWAYS_ON_TOP   40074
#define IDM_VIEW_FULL_SCREEN     40075

#define IDM_START_WITH_WINDOWS   40040

#define IDM_TRAY_NEW             40100
#define IDM_TRAY_EXIT            40101
#define IDM_TRAY_POOL_SIZE       40102

#define IDD_POOL_SIZE            200
#define IDC_POOL_SIZE            1001

#define IDR_SHELL_EXTENSION      300

#define IDD_GOTO                 201
#define IDC_GOTO_LINE            1002

#ifndef IDC_STATIC
#define IDC_STATIC               (-1)
#endif
