#ifdef _WIN32
#include "plugin_core.hpp"
#include <plugin.hpp>
#include <wincodec.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <shlwapi.h> // Необходим для Path-функций работы с файлом ini
#include <string>
#include <cwchar>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

// Глобальные переменные состояния и конфигурации Windows-версии
struct PluginStartupInfo Info;
struct FarStandardFunctions FSF;

// Параметры конфигурации из config.ini по умолчанию
int      g_PrefetchRadius = 1;
int      g_AsyncTimeoutMs = 150;
int      g_DropCacheOnZoom = 0;
bool     g_IsDarkScheme = true;
float    g_BgColorR = 0.05f, g_BgColorG = 0.05f, g_BgColorB = 0.05f, g_BgColorA = 1.0f;
float    g_LineColorR = 0.19f, g_LineColorG = 0.80f, g_LineColorB = 0.19f, g_LineColorA = 1.0f;
float    g_MinStrokeWidth = 0.5f;
float    g_KeyboardZoomStep = 1.10f;
float    g_MouseWheelZoomStep = 1.05f;
float    g_KeyboardScrollSpeed = 25.0f;
int      g_ShowStatusBar = 1;
float    g_StatusBarAlpha = 0.70f;

// Переменные состояния просмотра
float g_Zoom = 1.0f;
float g_OffsetX = 0.0f;
float g_OffsetY = 0.0f;
uint32_t g_CurrentPage = 0;
std::wstring g_CurrentFilePath;
std::wstring g_LastSearchQuery;

RawImageData g_CurrentData;
PageCacheManager g_Cache;
std::vector<SearchResultQuad> g_SearchResults;

bool g_NeedResize = false;
bool g_TextureUploaded = false;

// ==============================================================================
// 1. ИНТЕРФЕЙС БЕЗОПАСНОГО ВСТРОЕННОГО ЧТЕНИЯ НАСТРОЕК (Win32 INI API)
// ==============================================================================
void LoadPluginConfig() {
    wchar_t iniPath[MAX_PATH];
    // Получаем путь к текущему двоичному файлу плагина
    GetModuleFileNameW(reinterpret_cast<HINSTANCE>(&__ImageBase), iniPath, MAX_PATH);
    PathRemoveFileSpecW(iniPath);
    PathAppendW(iniPath, L"config.ini");

    if (!PathFileExistsW(iniPath)) return; // Если ini-файла нет, остаются дефолтные параметры

    // Раздел [CacheManager]
    g_PrefetchRadius  = GetPrivateProfileIntW(L"CacheManager", L"PrefetchRadius", 1, iniPath);
    g_AsyncTimeoutMs  = GetPrivateProfileIntW(L"CacheManager", L"AsyncTimeoutMs", 150, iniPath);
    g_DropCacheOnZoom = GetPrivateProfileIntW(L"CacheManager", L"DropCacheOnZoom", 0, iniPath);

    // Раздел [CAD_Rendering_Engine]
    wchar_t scheme[32];
    GetPrivateProfileStringW(L"CAD_Rendering_Engine", L"ColorScheme", L"dark", scheme, 32, iniPath);
    g_IsDarkScheme = (wcscmp(scheme, L"dark") == 0);

    wchar_t buf[32];
    #define GET_INI_FLOAT(sec, key, def) \
        GetPrivateProfileStringW(sec, key, def, buf, 32, iniPath); \
        g_##key = wcstof(buf, nullptr);

    GET_INI_FLOAT(L"CAD_Rendering_Engine", BgColorR, L"0.05");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", BgColorG, L"0.05");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", BgColorB, L"0.05");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", BgColorA, L"1.00");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", LineColorR, L"0.19");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", LineColorG, L"0.80");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", LineColorB, L"0.19");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", LineColorA, L"1.00");
    GET_INI_FLOAT(L"CAD_Rendering_Engine", MinStrokeWidth, L"0.5");

    // Раздел [Controls]
    GET_INI_FLOAT(L"Controls", KeyboardZoomStep, L"1.10");
    GET_INI_FLOAT(L"Controls", MouseWheelZoomStep, L"1.05");
    GET_INI_FLOAT(L"Controls", KeyboardScrollSpeed, L"25.0");

    // Раздел [Interface]
    g_ShowStatusBar   = GetPrivateProfileIntW(L"Interface", L"ShowStatusBar", 1, iniPath);
    GET_INI_FLOAT(L"Interface", StatusBarAlpha, L"0.70");
    #undef GET_INI_FLOAT
}

// ==============================================================================
// 2. НАПРАВЛЕННЫЙ СИСТЕМНЫЙ ДЕКОДЕР ИЗОБРАЖЕНИЙ WINDOWS (WIC)
// ==============================================================================
bool DecodeViaWIC(const std::wstring& path, RawImageData& out_data) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    if (FAILED(frame->GetSize(&out_data.width, &out_data.height))) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        return false;
    }

    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    out_data.rgba_data.resize(out_data.width * out_data.height * 4);
    if (FAILED(converter->CopyPixels(nullptr, out_data.width * 4, static_cast<UINT>(out_data.rgba_data.size()), out_data.rgba_data.data()))) {
        return false;
    }

    out_data.is_cad = false;
    out_data.decoder_name = L"WIC System Engine";
    return true;
}

static void SendToWindowsClipboard(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(GetConsoleWindow())) return;
    EmptyClipboard();

    size_t byteSize = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteSize);
    if (hMem) {
        void* pMem = GlobalLock(hMem);
        if (pMem) {
            std::memcpy(pMem, text.c_str(), byteSize);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

// ==============================================================================
// 3. ГРАФИЧЕСКИЙ СТЭК GPU RENDERING (Direct2D + Динамическая палитра CAD из ini)
// ==============================================================================
class D2DConsoleRenderer {
private:
    ComPtr<ID2D1Factory>            m_pDirect2DFactory;
    ComPtr<ID2D1HwndRenderTarget>   m_pRenderTarget;
    ComPtr<ID2D1Bitmap>             m_pBitmap;
    ComPtr<IDWriteFactory>          m_pDWriteFactory;
    ComPtr<IDWriteTextFormat>       m_pTextFormat;
    ComPtr<ID2D1SolidColorBrush>    m_pLineBrush;
    ComPtr<ID2D1SolidColorBrush>    m_pTextBrush;
    ComPtr<ID2D1SolidColorBrush>    m_pHighlightBrush;
    ComPtr<ID2D1SolidColorBrush>    m_pBackgroundBrush;
    HWND                            m_hwndConsole;

    float m_cadMinX = 0.0f, m_cadMaxX = 0.0f;
    float m_cadMinY = 0.0f, m_cadMaxY = 0.0f;
    bool  m_bbCalculated = false;

public:
    D2DConsoleRenderer() {
        m_hwndConsole = GetConsoleWindow();
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, &m_pDirect2DFactory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &m_pDWriteFactory);
        
        m_pDWriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 
                                           DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"ru-RU", &m_pTextFormat);
        m_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    bool InitRenderTarget() {
        if (m_pRenderTarget) return true;
        RECT rc; GetClientRect(m_hwndConsole, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        HRESULT hr = m_pDirect2DFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE),
            D2D1::HwndRenderTargetProperties(m_hwndConsole, size),
            &m_pRenderTarget
        );

        if (SUCCEEDED(hr)) {
            // Применяем кастомные цвета линий и статус-бара из config.ini
            m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(g_LineColorR, g_LineColorG, g_LineColorB, g_LineColorA), &m_pLineBrush);
            m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_pTextBrush);
            m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow, 0.4f), &m_pHighlightBrush);
            m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, g_StatusBarAlpha), &m_pBackgroundBrush);
        }
        return SUCCEEDED(hr);
    }

    void HandleResize() {
        if (!m_pRenderTarget) return;
        RECT rc; GetClientRect(m_hwndConsole, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        m_pRenderTarget->Resize(size);
        g_NeedResize = false;
    }

    bool UploadRasterTexture(const RawImageData& img) {
        if (!InitRenderTarget()) return false;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        HRESULT hr = m_pRenderTarget->CreateBitmap(D2D1::SizeU(img.width, img.height), img.rgba_data.data(), img.width * 4, &props, &m_pBitmap);
        return SUCCEEDED(hr);
    }

    void DropTextureCache() {
        m_pBitmap.Reset();
        m_bbCalculated = false;
    }

    void RenderRaster(float zoom, float ox, float oy, const RawImageData& meta) {
        if (!m_pRenderTarget || !m_pBitmap) return;
        m_pRenderTarget->BeginDraw();
        m_pRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        D2D1_SIZE_F imgSize = m_pBitmap->GetSize();
        D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Scale(zoom, zoom) * D2D1::Matrix3x2F::Translation(ox, oy);
        m_pRenderTarget->SetTransform(transform);

        D2D1_RECT_F destRect = D2D1::RectF(0, 0, imgSize.width, imgSize.height);
        m_pRenderTarget->DrawBitmap(m_pBitmap.Get(), &destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        if (!g_SearchResults.empty()) {
            for (const auto& q : g_SearchResults) {
                D2D1_RECT_F r = D2D1::RectF(q.ulx, q.uly, q.lrx, q.lry);
                m_pRenderTarget->FillRectangle(&r, m_pHighlightBrush.Get());
            }
        }

        m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        if (g_ShowStatusBar) DrawStatusBar(meta, zoom);
        m_pRenderTarget->EndDraw();
    }

    void RenderCAD(const std::vector<CADLine>& lines, float userZoom, float ox, float oy, const RawImageData& meta) {
        if (!m_pRenderTarget || lines.empty()) return;
        m_pRenderTarget->BeginDraw();

        // Управление подложкой фона чертежа на основе параметров config.ini
        if (g_IsDarkScheme) {
            m_pRenderTarget->Clear(D2D1::ColorF(g_BgColorR, g_BgColorG, g_BgColorB, g_BgColorA));
        } else {
            m_pRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::White));
            if (m_pLineBrush) m_pLineBrush->SetColor(D2D1::ColorF(D2D1::ColorF::Black)); // Принудительный инверсный черный
        }

        if (!m_bbCalculated) {
            m_cadMinX = m_cadMaxX = lines.x1; m_cadMinY = m_cadMaxY = lines.y1;
            for (const auto& l : lines) {
                m_cadMinX = (std::min)({m_cadMinX, l.x1, l.x2}); m_cadMaxX = (std::max)({m_cadMaxX, l.x1, l.x2});
                m_cadMinY = (std::min)({m_cadMinY, l.y1, l.y2}); m_cadMaxY = (std::max)({m_cadMaxY, l.y1, l.y2});
            }
            m_bbCalculated = true;
        }

        D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();
        float cw = m_cadMaxX - m_cadMinX, ch = m_cadMaxY - m_cadMinY;
        float autoFit = (std::min)((rtSize.width - 40.0f) / (cw ? cw : 1.0f), (rtSize.height - 40.0f) / (ch ? ch : 1.0f));

        D2D1_MATRIX_3X2_F toOrigin   = D2D1::Matrix3x2F::Translation(-m_cadMinX, -m_cadMinY);
        D2D1_MATRIX_3X2_F invertY    = D2D1::Matrix3x2F::Scale(1.0f, -1.0f);
        D2D1_MATRIX_3X2_F pushBack   = D2D1::Matrix3x2F::Translation(20.0f, rtSize.height - 20.0f);
        D2D1_MATRIX_3X2_F userTrans  = D2D1::Matrix3x2F::Scale(userZoom, userZoom, D2D1::Point2F(rtSize.width / 2, rtSize.height / 2)) * D2D1::Matrix3x2F::Translation(ox, oy);

        m_pRenderTarget->SetTransform(toOrigin * invertY * D2D1::Matrix3x2F::Scale(autoFit, autoFit) * pushBack * userTrans);
        m_pRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        float strokeWidth = 1.0f / (autoFit * userZoom);
        if (strokeWidth < g_MinStrokeWidth) strokeWidth = g_MinStrokeWidth; // Безопасный порог видимости из ini

        for (const auto& l : lines) {
            m_pRenderTarget->DrawLine(D2D1::Point2F(l.x1, l.y1), D2D1::Point2F(l.x2, l.y2), m_pLineBrush.Get(), strokeWidth);
        }

        m_pRenderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
        if (g_ShowStatusBar) DrawStatusBar(meta, userZoom);
        m_pRenderTarget->EndDraw();
    }

private:
    void DrawStatusBar(const RawImageData& meta, float zoom) {
        D2D1_SIZE_F rtSize = m_pRenderTarget->GetSize();
        wchar_t sb;
        
        const wchar_t* lblPage   = Info.GetMsg(Info.ModuleNumber, 7); 
        const wchar_t* lblZoom   = Info.GetMsg(Info.ModuleNumber, 8); 
        const wchar_t* lblEngine = Info.GetMsg(Info.ModuleNumber, 9); 

        swprintf_s(sb, L" %dx%d | %s %d/%d | %s %d%% | %s %s %s ", 
                   meta.width, meta.height, 
                   lblPage, meta.current_page + 1, meta.total_pages, 
                   lblZoom, static_cast<int>(zoom * 100), 
                   lblEngine, meta.decoder_name.c_str(),
                   meta.has_text ? L"|[Ctrl+C]" : L"");

        size_t len = wcslen(sb);
        D2D1_RECT_F textRect = D2D1::RectF(rtSize.width - 550.0f, 10.0f, rtSize.width - 10.0f, 35.0f);
        m_pRenderTarget->FillRectangle(&textRect, m_pBackgroundBrush.Get());
        m_pRenderTarget->DrawTextW(sb, static_cast<UINT32>(len), m_pTextFormat.Get(), &textRect, m_pTextBrush.Get());
    }
} g_D2D;

// ==============================================================================
// 4. ЭКСПОРТ И ИНТЕГРАЦИЯ API FAR MANAGER 3 SDK
// ==============================================================================
void WINAPI SetStartupInfoW(const struct PluginStartupInfo *PSInfo) {
    Info = *PSInfo;
    FSF = *PSInfo->FSF;
    LoadPluginConfig(); // Загружаем внешние настройки при старте плагина
}

void WINAPI GetPluginInfoW(struct PluginInfo *PInfo) {
    PInfo->StructSize = sizeof(struct PluginInfo);
    PInfo->Flags = PF_DISABLEPANELS;

    static const wchar_t* PluginMenuStrings;
    PluginMenuStrings = Info.GetMsg(Info.ModuleNumber, 0); 
    
    PInfo->PluginMenu.Strings = PluginMenuStrings;
    PInfo->PluginMenu.Count = 1;
}

intptr_t WINAPI ProcessViewerEventW(const struct ProcessViewerEventInfo *PInfo) {
    if (PInfo->Event == VE_SIZE) { 
        g_NeedResize = true; 
    }
    if (PInfo->Event == VE_READ) {
        g_D2D.InitRenderTarget();
        if (g_NeedResize) g_D2D.HandleResize();

        if (g_Cache.GetPage(g_CurrentPage, g_CurrentData)) {
            if (g_CurrentData.is_cad) {
                g_D2D.RenderCAD(g_CurrentData.cad_lines, g_Zoom, g_OffsetX, g_OffsetY, g_CurrentData);
            } else {
                if (!g_TextureUploaded) {
                    g_D2D.UploadRasterTexture(g_CurrentData);
                    g_TextureUploaded = true;
                }
                g_D2D.RenderRaster(g_Zoom, g_OffsetX, g_OffsetY, g_CurrentData);
            }
        }
    }
    return VIEWER_SUCCESS;
}

intptr_t WINAPI ProcessConsoleInputW(const struct ProcessConsoleInputInfo *PInfo) {
    if (PInfo->Rec.EventType == KEY_EVENT && PInfo->Rec.Event.KeyEvent.bKeyDown) {
        WORD vk = PInfo->Rec.Event.KeyEvent.wVirtualKeyCode;
        DWORD ctrl = PInfo->Rec.Event.KeyEvent.dwControlKeyState;
        float speed = g_KeyboardScrollSpeed / g_Zoom; // Подгружаем базовую скорость из ini

        switch (vk) {
            case VK_UP:    g_OffsetY += speed; goto force_redraw;
            case VK_DOWN:  g_OffsetY -= speed; goto force_redraw;
            case VK_LEFT:  g_OffsetX += speed; goto force_redraw;
            case VK_RIGHT: g_OffsetX -= speed; goto force_redraw;

            case VK_ADD:      
            case 0xBB: 
                g_Zoom *= g_KeyboardZoomStep; goto force_redraw; // Множитель зума из ini
            case VK_SUBTRACT: 
            case 0xBD: 
                g_Zoom /= g_KeyboardZoomStep; if (g_Zoom < 0.05f) g_Zoom = 0.05f; if(g_DropCacheOnZoom) g_D2D.DropTextureCache(); goto force_redraw;

            case 0x43: 
                if (ctrl & LEFT_CTRL_PRESSED || ctrl & RIGHT_CTRL_PRESSED) {
                    if (g_CurrentData.has_text) {
                        SendToWindowsClipboard(g_CurrentData.extracted_text);
                        const wchar_t* msgTitle = Info.GetMsg(Info.ModuleNumber, 1);
                        const wchar_t* msgText  = Info.GetMsg(Info.ModuleNumber, 6);
                        Info.Message(Info.ModuleNumber, FMSG_MB_OK, nullptr, &msgTitle, 1, 0);
                    }
                    return VIEWER_SUCCESS;
                }
                break;

            case VK_F7: { 
                wchar_t buf = {0};
                const wchar_t* dlgTitle = Info.GetMsg(Info.ModuleNumber, 4);
                const wchar_t* dlgPrompt = Info.GetMsg(Info.ModuleNumber, 5);

                if (Info.InputBox(dlgTitle, dlgPrompt, L"ViewerSearch", g_LastSearchQuery.c_str(), buf, 256, FIB_BUTTONS)) {
                    g_LastSearchQuery = buf;
                    g_SearchResults = ImageDecoder::SearchTextOnPage(g_CurrentFilePath, g_CurrentPage, g_LastSearchQuery, g_Zoom);
                    goto force_redraw;
                }
                return VIEWER_SUCCESS;
            }

            case VK_NEXT: 
                if ((ctrl & LEFT_CTRL_PRESSED || ctrl & RIGHT_CTRL_PRESSED) && g_CurrentPage < g_CurrentData.total_pages - 1) {
                    g_CurrentPage++; g_TextureUploaded = false; g_SearchResults.clear(); g_D2D.DropTextureCache(); goto force_redraw;
                }
                break;
            case VK_PRIOR: 
                if ((ctrl & LEFT_CTRL_PRESSED || ctrl & RIGHT_CTRL_PRESSED) && g_CurrentPage > 0) {
                    g_CurrentPage--; g_TextureUploaded = false; g_SearchResults.clear(); g_D2D.DropTextureCache(); goto force_redraw;
                }
                break;
        }
    }

    if (PInfo->Rec.EventType == MOUSE_EVENT) {
        MOUSE_EVENT_RECORD mer = PInfo->Rec.Event.MouseEvent;
        if (mer.dwEventFlags == MOUSE_WHEELED) {
            if (static_cast<short>(HIWORD(mer.dwButtonState)) > 0) g_Zoom *= g_MouseWheelZoomStep; // Шаг колесика мыши из ini
            else g_Zoom /= g_MouseWheelZoomStep;
            if (g_Zoom < 0.05f) g_Zoom = 0.05f;
            if(g_DropCacheOnZoom) g_D2D.DropTextureCache();
            goto force_redraw;
        }
    }
    return VIEWER_NONE;

force_redraw:
    Info.ConsoleInfo.ActlControl(HANDLE_CURRENT, VCTL_REDRAW, 0, nullptr);
    return VIEWER_SUCCESS;
}
#endif
