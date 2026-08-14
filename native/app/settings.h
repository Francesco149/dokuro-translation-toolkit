// Portable key=value settings file next to the exe (dokuro_editor.ini).
#pragma once
#include <string>
#include <vector>

struct AppSettings
{
    std::string last_project;
    std::vector<std::string> recent_projects; // most recent first
    std::string projects_dir; // where new project folders are created ("" = <My Documents>\dokuro\projects)
    float font_size = 19.0f;                  // UI font px (WinForms default was ~12)
    int undo_ram_cap = 200;                   // steps kept decompressed in RAM
    int undo_disk_keep = 0;                   // 0 = unlimited on disk
    bool autosave_iso = true;                 // rebuild the working ISO on changes
    int window_w = 1440, window_h = 900;
    int window_x = -1, window_y = -1;
};

// Missing/unknown keys keep defaults. Never fails.
void settings_load(const std::string& path, AppSettings& s);
void settings_save(const std::string& path, const AppSettings& s);
void settings_push_recent(AppSettings& s, const std::string& projectPath);
