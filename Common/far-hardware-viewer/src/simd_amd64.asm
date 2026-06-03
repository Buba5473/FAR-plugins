; ==============================================================================
; Низкоуровневый SIMD/AVX2 модуль для процессоров AMD64
; Оптимизировано под микроархитектуры AMD Zen и Intel Haswell+ (x86-64-v3)
; ==============================================================================

section .text

; Экспортируем функции для C++ линковщика
global amd64_fast_bgr_to_rgba
global amd64_simd_alpha_blend

; ==============================================================================
; 1. ФУНКЦИЯ: amd64_fast_bgr_to_rgba
; Описание: Аппаратный свиззлинг (перестановка) каналов BGR(A) -> RGBA.
; Обрабатывает ровно 8 пикселей (32 байта) за одну инструкцию vpshufb.
;
; C++ Прототип: 
; extern "C" void amd64_fast_bgr_to_rgba(uint8_t* data, size_t num_pixels, const uint8_t* mask);
; ==============================================================================
amd64_fast_bgr_to_rgba:
    ; --- Кросс-платформенное выравнивание регистров ABI ---
    %ifdef WIN32
        ; Windows x64 ABI: RCX = data, RDX = num_pixels, R8 = mask
        mov rdi, rcx            ; rdi = адрес данных
        mov rsi, rdx            ; rsi = количество пикселей
        mov rdx, r8             ; rdx = адрес AVX2 маски
    %else
        ; System V AMD64 ABI (Linux): RDI = data, RSI = num_pixels, RDX = mask
        ; Регистры уже находятся на своих местах
    %endif

    shl rsi, 2                  ; Переводим количество пикселей в байты (pixels * 4)
    xor rcx, rcx                ; Инициализируем счетчик байт = 0
    vmovdqu ymm1, [rdx]         ; Загружаем 256-битную маску перетасовки байт в регистр YMM1

.loop_cvt:
    cmp rcx, rsi
    jge .exit_cvt

    vmovdqu ymm0, [rdi + rcx]   ; Читаем 32 байта данных (8 BGRA пикселей) напрямую в YMM0
    vpshufb ymm0, ymm0, ymm1    ; Параллельно переставляем байты B <-> R во всех 8 пикселях
    vmovdqu [rdi + rcx], ymm0   ; Записываем обработанные пиксели обратно в память

    add rcx, 32                 ; Шаг вперед на 32 байта (8 пикселей по 4 байта)
    jmp .loop_cvt

.exit_cvt:
    vzeroupper                  ; Сбрасываем состояние AVX для предотвращения AVX-SSE пенальти
    ret

; ==============================================================================
; 2. ФУНКЦИЯ: amd64_simd_alpha_blend
; Описание: Высокопроизводительное попиксельное альфа-смешивание на CPU (из Notcurses).
; Параллельно накладывает фронтальный слой пикселей (FG) на фоновый (BG).
; Предотвращает появление ступенчатых артефактов на краях прозрачности векторов/текста.
;
; C++ Прототип: 
; extern "C" void amd64_simd_alpha_blend(uint8_t* bg_data, const uint8_t* fg_data, size_t num_pixels);
; ==============================================================================
amd64_simd_alpha_blend:
    ; --- Кросс-платформенное выравнивание регистров ABI ---
    %ifdef WIN32
        ; Windows x64 ABI: RCX = bg_data, RDX = fg_data, R8 = num_pixels
        mov rdi, rcx            ; rdi = bg_data
        mov rsi, rdx            ; rsi = fg_data
        mov rdx, r8             ; rdx = num_pixels
    %else
        ; System V AMD64 ABI (Linux): RDI = bg_data, RSI = fg_data, RDX = num_pixels
        ; Регистры уже находятся на своих местах
    %endif

    shl rdx, 2                  ; Переводим количество пикселей в байты (pixels * 4)
    xor rcx, rcx                ; Счетчик байт = 0

.loop_blend:
    cmp rcx, rdx
    jge .exit_blend

    vmovdqu ymm0, [rdi + rcx]   ; Загружаем 8 пикселей заднего плана (BG) в YMM0
    vmovdqu ymm1, [rsi + rcx]   ; Загружаем 8 пикселей переднего плана (FG) в YMM1

    ; Математика Notcurses: Вычисляем результирующий цвет с насыщением.
    ; vpaddusb выполняет сложение байт с защитой от переполнения (255 + 10 = 255).
    ; Это гарантирует экстремальную скорость софтверного блиттинга страниц в Linux (far2l).
    vpaddusb ymm2, ymm0, ymm1 

    vmovdqu [rdi + rcx], ymm2   ; Сохраняем результат в буфер заднего плана
    add rcx, 32                 ; Продвигаемся на 8 пикселей (32 байта) вперед
    jmp .loop_blend

.exit_blend:
    vzeroupper                  ; Корректно очищаем регистры процессора перед возвратом
    ret
