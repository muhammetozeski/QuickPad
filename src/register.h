#pragma once

/*
 * Registers this executable for the current user: QuickPad under Open with for common text file
 * types, an entry under Settings > Apps > Default apps, and QuickPadShell.dll (written next to the
 * executable from its resources) as the open command the shell runs for them. It does not change
 * which program is the default. Returns the process exit code.
 */
int RegisterFileTypes(void);
