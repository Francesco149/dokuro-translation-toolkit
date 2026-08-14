// Win32 helpers — UTF-8 file dialogs, explorer, path utils. All paths are
// UTF-8 strings internally; converted to wide only at the Win32 boundary.
#pragma once
#include <cstdint>
#include <string>

namespace win32util {

// Directory of the running exe (UTF-8, no trailing slash).
std::string exe_dir();

// Converts between UTF-8 and UTF-16.
std::string wide_to_utf8(const wchar_t* s);
std::wstring utf8_to_wide(const std::string& s);

// Opens a folder in Explorer (or the file's folder if a file is passed).
bool open_in_explorer(const std::string& path);

// Common file dialogs. filter like "Script files (*.UNI)\0*.UNI\0All files\0*.*\0".
// Returns true and fills out on OK.
bool dialog_open(const std::string& title, const std::string& filter, std::string& out);
bool dialog_save(const std::string& title, const std::string& filter,
                 const std::string& defaultName, std::string& out);

// Builds a dialog filter string from a literal that contains embedded \0
// separators. A bare literal converts to std::string via strlen, which
// truncates at the FIRST \0 — the pattern part ("*.UNI") is lost and the
// dialog matches nothing. This keeps every byte, including the explicit
// trailing \0, so the result is double-null-terminated via c_str().
template <size_t N>
std::string filter_string(const char (&lit)[N])
{
    return std::string(lit, N - 1);
}

// Path helpers (work with / and \ separators).
std::string dirname(const std::string& p);
std::string basename(const std::string& p);       // without extension
std::string basename_full(const std::string& p);  // with extension
std::string join(const std::string& a, const std::string& b);
bool make_dirs(const std::string& path);
bool copy_file_sync(const std::string& src, const std::string& dst, std::string* err);

// Human-readable size ("1.24 GB").
std::string human_size(uint64_t bytes);

} // namespace win32util
