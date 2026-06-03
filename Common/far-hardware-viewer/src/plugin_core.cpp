#include "plugin_core.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <codecvt>
#include <locale>

// Внешние движки-декодеры, входящие в проект
#include <mupdf/fitz.h>
#include <libraw.h>
#include <FreeImage.h>
#include <lunasvg.h>
#include <archive.h>
#include <archive_entry.h>
#include <libdxfrw.h>
#include <drw_interface.h>

#ifdef _WIN32
bool DecodeViaWIC(const std::wstring& path, RawImageData& out_data);
#endif

// ==============================================================================
// 1. МОДУЛЬ ОПРЕДЕЛЕНИЯ ФОРМАТА ПО СИГНАТУРАМ (из fumiama/Imagine-Plugin)
// ==============================================================================
enum class ImageFormat { UNKNOWN, JPEG, PNG, GIF, PDF_EPS, CAD_DXF };

static ImageFormat DetectFormatByMagicBytes(const std::wstring& path) {
#ifdef _WIN32
    std::ifstream file(path, std::ios::binary);
#else
    // Конвертация пути для POSIX-совместимого std::ifstream в Linux
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
    std::ifstream file(cvt.to_bytes(path), std::ios::binary);
#endif
    if (!file) return ImageFormat::UNKNOWN;
    
    uint8_t magic[16] = {0};
    file.read(reinterpret_cast<char*>(magic), 16);
    
    if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return ImageFormat::JPEG;
    if (magic[0] == 0x89 && magic[1] == 'P'  && magic[2] == 'N' && magic[3] == 'G') return ImageFormat::PNG;
    if (magic[0] == 'G'  && magic[1] == 'I'  && magic[2] == 'F') return ImageFormat::GIF;
    if (magic[0] == '%'  && magic[1] == 'P'  && magic[2] == 'D' && magic[3] == 'F') return ImageFormat::PDF_EPS;
    if (magic[0] == '%'  && magic[1] == 'P'  && magic[2] == 'S') return ImageFormat::PDF_EPS; // EPS
    
    std::string start_chunk(reinterpret_cast<char*>(magic), 16);
    if (start_chunk.find("0") != std::string::npos || start_chunk.find("SECTION") != std::string::npos) {
        return ImageFormat::CAD_DXF;
    }
    
    return ImageFormat::UNKNOWN;
}

// ==============================================================================
// 2. МОДУЛЬ БЕЗДИСКОВОЙ РАСПАКОВКИ ИЗ АРХИВОВ В RAM (через libarchive)
// ==============================================================================
bool ExtractFromArchiveToRAM(const std::wstring& archive_path, const std::wstring& target_file, std::vector<uint8_t>& out_bytes) {
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
    std::string a_path_utf8 = cvt.to_bytes(archive_path);
    std::string t_file_utf8 = cvt.to_bytes(target_file);

    if (archive_read_open_filename(a, a_path_utf8.c_str(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    struct archive_entry *entry;
    bool found = false;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (std::string(archive_entry_pathname(entry)) == t_file_utf8) {
            size_t size = archive_entry_size(entry);
            out_bytes.resize(size);
            archive_read_data(a, out_bytes.data(), size);
            found = true;
            break;
        }
    }
    archive_read_support_format_free(a);
    archive_read_free(a);
    return found;
}

// ==============================================================================
// 3. МОДУЛЬ ВЕКТОРНОГО CAD ПАРСЕРА ЧЕРТЕЖЕЙ (через libdxfrw)
// ==============================================================================
class FarCADInterface : public DRW_Interface {
public:
    std::vector<CADLine> lines;
    void addLine(const DRW_Line& d) override {
        lines.push_back({static_cast<float>(d.basePoint.x), static_cast<float>(d.basePoint.y), 
                         static_cast<float>(d.secPoint.x),  static_cast<float>(d.secPoint.y)});
    }
    void addCircle(const DRW_Circle&) override {}
    void addText(const DRW_Text&) override {}
};

static bool DecodeViaLibDXFRW(const std::wstring& wpath, RawImageData& out_data) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
    FarCADInterface dxf;
    dxf_reader file(cvt.to_bytes(wpath).c_str());
    
    if (!file.readDxf(&dxf, false)) return false;
    
    out_data.cad_lines = std::move(dxf.lines);
    out_data.is_cad = true;
    out_data.width = 1920;  // Виртуальный размер координатной сетки по умолчанию
    out_data.height = 1080;
    out_data.decoder_name = L"libdxfrw CAD Engine";
    return true;
}

// ==============================================================================
// 4. МОДУЛЬ ВЕКТОРНЫХ ДОКУМЕНТОВ И ИЗВЛЕЧЕНИЯ ТЕКСТА (через MuPDF)
// ==============================================================================
static bool DecodeViaMuPDF(const std::wstring& wpath, uint32_t page_num, float zoom, RawImageData& out_data) {
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    fz_register_document_handlers(ctx);
    
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
    fz_document *doc = fz_open_document(ctx, cvt.to_bytes(wpath).c_str());
    if (!doc) { fz_drop_context(ctx); return false; }
    
    out_data.total_pages = fz_count_pages(ctx, doc);
    out_data.current_page = page_num;
    
    fz_page *page = fz_load_page(ctx, doc, page_num);
    fz_matrix ctm = fz_scale(zoom, zoom);
    fz_pixmap *pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);
    
    out_data.width = fz_pixmap_width(ctx, pix);
    out_data.height = fz_pixmap_height(ctx, pix);
    out_data.rgba_data.resize(out_data.width * out_data.height * 4);
    std::memcpy(out_data.rgba_data.data(), fz_pixmap_samples(ctx, pix), out_data.rgba_data.size());
    
    // Интеграция Text Extraction (Извлечение текстового слоя для Ctrl+C)
    fz_text_page *tp = fz_new_text_page(ctx, fz_bound_page(ctx, page));
    fz_device *dev = fz_new_text_device(ctx, tp, NULL);
    fz_run_page(ctx, page, dev, fz_identity, NULL);
    fz_close_device(ctx, dev);
    
    fz_buffer *buf = fz_new_buffer(ctx, 1024);
    fz_print_text_page_as_text(ctx, buf, tp);
    out_data.extracted_text = cvt.from_bytes(fz_string_from_buffer(ctx, buf));
    out_data.has_text = !out_data.extracted_text.empty();
    
    fz_drop_buffer(ctx, buf);
    fz_drop_device(ctx, dev);
    fz_drop_text_page(ctx, tp);
    fz_drop_pixmap(ctx, pix);
    fz_drop_page(ctx, page);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    
    out_data.is_cad = false;
    out_data.decoder_name = L"MuPDF Engine";
    return true;
}

std::vector<SearchResultQuad> ImageDecoder::SearchTextOnPage(const std::wstring& wpath, uint32_t page_num, const std::wstring& query, float zoom) {
    std::vector<SearchResultQuad> res;
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) return res;
    fz_register_document_handlers(ctx);
    
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
    fz_document *doc = fz_open_document(ctx, cvt.to_bytes(wpath).c_str());
    if (!doc) { fz_drop_context(ctx); return res; }
    
    fz_page *page = fz_load_page(ctx, doc, page_num);
    fz_matrix ctm = fz_scale(zoom, zoom);
    
    const int MAX_HITS = 256;
    fz_quad hits[MAX_HITS];
    int count = fz_search_page(ctx, page, cvt.to_bytes(query).c_str(), hits, MAX_HITS);
    
    for (int i = 0; i < count; ++i) {
        fz_quad q = fz_transform_quad(hits[i], ctm);
        res.push_back({q.ul.x, q.ul.y, q.ur.x, q.ur.y, q.ll.x, q.ll.y, q.lr.x, q.lr.y});
    }
    
    fz_drop_page(ctx, page);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return res;
}

// ==============================================================================
// 5. ГЛАВНЫЙ ФОЛБЕК-ДИСПЕТЧЕР ДЕКОДИРОВАНИЯ ЦЕПОЧЕК
// ==============================================================================
RawImageData ImageDecoder::DecodeImage(const std::wstring& file_path, uint32_t target_page, float zoom) {
    RawImageData res;
    ImageFormat fmt = DetectFormatByMagicBytes(file_path);
    
    switch (fmt) {
        case ImageFormat::CAD_DXF:
            if (DecodeViaLibDXFRW(file_path, res)) return res;
            break;
        case ImageFormat::PDF_EPS:
            if (DecodeViaMuPDF(file_path, target_page, zoom, res)) return res;
            break;
        default:
            // Если векторные или специфические движки не сработали, уходим на стандартный растр
#ifdef _WIN32
            // В Windows вызываем ультра-быстрый системный WIC (декодирует JPG/PNG/WebP силами ОС)
            if (DecodeViaWIC(file_path, res)) return res;
#else
            // В Linux вызываем FreeImage, собранный под x86-64-v3
            // (Реализация FreeImage-декодера опускается для монолитности, логика аналогична WIC)
#endif
            break;
    }
    return res;
}

// ==============================================================================
// 6. МЕНЕДЖЕР ПОТОКОВ УПРЕЖДАЮЩЕГО КЭША (CacheManager)
// ==============================================================================
bool PageCacheManager::GetPage(uint32_t page_num, RawImageData& out_data) {
    std::lock_guard<std::mutex> lk(m_mutex);
    
    // Если запрашиваемая страница уже скомпилирована в ОЗУ — возвращаем за 0 мс
    if (m_pageMap.count(page_num)) {
        out_data = m_pageMap[page_num];
        MaintainCache(page_num, out_data.total_pages);
        return true;
    }
    
    // Если страница рассчитывается в фоновом пуле асинхронно
    if (m_workerMap.count(page_num)) {
        auto& fut = m_workerMap[page_num];
        if (fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            m_pageMap[page_num] = fut.get();
            m_workerMap.erase(page_num);
            out_data = m_pageMap[page_num];
            MaintainCache(page_num, out_data.total_pages);
            return true;
        }
    } else {
        // Холодный старт: если страница не запрашивалась ранее в фоне
        TriggerAsyncDecode(page_num);
    }
    return false;
}

void PageCacheManager::TriggerAsyncDecode(uint32_t page_num) {
    std::wstring path = m_currentPath;
    float zoom = m_currentZoom;
    
    // Вызов фонового асинхронного таска из неблокирующего пула ОС
    m_workerMap[page_num] = std::async(std::launch::async, [path, page_num, zoom]() {
        return ImageDecoder::DecodeImage(path, page_num, zoom);
    });
}

void PageCacheManager::MaintainCache(uint32_t active_page, uint32_t total_pages) {
    // Кольцевой радиус удержания страниц в ОЗУ: удерживаем текущую, прошлую и следующую
    std::vector<uint32_t> keep = { active_page };
    if (active_page > 0) keep.push_back(active_page - 1);
    if (active_page < total_pages - 1) keep.push_back(active_page + 1);
    
    // Стираем из памяти всё, что вышло за рамки радиуса соседей (сбережение RAM)
    for (auto it = m_pageMap.begin(); it != m_pageMap.end();) {
        if (std::find(keep.begin(), keep.end(), it->first) == keep.end()) {
            it = m_pageMap.erase(it);
        } else {
            ++it;
        }
    }
    
    // Запускаем упреждающую фоновую сборку для отсутствующих соседей
    for (uint32_t target : keep) {
        if (!m_pageMap.count(target) && !m_workerMap.count(target)) {
            TriggerAsyncDecode(target);
        }
    }
}

