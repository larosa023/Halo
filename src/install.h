// Halo - installing Halo for this user, the same way Hangar installs it:
//   %LOCALAPPDATA%\Programs\Halo\Halo.exe + a Start menu shortcut + an Apps & features entry
// whose uninstall command is `Halo.exe --uninstall`.
#pragma once
#include "common.h"

namespace install {

std::wstring InstallDir();                 // %LOCALAPPDATA%\Programs\Halo
bool RunningFromInstallDir();
void RefreshRegistration();                // when installed: keep the shortcut and Apps & features entry current
bool InstallSelf(std::wstring* error);     // copies this exe into InstallDir() and registers it
int Uninstall();                           // `Halo.exe --uninstall` (asks first); returns the exit code

} // namespace install
