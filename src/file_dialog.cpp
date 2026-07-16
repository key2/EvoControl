#include "file_dialog.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace evo {

namespace {

// Run a command and capture its first line of stdout (the chosen path).
std::string runCapture(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return {};
    std::array<char, 4096> buf{};
    while (fgets(buf.data(), (int)buf.size(), p)) out += buf.data();
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    // Trim trailing newline/CR.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

bool haveCmd(const char* name) {
#if defined(_WIN32)
    return true;  // where.exe check is noisy; assume PowerShell exists.
#else
    std::string c = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(c.c_str()) == 0;
#endif
}

}  // namespace

std::string openImageFileDialog(const std::string& title) {
#if defined(_WIN32)
    // PowerShell OpenFileDialog.
    std::string ps =
        "powershell -NoProfile -STA -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "$f=New-Object System.Windows.Forms.OpenFileDialog;"
        "$f.Title='" + title + "';"
        "$f.Filter='Images|*.png;*.jpg;*.jpeg;*.webp;*.gif;*.bmp|All files|*.*';"
        "if($f.ShowDialog() -eq 'OK'){[Console]::Out.Write($f.FileName)}\"";
    return runCapture(ps);
#elif defined(__APPLE__)
    std::string osa =
        "osascript -e 'POSIX path of (choose file with prompt \"" + title +
        "\" of type {\"public.image\"})' 2>/dev/null";
    return runCapture(osa);
#else
    // Linux: prefer zenity, fall back to kdialog.
    if (haveCmd("zenity")) {
        std::string c =
            "zenity --file-selection --title=\"" + title +
            "\" --file-filter='Images | *.png *.jpg *.jpeg *.webp *.gif *.bmp' "
            "2>/dev/null";
        return runCapture(c);
    }
    if (haveCmd("kdialog")) {
        std::string c =
            "kdialog --getopenfilename . "
            "'*.png *.jpg *.jpeg *.webp *.gif *.bmp|Images' --title \"" +
            title + "\" 2>/dev/null";
        return runCapture(c);
    }
    return {};
#endif
}

}  // namespace evo
