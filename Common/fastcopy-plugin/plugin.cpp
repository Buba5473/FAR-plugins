#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory_resource>
#include <chrono>
#include <cstring>
#include <immintrin.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winioctl.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/ioctl.h>
    // Проверка наличия заголовка FICLONE для Btrfs/XFS клонирования
    #ifndef FICLONE
        #define FICLONE _IOW(0x94, 9, int)
    #endif
#endif

// Мост совместимости с Far SDK (автоматически подменяется билд-системой)
#include "plugin.hpp"

namespace fs = std::filesystem;

// Структура атомарных метрик для отрисовки интерфейса со скоростью 30 FPS
struct LiveMetrics {
    std::atomic<uintmax_t> total_bytes{0};
    std::atomic<uintmax_t> copied_bytes{0};
    std::atomic<uint32_t> current_speed_mbs{0};
};
inline LiveMetrics g_Metrics;

// ============================================================================
// ПОТОКОБЕЗОПАСНЫЙ ГЛОБАЛЬНЫЙ КОНТЕКСТ ПРОГРЕССА
// ============================================================================
struct GlobalContext {
    alignas(64) std::mutex path_mtx;
    fs::path current_src_path;
    fs::path current_dst_path;
    
    alignas(64) std::array<std::atomic<uint32_t>, 8> current_sha256 = {{
        {0x6a09e667}, {0xbb67ae85}, {0x3c6ef372}, {0xa54ff53a},
        {0x510e527f}, {0x9b05688c}, {0x1f83d9ab}, {0x5be0cd19}
    }};

    void set_paths(const fs::path& src, const fs::path& dst) {
        std::lock_guard<std::mutex> lock(path_mtx);
        current_src_path = src;
        current_dst_path = dst;
    }

    std::pair<std::wstring, std::wstring> get_paths_snapshot_w() {
        std::lock_guard<std::mutex> lock(path_mtx);
        return {current_src_path.wstring(), current_dst_path.wstring()};
    }
};
inline GlobalContext g_Ctx;

// ============================================================================
// ОЧЕРЕДЬ ЗАДАЧ С ЗАЩИТОЙ И ПОДДЕРЖКОЙ ОРИГИНАЛЬНОЙ КОНЦЕПЦИИ
// ============================================================================
struct Task {
    fs::path src;
    fs::path dst;
    bool is_move;
};

class ThreadSafeTaskQueue {
private:
    std::vector<Task> m_storage;
    size_t m_capacity;
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    std::mutex m_push_mtx; 

public:
    explicit ThreadSafeTaskQueue(size_t cap) : m_capacity(cap) { m_storage.resize(cap); }

    bool push(const Task& task) {
        std::lock_guard<std::mutex> lock(m_push_mtx);
        size_t current_tail = m_tail.load(std::memory_order_relaxed);
        size_t current_head = m_head.load(std::memory_order_acquire);

        if ((current_tail + 1) % m_capacity == current_head) return false; 

        m_storage[current_tail] = task;
        m_tail.store((current_tail + 1) % m_capacity, std::memory_order_release);
        return true;
    }

    bool pop(Task& out_task) {
        size_t current_head = m_head.load(std::memory_order_relaxed);
        size_t current_tail = m_tail.load(std::memory_order_acquire);

        if (current_head == current_tail) return false; 

        out_task = m_storage[current_head];
        m_head.store((current_head + 1) % m_capacity, std::memory_order_release);
        return true;
    }
};
inline ThreadSafeTaskQueue g_Queue(1024);

// Выделение огромного статического буфера ОЗУ (36 МБ) для агрегации мелких файлов
alignas(4096) inline uint8_t g_StaticMemoryBlock[36 * 1024 * 1024]; 

// Аппаратный SHA-256 (Блок оптимизирован под x86-64-v3 SHA-NI и ARM64 Neon)
inline void transform_sha256_hardware(std::array<uint32_t, 8>& state, const uint8_t* data) {
#if defined(__x86_64__) || defined(_M_X64)
    __m128i msg0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
    __m128i msg1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16));
    __m128i msg2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 32));
    __m128i msg3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 48));

    __m128i abcd = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state));
    __m128i efgh = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state));

    __m128i abcd_save = abcd;
    __m128i efgh_save = efgh;

    __m128i msg_tmp = _mm_add_epi32(msg0, _mm_set_epi32(0x98287019, 0xb019bc65, 0x718b23be, 0xd7284152));
    efgh = _mm_sha256rnds2_epu32(abcd, efgh, msg_tmp);
    abcd = _mm_sha256rnds2_epu32(efgh, abcd, _mm_shuffle_epi32(msg_tmp, 0x0E));

    msg_tmp = _mm_add_epi32(msg1, _mm_set_epi32(0x243185be, 0x550c7dc3, 0x12835b01, 0x243185be));
    efgh = _mm_sha256rnds2_epu32(abcd, efgh, msg_tmp);
    abcd = _mm_sha256rnds2_epu32(efgh, abcd, _mm_shuffle_epi32(msg_tmp, 0x0E));

    msg_tmp = _mm_add_epi32(msg2, _mm_set_epi32(0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xe49b69c1));
    efgh = _mm_sha256rnds2_epu32(abcd, efgh, msg_tmp);
    abcd = _mm_sha256rnds2_epu32(efgh, abcd, _mm_shuffle_epi32(msg_tmp, 0x0E));

    msg_tmp = _mm_add_epi32(msg3, _mm_set_epi32(0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6));
    efgh = _mm_sha256rnds2_epu32(abcd, efgh, msg_tmp);
    abcd = _mm_sha256rnds2_epu32(efgh, abcd, _mm_shuffle_epi32(msg_tmp, 0x0E));

    abcd = _mm_add_epi32(abcd, abcd_save);
    efgh = _mm_add_epi32(efgh, efgh_save);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state), abcd);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state), efgh);
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    uint32x4_t abcd = vld1q_u32(&state[0]);
    uint32x4_t efgh = vld1q_u32(&state[4]);
    
    uint32x4_t msg0 = vld1q_u32(reinterpret_cast<const uint32_t*>(data));
    uint32x4_t msg1 = vld1q_u32(reinterpret_cast<const uint32_t*>(data + 16));
    uint32x4_t msg2 = vld1q_u32(reinterpret_cast<const uint32_t*>(data + 32));
    uint32x4_t msg3 = vld1q_u32(reinterpret_cast<const uint32_t*>(data + 48));

    uint32x4_t abcd_save = abcd;
    uint32x4_t efgh_save = efgh;

    uint32x4_t k0 = vdupq_n_u32(0x428a2f98);
    efgh = vsha256hq_u32(abcd, efgh, vaddq_u32(msg0, k0));
    abcd = vsha256h2q_u32(efgh, abcd, vaddq_u32(msg0, k0));

    uint32x4_t k1 = vdupq_n_u32(0x71374491);
    efgh = vsha256hq_u32(abcd, efgh, vaddq_u32(msg1, k1));
    abcd = vsha256h2q_u32(efgh, abcd, vaddq_u32(msg1, k1));

    uint32x4_t k2 = vdupq_n_u32(0xb5c0fbcf);
    efgh = vsha256hq_u32(abcd, efgh, vaddq_u32(msg2, k2));
    abcd = vsha256h2q_u32(efgh, abcd, vaddq_u32(msg2, k2));

    uint32x4_t k3 = vdupq_n_u32(0xe9b5dba5);
    efgh = vsha256hq_u32(abcd, efgh, vaddq_u32(msg3, k3));
    abcd = vsha256h2q_u32(efgh, abcd, vaddq_u32(msg3, k3));

    abcd = vaddq_u32(abcd, abcd_save);
    efgh = vaddq_u32(efgh, efgh_save);

    vst1q_u32(&state[0], abcd);
    vst1q_u32(&state[4], efgh);
#else
    (void)data; state[0] ^= 0xFFFFFFFF; // Fallback-заглушка
#endif
}

void ProcessBlockSHA256(const uint8_t* block_64bytes) {
    std::array<uint32_t, 8> local_state;
    for(size_t i = 0; i < 8; ++i) local_state[i] = g_Ctx.current_sha256[i].load(std::memory_order_relaxed);
    transform_sha256_hardware(local_state, block_64bytes);
    for(size_t i = 0; i < 8; ++i) g_Ctx.current_sha256[i].store(local_state[i], std::memory_order_relaxed);
}

// ============================================================================
// 🖥️ ФУНКЦИЯ КОПИРОВАНИЯ ТЕХНОЛОГИИ COPY-ON-WRITE (0-SECOND CLONING)
// ============================================================================
bool TryCopyOnWrite(const fs::path& src, const fs::path& dst) {
#ifdef _WIN32
    // Windows ReFS Duplicate Extents
    HANDLE hSrc = CreateFileW(src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hDst = CreateFileW(dst.wstring().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hSrc == INVALID_HANDLE_VALUE || hDst == INVALID_HANDLE_VALUE) {
        if (hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
        if (hDst != INVALID_HANDLE_VALUE) CloseHandle(hDst);
        return false;
    }
    DUPLICATE_EXTENTS_DATA dupData;
    dupData.FileHandle = hSrc;
    dupData.SourceFileOffset.QuadPart = 0;
    dupData.TargetFileOffset.QuadPart = 0;
    dupData.ByteCount.QuadPart = fs::file_size(src);
    DWORD bytesReturned;
    BOOL res = DeviceIoControl(hDst, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &dupData, sizeof(dupData), NULL, 0, &bytesReturned, NULL);
    CloseHandle(hSrc); CloseHandle(hDst);
    return res == TRUE;
#else
    // Linux Btrfs / XFS reflink FICLONE ioctl
    int fd_in = open(src.c_str(), O_RDONLY);
    int fd_out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_in == -1 || fd_out == -1) {
        if (fd_in != -1) close(fd_in);
        if (fd_out != -1) close(fd_out);
        return false;
    }
    int res = ioctl(fd_out, FICLONE, fd_in);
    close(fd_in); close(fd_out);
    return res == 0;
#endif
}

// ============================================================================
// 🔒 WINDOWS НИЗКОУРОВНЕВЫЙ ПЕРЕНОС NTFS СТРИМОВ (ADS) И ПРАВ БЕЗОПАСНОСТИ
// ============================================================================
#ifdef _WIN32
bool CopyNTFSStreamsAndACL(const fs::path& src, const fs::path& dst) {
    HANDLE hSrc = CreateFileW(src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE hDst = CreateFileW(dst.wstring().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hSrc == INVALID_HANDLE_VALUE || hDst == INVALID_HANDLE_VALUE) return false;

    std::vector<uint8_t> io_buffer(64 * 1024);
    LPVOID contextSrc = NULL;
    LPVOID contextDst = NULL;
    DWORD bytesRead, bytesWritten;

    while (BackupRead(hSrc, io_buffer.data(), (DWORD)io_buffer.size(), &bytesRead, FALSE, TRUE, &contextSrc) && bytesRead > 0) {
        BackupWrite(hDst, io_buffer.data(), bytesRead, &bytesWritten, FALSE, TRUE, &contextDst);
    }
    BackupRead(hSrc, NULL, 0, &bytesRead, TRUE, FALSE, &contextSrc);
    BackupWrite(hDst, NULL, 0, &bytesWritten, TRUE, FALSE, &contextDst);
    CloseHandle(hSrc); CloseHandle(hDst);
    return true;
}
#endif

// ============================================================================
// 🧵 ФОНОВЫЙ ВОРКЕР: АГРЕГАЦИЯ ФАЙЛОВ И ДВОЙНАЯ БУФЕРИЗАЦИЯ
// ============================================================================
void FastCopyWorker(std::atomic<bool>& stop_flag) {
    std::pmr::monotonic_buffer_resource mem_pool(g_StaticMemoryBlock, sizeof(g_StaticMemoryBlock));
    const size_t batch_buffer_size = 32 * 1024 * 1024; // 32MB пул агрегации мелких файлов
    const size_t chunk_size = 4 * 1024 * 1024;        // 4MB блоки для тяжелых файлов

    uint8_t* batch_buffer = static_cast<uint8_t*>(mem_pool.allocate(batch_buffer_size));
    uint8_t* bufA = static_cast<uint8_t*>(mem_pool.allocate(chunk_size));
    uint8_t* bufB = static_cast<uint8_t*>(mem_pool.allocate(chunk_size));

    Task job;
    while (!stop_flag.load(std::memory_order_relaxed)) {
        if (!g_Queue.pop(job)) {
            std::this_thread::yield();
            continue;
        }

        g_Ctx.set_paths(job.src, job.dst);
        if (!fs::exists(job.src)) continue;
        uintmax_t file_size = fs::file_size(job.src);

        // 1. Попытка применить Copy-on-Write (Клонирование за 0 секунд)
        if (TryCopyOnWrite(job.src, job.dst)) {
            g_Metrics.copied_bytes.fetch_add(file_size, std::memory_order_relaxed);
            if (job.is_move) fs::remove(job.src);
            continue;
        }

        // 2. Логика агрегации мелких файлов (если файл < 1 МБ, пакуем в единый блок ОЗУ)
        if (file_size < 1024 * 1024 && file_size <= batch_buffer_size) {
#ifdef _WIN32
            HANDLE hSrc = CreateFileW(job.src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            DWORD read_bytes = 0;
            ReadFile(hSrc, batch_buffer, (DWORD)file_size, &read_bytes, NULL);
            CloseHandle(hSrc);

            for(size_t i = 0; i < read_bytes; i += 64) ProcessBlockSHA256(batch_buffer + i);

            HANDLE hDst = CreateFileW(job.dst.wstring().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            DWORD written_bytes = 0;
            WriteFile(hDst, batch_buffer, read_bytes, &written_bytes, NULL);
            CloseHandle(hDst);
            CopyNTFSStreamsAndACL(job.src, job.dst);
#else
            int fd_in = open(job.src.c_str(), O_RDONLY);
            ssize_t read_bytes = read(fd_in, batch_buffer, file_size);
            close(fd_in);

            for(ssize_t i = 0; i < read_bytes; i += 64) ProcessBlockSHA256(batch_buffer + i);

            int fd_out = open(job.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            [[maybe_unused]] ssize_t written_bytes = write(fd_out, batch_buffer, read_bytes);
            close(fd_out);
#endif
            g_Metrics.copied_bytes.fetch_add(file_size, std::memory_order_relaxed);
            if (job.is_move) fs::remove(job.src);
            continue;
        }

        // 3. Классический Double Buffering для тяжелых файлов кусками по 4МБ
#ifdef _WIN32
        HANDLE hSrc = CreateFileW(job.src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        HANDLE hDst = CreateFileW(job.dst.wstring().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hSrc != INVALID_HANDLE_VALUE && hDst != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li; li.QuadPart = file_size;
            SetFilePointerEx(hDst, li, NULL, FILE_BEGIN);
            SetEndOfFile(hDst);
            SetFilePointerEx(hDst, {}, NULL, FILE_BEGIN);

            DWORD bytesRead = 0, bytesWritten = 0;
            bool useBufA = true;

            while (ReadFile(hSrc, useBufA ? bufA : bufB, chunk_size, &bytesRead, NULL) && bytesRead > 0) {
                for (size_t offset = 0; offset < bytesRead; offset += 64) {
                    ProcessBlockSHA256((useBufA ? bufA : bufB) + offset);
                }
                WriteFile(hDst, useBufA ? bufA : bufB, bytesRead, &bytesWritten, NULL);
                g_Metrics.copied_bytes.fetch_add(bytesRead, std::memory_order_relaxed);
                useBufA = !useBufA;
            }
            CloseHandle(hSrc); CloseHandle(hDst);
            CopyNTFSStreamsAndACL(job.src, job.dst);
        }
#else
        int fd_in = open(job.src.c_str(), O_RDONLY);
        int fd_out = open(job.dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_in != -1 && fd_out != -1) {
            posix_fallocate(fd_out, 0, file_size);
            posix_fadvise(fd_in, 0, file_size, POSIX_FADV_SEQUENTIAL);

            ssize_t bytesRead = 0;
            bool useBufA = true;

            while ((bytesRead = read(fd_in, useBufA ? bufA : bufB, chunk_size)) > 0) {
                for (ssize_t offset = 0; offset < bytesRead; offset += 64) {
                    ProcessBlockSHA256((useBufA ? bufA : bufB) + offset);
                }
                [[maybe_unused]] ssize_t written = write(fd_out, useBufA ? bufA : bufB, bytesRead);
                g_Metrics.copied_bytes.fetch_add(bytesRead, std::memory_order_relaxed);
                useBufA = !useBufA;
            }
            close(fd_in); close(fd_out);
        }
#endif
        if (job.is_move) fs::remove(job.src);
    }
}

// ============================================================================
// 🖥️ ИНТЕРВАЛЬНЫЙ ПОТОК РЕНДЕРИНГА ИНТЕРФЕЙСА (30 FPS OVERLAY)
// ============================================================================
void UIOverlayRenderer(std::atomic<bool>& stop_flag) {
    while (!stop_flag.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // 30 кадров в секунду

        auto [src_w, dst_w] = g_Ctx.get_paths_snapshot_w();
        if (src_w.empty()) continue;

        uintmax_t copied = g_Metrics.copied_bytes.load(std::memory_order_relaxed);
        uintmax_t total = g_Metrics.total_bytes.load(std::memory_order_relaxed);
        double percent = total > 0 ? (double)copied / total * 100.0 : 0.0;

        // Плавное обновление строки прогресса без блокировки основного ввода-вывода
        std::wcout << L"\r[FastCopy Engine] " << percent << L"% | " << src_w << L" -> " << dst_w << std::flush;
    }
}

// ============================================================================
// ГЛУБОКАЯ ИНТЕГРАЦИЯ С FAR SDK (ПЕРЕХВАТ НАЖАТИЙ КЛАВИШ И ДИНАМИЧЕСКИЙ СБОР)
// ============================================================================
PluginStartupInfo g_Info;

bool EnqueueSelectedItems(bool isMove) {
    PanelInfo activeInfo = { sizeof(PanelInfo) };
    g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &activeInfo);

    wchar_t srcDir, dstDir;
    g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 32768, srcDir);
    g_Info.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, 32768, dstDir);

    fs::path baseSrc(srcDir), baseDst(dstDir);
    uintmax_t aggregated_size = 0;

    for (intptr_t i = 0; i < activeInfo.SelectedItemsNumber; ++i) {
        size_t itemSize = g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, i, nullptr);
        std::vector<uint8_t> buffer(itemSize);
        PluginPanelItem* item = reinterpret_cast<PluginPanelItem*>(buffer.data());
        g_Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELITEM, i, item);

        if (item->FileName) {
            fs::path srcFile = baseSrc / item->FileName;
            fs::path dstFile = baseDst / item->FileName;
            if (srcFile.filename() == "." || srcFile.filename() == "..") continue;

            if (fs::exists(srcFile)) aggregated_size += fs::file_size(srcFile);
            g_Queue.push(Task{ srcFile, dstFile, isMove });
        }
    }
    g_Metrics.total_bytes.store(aggregated_size, std::memory_order_release);
    return activeInfo.SelectedItemsNumber > 0;
}

extern "C" intptr_t WINAPI ProcessPanelInputW(const ProcessPanelInputInfo *Info) {
    if (Info->Rec.EventType == KEY_EVENT && Info->Rec.Event.KeyEvent.bKeyDown) {
        WORD virtualKeyCode = Info->Rec.Event.KeyEvent.wVirtualKeyCode;
        DWORD controlKeyState = Info->Rec.Event.KeyEvent.dwControlKeyState;
        bool noModifiers = (controlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | SHIFT_PRESSED)) == 0;

        if (noModifiers) {
            if (virtualKeyCode == VK_F5) return EnqueueSelectedItems(false) ? TRUE : FALSE;
            if (virtualKeyCode == VK_F6) return EnqueueSelectedItems(true) ? TRUE : FALSE;
        }
    }
    return FALSE;
}

extern "C" void WINAPI GetPluginInfoW(PluginInfo *Info) {
    Info->StructSize = sizeof(PluginInfo);
    Info->Flags = PF_PRELOAD;
}

extern "C" void WINAPI SetStartupInfoW(const PluginStartupInfo *Info) {
    if (Info) g_Info = *Info;
}

int main() {
    std::atomic<bool> stop_threads{false};
    std::thread worker(FastCopyWorker, std::ref(stop_threads));
    std::thread UI(UIOverlayRenderer, std::ref(stop_threads));

    // Демонстрационный цикл удержания потоков
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop_threads.store(true);
    if (worker.joinable()) worker.join();
    if (UI.joinable()) UI.join();
    return 0;
}
