#pragma once

// openImageFileDialog — show the OS's native "open file" dialog and return the
// chosen image path (empty if cancelled). Portable, dependency-free: uses
// zenity/kdialog on Linux, PowerShell on Windows, and osascript on macOS. This
// avoids bundling a file-picker library and keeps GLFW (which has none) as-is.

#include <string>

namespace evo {

/// Returns the selected file path, or "" if the user cancelled / no dialog tool
/// is available. `title` is the dialog title.
std::string openImageFileDialog(const std::string& title = "Choose a picture");

}  // namespace evo
