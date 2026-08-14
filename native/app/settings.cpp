#include "settings.h"
#include <cstdio>
#include <fstream>
#include <sstream>

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

void settings_load(const std::string& path, AppSettings& s)
{
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key == "last_project") s.last_project = val;
        else if (key == "recent") s.recent_projects.push_back(val);
        else if (key == "projects_dir") s.projects_dir = val;
        else if (key == "font_size") s.font_size = (float)atof(val.c_str());
        else if (key == "undo_ram_cap") s.undo_ram_cap = atoi(val.c_str());
        else if (key == "undo_disk_keep") s.undo_disk_keep = atoi(val.c_str());
        else if (key == "autosave_iso") s.autosave_iso = atoi(val.c_str()) != 0;
        else if (key == "window_w") s.window_w = atoi(val.c_str());
        else if (key == "window_h") s.window_h = atoi(val.c_str());
        else if (key == "window_x") s.window_x = atoi(val.c_str());
        else if (key == "window_y") s.window_y = atoi(val.c_str());
    }
}

void settings_save(const std::string& path, const AppSettings& s)
{
    std::string out;
    out += "# Dokuro-chan script editor settings\n";
    out += "last_project = " + s.last_project + "\n";
    if (!s.projects_dir.empty()) out += "projects_dir = " + s.projects_dir + "\n";
    for (const auto& r : s.recent_projects) out += "recent = " + r + "\n";
    char buf[128];
    snprintf(buf, sizeof(buf), "font_size = %.1f\n", s.font_size);
    out += buf;
    snprintf(buf, sizeof(buf), "undo_ram_cap = %d\n", s.undo_ram_cap);
    out += buf;
    snprintf(buf, sizeof(buf), "undo_disk_keep = %d\n", s.undo_disk_keep);
    out += buf;
    snprintf(buf, sizeof(buf), "autosave_iso = %d\n", s.autosave_iso ? 1 : 0);
    out += buf;
    snprintf(buf, sizeof(buf), "window_w = %d\nwindow_h = %d\n", s.window_w, s.window_h);
    out += buf;
    snprintf(buf, sizeof(buf), "window_x = %d\nwindow_y = %d\n", s.window_x, s.window_y);
    out += buf;
    std::ofstream f(path, std::ios::trunc);
    if (f) f.write(out.data(), (std::streamsize)out.size());
}

void settings_push_recent(AppSettings& s, const std::string& projectPath)
{
    for (auto it = s.recent_projects.begin(); it != s.recent_projects.end(); ++it)
        if (*it == projectPath) { s.recent_projects.erase(it); break; }
    s.recent_projects.insert(s.recent_projects.begin(), projectPath);
    while (s.recent_projects.size() > 8) s.recent_projects.pop_back();
}
