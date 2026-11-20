// Halo - installing Halo for this user, the same way Hangar installs it:
//   %LOCALAPPDATA%\Programs\Halo\HaloMouse.exe + a Start menu shortcut + an Apps & features entry
// whose uninstall command is `HaloMouse.exe --uninstall`.
#pragma once
#include "common.h"

namespace install {

std::wstring InstallDir();                 // %LOCALAPPDATA%\Programs\Halo
bool RunningFromInstallDir();
void RefreshRegistration();                // when installed: keep the shortcut and Apps & features entry current
bool InstallSelf(std::wstring* error);     // copies this exe into InstallDir() and registers it
// Versions up to 0.1.0 were Halo.exe, which Discord shows as Halo: Combat Evolved. When this copy
// is still called that, it renames itself to HaloMouse.exe, repoints the shortcut, Apps & features
// and startup entries, and starts the renamed copy where this one is (restoreJson). Returns true
// when the caller should exit.
bool MigrateExeName(const std::wstring& restoreJson);
int Uninstall();                           // `HaloMouse.exe --uninstall` (asks first); returns the exit code

} // namespace install
