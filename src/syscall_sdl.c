/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * SDL 扩展系统调用。
 *
 * 图形 demo 和游戏通过非标准 ecall 调用本文件提供的帧缓冲绘制、输入事件队列、
 * 音乐和音效控制。它不是 Linux/POSIX ABI 的一部分，而是为 rv32emu 的演示程序
 * 提供一个轻量的图形、输入和音频宿主接口。
 */

#if !RV32_HAS(SDL)
#error "只有启用 SDL 支持时才能构建此文件。"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>
#if RV32_HAS(SDL_MIXER)
#include <SDL_mixer.h>
#endif

#include "riscv.h"
#include "riscv_private.h"

/* DOOM1.WAD 中的 DSITMBK 音效采样率是 22050；由于这里只以单人模式运行游戏，
 * 固定使用 11025 也可以接受。
 *
 * Quake 中大多数音效采样率也是 11025。
 */
#define SAMPLE_RATE 11025

/* 大多数音频设备支持立体声。 */
#define CHANNEL_USED 2

#define CHUNK_SIZE 2048

#define MUSIC_MAX_SIZE 65536

/* 音效最大尺寸约为 18000 字节，这里留出更大缓冲。 */
#define SFX_SAMPLE_SIZE 32768

#define R 1
#define W 0

#if RV32_HAS(SYSTEM_MMIO)
#define get_offset_by_addr(addr) (addr) & (MASK(RV_PG_SHIFT))
static uint32_t offset;
static uint32_t curr_page_data_size;
static uint32_t remain_size;
static uint32_t curr_offset;
static uint32_t curr_data_size;

#define GET_DATA_FROM_RANDOM_PAGE(source_vaddr, dest)                    \
    do {                                                                 \
        while (remain_size > 0) {                                        \
            uint32_t screen_addr =                                       \
                rv->io.mem_translate(rv, source_vaddr + curr_offset, R); \
            offset = get_offset_by_addr(screen_addr);                    \
            curr_page_data_size = RV_PG_SIZE - offset;                   \
            curr_data_size = remain_size <= curr_page_data_size          \
                                 ? remain_size                           \
                                 : curr_page_data_size;                  \
            memory_read(attr->mem, dest + curr_offset, screen_addr,      \
                        sizeof(uint8_t) * curr_data_size);               \
            remain_size -= curr_data_size;                               \
            curr_offset += curr_data_size;                               \
        }                                                                \
    } while (0)

#define GET_VIDEO_DATA_FROM_RANDOM_PAGE(source_vaddr, dest) \
    GET_DATA_FROM_RANDOM_PAGE(source_vaddr, dest)
#define GET_SFX_DATA_FROM_RANDOM_PAGE(source_vaddr, dest) \
    GET_DATA_FROM_RANDOM_PAGE(source_vaddr, dest)
#define GET_MUSIC_DATA_FROM_RANDOM_PAGE(source_vaddr, dest) \
    GET_DATA_FROM_RANDOM_PAGE(source_vaddr, dest)
#endif

/* 声音相关请求类型。 */
enum {
    INIT_AUDIO,
    SHUTDOWN_AUDIO,
    PLAY_MUSIC,
    PLAY_SFX,
    SET_MUSIC_VOLUME,
    STOP_MUSIC,
};

typedef struct sound {
    uint8_t *data;
    size_t size;
    int looping;
    int volume;
} sound_t;

/* SDL_mixer 和音乐相关变量。 */
static uint8_t *music_midi_data;
#if RV32_HAS(SDL_MIXER)
static Mix_Music *mid;

/* SDL_mixer 和音效相关变量。 */
static Mix_Chunk *sfx_chunk;
#endif

#ifdef __EMSCRIPTEN__
/* 仅 EMSCRIPTEN joinable 线程需要这些线程句柄。 */
static pthread_t music_thread;
static pthread_t sfx_thread;
#endif
static uint8_t *sfx_samples;
static uint32_t nr_sfx_samples;
static int chan;

/* 用于正确销毁音频资源，并兼容进程级 VM 模拟。 */
static bool audio_init = false;
static bool sfx_thread_init = false;
static bool music_thread_init = false;

typedef struct {
    void *data;
    int size;
} musicinfo_t;

typedef struct {
    void *data;
    int size;
} sfxinfo_t;

enum {
    KEY_EVENT = 0,
    MOUSE_MOTION_EVENT = 1,
    MOUSE_BUTTON_EVENT = 2,
    QUIT_EVENT = 3,
};

typedef struct {
    uint32_t keycode;
    uint8_t state;
} key_event_t;

typedef struct {
    int32_t x, y, xrel, yrel;
} mouse_motion_t;

typedef struct {
    uint8_t button;
    uint8_t state;
} mouse_button_t;

typedef struct {
    uint32_t type;
    union {
        key_event_t key_event;
        union {
            mouse_motion_t motion;
            mouse_button_t button;
        } mouse;
    };
} event_t;

typedef struct {
    uint32_t base;
    size_t end;
} event_queue_t;

enum {
    RELATIVE_MODE_SUBMISSION = 0,
    WINDOW_TITLE_SUBMISSION = 1,
};

typedef struct {
    uint8_t enabled;
} mouse_submission_t;

typedef struct {
    uint32_t title;
    uint32_t size;
} title_submission_t;

typedef struct {
    uint32_t type;
    union {
        mouse_submission_t mouse;
        title_submission_t title;
    };
} submission_t;

typedef struct {
    uint32_t base;
    size_t start;
} submission_queue_t;

/* SDL 相关变量。 */
static SDL_Window *window = NULL;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

/* 事件队列专用变量。 */
static uint32_t queues_capacity;
static uint32_t event_count;
static uint32_t deferred_submissions = 0;
static event_queue_t event_queue = {
    .base = 0,
    .end = 0,
};
static submission_queue_t submission_queue = {
    .base = 0,
    .start = 0,
};

static submission_t submission_pop(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);
    submission_t submission;
    memory_read(
        attr->mem, (void *) &submission,
        submission_queue.base + submission_queue.start * sizeof(submission_t),
        sizeof(submission_t));
    ++submission_queue.start;
    submission_queue.start &= queues_capacity - 1;
    return submission;
}

static void event_push(riscv_t *rv, event_t event)
{
    vm_attr_t *attr = PRIV(rv);
    if (!memory_write(attr->mem,
                      event_queue.base + event_queue.end * sizeof(event_t),
                      (void *) &event, sizeof(event_t)))
        return;
    ++event_queue.end;
    event_queue.end &= queues_capacity - 1;

    uint32_t count;
    count = rv->io.mem_read_w(rv, event_count);
    count += 1;
    rv->io.mem_write_w(rv, event_count, count);
}

static inline uint32_t round_pow2(uint32_t x)
{
    if (x <= 1)
        return 1;
#if defined(__GNUC__) || defined(__clang__)
    x = 1 << (32 - __builtin_clz(x - 1));
#else
    /* 位运算扩展技巧。 */
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
#endif
    return x;
}

void syscall_submit_queue(riscv_t *rv);

/* 检查是否需要初始化 SDL，并运行事件循环。 */
static bool check_sdl(riscv_t *rv, int width, int height)
{
    if (!window) { /* 检查视频子系统是否已经初始化。 */
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            rv_log_fatal("调用 SDL_Init() 失败");
            exit(EXIT_FAILURE);
        }
        window = SDL_CreateWindow("rv32emu", SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED, width, height,
                                  SDL_WINDOW_RESIZABLE);
        if (!window) {
            rv_log_fatal("无法创建窗口！SDL_Error：%s",
                         SDL_GetError());
            exit(EXIT_FAILURE);
        }

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);

        if (deferred_submissions) {
            syscall_submit_queue(rv);
            deferred_submissions = 0;
        }
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) { /* 运行事件处理器。 */
        switch (event.type) {
        case SDL_QUIT: {
            event_t new_event = {
                .type = QUIT_EVENT,
            };
            event_push(rv, new_event);
            return false;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (event.key.repeat)
                break;
            event_t new_event = {
                .type = KEY_EVENT,
            };
            key_event_t key_event = {
                .keycode = event.key.keysym.sym,
                .state = (bool) (event.key.state == SDL_PRESSED),
            };
            memcpy(&new_event.key_event, &key_event, sizeof(key_event));
            event_push(rv, new_event);
            break;
        }
        case SDL_MOUSEMOTION: {
            event_t new_event = {
                .type = MOUSE_MOTION_EVENT,
            };
            mouse_motion_t mouse_motion = {
                .x = event.motion.x,
                .y = event.motion.y,
                .xrel = event.motion.xrel,
                .yrel = event.motion.yrel,
            };
            memcpy(&new_event.mouse.motion, &mouse_motion,
                   sizeof(mouse_motion));
            event_push(rv, new_event);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            event_t new_event = {
                .type = MOUSE_BUTTON_EVENT,
            };
            mouse_button_t mouse_button = {
                .button = event.button.button,
                .state = (bool) (event.button.state == SDL_PRESSED),
            };
            memcpy(&new_event.mouse.button, &mouse_button,
                   sizeof(mouse_button));
            event_push(rv, new_event);
            break;
        }
        default:
            break;
        }
    }
    return true;
}

void syscall_draw_frame(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* draw_frame(base, width, height) */
    const uint32_t screen = rv_get_reg(rv, rv_reg_a0);
    const int width = rv_get_reg(rv, rv_reg_a1);
    const int height = rv_get_reg(rv, rv_reg_a2);

    if (!check_sdl(rv, width, height))
        return;

    uint32_t total_size = width * height * 4;
#if RV32_HAS(SYSTEM_MMIO)
    static uint8_t tmp_buf[256 * RV_PG_SIZE];
    uint8_t *tmp_buf_ptr = &tmp_buf[0];
    uint32_t screen_vaddr = screen;
    remain_size = total_size;
    curr_offset = 0;
    memset(tmp_buf_ptr, 0, sizeof(tmp_buf));

    GET_VIDEO_DATA_FROM_RANDOM_PAGE(screen_vaddr, tmp_buf_ptr);
#endif
    int pitch = 0;
    void *pixels_ptr;
    if (SDL_LockTexture(texture, NULL, &pixels_ptr, &pitch))
        exit(EXIT_FAILURE);
#if RV32_HAS(SYSTEM_MMIO)
    memcpy(pixels_ptr, tmp_buf_ptr, total_size);
#else
    memory_read(attr->mem, pixels_ptr, screen, total_size);
#endif
    SDL_UnlockTexture(texture);

    int actual_width, actual_height;
    SDL_GetWindowSize(window, &actual_width, &actual_height);
    SDL_RenderCopy(renderer, texture, NULL,
                   &(SDL_Rect) {0, 0, actual_width, actual_height});
    SDL_RenderPresent(renderer);
}

void syscall_setup_queue(riscv_t *rv)
{
#if RV32_HAS(SYSTEM_MMIO)
    /*
     * The guestOS might exit and execute the SDL-based program again
     * thus clearing the queue is required to avoid using the
     * access the old events.
     */
    event_queue.base = event_queue.end = 0;
    submission_queue.base = submission_queue.start = 0;
    PRIV(rv)->running_sdl = true;
#endif

    /* setup_queue(base, capacity, event_count) */
    uint32_t base = rv_get_reg(rv, rv_reg_a0);
    queues_capacity = rv_get_reg(rv, rv_reg_a1);
    event_count = rv_get_reg(rv, rv_reg_a2);

#if RV32_HAS(SYSTEM_MMIO)
    uint32_t submission_queue_addr =
        rv->io.mem_translate(rv, base + sizeof(event_t) * queues_capacity, R);

    uint32_t event_queue_addr = rv->io.mem_translate(rv, base, R);

    /* 此时 base 已经是 gPA，宿主可直接访问。 */
    event_queue.base = event_queue_addr;
    submission_queue.base = submission_queue_addr;
#else
    event_queue.base = base;
    submission_queue.base = base + sizeof(event_t) * queues_capacity;
#endif
    queues_capacity = round_pow2(queues_capacity);
}

void syscall_submit_queue(riscv_t *rv)
{
    /* submit_queue(count) */
    uint32_t count = rv_get_reg(rv, rv_reg_a0);

    if (!window) {
        deferred_submissions += count;
        return;
    }

    if (deferred_submissions)
        count = deferred_submissions;

    while (count--) {
        submission_t submission = submission_pop(rv);

        char *title;
        switch (submission.type) {
        case RELATIVE_MODE_SUBMISSION:
            SDL_SetRelativeMouseMode(submission.mouse.enabled);
            break;
        case WINDOW_TITLE_SUBMISSION:
            title = malloc(submission.title.size + 1);
            if (unlikely(!title))
                return;

#if RV32_HAS(SYSTEM_MMIO)
            uint32_t addr = rv->io.mem_translate(rv, submission.title.title, R);
            memory_read(PRIV(rv)->mem, (uint8_t *) title, addr,
                        submission.title.size);
#else
            memory_read(PRIV(rv)->mem, (uint8_t *) title,
                        submission.title.title, submission.title.size);
#endif
            title[submission.title.size] = 0;

            SDL_SetWindowTitle(window, title);
            free(title);
            break;
        }
    }
}

/* Portions Copyright (C) 2021-2022 Steve Clark
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would
 *    be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

/* 这是一个简单的 MUS 到 MIDI 转换器，用于 DOOM 这类使用 MIDI 存储声音的程序。
 *
 * sfx_handler 也能管理 Quake 音效，因为它们都是 WAV 格式。
 */

typedef PACKED(struct {
    char id[4];
    uint16_t score_len;
    uint16_t score_start;
}) mus_header_t;

typedef PACKED(struct {
    char id[4];
    int length;
    uint16_t type;
    uint16_t ntracks;
    uint16_t ticks;
}) midi_header_t;

static const char magic_mus[4] = {
    'M',
    'U',
    'S',
    0x1a,
};
static const char magic_midi[4] = {
    'M',
    'T',
    'h',
    'd',
};
static const char magic_track[4] = {
    'M',
    'T',
    'r',
    'k',
};
static const uint8_t magic_end_of_track[4] = {
    0x00,
    0xff,
    0x2f,
    0x00,
};

static const int controller_map[16] = {
    -1, 0, 1, 7, 10, 11, 91, 93, 64, 67, 120, 123, 126, 127, 121, -1,
};

static uint8_t *midi_data;
static int midi_size;

static uint8_t *mus_pos;
static int mus_end_of_track;

static uint8_t delta_bytes[4];
static int delta_cnt;

/* 维护通道音量列表。 */
static uint8_t mus_channel[16];

/* MUS 到 MIDI 的主转换例程。 */
static int convert(void)
{
    uint8_t data, last, channel;
    uint8_t event[3] = {0};
    int count = 0;
    uint8_t *midi_data_tmp;

    data = *mus_pos++;
    last = data & 0x80;
    channel = data & 0xf;

    switch (data & 0x70) {
    case 0x00:
        event[0] = 0x80;
        event[1] = *mus_pos++ & 0x7f;
        event[2] = mus_channel[channel];
        count = 3;
        break;

    case 0x10:
        event[0] = 0x90;
        data = *mus_pos++;
        event[1] = data & 0x7f;
        event[2] = data & 0x80 ? *mus_pos++ : mus_channel[channel];
        mus_channel[channel] = event[2];
        count = 3;
        break;

    case 0x20:
        event[0] = 0xe0;
        event[1] = (*mus_pos & 0x01) << 6;
        event[2] = *mus_pos++ >> 1;
        count = 3;
        break;

    case 0x30:
        event[0] = 0xb0;
        event[1] = controller_map[*mus_pos++ & 0xf];
        event[2] = 0x7f;
        count = 3;
        break;

    case 0x40:
        data = *mus_pos++;
        if (data == 0) {
            event[0] = 0xc0;
            event[1] = *mus_pos++;
            count = 2;
            break;
        }
        event[0] = 0xb0;
        event[1] = controller_map[data & 0xf];
        event[2] = *mus_pos++;
        count = 3;
        break;

    case 0x50:
        return 0;

    case 0x60:
        mus_end_of_track = 1;
        return 0;

    case 0x70:
        mus_pos++;
        return 0;
    }

    if (channel == 9)
        channel = 15;
    else if (channel == 15)
        channel = 9;

    event[0] |= channel;

    midi_data_tmp = realloc(midi_data, midi_size + delta_cnt + count);
    if (unlikely(!midi_data_tmp)) {
        free(midi_data);
        return -ENOMEM;
    }
    midi_data = midi_data_tmp;

    memcpy(midi_data + midi_size, &delta_bytes, delta_cnt);
    midi_size += delta_cnt;
    memcpy(midi_data + midi_size, &event, count);
    midi_size += count;

    if (last) {
        delta_cnt = 0;
        do {
            data = *mus_pos++;
            delta_bytes[delta_cnt] = data;
            delta_cnt++;
        } while (data & 128);
    } else {
        delta_bytes[0] = 0;
        delta_cnt = 1;
    }

    return 0;
}

uint8_t *mus2midi(uint8_t *data, int *length)
{
    mus_header_t *mus_hdr = (mus_header_t *) data;
    midi_header_t midi_hdr;
    uint8_t *mid_track_len;
    int track_len;
    uint8_t *midi_data_tmp;

    if (strncmp(mus_hdr->id, magic_mus, 4))
        return NULL;

    if (*length != mus_hdr->score_start + mus_hdr->score_len)
        return NULL;

    midi_size = sizeof(midi_header_t);
    memcpy(midi_hdr.id, magic_midi, 4);
    midi_hdr.length = bswap32(6);
    midi_hdr.type = bswap16(0); /* 单轨 MIDI 应使用 type 0。 */
    midi_hdr.ntracks = bswap16(1);
    /* 近似设置为 140ppqn，并把 tempo 设为 1000000us。 */
    midi_hdr.ticks =
        bswap16(70); /* 默认 tempo=500000us 时，70 ppqn = 每秒 140 tick。 */
    midi_data = malloc(midi_size);
    if (unlikely(!midi_data))
        return NULL;
    memcpy(midi_data, &midi_hdr, midi_size);

    midi_data_tmp = realloc(midi_data, midi_size + 8);
    if (unlikely(!midi_data_tmp)) {
        free(midi_data);
        return NULL;
    }
    midi_data = midi_data_tmp;
    memcpy(midi_data + midi_size, magic_track, 4);
    midi_size += 4;
    mid_track_len = midi_data + midi_size;
    midi_size += 4;

    track_len = 0;

    mus_pos = data + mus_hdr->score_start;
    mus_end_of_track = 0;
    delta_bytes[0] = 0;
    delta_cnt = 1;

    for (int i = 0; i < 16; i++)
        mus_channel[i] = 0;

    while (!mus_end_of_track)
        if (unlikely(convert() < 0))
            return NULL;

    /* track 结束事件前必须追加最后一个 delta time。 */
    midi_data_tmp = realloc(midi_data, midi_size + delta_cnt);
    if (unlikely(!midi_data_tmp)) {
        free(midi_data);
        return NULL;
    }
    midi_data = midi_data_tmp;
    memcpy(midi_data + midi_size, &delta_bytes, delta_cnt);
    midi_size += delta_cnt;

    midi_data_tmp = realloc(midi_data, midi_size + 3);
    if (unlikely(!midi_data_tmp)) {
        free(midi_data);
        return NULL;
    }
    midi_data = midi_data_tmp;
    memcpy(midi_data + midi_size, magic_end_of_track + 1, 3);
    midi_size += 3;

    track_len = bswap32(midi_size - sizeof(midi_header_t) - 8);
    memcpy(mid_track_len, &track_len, 4);

    *length = midi_size;

    return midi_data;
}

#if RV32_HAS(SDL_MIXER)
static void *sfx_handler(void *arg)
{
    sound_t *sfx = (sound_t *) arg;
    uint8_t *ptr = sfx->data;

    if (*ptr & 0x3) { /* Doom WAV 格式。 */
        ptr += 2;     /* 跳过格式字段。 */
        ptr += 2;     /* SAMPLE_RATE 已定义，跳过采样率字段。 */
        nr_sfx_samples = *(uint32_t *) ptr;
        ptr += 4;
        ptr += 4;  /* 跳过填充字节。 */
    } else {       /* 普通 WAV 格式。 */
        ptr += 44; /* 跳过 RIFF header。 */
        nr_sfx_samples = sfx->size - 44;
    }

    memcpy(sfx_samples, ptr, sizeof(uint8_t) * nr_sfx_samples);
    sfx_chunk = Mix_QuickLoad_RAW(sfx_samples, nr_sfx_samples);
    if (!sfx_chunk) {
        free(sfx);
        return NULL;
    }

    chan = Mix_PlayChannel(-1, sfx_chunk, 0);
    if (chan == -1) {
        free(sfx);
        return NULL;
    }

    if (*ptr & 0x3) {
        /* Doom：sfx->volume 最大为 15，因此乘以 8。 */
        Mix_Volume(chan, sfx->volume * 8);
    } else {
        /* Quake：sfx->volume 最大为 255，而 Mix_Volume 最大为 128，
         * 因此使用 (volume + 1) % 128 映射。
         */
        Mix_Volume(chan, (sfx->volume + 1) % 128);
    }

    free(sfx);
    return NULL;
}

static void *music_handler(void *arg)
{
    sound_t *music = (sound_t *) arg;
    int looping = music->looping ? -1 : 1;

    /* 释放上一次的 MIDI 数据。 */
    free(music_midi_data);
    music_midi_data = NULL;

    /* 释放上一次的音乐资源，避免内存泄漏。 */
    if (mid) {
        Mix_HaltMusic();
        Mix_FreeMusic(mid);
        mid = NULL;
    }

    music_midi_data = mus2midi(music->data, (int *) &music->size);
    if (!music_midi_data) {
        rv_log_error("mus2midi() 失败");
        free(music);
        return NULL;
    }

    SDL_RWops *rwops = SDL_RWFromMem(music_midi_data, music->size);
    if (!rwops) {
        rv_log_error("SDL_RWFromMem 失败：%s", SDL_GetError());
        free(music);
        return NULL;
    }

    mid = Mix_LoadMUSType_RW(rwops, MUS_MID, SDL_TRUE);
    if (!mid) {
        rv_log_error("Mix_LoadMUSType_RW 失败：%s", Mix_GetError());
        free(music);
        return NULL;
    }

    /* sfx->volume 最大为 15，因此乘以 8。
     * 后续可通过 syscall_set_music_volume 继续设置音量。
     */
    Mix_VolumeMusic(music->volume * 8);

    if (Mix_PlayMusic(mid, looping) == -1) {
        rv_log_error("Mix_PlayMusic 失败：%s", Mix_GetError());
        free(music);
        return NULL;
    }

    free(music);
    return NULL;
}

static void play_sfx(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    const uint32_t sfxinfo_addr = (uint32_t) rv_get_reg(rv, rv_reg_a1);
    int volume = rv_get_reg(rv, rv_reg_a2);

    sfxinfo_t sfxinfo;
    uint32_t sfx_data_size;

#if RV32_HAS(SYSTEM_MMIO)
    uint32_t addr = rv->io.mem_translate(rv, sfxinfo_addr, R);
    memory_read(attr->mem, (uint8_t *) &sfxinfo, addr, sizeof(sfxinfo_t));

    uint32_t sfx_data_offset =
        *((uint32_t *) ((uint8_t *) attr->mem->mem_base + addr));
    sfx_data_size = *(uint32_t *) ((uint8_t *) attr->mem->mem_base + addr + 4);
#else
    memory_read(attr->mem, (uint8_t *) &sfxinfo, sfxinfo_addr,
                sizeof(sfxinfo_t));

    /* 应用中的 data 和 size 必须位于结构体前两个字段。
     * 这样模拟器访问不同 sfxinfo_t 实例时可兼容多种应用。
     */
    uint32_t sfx_data_offset = *((uint32_t *) &sfxinfo);
    sfx_data_size = *(uint32_t *) ((uint32_t *) &sfxinfo + 1);
#endif

#if RV32_HAS(SDL_MIXER)
    /* 校验大小，避免不可信 guest 触发过量分配。 */
    if (sfx_data_size == 0 || sfx_data_size > SFX_SAMPLE_SIZE)
        return;

    /* 在堆上同时分配 sound_t 和数据缓冲区；线程接管其所有权。 */
    sound_t *sfx = malloc(sizeof(sound_t) + sfx_data_size);
    if (!sfx)
        return;

    sfx->data = (uint8_t *) (sfx + 1); /* 数据紧随结构体之后。 */
    sfx->size = sfx_data_size;
    sfx->volume = volume;

#if RV32_HAS(SYSTEM_MMIO)
    uint32_t sfx_data_vaddr = sfx_data_offset;
    uint8_t *sfx_data_ptr = sfx->data;
    remain_size = sfx_data_size;
    curr_offset = 0;

    GET_SFX_DATA_FROM_RANDOM_PAGE(sfx_data_vaddr, sfx_data_ptr);
#else
    memory_read(attr->mem, sfx->data, sfx_data_offset,
                sizeof(uint8_t) * sfx_data_size);
#endif

#ifdef __EMSCRIPTEN__
    /* Web 浏览器：使用 joinable 线程，并等待播放处理完成。 */
    if (pthread_create(&sfx_thread, NULL, sfx_handler, sfx) != 0) {
        free(sfx);
        return;
    }
    pthread_join(sfx_thread, NULL);
    /* 线程已 join，shutdown_audio 中不要再次 join。 */
#else
    /* 原生环境：使用 detached 线程进行非阻塞播放。 */
    pthread_t thread;
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &thread_attr, sfx_handler, sfx) != 0) {
        pthread_attr_destroy(&thread_attr);
        free(sfx);
        return;
    }
    pthread_attr_destroy(&thread_attr);
#endif
#endif /* RV32_HAS(SDL_MIXER) */
}

static void play_music(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    const uint32_t musicinfo_addr = (uint32_t) rv_get_reg(rv, rv_reg_a1);
    int volume = rv_get_reg(rv, rv_reg_a2);
    int looping = rv_get_reg(rv, rv_reg_a3);

    musicinfo_t musicinfo;
    uint32_t music_data_size;

#if RV32_HAS(SYSTEM_MMIO)
    uint32_t addr = rv->io.mem_translate(rv, musicinfo_addr, R);
    memory_read(attr->mem, (uint8_t *) &musicinfo, addr, sizeof(musicinfo_t));

    /* 应用中的 data 和 size 必须位于结构体前两个字段。
     * 这样模拟器访问不同 musicinfo_t 实例时可兼容多种应用。
     */
    uint32_t music_data_offset =
        *((uint32_t *) ((uint8_t *) attr->mem->mem_base + addr));
    music_data_size =
        *(uint32_t *) ((uint8_t *) attr->mem->mem_base + addr + 4);
#else
    memory_read(attr->mem, (uint8_t *) &musicinfo, musicinfo_addr,
                sizeof(musicinfo_t));

    /* 应用中的 data 和 size 必须位于结构体前两个字段。
     * 这样模拟器访问不同 musicinfo_t 实例时可兼容多种应用。
     */
    uint32_t music_data_offset = *((uint32_t *) &musicinfo);
    music_data_size = *(uint32_t *) ((uint32_t *) &musicinfo + 1);
#endif

#if RV32_HAS(SDL_MIXER)
    /* 校验大小，避免不可信 guest 触发过量分配。 */
    if (music_data_size == 0 || music_data_size > MUSIC_MAX_SIZE)
        return;

    /* 在堆上同时分配 sound_t 和数据缓冲区；线程接管其所有权。 */
    sound_t *music = malloc(sizeof(sound_t) + music_data_size);
    if (!music)
        return;

    music->data = (uint8_t *) (music + 1); /* 数据紧随结构体之后。 */
    music->size = music_data_size;
    music->looping = looping;
    music->volume = volume;

#if RV32_HAS(SYSTEM_MMIO)
    uint32_t music_data_vaddr = music_data_offset;
    uint8_t *music_data_ptr = music->data;
    remain_size = music_data_size;
    curr_offset = 0;

    GET_MUSIC_DATA_FROM_RANDOM_PAGE(music_data_vaddr, music_data_ptr);
#else
    memory_read(attr->mem, music->data, music_data_offset, music_data_size);
#endif

#ifdef __EMSCRIPTEN__
    /* Web 浏览器：使用 joinable 线程，并等待播放处理完成。 */
    if (pthread_create(&music_thread, NULL, music_handler, music) != 0) {
        free(music);
        return;
    }
    pthread_join(music_thread, NULL);
    /* 线程已 join，shutdown_audio 中不要再次 join。 */
#else
    /* 原生环境：使用 detached 线程进行非阻塞播放。 */
    pthread_t thread;
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &thread_attr, music_handler, music) != 0) {
        pthread_attr_destroy(&thread_attr);
        free(music);
        return;
    }
    pthread_attr_destroy(&thread_attr);
#endif
#endif /* RV32_HAS(SDL_MIXER) */
}

static void stop_music()
{
    if (Mix_PlayingMusic())
        Mix_HaltMusic();
}

static void set_music_volume(riscv_t *rv)
{
    int volume = rv_get_reg(rv, rv_reg_a1);

    /* volume 最大为 15，因此乘以 8。 */
    Mix_VolumeMusic(volume * 8);
}
#endif /* RV32_HAS(SDL_MIXER) */

static void init_audio(void)
{
    if (!(SDL_WasInit(-1) & SDL_INIT_AUDIO)) {
        if (SDL_Init(SDL_INIT_AUDIO) != 0) {
            rv_log_fatal("调用 SDL_Init() 失败");
            exit(EXIT_FAILURE);
        }
    }

    /* sfx 采样缓冲区。 */
    sfx_samples = malloc(SFX_SAMPLE_SIZE);
    if (unlikely(!sfx_samples)) {
        rv_log_fatal("为缓冲区分配内存失败");
        exit(EXIT_FAILURE);
    }

#if RV32_HAS(SDL_MIXER)
    /* 初始化 SDL2 Mixer。 */
    if (Mix_Init(MIX_INIT_MID) != MIX_INIT_MID) {
        rv_log_fatal("Mix_Init 失败：%s", Mix_GetError());
        exit(EXIT_FAILURE);
    }
    if (Mix_OpenAudio(SAMPLE_RATE, AUDIO_U8, CHANNEL_USED, CHUNK_SIZE) == -1) {
        rv_log_fatal("Mix_OpenAudio 失败：%s", Mix_GetError());
        Mix_Quit();
        exit(EXIT_FAILURE);
    }
#endif
    audio_init = true;
}

static void shutdown_audio()
{
#if RV32_HAS(SDL_MIXER)
    /* 先停止所有播放。 */
    Mix_HaltMusic();
    Mix_HaltChannel(-1);

#ifdef __EMSCRIPTEN__
    /*
     * EMSCRIPTEN 中线程是 joinable 的；若线程已初始化，需要先 join，确保 handler
     * 已完成后再释放资源。
     */
    if (music_thread_init)
        pthread_join(music_thread, NULL);
    if (sfx_thread_init)
        pthread_join(sfx_thread, NULL);
#endif

    /* 释放音乐资源。 */
    if (mid) {
        Mix_FreeMusic(mid);
        mid = NULL;
    }
    free(music_midi_data);
    music_midi_data = NULL;

    /* 释放 sfx 资源。 */
    if (sfx_chunk) {
        Mix_FreeChunk(sfx_chunk);
        sfx_chunk = NULL;
    }

    Mix_CloseAudio();
    Mix_Quit();
#endif

    if (sfx_samples) {
        free(sfx_samples);
        sfx_samples = NULL;
    }

    audio_init = sfx_thread_init = music_thread_init = false;
}

void sdl_video_audio_cleanup()
{
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    /*
     * 音频配置初始化期间若快速触发 Ctrl-C，sfx_or_music_thread_init 标志可能
     * 尚未设置。因此需要额外检查 audio_init 标志来销毁音频设置。
     */
    bool sfx_or_music_thread_init = sfx_thread_init | music_thread_init;
    if (sfx_or_music_thread_init || (!sfx_or_music_thread_init && audio_init))
        shutdown_audio();
    SDL_Quit();
}

void syscall_setup_audio(riscv_t *rv)
{
    /* setup_audio(request) */
    const int request = rv_get_reg(rv, rv_reg_a0);

    switch (request) {
    case INIT_AUDIO:
        init_audio();
        break;
    case SHUTDOWN_AUDIO:
        shutdown_audio();
        break;
    default:
        rv_log_error("未知声音请求：%d", request);
        break;
    }
}

void syscall_control_audio(riscv_t *rv)
{
    /* control_audio(request) */
    const int request = rv_get_reg(rv, rv_reg_a0);

    switch (request) {
    case PLAY_MUSIC:
#if RV32_HAS(SDL_MIXER)
        play_music(rv);
#endif
        break;
    case PLAY_SFX:
#if RV32_HAS(SDL_MIXER)
        play_sfx(rv);
#endif
        break;
    case SET_MUSIC_VOLUME:
#if RV32_HAS(SDL_MIXER)
        set_music_volume(rv);
#endif
        break;
    case STOP_MUSIC:
#if RV32_HAS(SDL_MIXER)
        stop_music();
#endif
        break;
    default:
        rv_log_error("未知声音控制请求：%d", request);
        break;
    }
}
