// Dokuro-chan script editor — Win32 + DX11 + Dear ImGui shell.
// Mirrors the OpenSummoners res_explorer build (i686 mingw, DX11, WINVER 0x0601)
// so it runs on Windows 7 (32- and 64-bit). Theme is adapted from res_explorer.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiTabBar/ImGuiTabItem (uitest tab rects)
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "app.h"
#include "win32_util.h"
#include "uitest.h"

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

static App g_app;
static bool g_quit = false;
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static ImFont* g_font_ui = nullptr;
static ImFont* g_font_mono = nullptr;

ImFont* app_font_mono() { return g_font_mono; }

// UI entry points (project_ui.cpp, editor_ui.cpp, asm_ui.cpp)
void ui_project_toolbar(App& a);
void ui_project_panel(App& a);
void ui_editor_tab(App& a);
void ui_asm_tab(App& a);
void ui_settings_window(App& a);
void ui_busy_modal(App& a);
void ui_welcome(App& a);
void ui_confirm_popups(App& a);
void app_tick(App& a);
int run_selftest(int argc, char** argv);

// ---------- crash diagnostics ----------
//
// Writes crash.log next to the exe: first-chance access violations (vectored
// handler) and unhandled exceptions (SetUnhandledExceptionFilter), with the
// full register context + stack dump. Used to diagnose the startup crash.

static FILE* crash_log_file()
{
    static char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "crash.log");
    else strcpy(path, "crash.log");
    return fopen(path, "a");
}

void app_log(const char* fmt, ...)
{
    FILE* f = crash_log_file();
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void log_exception(const EXCEPTION_RECORD* rec, const CONTEXT* ctx)
{
    FILE* f = crash_log_file();
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "\n==== %02u:%02u:%02u code=%08lx addr=%p flags=%08lx npar=%lu ====\n",
            st.wHour, st.wMinute, st.wSecond, (unsigned long)rec->ExceptionCode,
            rec->ExceptionAddress, (unsigned long)rec->ExceptionFlags,
            (unsigned long)rec->NumberParameters);
    for (DWORD i = 0; i < rec->NumberParameters && i < 16; i++)
        fprintf(f, "  param[%lu] = %p\n", (unsigned long)i,
                (void*)(uintptr_t)rec->ExceptionInformation[i]);
    if (ctx)
    {
        fprintf(f,
                "EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx\n"
                "EBP=%08lx ESP=%08lx EIP=%08lx EFLAGS=%08lx\n",
                ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi,
                ctx->Ebp, ctx->Esp, ctx->Eip, ctx->EFlags);
        const unsigned char* sp = (const unsigned char*)ctx->Esp;
        fprintf(f, "STACK @ ESP=%p:\n", (void*)ctx->Esp);
        for (long off = -0x40; off < 0x600; off += 16)
        {
            const unsigned char* p = sp + off;
            if ((uintptr_t)p < 0x10000 || IsBadReadPtr(p, 16))
            {
                fprintf(f, "%08lx: (unmapped)\n", (unsigned long)(uintptr_t)p);
                continue;
            }
            fprintf(f, "%08lx: ", (unsigned long)(uintptr_t)p);
            for (int j = 0; j < 16; j++) fprintf(f, "%02x ", p[j]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
}

static LONG CALLBACK crash_filter(EXCEPTION_POINTERS* ep)
{
    log_exception(ep->ExceptionRecord, ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH; // let WER run its normal flow
}

static LONG CALLBACK first_chance_av(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005)
        log_exception(ep->ExceptionRecord, ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------- frame timing + test hooks ----------

// Rolling frame-time window (shown in the status bar; shared by the normal
// loop and the uitest loop).
static double g_frameHistory[120] = {};
static int g_frameCount = 0;
static double g_lastFrameAt = 0;

// Tab geometry recorded during rendering (client coords) — infrastructure
// for uitest coordinate tests. g_activeTab is defined in uitest.cpp.
float g_tabX0[2] = { -1, -1 };
float g_tabY0 = 0, g_tabY1 = 0;
int g_forceTab = 0; // uitest: force-select a tab next frame (SetSelected)

// ---------- DX11 ----------

static void create_render_target()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static bool create_device_d3d11(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL outLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevels,
        3, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &outLevel, &g_pd3dContext);
    if (hr == DXGI_ERROR_UNSUPPORTED || FAILED(hr))
    {
        // WARP fallback (VM / old GPU without WDDM 1.1)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevels,
            3, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &outLevel, &g_pd3dContext);
    }
    if (FAILED(hr)) return false;
    create_render_target();
    return true;
}

static void cleanup_device_d3d11()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dContext) { g_pd3dContext->Release(); g_pd3dContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// ---------- window proc ----------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && g_pd3dDevice && g_pSwapChain)
        {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            create_render_target();
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 900;
        mmi->ptMinTrackSize.y = 560;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------- theme + fonts (adapted from OpenSummoners tools/res_explorer) ----------

static void apply_theme()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 3);
    s.WindowPadding = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    static const ImVec4 ACCENT(0.91f, 0.70f, 0.29f, 1.00f);   // Dokuro gold
    static const ImVec4 ACCENT_DIM(0.91f, 0.70f, 0.29f, 0.55f);
    c[ImGuiCol_WindowBg]         = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.11f, 0.11f, 0.13f, 0.98f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f, 0.21f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.28f, 0.25f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.91f, 0.70f, 0.29f, 0.28f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.91f, 0.70f, 0.29f, 0.40f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.91f, 0.70f, 0.29f, 0.55f);
    c[ImGuiCol_Button]           = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.91f, 0.70f, 0.29f, 0.45f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.91f, 0.70f, 0.29f, 0.70f);
    c[ImGuiCol_CheckMark]        = ACCENT;
    c[ImGuiCol_SliderGrab]       = ACCENT_DIM;
    c[ImGuiCol_SliderGrabActive] = ACCENT;
    c[ImGuiCol_Tab]              = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.91f, 0.70f, 0.29f, 0.40f);
    c[ImGuiCol_TabActive]        = ImVec4(0.91f, 0.70f, 0.29f, 0.30f);
    c[ImGuiCol_TableHeaderBg]    = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]    = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    c[ImGuiCol_SeparatorHovered] = ACCENT_DIM;
    c[ImGuiCol_SeparatorActive]  = ACCENT;
    c[ImGuiCol_NavHighlight]     = ACCENT;
    c[ImGuiCol_TextSelectedBg]   = ImVec4(0.91f, 0.70f, 0.29f, 0.30f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
    c[ImGuiCol_Border]           = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
}

// Rebuilds the font atlas at the requested UI size. Segoe UI + MS Gothic merge
// (Japanese), Consolas for the script editor. Cyrillic comes with Segoe UI.
//
// The game text contains punctuation OUTSIDE ImGui's stock ranges — the em-dash
// U+2015 (――, 168 occurrences), ellipsis U+2026 (…, 4840), and eighth note
// U+266A (♪, 46) render as "?" otherwise. General Punctuation + the note are
// added to both the base fonts and the Japanese merge (whichever font carries
// the glyph wins).
void rebuild_fonts(float size)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const char* ui = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* jp1 = "C:\\Windows\\Fonts\\meiryo.ttc";
    const char* jp2 = "C:\\Windows\\Fonts\\msgothic.ttc";
    const char* mono = "C:\\Windows\\Fonts\\consola.ttf";
    // ImGui's default ranges are Latin-only (1.91); the tool's whole point is
    // Russian text, so add Cyrillic explicitly (it includes the Latin base).
    static const ImWchar s_cyrRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
        0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
        0x2DE0, 0x2DFF, // Cyrillic Extended-A
        0xA640, 0xA69F, // Cyrillic Extended-B
        0x2000, 0x206F, // General Punctuation (―― dash, … ellipsis)
        0x266A, 0x266A, // ♪ eighth note (game music symbols)
        0,
    };
    static const ImWchar s_jpRanges[] = {
        0x3000, 0x30FF, // CJK Symbols and Punctuation, Hiragana, Katakana
        0x31F0, 0x31FF, // Katakana Phonetic Extensions
        0xFF00, 0xFFEF, // Half-width forms
        0xFFFD, 0xFFFD, // Replacement character
        0x4E00, 0x9FAF, // CJK Ideograms
        0x2000, 0x206F, // General Punctuation
        0x266A, 0x266A, // ♪
        0,
    };
    g_font_ui = io.Fonts->AddFontFromFileTTF(ui, size, nullptr, s_cyrRanges);
    if (!g_font_ui) g_font_ui = io.Fonts->AddFontDefault();
    ImFontConfig cfg;
    cfg.MergeMode = true;
    const char* jp = GetFileAttributesA(jp1) != INVALID_FILE_ATTRIBUTES ? jp1
                    : GetFileAttributesA(jp2) != INVALID_FILE_ATTRIBUTES ? jp2 : nullptr;
    if (jp) io.Fonts->AddFontFromFileTTF(jp, size + 1.0f, &cfg, s_jpRanges);
    // mono font: standalone (Consolas + Cyrillic), then merge Japanese in —
    // without the merge the direct script editor renders every JP character
    // as "?" (Consolas has no CJK glyphs).
    if (GetFileAttributesA(mono) != INVALID_FILE_ATTRIBUTES)
        g_font_mono = io.Fonts->AddFontFromFileTTF(mono, size - 2.0f, nullptr, s_cyrRanges);
    if (!g_font_mono) g_font_mono = g_font_ui;
    if (jp && g_font_mono != g_font_ui)
    {
        cfg.MergeMode = true; // merges into the last added font (the mono one)
        io.Fonts->AddFontFromFileTTF(jp, size - 1.0f, &cfg, s_jpRanges);
    }
    io.Fonts->Build();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
}

// ---------- frame loop helpers (shared by the normal loop and the test modes) ----------

static bool pump_messages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) g_quit = true;
    }
    return g_quit;
}

static void uitest_after_render(); // defined below (framebuffer dump hook)

static void render_frame(App& a)
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // The whole editor UI (toolbar, tabs, panels) must live inside a real
    // window: bare ImGui calls outside Begin/End accumulate in the hidden
    // "Debug##Default" implicit window (400x400 from imgui.ini), which
    // clips the layout. Pin a full-client window here.
    {
        ImGuiIO& io2 = ImGui::GetIO();
        const float barH = 26.0f;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io2.DisplaySize.x, io2.DisplaySize.y - barH));
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);

        app_tick(a);
        ui_project_toolbar(a);
        ui_project_panel(a);
        if (a.have_project)
        {
            if (ImGui::BeginTabBar("MainTabs"))
            {
                if (ImGui::BeginTabItem("Dialogue editor", nullptr,
                                        g_forceTab == 2 ? ImGuiTabItemFlags_SetSelected
                                                        : ImGuiTabItemFlags_None))
                {
                    g_activeTab = 0;
                    ui_editor_tab(a);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Direct script", nullptr,
                                        g_forceTab == 1 ? ImGuiTabItemFlags_SetSelected
                                                        : ImGuiTabItemFlags_None))
                {
                    g_activeTab = 1;
                    ui_asm_tab(a);
                    ImGui::EndTabItem();
                }
                g_forceTab = 0; // one-shot
                // record tab geometry for uitest (TabBar internals give rects
                // for ALL tabs, not just the selected one)
                if (ImGuiTabBar* bar = ImGui::GetCurrentTabBar())
                {
                    for (int t = 0; t < bar->Tabs.Size && t < 2; t++)
                    {
                        const ImGuiTabItem& ti = bar->Tabs[t];
                        int idx = ti.BeginOrder >= 0 && ti.BeginOrder < 2 ? ti.BeginOrder : t;
                        float x0 = bar->BarRect.Min.x + ti.Offset - bar->ScrollingAnim;
                        g_tabX0[idx] = x0 + ti.Width * 0.5f;
                        g_tabY0 = bar->BarRect.Min.y;
                        g_tabY1 = bar->BarRect.Max.y;
                    }
                }
                ImGui::EndTabBar();
            }
        }
        else
        {
            ui_welcome(a);
        }
        ImGui::End();
    }
    ui_busy_modal(a);
    ui_confirm_popups(a);
    ui_settings_window(a);

    // status bar — must live in a real window: bare ImGui calls outside
    // Begin/End render into the hidden "Debug##Default" implicit window
    // and are never visible.
    {
        ImGuiIO& io2 = ImGui::GetIO();
        const float barH = 26.0f;
        ImGui::SetNextWindowPos(ImVec2(0, io2.DisplaySize.y - barH));
        ImGui::SetNextWindowSize(ImVec2(io2.DisplaySize.x, barH));
        ImGui::Begin("##statusbar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
        ImGui::Separator();
        if (a.status_until > (double)app_now_ms())
            ImGui::TextUnformatted(a.status.c_str());
        else if (a.have_project)
        {
            char buf[320];
            // cached counts (entries() walks the whole model — too slow per frame)
            int translated = a.translated_count, total = a.total_count;
            double sum = 0;
            int n = g_frameCount < 120 ? g_frameCount : 120;
            for (int i = 0; i < n; i++) sum += g_frameHistory[i];
            double avgMs = n ? sum / n : 0.0;
            snprintf(buf, sizeof(buf),
                     "%d/%d translated   |   undo: %zu steps   |   %s   |   %.1f ms/frame (%.0f FPS)",
                     translated, total, a.undo.steps(),
                     a.dirty ? "unsaved changes (autosaved)" : "all saved",
                     avgMs, avgMs > 0 ? 1000.0 / avgMs : 0.0);
            ImGui::TextUnformatted(buf);
        }
        else
        {
            ImGui::TextUnformatted("No project open — pick a SCRIPT.UNI to start.");
        }
        ImGui::End();
    }

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImVec4 clear(0.06f, 0.06f, 0.08f, 1.0f);
    g_pd3dContext->ClearRenderTargetView(g_mainRenderTargetView, (float*)&clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    uitest_after_render();
    g_pSwapChain->Present(1, 0);
}

// Reads the swapchain backbuffer into a PNG (headless UI test dumps).
static void dump_framebuffer_png(const char* path)
{
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    D3D11_TEXTURE2D_DESC desc;
    back->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stDesc = desc;
    stDesc.BindFlags = 0;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stDesc.Usage = D3D11_USAGE_STAGING;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&stDesc, nullptr, &staging)))
    {
        g_pd3dContext->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(g_pd3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
        {
            uitest_save_png(path, (const unsigned char*)map.pData, desc.Width, desc.Height,
                            (unsigned long)map.RowPitch);
            g_pd3dContext->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

// After-render hook: fulfills pending uitest/screenshot framebuffer dumps.
static void uitest_after_render()
{
    if (g_pendingDump.empty()) return;
    std::string path = g_pendingDump;
    g_pendingDump.clear();
    dump_framebuffer_png(path.c_str());
}

// --uitest <project.txt|SCRIPT.UNI> [outdir]: scripted UI regression with
// framebuffer dumps (see uitest.h). Returns the process exit code.
static int run_uitest_mode(App& a, const std::string& project, const std::string& outdir)
{
    std::string logPath = outdir + "/uitest.log";
    FILE* f = freopen(logPath.c_str(), "w", stdout);
    if (f) *stderr = *stdout;
    printf("uitest: project=%s outdir=%s\n", project.c_str(), outdir.c_str());
    fflush(stdout);

    std::string err;
    bool isUni = project.size() > 4 &&
                 (project.compare(project.size() - 4, 4, ".uni") == 0 ||
                  project.compare(project.size() - 4, 4, ".UNI") == 0);
    if (isUni)
    {
        // isolate the test project inside the outdir (never Documents)
        a.settings.projects_dir = outdir + "/projects";
        if (!app_create_project(a, project, "", &err))
        {
            printf("uitest: cannot create project: %s\n", err.c_str());
            return 2;
        }
    }
    else if (!app_open_project(a, project, &err))
    {
        printf("uitest: cannot open project: %s\n", err.c_str());
        return 2;
    }
    printf("uitest: project ready: %s\n", a.paths.root.c_str());
    uitest_begin(a, outdir);

    while (!g_quit && !uitest_finished())
    {
        if (pump_messages()) break;
        uitest_on_frame(a);
        double frameNow = (double)app_now_ms();
        g_frameHistory[g_frameCount % 120] = frameNow - g_lastFrameAt;
        g_frameCount++;
        g_lastFrameAt = frameNow;
        render_frame(a);
    }
    return uitest_summary();
}

// --screenshot <out.png> [project]: pump ~90 frames, save one framebuffer
// dump, exit.
static int run_screenshot_mode(App& a, const std::string& outPath, const std::string& project)
{
    FILE* f = freopen((outPath + ".log").c_str(), "w", stdout);
    if (f) *stderr = *stdout;
    printf("screenshot: out=%s project=%s\n", outPath.c_str(), project.c_str());
    fflush(stdout);
    if (!project.empty())
    {
        std::string err;
        bool isUni = project.size() > 4 &&
                     (project.compare(project.size() - 4, 4, ".uni") == 0 ||
                      project.compare(project.size() - 4, 4, ".UNI") == 0);
        if (isUni)
        {
            a.settings.projects_dir = win32util::dirname(outPath) + "/projects";
            if (!app_create_project(a, project, "", &err))
            {
                printf("screenshot: cannot create project: %s\n", err.c_str());
                return 2;
            }
        }
        else if (!app_open_project(a, project, &err))
        {
            printf("screenshot: cannot open project: %s\n", err.c_str());
            return 2;
        }
    }
    for (int i = 0; i < 90 && !g_quit; i++)
    {
        if (pump_messages()) break;
        double frameNow = (double)app_now_ms();
        g_frameHistory[g_frameCount % 120] = frameNow - g_lastFrameAt;
        g_frameCount++;
        g_lastFrameAt = frameNow;
        render_frame(a);
    }
    uitest_request_dump(outPath);
    if (!g_quit)
    {
        pump_messages();
        render_frame(a);
    }
    return 0;
}

// ---------- main ----------

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, first_chance_av);
    app_log("app start (cmdline: %s)", lpCmdLine ? lpCmdLine : "");

    // convert the ANSI command line to UTF-8 argv (drop argv[0])
    std::vector<std::string> args;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        for (int i = 1; i < wargc; i++) args.push_back(win32util::wide_to_utf8(wargv[i]));
        LocalFree(wargv);
    }

    // --selftest <script.uni> [tmpdir]: headless core smoke test of the binary
    if (!args.empty() && args[0] == "--selftest")
    {
        std::vector<char*> argv(args.size());
        for (size_t i = 0; i < args.size(); i++) argv[i] = &args[i][0];
        return run_selftest((int)args.size(), argv.data());
    }

    // --uitest <project.txt|SCRIPT.UNI> [outdir] / --screenshot <out.png> [project]
    std::string uitestProject, uitestOutdir, shotOut, shotProject;
    bool uitestMode = false, shotMode = false;
    if (!args.empty() && args[0] == "--uitest" && args.size() >= 2)
    {
        uitestMode = true;
        uitestProject = args[1];
        uitestOutdir = args.size() >= 3 ? args[2] : "uitest_out";
    }
    else if (!args.empty() && args[0] == "--screenshot" && args.size() >= 2)
    {
        shotMode = true;
        shotOut = args[1];
        shotProject = args.size() >= 3 ? args[2] : "";
    }

    g_app.settings_path = win32util::join(win32util::exe_dir(), "dokuro_editor.ini");
    settings_load(g_app.settings_path, g_app.settings);
    g_app.undo.set_ram_cap((size_t)g_app.settings.undo_ram_cap);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DokuroEditorClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int w = g_app.settings.window_w > 0 ? g_app.settings.window_w : 1440;
    int h = g_app.settings.window_h > 0 ? g_app.settings.window_h : 900;
    RECT rc{ 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    // test modes run the window hidden off-screen (headless: rendering goes
    // to the swapchain; nothing appears on the user's desktop)
    bool testMode = uitestMode || shotMode;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Dokuro-chan Script Editor",
                              WS_OVERLAPPEDWINDOW,
                              testMode ? -10000
                                       : (g_app.settings.window_x >= 0 ? g_app.settings.window_x : CW_USEDEFAULT),
                              testMode ? -10000
                                       : (g_app.settings.window_y >= 0 ? g_app.settings.window_y : CW_USEDEFAULT),
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Could not create the editor window.", L"Dokuro editor", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, testMode ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    if (!create_device_d3d11(hwnd))
    {
        MessageBoxW(nullptr, L"D3D11 init failed.\n\nWindows 7 needs the Platform Update (KB2670838) "
                             L"for DirectX 11 support.", L"Dokuro editor", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "dokuro_imgui.ini";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    apply_theme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);
    rebuild_fonts(g_app.settings.font_size);

    uitest_set_hwnd((void*)hwnd);
    g_lastFrameAt = (double)app_now_ms();

    // headless modes: no settings persistence (the window is off-screen and
    // must not leak its position), full device/imgui teardown
    if (uitestMode || shotMode)
    {
        int rc = 1;
        if (uitestMode)
        {
            win32util::make_dirs(uitestOutdir);
            rc = run_uitest_mode(g_app, uitestProject, uitestOutdir);
        }
        else
        {
            rc = run_screenshot_mode(g_app, shotOut, shotProject);
        }
        app_close_project(g_app);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        cleanup_device_d3d11();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return rc;
    }

    // auto-open the last project if it still exists
    if (!g_app.settings.last_project.empty() &&
        GetFileAttributesA(g_app.settings.last_project.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        std::string err;
        if (!app_open_project(g_app, g_app.settings.last_project, &err))
            app_set_status(g_app, "Could not reopen the last project: " + err, 10);
    }

    while (!g_quit)
    {
        if (pump_messages()) break;
        double frameNow = (double)app_now_ms();
        g_frameHistory[g_frameCount % 120] = frameNow - g_lastFrameAt;
        g_frameCount++;
        g_lastFrameAt = frameNow;
        render_frame(g_app);
    }

    // persist window state + settings
    RECT rc2;
    if (GetWindowRect(hwnd, &rc2))
    {
        g_app.settings.window_w = rc2.right - rc2.left;
        g_app.settings.window_h = rc2.bottom - rc2.top;
        g_app.settings.window_x = rc2.left;
        g_app.settings.window_y = rc2.top;
    }
    if (g_app.have_project) g_app.settings.last_project = win32util::join(g_app.paths.root, "project.txt");
    settings_save(g_app.settings_path, g_app.settings);
    app_close_project(g_app);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device_d3d11();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}
