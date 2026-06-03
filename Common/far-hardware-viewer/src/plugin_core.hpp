#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <mutex>
#include <future>

// ==============================================================================
// 1. БАЗОВЫЕ ГЕОМЕТРИЧЕСКИЕ И ГРАФИЧЕСКИЕ СТРУКТУРЫ ДАННЫХ
// ==============================================================================

// Структура плоской CAD-линии (используется для чертежей DXF/DWG)
struct CADLine {
    float x1, y1;
    float x2, y2;
};

// Экранный четырехугольник для точной подсветки найденных слов (F7)
struct SearchResultQuad {
    float ulx, uly; // Верхний левый угол (Upper-Left)
    float urx, ury; // Верхний правый угол (Upper-Right)
    float llx, lly; // Нижний левый угол (Lower-Left)
    float lrx, lry; // Нижний правый угол (Lower-Right)
};

// Универсальный контейнер несжатого кадра/страницы в оперативной памяти
struct RawImageData {
    std::vector<uint8_t> rgba_data; // Попиксельный буфер для растра (WIC/FreeImage/MuPDF)
    std::vector<CADLine> cad_lines; // Массив векторных линий для инженерных чертежей
    
    uint32_t width = 0;             // Ширина изображения или координатной сетки
    uint32_t height = 0;            // Высота изображения или координатной сетки
    uint32_t current_page = 0;      // Индекс текущей открытой страницы (начиная с 0)
    uint32_t total_pages = 1;       // Общее количество страниц в документе/книге
    
    std::wstring decoder_name;      // Имя активного движка для вывода в статус-бар
    std::wstring extracted_text;    // Текстовый слой текущей страницы для Clipboard
    
    bool has_text = false;          // Флаг: содержит ли страница извлекаемый текст
    bool is_cad = false;            // Флаг: чертеж перед нами или растровая картинка
};

// ==============================================================================
// 2. ИНТЕРФЕЙС КРОСС-ПЛАТФОРМЕННОГО ПОЛИДВИЖКА ДЕКОДИРОВАНИЯ
// ==============================================================================
class ImageDecoder {
public:
    // Автоматический анализ сигнатур (Magic Bytes) и неблокирующее декодирование в RAM
    static RawImageData DecodeImage(const std::wstring& file_path, uint32_t target_page, float zoom);
    
    // Векторный поиск по тексту страницы средствами MuPDF с возвратом координат совпадений
    static std::vector<SearchResultQuad> SearchTextOnPage(const std::wstring& path, uint32_t page, const std::wstring& query, float zoom);
};

// ==============================================================================
// 3. МЕНЕДЖЕР КОЛЬЦЕВОГО УПРЕЖДАЮЩЕГО КЭШИРОВАНИЯ СТРАНИЦ (Prefetching)
// ==============================================================================
class PageCacheManager {
private:
    std::map<uint32_t, RawImageData> m_pageMap;                // Кэш готовых страниц в RAM
    std::map<uint32_t, std::future<RawImageData>> m_workerMap; // Активные фоновые потоки std::async
    std::mutex m_mutex;                                        // Мьютекс для защиты многопоточных операций
    
    std::wstring m_currentPath;                                 // Путь к текущему открытому документу
    float m_currentZoom = 1.0f;                                 // Текущий коэффициент масштабирования

public:
    PageCacheManager() = default;
    ~PageCacheManager() = default;

    // Запрет копирования менеджера кэша во избежание десинхронизации потоков
    PageCacheManager(const PageCacheManager&) = delete;
    PageCacheManager& operator=(const PageCacheManager&) = delete;

    // Установка контекста текущего файла при холодном старте или изменении масштаба
    void SetContext(const std::wstring& path, float zoom) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_currentPath = path;
        m_currentZoom = zoom;
    }

    // Запрос страницы. Возвращает true, если она готова. Запускает префетч соседей.
    bool GetPage(uint32_t page_num, RawImageData& out_data);

private:
    // Постановка задачи на декодирование конкретной страницы в фоновый пул потоков
    void TriggerAsyncDecode(uint32_t page_num);
    
    // Логика удержания страниц (N-1, N, N+1) и высвобождения неиспользуемой памяти RAM
    void MaintainCache(uint32_t active_page, uint32_t total_pages);
};

// ==============================================================================
// 4. ЭКСПОРТ НИЗКОУРОВНЕВЫХ АССЕМБЛЕРНЫХ ФУНКЦИЙ (AMD64 / AVX2)
// ==============================================================================
extern "C" {
    // Высокоскоростная аппаратная перестановка байт BGR(A) -> RGBA пакетами по 8 пикселей
    void amd64_fast_bgr_to_rgba(uint8_t* data, size_t num_pixels, const uint8_t* mask);
    
    // Попиксельное альфа-смешивание слоев (FG поверх BG) на базе алгоритмов из Notcurses
    void amd64_simd_alpha_blend(uint8_t* bg_data, const uint8_t* fg_data, size_t num_pixels);
}
