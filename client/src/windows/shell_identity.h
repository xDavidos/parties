#pragma once

#include <windows.h>

// Gives an installed app's window the AppUserModelID of the Start menu
// shortcuts and relaunch properties that name the boot stub, so a taskbar
// pin starts Parties.exe instead of this build's exe, which build garbage
// collection later deletes. Does nothing for portable or development copies.
void apply_shell_identity(HWND hwnd);

// The shell requires the properties to be cleared before the window goes.
void clear_shell_identity(HWND hwnd);
