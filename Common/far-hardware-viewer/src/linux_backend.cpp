#ifndef _WIN32
#include "plugin_core.hpp"
#include <far2l/plugin.hpp> // Официальный SDK из состава исходных кодов far2l
#include <cwchar>
#include <algorithm>
#include <cstring>
#include <codecvt>
#include <locale>
#include <fstream>
#include <sstream>

// Глобальные переменные состояния и конфигурации Linux-версии (far2l)
struct PluginStartupInfo Info;

// Параметры конфигурации из config.ini по умолчанию (синхронизировано с Win32)
int      g_LinuxPrefetchRadius = 1;
int      g_LinuxAsyncTimeoutMs = 150;
int      g_LinuxDropCacheOnZoom = 0;
bool     g_LinuxIsDarkScheme = true;
float    g_LinuxBgColorR = 0.05f, g_LinuxBgColorG = 0.05f, g_LinuxBgColorB = 0.05f, g_LinuxBgColorA = 1.0f;
float    g_LinuxLineColorR = 0.19f, g_LinuxLineColorG = 0.80f, g_LinuxLineColorB = 0.19f, g_LinuxLineColorA = 1.0f;
float    g_LinuxMinStrokeWidth = 0.5f;
float    g_LinuxKeyboardZoomStep = 1.10f;
float    g_LinuxMouseWheelZoomStep = 1.05f;
float    g_LinuxKeyboardScrollSpeed = 25.0f;
int      g_LinuxShowStatusBar = 1;
float    g_LinuxStatusBarAlpha = 0.70f;

// Переменные состояния просмотра
float g_LinuxZoom = 1.0f;
float g_LinuxOffsetX = 0.0f;
float g_LinuxOffsetY = 0.0f;
uint32_t g_LinuxCurrentPage = 0;
std::wstring g_LinuxCurrentPath;
std::wstring g_LinuxSearchQuery;

RawImageData g_LinuxData;
PageCacheManager g_LinuxCache;
std::vector<SearchResultQuad> g_LinuxSearchResults;

// ==============================================================================
// 1. ЛЕГКОВЕСНЫЙ ПОТОКОВЫЙ ПАРСЕР КОНФИГУРАЦИИ (POSIX INI Parser)
// ==============================================================================
static std::wstring TrimString(const std::wstring& str) {
    size_t first = str.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    size_t last = str.find_last_of(L" \t\r\n");
    return str.substr(first, (last - first + 1));
}

void LoadLinuxPluginConfig() {
    // Формируем путь к config.ini в рабочей папке плагина far2l
    std::wstring iniPath = L"~/.config/far2l/plugins/amd64_viewer/config.ini";
    
    // Поддержка раскрытия домашней директории пользователя в POSIX
    if (!iniPath.empty() && iniPath[0] == L'~') {
        const char* home = getenv("HOME");
        if (home) {
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
            iniPath = cvt.from_bytes(home) + iniPath.substr(1);
        }
    }

    std::wifstream file(std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>>().to_bytes(iniPath));
    if (!file.is_open()) return; // Если файла нет, плагин работает на дефолтных значениях

    std::wstring line;
    while (std::getline(file, line)) {
        line = TrimString(line);
        if (line.empty() || line[0] == L';' || line[0] == L'[') continue;

        size_t delim = line.find(L'=');
        if (delim == std::wstring::npos) continue;

        std::wstring key = TrimString(line.substr(0, delim));
        std::wstring val = TrimString(line.substr(delim + 1));

        if (key == L"PrefetchRadius")      g_LinuxPrefetchRadius = std::wcstol(val.c_str(), nullptr, 10);
        else if (key == L"AsyncTimeoutMs")  g_LinuxAsyncTimeoutMs = std::wcstol(val.c_str(), nullptr, 10);
        else if (key == L"DropCacheOnZoom") g_LinuxDropCacheOnZoom = std::wcstol(val.c_str(), nullptr, 10);
        else if (key == L"ColorScheme")     g_LinuxIsDarkScheme = (val == L"dark");
        else if (key == L"BgColorR")        g_LinuxBgColorR = std::wcstof(val.c_str(), nullptr);
        else if (key == L"BgColorG")        g_LinuxBgColorG = std::wcstof(val.c_str(), nullptr);
        else if (key == L"BgColorB")        g_LinuxBgColorB = std::wcstof(val.c_str(), nullptr);
        else if (key == L"BgColorA")        g_LinuxBgColorA = std::wcstof(val.c_str(), nullptr);
        else if (key == L"LineColorR")      g_LinuxLineColorR = std::wcstof(val.c_str(), nullptr);
        else if (key == L"LineColorG")      g_LinuxLineColorG = std::wcstof(val.c_str(), nullptr);
        else if (key == L"LineColorB")      g_LinuxLineColorB = std::wcstof(val.c_str(), nullptr);
        else if (key == L"LineColorA")      g_LinuxLineColorA = std::wcstof(val.c_str(), nullptr);
        else if (key == L"MinStrokeWidth")  g_LinuxMinStrokeWidth = std::wcstof(val.c_str(), nullptr);
        else if (key == L"KeyboardZoomStep") g_LinuxKeyboardZoomStep = std::wcstof(val.c_str(), nullptr);
        else if (key == L"MouseWheelZoomStep") g_LinuxMouseWheelZoomStep = std::wcstof(val.c_str(), nullptr);
        else if (key == L"KeyboardScrollSpeed") g_LinuxKeyboardScrollSpeed = std::wcstof(val.c_str(), nullptr);
        else if (key == L"ShowStatusBar")    g_LinuxShowStatusBar = std::wcstol(val.c_str(), nullptr, 10);
        else if (key == L"StatusBarAlpha")   g_LinuxStatusBarAlpha = std::wcstof(val.c_str(), nullptr);
    }
}

// ==============================================================================
// 2. ГРАФИЧЕСКИЙ СТЭК ДЛЯ LINUX (Chauvinist GUI VT Bridge + Эмуляция Слоев Notcurses)
// ==============================================================================
struct Far2LGraphicsContext {
    void* native_window_ptr; 
    uint32_t width;          
    uint32_t height;         
    uint8_t* framebuffer;    
    uint32_t stride;         
};

class LinuxGraphicsEngine {
public:
    bool AcquireTerminalCanvas(Far2LGraphicsContext& ctx) {
        intptr_t rc = ::Info.AdvControl(Info.ModuleNumber, ACTL_GETWINDOWINFO, &ctx);
        return (rc == 1 && ctx.framebuffer != nullptr);
    }

    void RenderToFramebuffer(const RawImageData& img, float zoom, float ox, float oy) {
        Far2LGraphicsContext canvas;
        if (!AcquireTerminalCanvas(canvas)) return;

        // Очистка графического холста far2l фоновым цветом на основе параметров config.ini
        uint32_t total_bytes = canvas.stride * canvas.height;
        uint8_t fill_byte = g_LinuxIsDarkScheme ? static_cast<uint8_t>(g_LinuxBgColorR * 255) : 255;
        std::memset(canvas.framebuffer, fill_byte, total_bytes);

        if (img.rgba_data.empty() && img.cad_lines.empty()) {
            const wchar_t* msgLoading = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 3));
            DrawStatusText(canvas, msgLoading);
            return;
        }

        // Софтверное копирование растрового буфера
        if (!img.is_cad && !img.rgba_data.empty()) {
            alignas(32) uint8_t shuffle_mask = {
                2, 1, 0, 3,  6, 5, 4, 7,  10, 9, 8, 11,  14, 13, 12, 15,
                2, 1, 0, 3,  6, 5, 4, 7,  10, 9, 8, 11,  14, 13, 12, 15
            };

            uint32_t target_h = std::min(canvas.height, static_cast<uint32_t>(img.height * zoom));
            uint32_t target_w = std::min(canvas.width, static_cast<uint32_t>(img.width * zoom));

            for (uint32_t y = 0; y < target_h; ++y) {
                uint32_t src_y = static_cast<uint32_t>(y / zoom);
                if (src_y >= img.height) break;

                uint8_t* dst_row = canvas.framebuffer + (y * canvas.stride);
                const uint8_t* src_row = img.rgba_data.data() + (src_y * img.width * 4);

                std::memcpy(dst_row, src_row, target_w * 4);

                // Вызов нашего высокопроизводительного ассемблерного кода AVX2
                size_t num_pixels = target_w;
                amd64_fast_bgr_to_rgba(dst_row, num_pixels, shuffle_mask);
            }
        }

        // Отрисовка статус-бара с учетом настроек видимости
        if (g_LinuxShowStatusBar) {
            wchar_t statusBuffer;
            const wchar_t* lblPage   = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 7)); 
            const wchar_t* lblZoom   = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 8)); 
            const wchar_t* lblEngine = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 9)); 

            swprintf(statusBuffer, 256, L" %dx%d | %s %d/%d | %s %d%% | %s %s %s ", 
                     img.width, img.height, 
                     lblPage, img.current_page + 1, img.total_pages,
                     lblZoom, static_cast<int>(zoom * 100), 
                     lblEngine, img.decoder_name.c_str(),
                     img.has_text ? L"|[Ctrl+C]" : L"");
            
            DrawStatusText(canvas, statusBuffer);
        }
    }

private:
    void DrawStatusText(const Far2LGraphicsContext& canvas, const wchar_t* text) {
        size_t len = wcslen(text);
        uint32_t x_pos = canvas.width - (static_cast<uint32_t>(len) * 8) - 20; 
        uint32_t y_pos = 15; 

        // Применяем коэффициент прозрачности плашки StatusBarAlpha из config.ini
        uint8_t alpha_byte = static_cast<uint8_t>(g_LinuxStatusBarAlpha * 255);

        for (uint32_t line = 0; line < 20; ++line) {
            uint8_t* dst_ptr = canvas.framebuffer + ((y_pos + line) * canvas.stride) + (x_pos * 4);
            for (size_t p = 0; p < len * 8; ++p) {
                dst_ptr = 0;          // R
                dst_ptr = 0;          // G
                dst_ptr = 0;          // B
                dst_ptr = alpha_byte; // Динамическая прозрачность из ini
                dst_ptr += 4;
            }
        }
    }
} g_LinuxEngine;

static void SendToLinuxClipboard(const std::wstring& text) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::string utf8_text = converter.to_bytes(text);
    
    FILE* pipe = popen("wl-copy 2>/dev/null || xclip -selection clipboard", "w");
    if (pipe) {
        std::fputs(utf8_text.c_str(), pipe);
        pclose(pipe);
    }
}

// ==============================================================================
// 3. ЭКСПОРТ И ИНТЕГРАЦИЯ API FAR2L (LINUX POSIX)
// ==============================================================================

extern "C" void WINAPI SetStartupInfoW(const struct PluginStartupInfo *PSInfo) {
    Info = *PSInfo;
    LoadLinuxPluginConfig(); // Загружаем кастомные параметры из ini при старте сессии far2l
}

extern "C" void WINAPI GetPluginInfoW(struct PluginInfo *PInfo) {
    PInfo->StructSize = sizeof(struct PluginInfo);
    PInfo->Flags = PF_DISABLEPANELS; 

    static const wchar_t* PluginMenuStrings;
    PluginMenuStrings = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 0)); 
    
    PInfo->PluginMenu.Strings = &PluginMenuStrings;
    PInfo->PluginMenu.Count = 1;
}

extern "C" intptr_t WINAPI ProcessViewerEventW(const struct ProcessViewerEventInfo *PInfo) {
    if (PInfo->Event == VE_READ) {
        if (g_LinuxCache.GetPage(g_LinuxCurrentPage, g_LinuxData)) {
            g_LinuxEngine.RenderToFramebuffer(g_LinuxData, g_LinuxZoom, g_LinuxOffsetX, g_LinuxOffsetY);
        }
        return VIEWER_SUCCESS;
    }
    return VIEWER_NONE;
}

extern "C" intptr_t WINAPI ProcessConsoleInputW(const struct ProcessConsoleInputInfo *PInfo) {
    if (PInfo->Rec.EventType == KEY_EVENT && PInfo->Rec.Event.KeyEvent.bKeyDown) {
        DWORD key_code = PInfo->Rec.Event.KeyEvent.wVirtualKeyCode;
        DWORD ctrl = PInfo->Rec.Event.KeyEvent.dwControlKeyState;
        float speed = g_LinuxKeyboardScrollSpeed / g_LinuxZoom; // Шаг скролла из ini

        switch (key_code) {
            case KEY_UP:    g_LinuxOffsetY += speed; goto redraw_linux;
            case KEY_DOWN:  g_LinuxOffsetY -= speed; goto redraw_linux;
            case KEY_LEFT:  g_LinuxOffsetX += speed; goto redraw_linux;
            case KEY_RIGHT: g_LinuxOffsetX -= speed; goto redraw_linux;

            case '+':
            case '=':
                g_LinuxZoom *= g_LinuxKeyboardZoomStep; goto redraw_linux; // Шаг зума из ini
            case '-':
                g_LinuxZoom /= g_LinuxKeyboardZoomStep; 
                if (g_LinuxZoom < 0.05f) g_LinuxZoom = 0.05f; 
                if (g_LinuxDropCacheOnZoom) g_LinuxSearchResults.clear(); 
                goto redraw_linux;

            case 'C':
            case 'c':
                if (ctrl & LEFT_CTRL_PRESSED || ctrl & RIGHT_CTRL_PRESSED) {
                    if (g_LinuxData.has_text) {
                        SendToLinuxClipboard(g_LinuxData.extracted_text);
                        const wchar_t* msgText = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 6));
                        Info.Message(Info.ModuleNumber, FMSG_MB_OK, nullptr, &msgText, 1, 0);
                    }
                    return VIEWER_SUCCESS;
                }
                break;

            case KEY_F7: {
                wchar_t buf = {0};
                const wchar_t* dlgTitle = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 4));
                const wchar_t* dlgPrompt = reinterpret_cast<const wchar_t*>(Info.GetMsg(Info.ModuleNumber, 5));

                if (Info.InputBox(dlgTitle, dlgPrompt, L"ViewerSearch", g_LinuxSearchQuery.c_str(), buf, 256, FIB_BUTTONS)) {
                    g_LinuxSearchQuery = buf;
                    g_LinuxSearchResults = ImageDecoder::SearchTextOnPage(g_LinuxCurrentPath, g_LinuxCurrentPage, g_LinuxSearchQuery, g_LinuxZoom);
                    goto redraw_linux;
                }
                return VIEWER_SUCCESS;
            }

            // Перелистывание страниц (Ctrl + PageUp / PageDown)
            case KEY_DOWN | KEY_CTRL:
            case KEY_PGDN:
                if ((ctrl & LEFT_CTRL_PRESSED || ctrl & RIGHT_CTRL_PRESSED) && g_LinuxCurrentPage < g_LinuxData.total_pages - 1) {
                    g_LinuxCurrentPage++; g_LinuxSearchResults.clear(); goto redraw_linux;
                }
                break;
            case KEY_UP | KEY_CTRL:
            case KEY_PGUP:
                if ((ctrl & LEFT_CTRL_PRESSED || ctrl & RIGHT_CTRL_PRESSED) && g_LinuxCurrentPage > 0) {
                    g_LinuxCurrentPage--; g_LinuxSearchResults.clear(); goto redraw_linux;
                }
                break;
        }
    }
    return VIEWER_NONE;

redraw_linux:
    Info.ViewerControl(HANDLE_CURRENT, VCTL_REDRAW, 0, nullptr);
    return VIEWER_SUCCESS;
}
#endif
