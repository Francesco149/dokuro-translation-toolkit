#include "win32_util.h"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace win32util {

std::string wide_to_utf8(const wchar_t* s)
{
    if (!s) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string exe_dir()
{
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string p = wide_to_utf8(buf);
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(0, s);
}

bool open_in_explorer(const std::string& path)
{
    // explorer.exe misparses forward slashes in paths (it opens My Documents
    // instead); normalize to backslashes first.
    std::string native = path;
    for (auto& c : native)
        if (c == '/') c = '\\';
    // Explorer must receive a DIRECTORY. For files (e.g. the SCRIPT.UNI
    // rows), open the containing folder instead.
    DWORD attrs = GetFileAttributesA(native.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        native = dirname(native);
    std::wstring w = utf8_to_wide(native);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", w.c_str(), nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

static bool do_dialog(bool save, const std::string& title, const std::string& filter,
                      const std::string& defaultName, std::string& out)
{
    wchar_t fileBuf[32768] = L"";
    if (!defaultName.empty())
    {
        std::wstring wd = utf8_to_wide(defaultName);
        wcsncpy(fileBuf, wd.c_str(), 32767);
    }
    // Keep the converted strings alive for the duration of the dialog:
    // lpstrFilter/lpstrTitle point INTO these buffers (the filter is
    // double-null-terminated; the embedded \0 separators must survive).
    std::wstring wFilter = utf8_to_wide(filter);
    std::wstring wTitle = utf8_to_wide(title);
    // Belt-and-braces: the common dialog reads until a DOUBLE null. If a
    // caller's filter is not double-null-terminated (e.g. truncated at an
    // embedded \0), Windows keeps parsing past the end into garbage.
    if (wFilter.empty() || wFilter.back() != L'\0') wFilter += L'\0';
    if (wTitle.empty() || wTitle.back() != L'\0') wTitle += L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = wFilter.c_str();
    ofn.lpstrTitle = wTitle.c_str();
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                     : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    out = wide_to_utf8(fileBuf);
    return true;
}

bool dialog_open(const std::string& title, const std::string& filter, std::string& out)
{
    return do_dialog(false, title, filter, "", out);
}

bool dialog_save(const std::string& title, const std::string& filter,
                 const std::string& defaultName, std::string& out)
{
    return do_dialog(true, title, filter, defaultName, out);
}

std::string dirname(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? "." : p.substr(0, s);
}

std::string basename(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

std::string basename_full(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

bool make_dirs(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); i++)
    {
        cur += path[i];
        if (path[i] == '/' || path[i] == '\\' || i + 1 == path.size())
        {
            if (!cur.empty())
            {
                DWORD attrs = GetFileAttributesA(cur.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES)
                {
                    if (!CreateDirectoryA(cur.c_str(), nullptr) &&
                        GetLastError() != ERROR_ALREADY_EXISTS)
                        return false;
                }
                else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
                    return false;
            }
        }
    }
    return true;
}

bool copy_file_sync(const std::string& src, const std::string& dst, std::string* err)
{
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!in || !out)
    {
        if (err) *err = "cannot open for copy: " + src;
        return false;
    }
    out << in.rdbuf();
    return out.good();
}

std::string human_size(uint64_t bytes)
{
    char buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.2f MB", (double)bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}

} // namespace win32util
