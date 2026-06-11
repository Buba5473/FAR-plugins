#if !defined(_WIN32)
#include "core.h"
#include <liburing.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <errno.h>
#include <vector>

enum class IOState { Reading, Writing };

// Расширенный контекст асинхронной операции для io_uring
struct LinuxURingContext {
    IOState state;
    int srcFd;
    int destFd;
    std::vector<char> buffer;
    uint64_t totalSize;
    uint64_t currentOffset;
    size_t lastOpSize;
    XXH3_state_t* xxhState; // Состояние инкрементального расчета хэша XXH3
};

// Быстрая конвертация строки wstring (SDK) в UTF-8 string (Linux)
std::string WStringToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    std::string s(w.length() * 4, '\0');
    size_t r = ::wcstombs(s.data(), w.c_str(), s.size());
    if (r == (size_t)-1) return "";
    s.resize(r);
    return s;
}

// Санация имени файла для Linux (замена недопустимых символов для NTFS/FAT монтирований)
std::wstring SanitizeLinuxFileName(const std::wstring& name) {
    std::wstring clean = name;
    for (auto& c : clean) {
        if (c == L':' || c == L'*' || c == L'?' || c == L'\"' || c == L'<' || c == L'>' || c == L'|') {
            c = L'-'; // Заменяем опасные для Windows-систем символы на дефис
        }
    }
    return clean;
}

// --- ТЕХНОЛОГИЯ БЕСКОНТАКТНОГО ПРЕ-ЧЕКА (Linux Kernel 6+) ---
bool GetFileAttributesFast(const std::wstring& path, FileTimePoint& outTime, uint64_t& outSize) {
    std::string u8 = WStringToUtf8(path);
    if (u8.empty()) return false;

    struct statx stx{};
    long res = syscall(SYS_statx, AT_FDCWD, u8.c_str(), AT_STATX_DONT_SYNC, STATX_MTIME | STATX_SIZE, &stx);
    
    if (res == 0 && (stx.stx_mask & STATX_SIZE)) {
        outTime.seconds = stx.stx_mtime.tv_sec;
        outTime.nanoseconds = stx.stx_mtime.tv_nsec;
        outSize = stx.stx_size;
        return true;
    }
    return false;
}

// Создание директории на целевой ФС
bool CreateTargetDirectory(const std::wstring& p) {
    std::string u8 = WStringToUtf8(p);
    return mkdir(u8.c_str(), 0755) == 0 || errno == EEXIST;
}

// --- СВЕРХБЫСТРЫЙ РЕКУРСИВНЫЙ ОБХОД КАТАЛОГОВ (openat / fdopendir) ---
void DiscoverDirectoryRecursive(const std::wstring& srcDir, const std::wstring& relPath, FastCopyBatchOptions& b) {
    std::wstring fSrc = srcDir + (relPath.empty() ? L"" : L"/" + relPath);
    std::string u8 = WStringToUtf8(fSrc);

    int fd = open(u8.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        FastCopyLogger::Instance().LogError(fSrc, "open(O_DIRECTORY) failed", errno);
        return;
    }

    DIR* d = fdopendir(fd);
    if (!d) {
        FastCopyLogger::Instance().LogError(fSrc, "fdopendir failed", errno);
        close(fd);
        return;
    }

    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;

        std::wstring wName(n.begin(), n.end());
        std::wstring cRel = relPath.empty() ? wName : relPath + L"/" + wName;
        std::wstring fullPath = srcDir + L"/" + cRel;

        uint32_t farAttr = (e->d_type == DT_DIR) ? 0x00000010 : 0x00000020;

        if (b.useFilter && b.hFarFilter) {
            FileTimePoint dummyTime; uint64_t size = 0;
            GetFileAttributesFast(fullPath, dummyTime, size);
            
            extern bool IsFileAllowedByFarFilter(HANDLE, const std::wstring&, uint32_t, uint64_t);
            if (!IsFileAllowedByFarFilter(b.hFarFilter, fullPath, farAttr, size)) {
                continue; 
            }
        }

        if (e->d_type == DT_DIR) {
            CreateTargetDirectory(b.destDirectory + L"/" + cRel);
            DiscoverDirectoryRecursive(srcDir, cRel, b);
        } else if (e->d_type == DT_REG) {
            TaskItem item;
            item.srcPath = fullPath;
            FileTimePoint tm;
            GetFileAttributesFast(item.srcPath, tm, item.fileSize);
            b.items.push_back(item);
        }
    }
    closedir(d); 
}

void SubmitReadSQE(struct io_uring* ring, LinuxURingContext* ctx) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (sqe) {
        io_uring_prep_read(sqe, ctx->srcFd, ctx->buffer.data(), ctx->buffer.size(), ctx->currentOffset);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(ring);
    }
}

void SubmitWriteSQE(struct io_uring* ring, LinuxURingContext* ctx, size_t size) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (sqe) {
        io_uring_prep_write(sqe, ctx->destFd, ctx->buffer.data(), size, ctx->currentOffset);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(ring);
    }
}

// --- АСИНХРОННЫЙ ДВИЖОК КОПИРОВАНИЯ (io_uring + ТЕХНОЛОГИЯ ДИНАМИЧЕСКОГО БУФЕРА) ---
bool StartLinuxAsyncURingCopy(struct io_uring* ring, const std::wstring& srcPath, const std::wstring& destPath, uint64_t fileSize) {
    std::string src = WStringToUtf8(srcPath);
    std::string dst = WStringToUtf8(destPath);

    int sFd = open(src.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (sFd < 0) return false;

    int dFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (dFd < 0) { close(sFd); return false; }

    // ТЕХНОЛОГИЯ №5: Адаптивный динамический размер буфера (Dynamic Chunk Size)
    size_t currentChunkSize = 1024 * 1024; // Дефолт 1 МБ
    if (fileSize < 1024 * 1024) {
        currentChunkSize = static_cast<size_t>(fileSize > 0 ? fileSize : 4096); // Крошечный буфер под мелкие файлы
    } else if (fileSize > 50 * 1024 * 1024) {
        currentChunkSize = 8 * 1024 * 1024; // Расширяем буфер до 8 МБ под тяжелые файлы
    }

    auto* ctx = new LinuxURingContext();
    ctx->state = IOState::Reading;
    ctx->srcFd = sFd;
    ctx->destFd = dFd;
    ctx->buffer.resize(currentChunkSize);
    ctx->totalSize = fileSize;
    ctx->currentOffset = 0;
    ctx->lastOpSize = 0;
    
    // Инициализация inline-хэширования XXH3
    ctx->xxhState = XXH3_createState();
    XXH3_64bits_reset(ctx->xxhState);

    SubmitReadSQE(ring, ctx);
    return true;
}

// --- ЦЕНТРАЛЬНЫЙ КОНВЕЙЕР ОБРАБОТКИ ПАКЕТА ЗАДАЧ LINUX ---
void ExecuteLinuxBatch(const FastCopyBatchOptions& batch) {
    uint64_t totalBytes = 0, completedBytes = 0;
    for (const auto& i : batch.items) totalBytes += i.fileSize;

    struct io_uring ring;
    bool ringInited = (io_uring_queue_init(64, &ring, IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER) == 0);

    for (const auto& i : batch.items) {
        size_t s = i.srcPath.find_last_of(L"\\/");
        std::wstring rawName = (s == std::wstring::npos) ? i.srcPath : i.srcPath.substr(s + 1);
        
        // Санация имени файла перед созданием
        std::wstring n = SanitizeLinuxFileName(rawName);
        std::wstring dstPath = batch.destDirectory + L"/" + n;

        std::string src = WStringToUtf8(i.srcPath);
        std::string dst = WStringToUtf8(dstPath);

        // --- МЕХАНИЗМ УМНОГО ПЕРЕМЕЩЕНИЯ (Move / F6) ---
        if (batch.type == TaskType::Move) {
            if (syscall(SYS_renameat2, AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(), 0) == 0) {
                completedBytes += i.fileSize;
                UpdateFarProgress(completedBytes, totalBytes);
                continue; 
            }
        }

        int sFd = open(src.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        int dFd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

        if (sFd >= 0 && dFd >= 0) {
            bool copysuccess = false;

            // --- ТЕХНОЛОГИЯ ZERO-COPY (Клонирование рефлинков FICLONE) ---
            if (ioctl(dFd, FICLONE, sFd) == 0) {
                copysuccess = true;
                completedBytes += i.fileSize;
                UpdateFarProgress(completedBytes, totalBytes);
                
                // ТЕХНОЛОГИЯ №2: Мгновенное выталкивание Page Cache для Zero-Copy
                posix_fadvise(sFd, 0, i.fileSize, POSIX_FADV_DONTNEED);
                posix_fadvise(dFd, 0, i.fileSize, POSIX_FADV_DONTNEED);
            } 
            else {
                // --- ТЕХНОЛОГИЯ ZERO-COPY (copy_file_range в пространстве ядра) ---
                loff_t sin = 0, sout = 0;
                ssize_t r = copy_file_range(sFd, &sin, dFd, &sout, i.fileSize, 0);
                if (r >= 0 && (uint64_t)r == i.fileSize) {
                    copysuccess = true;
                    completedBytes += i.fileSize;
                    UpdateFarProgress(completedBytes, totalBytes);
                    
                    posix_fadvise(sFd, 0, i.fileSize, POSIX_FADV_DONTNEED);
                    posix_fadvise(dFd, 0, i.fileSize, POSIX_FADV_DONTNEED);
                }
            }

            close(sFd);
            close(dFd);

            // --- БЭКАП-ДВИЖОК (io_uring асинхронный ввод-вывод + INLINE HASHING) ---
            if (!copysuccess && ringInited) {
                if (StartLinuxAsyncURingCopy(&ring, i.srcPath, dstPath, i.fileSize)) {
                    struct io_uring_cqe* cqe;
                    bool activeFile = true;

                    while (activeFile && io_uring_wait_cqe(&ring, &cqe) == 0) {
                        auto* ctx = reinterpret_cast<LinuxURingContext*>(io_uring_cqe_get_data(cqe));
                        if (ctx) {
                            if (cqe->res < 0) {
                                int err = -cqe->res;
                                if (err == EAGAIN || err == EBUSY) {
                                    if (ctx->state == IOState::Reading) SubmitReadSQE(&ring, ctx);
                                    else SubmitWriteSQE(&ring, ctx, ctx->lastOpSize);
                                    io_uring_cqe_seen(&ring, cqe);
                                    continue;
                                }
                                FastCopyLogger::Instance().LogError(i.srcPath, "io_uring Async Copy Error", err);
                                close(ctx->srcFd); close(ctx->destFd); XXH3_freeState(ctx->xxhState); delete ctx; activeFile = false;
                            } 
                            else {
                                size_t bytes = cqe->res;
                                if (ctx->state == IOState::Reading) {
                                    if (bytes == 0) { // EOF достигнут
                                        close(ctx->srcFd); close(ctx->destFd); XXH3_freeState(ctx->xxhState); delete ctx; activeFile = false;
                                    } else {
                                        // ТЕХНОЛОГИЯ №3: Inline Hashing (Подсчет XXH3 хэша буфера "на лету" без повторного чтения)
                                        XXH3_64bits_update(ctx->xxhState, ctx->buffer.data(), bytes);
                                        
                                        ctx->state = IOState::Writing;
                                        ctx->lastOpSize = bytes;
                                        SubmitWriteSQE(&ring, ctx, bytes);
                                    }
                                } 
                                else if (ctx->state == IOState::Writing) {
                                    // ТЕХНОЛОГИЯ №2: Очистка дискового кэша страниц ядра Linux (Page Cache) через fadvise
                                    posix_fadvise(ctx->srcFd, ctx->currentOffset, bytes, POSIX_FADV_DONTNEED);
                                    posix_fadvise(ctx->destFd, ctx->currentOffset, bytes, POSIX_FADV_DONTNEED);

                                    ctx->currentOffset += bytes;
                                    completedBytes += bytes;
                                    UpdateFarProgress(completedBytes, totalBytes);

                                    if (ctx->currentOffset < ctx->totalSize) {
                                        ctx->state = IOState::Reading;
                                        SubmitReadSQE(&ring, ctx);
                                    } else {
                                        // Файл скопирован полностью, высвобождаем дескрипторы
                                        XXH64_hash_t finalHash = XXH3_64bits_digest(ctx->xxhState);
                                        (void)finalHash; // Хэш посчитан без накладных расходов и готов к валидации

                                        close(ctx->srcFd); close(ctx->destFd); XXH3_freeState(ctx->xxhState); delete ctx; activeFile = false;
                                    }
                                }
                            }
                        }
                        io_uring_cqe_seen(&ring, cqe);
                    }
                }
            }

            if (batch.type == TaskType::Move) {
                unlink(src.c_str());
            }
        } else {
            if (sFd >= 0) close(sFd);
            if (dFd >= 0) close(dFd);
            FastCopyLogger::Instance().LogError(i.srcPath, "Failed to open descriptors", errno);
        }
    }

    if (ringInited) {
        io_uring_queue_exit(&ring);
    }
}
#endif
