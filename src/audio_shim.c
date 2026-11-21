#include "audio_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>

#define AUDIO_TARGET_FREQ     48000
#define AUDIO_TARGET_CHANNELS 2
#define AUDIO_BUFFER_SAMPLES  1024
#define MAX_SFX_CHANNELS      32

/* Guest memory base offset (defined in xboxrecomp runtime) */
extern ptrdiff_t g_xbox_mem_offset;

/* ── IMA / Xbox ADPCM lookup tables ── */
static const uint16_t s_adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int s_adpcm_index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

#define ADPCM_CLAMP(val, min_val, max_val) do { \
    if ((val) > (max_val)) (val) = (max_val);   \
    else if ((val) < (min_val)) (val) = (min_val); \
} while (0)

/* ── Decoded Sound Sample Representation ── */
typedef struct {
    int16_t *samples;       /* 16-bit interleaved PCM */
    size_t   total_frames;  /* Frame count (1 frame = channels samples) */
    int      channels;
    int      sample_rate;
} DecodedSound;

/* ── Voice / Channel State ── */
typedef struct {
    DecodedSound *sound;
    uint64_t play_cursor_fp; /* 16.16 fixed-point frame cursor */
    uint32_t sample_rate;    /* Playback sample rate in Hz */
    float    volume;         /* 0.0 to 1.0 */
    float    pan_l;          /* 0.0 to 1.0 Left channel gain */
    float    pan_r;          /* 0.0 to 1.0 Right channel gain */
    bool     active;
    bool     looping;
} AudioVoice;

/* ── Global Audio Backend State ── */
static struct {
    SDL_AudioDeviceID device_id;
    SDL_AudioSpec     spec;
    bool              initialized;
    SDL_mutex        *lock;

    /* Music channel */
    AudioVoice        music_voice;
    DecodedSound      current_music;
    float             music_master_vol;

    /* Sound effects channels */
    AudioVoice        sfx_voices[MAX_SFX_CHANNELS];
    float             sfx_master_vol;

    char              asset_root[512];
} g_audio = {
    .device_id = 0,
    .initialized = false,
    .lock = NULL,
    .music_master_vol = 0.85f,
    .sfx_master_vol = 1.0f,
    .asset_root = ""
};

/* Environment variable override for music volume: -1.0 if not set */
static float s_music_vol_override = -1.0f;
static bool  s_override_checked = false;

/* ── Decibel / Millibel Utilities ── */

float Audio_MillibelsToLinear(int32_t mb)
{
    if (mb <= -9600) return 0.0f;
    if (mb >= 0) return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

int32_t Audio_LinearToMillibels(float linear)
{
    if (linear <= 0.00001f) return -10000;
    if (linear >= 1.0f) return 0;
    return (int32_t)(2000.0f * log10f(linear));
}

/* ── Volume Synchronization Logic ── */

static void sync_volumes_from_guest(void)
{
    /* Check AUDIO_SHIM_MUSIC_VOL environment override */
    if (!s_override_checked) {
        s_override_checked = true;
        const char *env_mvol = getenv("AUDIO_SHIM_MUSIC_VOL");
        if (env_mvol) {
            float f = (float)atof(env_mvol);
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            s_music_vol_override = f;
            fprintf(stderr, "[AUDIO] AUDIO_SHIM_MUSIC_VOL override set to %.2f\n", s_music_vol_override);
        }
    }

    if (s_music_vol_override >= 0.0f) {
        g_audio.music_master_vol = s_music_vol_override;
    } else if (g_xbox_mem_offset) {
        /* Read in-game Music volume slider from guest VA 0x002064E4 */
        float mvol = *(const volatile float *)((uintptr_t)g_xbox_mem_offset + 0x002064E4u);
        if (mvol >= 0.0f && mvol <= 1.0f && !isnan(mvol)) {
            g_audio.music_master_vol = mvol;
        }
    }

    /* Read in-game SFX volume slider from guest VA 0x002064E0 */
    if (g_xbox_mem_offset) {
        float svol = *(const volatile float *)((uintptr_t)g_xbox_mem_offset + 0x002064E0u);
        if (svol >= 0.0f && svol <= 1.0f && !isnan(svol)) {
            g_audio.sfx_master_vol = svol;
        }
    }

    static float s_last_mvol = -1.0f;
    static float s_last_svol = -1.0f;
    if (fabsf(g_audio.music_master_vol - s_last_mvol) > 0.005f ||
        fabsf(g_audio.sfx_master_vol - s_last_svol) > 0.005f) {
        fprintf(stderr, "[AUDIO] Volume sync: Music=%.2f%s, SFX=%.2f\n",
                g_audio.music_master_vol,
                (s_music_vol_override >= 0.0f) ? " (env override)" : " (slider)",
                g_audio.sfx_master_vol);
        s_last_mvol = g_audio.music_master_vol;
        s_last_svol = g_audio.sfx_master_vol;
    }
}

/* ── Xbox ADPCM Nibble Decoder ── */
static inline void decode_adpcm_nibble(uint8_t nibble, int32_t *pcm, int8_t *step_idx)
{
    int step = s_adpcm_step_table[*step_idx];
    int delta = ((2 * (nibble & 7) + 1) * step) >> 3;
    if (nibble & 8) delta = -delta;
    *pcm += delta;
    ADPCM_CLAMP(*pcm, -32768, 32767);
    *step_idx += s_adpcm_index_table[nibble & 7];
    ADPCM_CLAMP(*step_idx, 0, 88);
}

int Audio_DecodeXboxADPCM(const uint8_t *src, size_t src_size,
                          int16_t **out_pcm, size_t *out_samples,
                          int channels, int block_align)
{
    if (!src || src_size == 0 || !out_pcm || !out_samples) return -1;
    if (channels != 1 && channels != 2) return -1;
    if (block_align <= 0) block_align = (channels == 2) ? 72 : 36;

    size_t total_full_blocks = src_size / (size_t)block_align;
    size_t rem_bytes = src_size % (size_t)block_align;
    size_t rem_frames = 0;
    if (rem_bytes >= (size_t)channels * 4) {
        size_t rem_chunks = (rem_bytes - channels * 4) / (channels * 4);
        if (rem_chunks > 8) rem_chunks = 8;
        rem_frames = 1 + rem_chunks * 8;
        if (rem_frames > 64) rem_frames = 64;
    }

    size_t total_frames = total_full_blocks * 64 + rem_frames;
    if (total_frames == 0) return -1;

    size_t alloc_samples = total_frames * channels;
    int16_t *dst = (int16_t *)calloc(alloc_samples, sizeof(int16_t));
    if (!dst) return -1;

    size_t frame_idx = 0;
    size_t raw_ptr = 0;

    while (raw_ptr + (size_t)channels * 4 <= src_size) {
        size_t avail = src_size - raw_ptr;
        size_t blk_sz = (avail < (size_t)block_align) ? avail : (size_t)block_align;
        const uint8_t *block = src + raw_ptr;
        raw_ptr += blk_sz;

        size_t payload_bytes = blk_sz - (channels * 4);
        size_t chunks = payload_bytes / (channels * 4);
        if (chunks > 8) chunks = 8;
        size_t frames_this_block = (chunks == 8) ? 64 : (1 + chunks * 8);
        if (frame_idx + frames_this_block > total_frames) {
            frames_this_block = total_frames - frame_idx;
        }
        size_t block_end_frame = frame_idx + frames_this_block;

        int32_t pcm[2];
        int8_t step_idx[2];
        for (int ch = 0; ch < channels; ch++) {
            pcm[ch] = (int16_t)(block[ch * 4] | (block[ch * 4 + 1] << 8));
            step_idx[ch] = (int8_t)block[ch * 4 + 2];
            ADPCM_CLAMP(step_idx[ch], 0, 88);
            if (frame_idx < total_frames) {
                dst[frame_idx * channels + ch] = (int16_t)pcm[ch];
            }
        }

        size_t payload_ptr = channels * 4;
        size_t curr_frame = frame_idx + 1;

        for (size_t chunk = 0; chunk < chunks; chunk++) {
            for (int ch = 0; ch < channels; ch++) {
                size_t ch_frame = curr_frame;
                for (int i = 0; i < 4; i++) {
                    uint8_t byte_val = block[payload_ptr++];

                    /* Nibble 1 (low 4 bits) */
                    uint8_t n1 = byte_val & 0x0F;
                    decode_adpcm_nibble(n1, &pcm[ch], &step_idx[ch]);
                    if (ch_frame < block_end_frame) {
                        dst[ch_frame * channels + ch] = (int16_t)pcm[ch];
                    }
                    ch_frame++;

                    /* Nibble 2 (high 4 bits) */
                    uint8_t n2 = (byte_val >> 4) & 0x0F;
                    decode_adpcm_nibble(n2, &pcm[ch], &step_idx[ch]);
                    if (ch_frame < block_end_frame) {
                        dst[ch_frame * channels + ch] = (int16_t)pcm[ch];
                    }
                    ch_frame++;
                }
            }
            curr_frame += 8;
        }

        frame_idx += frames_this_block;
    }

    *out_pcm = dst;
    *out_samples = total_frames * channels;
    return 0;
}

/* ── Asset path resolution ── */
static bool resolve_asset_path(const char *rel_path, char *out_path, size_t max_len)
{
    const char *search_prefixes[] = {
        "",
        "../Lamborghini/",
        "../../Lamborghini/",
        "Lamborghini/",
        NULL
    };

    for (int i = 0; search_prefixes[i] != NULL; i++) {
        snprintf(out_path, max_len, "%s%s", search_prefixes[i], rel_path);
        FILE *f = fopen(out_path, "rb");
        if (f) {
            fclose(f);
            return true;
        }
    }
    return false;
}

/* ── RIFF WAV / WAX memory parser ── */
static int parse_riff_wav_memory(const uint8_t *buf, size_t file_size, DecodedSound *out_sound)
{
    if (!buf || file_size < 44 || !out_sound) return -1;

    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return -1;
    }

    size_t offset = 12;
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t block_align = 0;
    const uint8_t *data_ptr = NULL;
    uint32_t data_size = 0;

    while (offset + 8 <= file_size) {
        uint32_t chunk_size = *(const uint32_t *)(buf + offset + 4);
        const char *chunk_id = (const char *)(buf + offset);
        offset += 8;

        if (offset + chunk_size > file_size) {
            chunk_size = (uint32_t)(file_size - offset);
        }

        if (memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
            format_tag = *(const uint16_t *)(buf + offset);
            channels = *(const uint16_t *)(buf + offset + 2);
            sample_rate = *(const uint32_t *)(buf + offset + 4);
            block_align = *(const uint16_t *)(buf + offset + 12);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_ptr = buf + offset;
            data_size = chunk_size;
        }

        offset += (chunk_size + 1) & ~1u;
    }

    if (!data_ptr || data_size == 0) return -1;

    if (format_tag == 0x0001) {
        /* Standard 16-bit PCM */
        out_sound->samples = (int16_t *)malloc(data_size);
        if (!out_sound->samples) return -1;
        memcpy(out_sound->samples, data_ptr, data_size);
        out_sound->channels = channels;
        out_sound->sample_rate = sample_rate;
        out_sound->total_frames = data_size / (channels * sizeof(int16_t));
        return 0;
    } else if (format_tag == WAVE_FORMAT_XBOX_ADPCM) {
        /* Xbox ADPCM (0x0069) */
        size_t total_samples = 0;
        int res = Audio_DecodeXboxADPCM(data_ptr, data_size,
                                        &out_sound->samples, &total_samples,
                                        channels, block_align);
        if (res != 0) return -1;
        out_sound->channels = channels;
        out_sound->sample_rate = sample_rate;
        out_sound->total_frames = total_samples / channels;
        return 0;
    }

    return -1;
}

/* ── RIFF WAV / WAX file loader ── */
static int load_wax_or_wav(const char *path, DecodedSound *out_sound)
{
    char resolved[512];
    if (!resolve_asset_path(path, resolved, sizeof(resolved))) {
        return -1;
    }

    FILE *f = fopen(resolved, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 44) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    int ret = parse_riff_wav_memory(buf, (size_t)file_size, out_sound);
    free(buf);
    return ret;
}

/* ── Voice Resampling & Panning Mixer Helper ── */
static void mix_voice_samples(AudioVoice *v, float master_vol, int32_t *mix_l, int32_t *mix_r, int num_samples)
{
    if (!v->active || !v->sound || !v->sound->samples || v->sound->total_frames == 0) return;

    const DecodedSound *snd = v->sound;
    size_t total_frames = snd->total_frames;
    int channels = snd->channels;
    uint32_t srate = v->sample_rate ? v->sample_rate : (snd->sample_rate > 0 ? (uint32_t)snd->sample_rate : (uint32_t)AUDIO_TARGET_FREQ);

    /* 16.16 fixed-point phase step for fractional resampling to 48000 Hz */
    uint64_t step_fp = ((uint64_t)srate << 16) / AUDIO_TARGET_FREQ;
    if (step_fp == 0) step_fp = 1;

    uint64_t pos_fp = v->play_cursor_fp;
    uint64_t total_fp = (uint64_t)total_frames << 16;

    float vol = v->volume * master_vol;
    float vol_l = vol * v->pan_l;
    float vol_r = vol * v->pan_r;

    const int16_t *src = snd->samples;

    for (int i = 0; i < num_samples; i++) {
        size_t f0 = (size_t)(pos_fp >> 16);

        if (f0 >= total_frames) {
            if (v->looping) {
                pos_fp %= total_fp;
                f0 = (size_t)(pos_fp >> 16);
            } else {
                v->play_cursor_fp = 0;
                v->active = false;
                return;
            }
        }

        uint32_t frac = (uint32_t)(pos_fp & 0xFFFF);
        size_t f1 = f0 + 1;
        if (f1 >= total_frames) {
            f1 = v->looping ? 0 : f0;
        }

        int32_t s0_l, s1_l, s0_r, s1_r;
        if (channels >= 2) {
            s0_l = src[f0 * 2];
            s1_l = src[f1 * 2];
            s0_r = src[f0 * 2 + 1];
            s1_r = src[f1 * 2 + 1];
        } else {
            s0_l = src[f0];
            s1_l = src[f1];
            s0_r = s0_l;
            s1_r = s1_l;
        }

        /* Linear interpolation between adjacent frames */
        int32_t interp_l = s0_l + (int32_t)(((int64_t)(s1_l - s0_l) * (int64_t)frac) >> 16);
        int32_t interp_r = s0_r + (int32_t)(((int64_t)(s1_r - s0_r) * (int64_t)frac) >> 16);

        mix_l[i] += (int32_t)(interp_l * vol_l);
        mix_r[i] += (int32_t)(interp_r * vol_r);

        pos_fp += step_fp;
    }

    if (pos_fp >= total_fp) {
        if (v->looping) {
            pos_fp %= total_fp;
        } else {
            pos_fp = 0;
            v->active = false;
        }
    }
    v->play_cursor_fp = pos_fp;
}

/* ── Offline Audio Renderer ── */
void Audio_RenderOffline(int16_t *out_samples, int num_frames)
{
    /* Clear output buffer */
    memset(out_samples, 0, num_frames * AUDIO_TARGET_CHANNELS * sizeof(int16_t));

    if (!g_audio.initialized || !g_audio.lock) return;

    SDL_LockMutex(g_audio.lock);

    /* 32-bit mix accumulator */
    int32_t mix_l[AUDIO_BUFFER_SAMPLES];
    int32_t mix_r[AUDIO_BUFFER_SAMPLES];
    int frames_remaining = num_frames;
    int16_t *out = out_samples;

    while (frames_remaining > 0) {
        int chunk = (frames_remaining > AUDIO_BUFFER_SAMPLES) ? AUDIO_BUFFER_SAMPLES : frames_remaining;
        memset(mix_l, 0, chunk * sizeof(int32_t));
        memset(mix_r, 0, chunk * sizeof(int32_t));

        /* ── Mix Music Voice ── */
        if (g_audio.music_voice.active) {
            mix_voice_samples(&g_audio.music_voice, g_audio.music_master_vol, mix_l, mix_r, chunk);
        }

        /* ── Mix SFX Voices ── */
        for (int v = 0; v < MAX_SFX_CHANNELS; v++) {
            if (g_audio.sfx_voices[v].active) {
                mix_voice_samples(&g_audio.sfx_voices[v], g_audio.sfx_master_vol, mix_l, mix_r, chunk);
            }
        }

        /* Clamp to 16-bit PCM and write out */
        for (int i = 0; i < chunk; i++) {
            int32_t l = mix_l[i];
            int32_t r = mix_r[i];
            ADPCM_CLAMP(l, -32768, 32767);
            ADPCM_CLAMP(r, -32768, 32767);
            *out++ = (int16_t)l;
            *out++ = (int16_t)r;
        }

        frames_remaining -= chunk;
    }

    SDL_UnlockMutex(g_audio.lock);
}

/* ── SDL Audio Mixer Callback ── */
static void audio_sdl_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    int frames_to_mix = len / (AUDIO_TARGET_CHANNELS * sizeof(int16_t));
    sync_volumes_from_guest();
    Audio_RenderOffline((int16_t *)stream, frames_to_mix);
}

/* ── Public Audio Lifecycle API ── */

void Audio_Init(void)
{
    if (g_audio.initialized) return;

    fprintf(stderr, "[AUDIO] Initializing Audio Subsystem (SDL2 Audio backend)...\n");

    /* Check environment variable override for music */
    const char *env_mvol = getenv("AUDIO_SHIM_MUSIC_VOL");
    if (env_mvol) {
        float f = (float)atof(env_mvol);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        s_music_vol_override = f;
        g_audio.music_master_vol = f;
        s_override_checked = true;
        fprintf(stderr, "[AUDIO] AUDIO_SHIM_MUSIC_VOL override active at init: %.2f\n", s_music_vol_override);
    }

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "[AUDIO] Failed to initialize SDL Audio: %s\n", SDL_GetError());
            return;
        }
    }

    g_audio.lock = SDL_CreateMutex();

    SDL_AudioSpec wanted;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = AUDIO_TARGET_FREQ;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = AUDIO_TARGET_CHANNELS;
    wanted.samples = AUDIO_BUFFER_SAMPLES;
    wanted.callback = audio_sdl_callback;
    wanted.userdata = NULL;

    g_audio.device_id = SDL_OpenAudioDevice(NULL, 0, &wanted, &g_audio.spec, 0);
    if (g_audio.device_id == 0) {
        fprintf(stderr, "[AUDIO] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }

    g_audio.initialized = true;

    /* Start audio playback */
    SDL_PauseAudioDevice(g_audio.device_id, 0);

    fprintf(stderr, "[AUDIO] Audio device opened: %d Hz, %d channels, buffer %d samples\n",
            g_audio.spec.freq, g_audio.spec.channels, g_audio.spec.samples);

    /* Pre-load menu / garage music (disabled by default during gameplay to prevent clashing) */
    const char *play_music_env = getenv("RECOMP_AUDIO_SHIM_MUSIC");
    if (play_music_env && (strcmp(play_music_env, "1") == 0 || strcmp(play_music_env, "true") == 0)) {
        Audio_PlayMusic("Audio/gui/wav/garageloop.wax", 1);
    } else {
        fprintf(stderr, "[AUDIO] Music disabled by default (set RECOMP_AUDIO_SHIM_MUSIC=1 to enable menu music)\n");
    }
}

void Audio_InitOffline(void)
{
    if (g_audio.initialized) return;
    if (!g_audio.lock) {
        g_audio.lock = SDL_CreateMutex();
    }
    g_audio.initialized = true;
    g_audio.music_master_vol = 1.0f;
    g_audio.sfx_master_vol = 1.0f;
}

void Audio_Update(void)
{
    /* Polled once per frame to sync guest volume slider changes */
    sync_volumes_from_guest();
}

void Audio_Shutdown(void)
{
    if (!g_audio.initialized) return;

    if (g_audio.device_id != 0) {
        SDL_PauseAudioDevice(g_audio.device_id, 1);
        SDL_CloseAudioDevice(g_audio.device_id);
        g_audio.device_id = 0;
    }

    if (g_audio.lock) {
        SDL_DestroyMutex(g_audio.lock);
        g_audio.lock = NULL;
    }

    if (g_audio.current_music.samples) {
        free(g_audio.current_music.samples);
        g_audio.current_music.samples = NULL;
    }

    for (int i = 0; i < MAX_SFX_CHANNELS; i++) {
        AudioVoice *sv = &g_audio.sfx_voices[i];
        if (sv->sound && sv->sound != &g_audio.current_music) {
            free(sv->sound->samples);
            free(sv->sound);
            sv->sound = NULL;
        }
    }

    g_audio.initialized = false;
    fprintf(stderr, "[AUDIO] Audio Subsystem shutdown complete.\n");
}

/* ── Music Control ── */

int Audio_PlayMusic(const char *rel_path, int loop)
{
    if (!g_audio.initialized || !rel_path) return -1;

    DecodedSound loaded;
    memset(&loaded, 0, sizeof(loaded));
    if (load_wax_or_wav(rel_path, &loaded) != 0) {
        return -1;
    }

    SDL_LockMutex(g_audio.lock);

    /* Swap out previous music */
    if (g_audio.current_music.samples) {
        free(g_audio.current_music.samples);
    }
    g_audio.current_music = loaded;

    g_audio.music_voice.sound = &g_audio.current_music;
    g_audio.music_voice.play_cursor_fp = 0;
    g_audio.music_voice.sample_rate = loaded.sample_rate ? loaded.sample_rate : 44100;
    g_audio.music_voice.volume = 1.0f;
    g_audio.music_voice.pan_l = 1.0f;
    g_audio.music_voice.pan_r = 1.0f;
    g_audio.music_voice.looping = (loop != 0);
    g_audio.music_voice.active = true;

    SDL_UnlockMutex(g_audio.lock);

    fprintf(stderr, "[AUDIO] Playing music: %s (%zu frames, %d ch, %d Hz, loop=%d)\n",
            rel_path, loaded.total_frames, loaded.channels, loaded.sample_rate, loop);
    return 0;
}

void Audio_StopMusic(void)
{
    if (!g_audio.initialized) return;
    SDL_LockMutex(g_audio.lock);
    g_audio.music_voice.active = false;
    SDL_UnlockMutex(g_audio.lock);
}

void Audio_SetMusicVolume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    g_audio.music_master_vol = volume;
}

void Audio_SetMusicVolumeMillibels(int32_t mb)
{
    Audio_SetMusicVolume(Audio_MillibelsToLinear(mb));
}

float Audio_GetMusicVolume(void)
{
    return g_audio.music_master_vol;
}

/* ── SFX Control ── */

void Audio_SetSfxVolume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    g_audio.sfx_master_vol = volume;
}

void Audio_SetSfxVolumeMillibels(int32_t mb)
{
    Audio_SetSfxVolume(Audio_MillibelsToLinear(mb));
}

float Audio_GetSfxVolume(void)
{
    return g_audio.sfx_master_vol;
}

void Audio_SetVoiceVolume(int voice_slot, float volume)
{
    if (voice_slot < 0 || voice_slot >= MAX_SFX_CHANNELS) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    if (g_audio.lock) SDL_LockMutex(g_audio.lock);
    g_audio.sfx_voices[voice_slot].volume = volume;
    if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);
}

void Audio_SetVoiceVolumeMillibels(int voice_slot, int32_t mb)
{
    Audio_SetVoiceVolume(voice_slot, Audio_MillibelsToLinear(mb));
}

void Audio_SetVoicePan(int voice_slot, float pan_l, float pan_r)
{
    if (voice_slot < 0 || voice_slot >= MAX_SFX_CHANNELS) return;
    if (pan_l < 0.0f) pan_l = 0.0f;
    if (pan_r < 0.0f) pan_r = 0.0f;

    if (g_audio.lock) SDL_LockMutex(g_audio.lock);
    g_audio.sfx_voices[voice_slot].pan_l = pan_l;
    g_audio.sfx_voices[voice_slot].pan_r = pan_r;
    if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);
}

void Audio_SetVoiceSpatial(int voice_slot, float pan_l, float pan_r, int itd_delay)
{
    (void)itd_delay;
    Audio_SetVoicePan(voice_slot, pan_l, pan_r);
}

void Audio_StopSound(int voice_slot)
{
    if (voice_slot < 0 || voice_slot >= MAX_SFX_CHANNELS) return;
    if (g_audio.lock) SDL_LockMutex(g_audio.lock);
    g_audio.sfx_voices[voice_slot].active = false;
    if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);
}

int Audio_GetActiveSfxVoiceCount(void)
{
    int count = 0;
    if (g_audio.lock) SDL_LockMutex(g_audio.lock);
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) {
        if (g_audio.sfx_voices[i].active) count++;
    }
    if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);
    return count;
}

int Audio_PlaySound(const char *rel_path, float volume)
{
    if (!g_audio.initialized || !rel_path) return -1;

    int slot = -1;
    SDL_LockMutex(g_audio.lock);
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) {
        if (!g_audio.sfx_voices[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        SDL_UnlockMutex(g_audio.lock);
        return -1;
    }

    DecodedSound *snd = (DecodedSound *)calloc(1, sizeof(DecodedSound));
    if (load_wax_or_wav(rel_path, snd) != 0) {
        free(snd);
        SDL_UnlockMutex(g_audio.lock);
        return -1;
    }

    AudioVoice *v = &g_audio.sfx_voices[slot];
    if (v->sound && v->sound != &g_audio.current_music) {
        free(v->sound->samples);
        free(v->sound);
    }

    v->sound = snd;
    v->play_cursor_fp = 0;
    v->sample_rate = snd->sample_rate ? snd->sample_rate : 44100;
    v->volume = (volume < 0.0f) ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    v->pan_l = 1.0f;
    v->pan_r = 1.0f;
    v->looping = false;
    v->active = true;

    SDL_UnlockMutex(g_audio.lock);

    fprintf(stderr, "[AUDIO] SFX voice %d triggered: %zu frames, %d ch, %d Hz, vol=%.3f, pan=(%.2f, %.2f)\n",
            slot, snd->total_frames, snd->channels, snd->sample_rate, v->volume, v->pan_l, v->pan_r);
    return slot;
}

int Audio_PlaySoundMemory(const void *data, size_t size, float volume, int loop)
{
    if (!g_audio.initialized || !data || size == 0) return -1;

    int slot = -1;
    SDL_LockMutex(g_audio.lock);
    for (int i = 0; i < MAX_SFX_CHANNELS; i++) {
        if (!g_audio.sfx_voices[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        SDL_UnlockMutex(g_audio.lock);
        return -1;
    }

    DecodedSound *snd = (DecodedSound *)calloc(1, sizeof(DecodedSound));
    if (parse_riff_wav_memory((const uint8_t *)data, size, snd) != 0) {
        free(snd);
        SDL_UnlockMutex(g_audio.lock);
        return -1;
    }

    AudioVoice *v = &g_audio.sfx_voices[slot];
    if (v->sound && v->sound != &g_audio.current_music) {
        free(v->sound->samples);
        free(v->sound);
    }

    v->sound = snd;
    v->play_cursor_fp = 0;
    v->sample_rate = snd->sample_rate ? snd->sample_rate : 44100;
    v->volume = (volume < 0.0f) ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    v->pan_l = 1.0f;
    v->pan_r = 1.0f;
    v->looping = (loop != 0);
    v->active = true;

    SDL_UnlockMutex(g_audio.lock);

    fprintf(stderr, "[AUDIO] SFX memory voice %d triggered: %zu frames, %d ch, %d Hz, vol=%.3f, loop=%d\n",
            slot, snd->total_frames, snd->channels, snd->sample_rate, v->volume, loop);
    return slot;
}

/* ================================================================
 * DirectSound8 / Secondary Sound Buffer Compatibility Layer
 * ================================================================ */

typedef struct ShimSoundBuffer {
    IDirectSoundBuffer8 iface;
    LONG         ref_count;
    void        *raw_data;
    DWORD        raw_size;
    DecodedSound sound;
    DWORD        frequency;
    WORD         channels;
    WORD         bits_per_sample;
    WORD         block_align;
    WORD         format_tag;
    LONG         volume_mb;       /* Hundredths of dB: -10000 to 0 */
    float        linear_volume;   /* 0.0 to 1.0 */
    LONG         pan_mb;          /* DirectSound millibel pan: -10000 to +10000 */
    float        pan_l;           /* Left gain factor (default 1.0) */
    float        pan_r;           /* Right gain factor (default 1.0) */
    float        pos_x, pos_y, pos_z; /* 3D coordinates */
    bool         has_3d_pos;
    bool         has_custom_pan;
    int          voice_slot;      /* Voice channel index in g_audio.sfx_voices */
    bool         looping;
    uint64_t     play_cursor_fp;  /* 16.16 fixed-point */
} ShimSoundBuffer;

static ShimSoundBuffer *shim_buf_from_iface(IDirectSoundBuffer8 *iface)
{
    return (ShimSoundBuffer *)iface;
}

HRESULT Audio_SetBufferPan(IDirectSoundBuffer8 *self, LONG lPan)
{
    if (!self) return E_INVALIDARG;
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    buf->pan_mb = lPan;
    buf->has_custom_pan = true;

    /* DirectSound pan in hundredths of dB: -10000 (full left) to +10000 (full right) */
    if (lPan <= -10000) {
        buf->pan_l = 1.0f;
        buf->pan_r = 0.0f;
    } else if (lPan >= 10000) {
        buf->pan_l = 0.0f;
        buf->pan_r = 1.0f;
    } else if (lPan < 0) {
        buf->pan_l = 1.0f;
        buf->pan_r = powf(10.0f, (float)lPan / 2000.0f);
    } else if (lPan > 0) {
        buf->pan_l = powf(10.0f, (float)-lPan / 2000.0f);
        buf->pan_r = 1.0f;
    } else {
        buf->pan_l = 1.0f;
        buf->pan_r = 1.0f;
    }

    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        Audio_SetVoicePan(buf->voice_slot, buf->pan_l, buf->pan_r);
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_QueryInterface(IDirectSoundBuffer8 *self, const void *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall shim_buf_AddRef(IDirectSoundBuffer8 *self)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    return (ULONG)__sync_add_and_fetch(&buf->ref_count, 1);
}

static ULONG __stdcall shim_buf_Release(IDirectSoundBuffer8 *self)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    LONG ref = __sync_sub_and_fetch(&buf->ref_count, 1);
    if (ref <= 0) {
        if (buf->voice_slot >= 0) {
            Audio_StopSound(buf->voice_slot);
        }
        free(buf->sound.samples);
        free(buf->raw_data);
        free(buf);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall shim_buf_SetBufferData(IDirectSoundBuffer8 *self, const void *pvBufferData, DWORD dwBufferBytes)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    if (buf->voice_slot >= 0) {
        Audio_StopSound(buf->voice_slot);
    }

    free(buf->sound.samples);
    buf->sound.samples = NULL;
    buf->sound.total_frames = 0;

    free(buf->raw_data);
    buf->raw_data = NULL;
    buf->raw_size = dwBufferBytes;

    if (pvBufferData && dwBufferBytes > 0) {
        buf->raw_data = malloc(dwBufferBytes);
        if (buf->raw_data) {
            memcpy(buf->raw_data, pvBufferData, dwBufferBytes);
        }

        if (buf->format_tag == WAVE_FORMAT_XBOX_ADPCM) {
            size_t total_samples = 0;
            int align = buf->block_align ? buf->block_align : ((buf->channels == 2) ? 72 : 36);
            Audio_DecodeXboxADPCM((const uint8_t *)pvBufferData, dwBufferBytes,
                                  &buf->sound.samples, &total_samples,
                                  buf->channels, align);
            buf->sound.channels = buf->channels;
            buf->sound.sample_rate = buf->frequency;
            buf->sound.total_frames = total_samples / (buf->channels ? buf->channels : 1);
        } else {
            /* 16-bit PCM default */
            buf->sound.samples = (int16_t *)malloc(dwBufferBytes);
            if (buf->sound.samples) {
                memcpy(buf->sound.samples, pvBufferData, dwBufferBytes);
            }
            buf->sound.channels = buf->channels ? buf->channels : 1;
            buf->sound.sample_rate = buf->frequency ? buf->frequency : 44100;
            buf->sound.total_frames = dwBufferBytes / (buf->sound.channels * sizeof(int16_t));
        }
    }

    buf->play_cursor_fp = 0;
    return S_OK;
}

static HRESULT __stdcall shim_buf_SetPlayRegion(IDirectSoundBuffer8 *self, DWORD dwPlayStart, DWORD dwPlayLength)
{
    (void)self; (void)dwPlayStart; (void)dwPlayLength;
    return S_OK;
}

static HRESULT __stdcall shim_buf_Lock(IDirectSoundBuffer8 *self, DWORD dwOffset, DWORD dwBytes,
                                       void **ppvAudioPtr1, DWORD *pdwAudioBytes1,
                                       void **ppvAudioPtr2, DWORD *pdwAudioBytes2, DWORD dwFlags)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);

    if (!buf->raw_data || buf->raw_size == 0) {
        if (ppvAudioPtr1) *ppvAudioPtr1 = NULL;
        if (pdwAudioBytes1) *pdwAudioBytes1 = 0;
        if (ppvAudioPtr2) *ppvAudioPtr2 = NULL;
        if (pdwAudioBytes2) *pdwAudioBytes2 = 0;
        return E_FAIL;
    }

    if (dwFlags & 0x00000001u) {
        DWORD play_pos = 0, write_pos = 0;
        self->lpVtbl->GetCurrentPosition(self, &play_pos, &write_pos);
        dwOffset = write_pos;
    }
    if (dwFlags & 0x00000002u) {
        dwOffset = 0;
        dwBytes = buf->raw_size;
    }

    dwOffset %= buf->raw_size;
    if (dwBytes == 0 || dwBytes > buf->raw_size) {
        dwBytes = buf->raw_size;
    }

    if (dwOffset + dwBytes <= buf->raw_size) {
        if (ppvAudioPtr1) *ppvAudioPtr1 = (uint8_t *)buf->raw_data + dwOffset;
        if (pdwAudioBytes1) *pdwAudioBytes1 = dwBytes;
        if (ppvAudioPtr2) *ppvAudioPtr2 = NULL;
        if (pdwAudioBytes2) *pdwAudioBytes2 = 0;
    } else {
        DWORD part1 = buf->raw_size - dwOffset;
        DWORD part2 = dwBytes - part1;
        if (ppvAudioPtr1) *ppvAudioPtr1 = (uint8_t *)buf->raw_data + dwOffset;
        if (pdwAudioBytes1) *pdwAudioBytes1 = part1;
        if (ppvAudioPtr2) *ppvAudioPtr2 = (uint8_t *)buf->raw_data;
        if (pdwAudioBytes2) *pdwAudioBytes2 = part2;
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_Unlock(IDirectSoundBuffer8 *self, void *p1, DWORD n1, void *p2, DWORD n2)
{
    (void)p1; (void)n1; (void)p2; (void)n2;
    ShimSoundBuffer *buf = shim_buf_from_iface(self);

    if (buf->raw_data && buf->raw_size > 0) {
        if (buf->format_tag == WAVE_FORMAT_XBOX_ADPCM) {
            int16_t *new_samples = NULL;
            size_t total_samples = 0;
            int align = buf->block_align ? buf->block_align : ((buf->channels == 2) ? 72 : 36);
            Audio_DecodeXboxADPCM((const uint8_t *)buf->raw_data, buf->raw_size,
                                  &new_samples, &total_samples,
                                  buf->channels, align);
            int ch = buf->channels ? buf->channels : 1;
            size_t new_frames = total_samples / (size_t)ch;

            if (g_audio.lock) SDL_LockMutex(g_audio.lock);
            int16_t *old_samples = buf->sound.samples;
            buf->sound.samples = new_samples;
            buf->sound.channels = ch;
            buf->sound.sample_rate = buf->frequency ? buf->frequency : 44100;
            buf->sound.total_frames = new_frames;
            if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);

            free(old_samples);
        } else {
            int16_t *new_samples = (int16_t *)malloc(buf->raw_size);
            if (new_samples) {
                memcpy(new_samples, buf->raw_data, buf->raw_size);
                int ch = buf->channels ? buf->channels : 1;
                size_t new_frames = buf->raw_size / ((size_t)ch * sizeof(int16_t));

                if (g_audio.lock) SDL_LockMutex(g_audio.lock);
                int16_t *old_samples = buf->sound.samples;
                buf->sound.samples = new_samples;
                buf->sound.channels = ch;
                buf->sound.sample_rate = buf->frequency ? buf->frequency : 44100;
                buf->sound.total_frames = new_frames;
                if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);

                free(old_samples);
            }
        }
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_SetCurrentPosition(IDirectSoundBuffer8 *self, DWORD dwNewPosition)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    uint64_t frame = 0;
    if (buf->format_tag == WAVE_FORMAT_XBOX_ADPCM) {
        uint32_t align = buf->block_align ? buf->block_align : ((buf->channels == 2) ? 72u : 36u);
        frame = ((uint64_t)dwNewPosition * 64u) / align;
    } else {
        uint32_t frame_bytes = (buf->sound.channels ? buf->sound.channels : 1) * sizeof(int16_t);
        frame = frame_bytes ? (dwNewPosition / frame_bytes) : 0;
    }
    buf->play_cursor_fp = frame << 16;
    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        if (g_audio.lock) SDL_LockMutex(g_audio.lock);
        g_audio.sfx_voices[buf->voice_slot].play_cursor_fp = buf->play_cursor_fp;
        if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_GetCurrentPosition(IDirectSoundBuffer8 *self, DWORD *pdwPlay, DWORD *pdwWrite)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    uint64_t cur_fp = buf->play_cursor_fp;
    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS && g_audio.sfx_voices[buf->voice_slot].active) {
        cur_fp = g_audio.sfx_voices[buf->voice_slot].play_cursor_fp;
    }
    uint64_t frame = cur_fp >> 16;
    DWORD pos = 0;
    if (buf->format_tag == WAVE_FORMAT_XBOX_ADPCM) {
        uint32_t align = buf->block_align ? buf->block_align : ((buf->channels == 2) ? 72u : 36u);
        pos = (DWORD)((frame * align) / 64u);
    } else {
        uint32_t frame_bytes = (buf->sound.channels ? buf->sound.channels : 1) * sizeof(int16_t);
        pos = (DWORD)(frame * frame_bytes);
    }
    if (buf->raw_size > 0) pos %= buf->raw_size;
    if (pdwPlay)  *pdwPlay = pos;
    if (pdwWrite) *pdwWrite = pos;
    return S_OK;
}

static HRESULT __stdcall shim_buf_GetStatus(IDirectSoundBuffer8 *self, DWORD *pdwStatus)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    DWORD status = 0;
    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        if (g_audio.sfx_voices[buf->voice_slot].active) {
            status |= DSBSTATUS_PLAYING;
            if (g_audio.sfx_voices[buf->voice_slot].looping) {
                status |= DSBSTATUS_LOOPING;
            }
        }
    }
    if (pdwStatus) *pdwStatus = status;
    return S_OK;
}

static HRESULT __stdcall shim_buf_Play(IDirectSoundBuffer8 *self, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags)
{
    (void)dwReserved1; (void)dwReserved2;
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    if (!buf->sound.samples || buf->sound.total_frames == 0) return S_OK;

    if (g_audio.lock) SDL_LockMutex(g_audio.lock);

    /* Allocate or reuse voice slot */
    int slot = buf->voice_slot;
    if (slot < 0 || slot >= MAX_SFX_CHANNELS || g_audio.sfx_voices[slot].sound != &buf->sound) {
        for (int i = 0; i < MAX_SFX_CHANNELS; i++) {
            if (!g_audio.sfx_voices[i].active) {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) slot = 0; /* Fallback to voice 0 */

    buf->voice_slot = slot;
    buf->looping = (dwFlags & DSBPLAY_LOOPING) != 0;

    AudioVoice *v = &g_audio.sfx_voices[slot];
    v->sound = &buf->sound;
    v->play_cursor_fp = buf->play_cursor_fp;
    v->sample_rate = buf->frequency ? buf->frequency : (buf->sound.sample_rate > 0 ? (DWORD)buf->sound.sample_rate : 44100u);
    v->volume = buf->linear_volume;
    v->pan_l = buf->pan_l;
    v->pan_r = buf->pan_r;
    v->looping = buf->looping;
    v->active = true;

    if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);

    static int play_count = 0;
    if (play_count < 25) {
        fprintf(stderr, "[AUDIO] SFX voice %d triggered: %zu frames, %d ch, %u Hz, vol=%.3f (mb=%d), pan=(%.2f, %.2f), loop=%d\n",
                slot, buf->sound.total_frames, buf->sound.channels, v->sample_rate,
                buf->linear_volume, (int)buf->volume_mb, buf->pan_l, buf->pan_r, (int)buf->looping);
        play_count++;
    }

    return S_OK;
}

static HRESULT __stdcall shim_buf_Stop(IDirectSoundBuffer8 *self)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        Audio_StopSound(buf->voice_slot);
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_SetVolume(IDirectSoundBuffer8 *self, LONG lVolume)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    buf->volume_mb = lVolume;
    buf->linear_volume = Audio_MillibelsToLinear(lVolume);

    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        Audio_SetVoiceVolume(buf->voice_slot, buf->linear_volume);
    }

    return S_OK;
}

static HRESULT __stdcall shim_buf_SetFrequency(IDirectSoundBuffer8 *self, DWORD dwFrequency)
{
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    buf->frequency = dwFrequency;
    buf->sound.sample_rate = dwFrequency;

    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        if (g_audio.lock) SDL_LockMutex(g_audio.lock);
        g_audio.sfx_voices[buf->voice_slot].sample_rate = dwFrequency;
        if (g_audio.lock) SDL_UnlockMutex(g_audio.lock);
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_SetMaxDistance(IDirectSoundBuffer8 *s, float f, DWORD a) { (void)s; (void)f; (void)a; return S_OK; }
static HRESULT __stdcall shim_buf_SetMinDistance(IDirectSoundBuffer8 *s, float f, DWORD a) { (void)s; (void)f; (void)a; return S_OK; }

static HRESULT __stdcall shim_buf_SetPosition(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dwApply)
{
    (void)dwApply;
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    buf->pos_x = x;
    buf->pos_y = y;
    buf->pos_z = z;
    buf->has_3d_pos = true;

    /* Constant-power panning from horizontal relative coordinates (x, z) */
    float dist_sq = x * x + z * z;
    if (dist_sq < 0.0001f) {
        buf->pan_l = 1.0f;
        buf->pan_r = 1.0f;
    } else {
        float dist = sqrtf(dist_sq);
        float norm_x = x / dist; /* -1.0 (left) to +1.0 (right) */
        if (norm_x < -1.0f) norm_x = -1.0f;
        if (norm_x > 1.0f)  norm_x = 1.0f;

        float angle = (norm_x + 1.0f) * (3.14159265f / 4.0f);
        float pl = cosf(angle) * 1.41421356f;
        float pr = sinf(angle) * 1.41421356f;
        if (pl > 1.0f) pl = 1.0f;
        if (pr > 1.0f) pr = 1.0f;
        buf->pan_l = pl;
        buf->pan_r = pr;
    }

    if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
        Audio_SetVoicePan(buf->voice_slot, buf->pan_l, buf->pan_r);
    }
    return S_OK;
}

static HRESULT __stdcall shim_buf_SetVelocity(IDirectSoundBuffer8 *s, float x, float y, float z, DWORD a) { (void)s; (void)x; (void)y; (void)z; (void)a; return S_OK; }
static HRESULT __stdcall shim_buf_SetConeAngles(IDirectSoundBuffer8 *s, DWORD i, DWORD o, DWORD a) { (void)s; (void)i; (void)o; (void)a; return S_OK; }
static HRESULT __stdcall shim_buf_SetConeOutsideVolume(IDirectSoundBuffer8 *s, LONG v, DWORD a) { (void)s; (void)v; (void)a; return S_OK; }

static HRESULT __stdcall shim_buf_SetMixBins(IDirectSoundBuffer8 *self, const void *pMixBins)
{
    if (!self || !pMixBins) return S_OK;
    ShimSoundBuffer *buf = shim_buf_from_iface(self);
    const DSMIXBINS *mb = (const DSMIXBINS *)pMixBins;
    if (mb->dwMixBinCount == 0 || !mb->lpMixBinVolumePairs) return S_OK;

    const DSMIXBINVOLUMEPAIR *pairs = (const DSMIXBINVOLUMEPAIR *)mb->lpMixBinVolumePairs;
    float sum_l = 0.0f;
    float sum_r = 0.0f;
    int count_l = 0;
    int count_r = 0;

    for (DWORD i = 0; i < mb->dwMixBinCount; i++) {
        DWORD bin = pairs[i].dwMixBin;
        LONG vol_mb = pairs[i].lVolume;
        float linear = Audio_MillibelsToLinear(vol_mb);

        /* Xbox MixBins:
         * 0 = Front Left, 4 = Back Left, 6 = Side Left, 8 = I3DL2/Reverb Left
         * 1 = Front Right, 5 = Back Right, 7 = Side Right, 9 = Reverb Right
         * 2 = Center, 3 = LFE (Subwoofer) */
        if (bin == 0 || bin == 4 || bin == 6 || bin == 8) {
            sum_l += linear;
            count_l++;
        } else if (bin == 1 || bin == 5 || bin == 7 || bin == 9) {
            sum_r += linear;
            count_r++;
        } else {
            /* Center / Subwoofer -> distributed equally */
            sum_l += linear * 0.7071f;
            sum_r += linear * 0.7071f;
            count_l++;
            count_r++;
        }
    }

    if (count_l > 0 || count_r > 0) {
        buf->has_custom_pan = true;
        buf->pan_l = (sum_l > 1.0f) ? 1.0f : sum_l;
        buf->pan_r = (sum_r > 1.0f) ? 1.0f : sum_r;
        if (buf->voice_slot >= 0 && buf->voice_slot < MAX_SFX_CHANNELS) {
            Audio_SetVoicePan(buf->voice_slot, buf->pan_l, buf->pan_r);
        }
    }
    return S_OK;
}

static const IDirectSoundBuffer8Vtbl g_shim_buffer_vtbl = {
    .QueryInterface = shim_buf_QueryInterface,
    .AddRef = shim_buf_AddRef,
    .Release = shim_buf_Release,
    .SetBufferData = shim_buf_SetBufferData,
    .SetPlayRegion = shim_buf_SetPlayRegion,
    .Lock = shim_buf_Lock,
    .Unlock = shim_buf_Unlock,
    .SetCurrentPosition = shim_buf_SetCurrentPosition,
    .GetCurrentPosition = shim_buf_GetCurrentPosition,
    .GetStatus = shim_buf_GetStatus,
    .Play = shim_buf_Play,
    .Stop = shim_buf_Stop,
    .SetVolume = shim_buf_SetVolume,
    .SetFrequency = shim_buf_SetFrequency,
    .SetMaxDistance = shim_buf_SetMaxDistance,
    .SetMinDistance = shim_buf_SetMinDistance,
    .SetPosition = shim_buf_SetPosition,
    .SetVelocity = shim_buf_SetVelocity,
    .SetConeAngles = shim_buf_SetConeAngles,
    .SetConeOutsideVolume = shim_buf_SetConeOutsideVolume,
    .SetMixBins = shim_buf_SetMixBins,
};

/* ── IDirectSound8 Object ── */

typedef struct ShimDirectSound {
    IDirectSound8 iface;
    LONG ref_count;
} ShimDirectSound;

static HRESULT __stdcall shim_ds_QueryInterface(IDirectSound8 *self, const void *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall shim_ds_AddRef(IDirectSound8 *self)
{
    ShimDirectSound *ds = (ShimDirectSound *)self;
    return (ULONG)__sync_add_and_fetch(&ds->ref_count, 1);
}

static ULONG __stdcall shim_ds_Release(IDirectSound8 *self)
{
    ShimDirectSound *ds = (ShimDirectSound *)self;
    LONG ref = __sync_sub_and_fetch(&ds->ref_count, 1);
    if (ref <= 0) {
        free(ds);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall shim_ds_CreateSoundBuffer(IDirectSound8 *self, const DSBUFFERDESC *desc,
                                                  IDirectSoundBuffer8 **ppBuffer, void *pUnkOuter)
{
    (void)self; (void)pUnkOuter;
    if (!ppBuffer) return E_INVALIDARG;

    ShimSoundBuffer *buf = (ShimSoundBuffer *)calloc(1, sizeof(ShimSoundBuffer));
    if (!buf) return E_OUTOFMEMORY;

    buf->iface.lpVtbl = &g_shim_buffer_vtbl;
    buf->ref_count = 1;
    buf->volume_mb = 0;
    buf->linear_volume = 1.0f;
    buf->pan_mb = 0;
    buf->pan_l = 1.0f;
    buf->pan_r = 1.0f;
    buf->has_3d_pos = false;
    buf->has_custom_pan = false;
    buf->play_cursor_fp = 0;
    buf->frequency = 44100;
    buf->channels = 2;
    buf->bits_per_sample = 16;
    buf->format_tag = 0x0001;
    buf->voice_slot = -1;

    if (desc && desc->lpwfxFormat) {
        WAVEFORMATEX *wfx = (WAVEFORMATEX *)desc->lpwfxFormat;
        buf->format_tag = wfx->wFormatTag;
        buf->channels = wfx->nChannels ? wfx->nChannels : 2;
        buf->frequency = wfx->nSamplesPerSec ? wfx->nSamplesPerSec : 44100;
        buf->bits_per_sample = wfx->wBitsPerSample ? wfx->wBitsPerSample : 16;
        buf->block_align = wfx->nBlockAlign;
    }

    if (desc && desc->dwBufferBytes > 0) {
        buf->raw_data = calloc(1, desc->dwBufferBytes);
        buf->raw_size = desc->dwBufferBytes;
    }

    *ppBuffer = &buf->iface;

    static int create_count = 0;
    if (create_count < 20) {
        fprintf(stderr, "[AUDIO] CreateSoundBuffer: %u bytes, tag=0x%04X, %d ch, %u Hz\n",
                desc ? desc->dwBufferBytes : 0, buf->format_tag, buf->channels, buf->frequency);
        create_count++;
    }

    return S_OK;
}

static HRESULT __stdcall shim_ds_CreateSoundStream(IDirectSound8 *s, const void *d, void **p, void *u) { (void)s; (void)d; (void)p; (void)u; return S_OK; }
static HRESULT __stdcall shim_ds_SetMixBinHeadroom(IDirectSound8 *s, DWORD b, DWORD h) { (void)s; (void)b; (void)h; return S_OK; }
static HRESULT __stdcall shim_ds_SetPosition(IDirectSound8 *s, float x, float y, float z, DWORD a) { (void)s; (void)x; (void)y; (void)z; (void)a; return S_OK; }
static HRESULT __stdcall shim_ds_SetVelocity(IDirectSound8 *s, float x, float y, float z, DWORD a) { (void)s; (void)x; (void)y; (void)z; (void)a; return S_OK; }
static HRESULT __stdcall shim_ds_SetDistanceFactor(IDirectSound8 *s, float f, DWORD a) { (void)s; (void)f; (void)a; return S_OK; }
static HRESULT __stdcall shim_ds_SetRolloffFactor(IDirectSound8 *s, float f, DWORD a) { (void)s; (void)f; (void)a; return S_OK; }
static HRESULT __stdcall shim_ds_SetDopplerFactor(IDirectSound8 *s, float f, DWORD a) { (void)s; (void)f; (void)a; return S_OK; }
static HRESULT __stdcall shim_ds_SetOrientation(IDirectSound8 *s, float xf, float yf, float zf, float xt, float yt, float zt, DWORD a) { (void)s; (void)xf; (void)yf; (void)zf; (void)xt; (void)yt; (void)zt; (void)a; return S_OK; }
static HRESULT __stdcall shim_ds_CommitDeferredSettings(IDirectSound8 *s) { (void)s; return S_OK; }

static const IDirectSound8Vtbl g_shim_dsound_vtbl = {
    .QueryInterface = shim_ds_QueryInterface,
    .AddRef = shim_ds_AddRef,
    .Release = shim_ds_Release,
    .CreateSoundBuffer = shim_ds_CreateSoundBuffer,
    .CreateSoundStream = shim_ds_CreateSoundStream,
    .SetMixBinHeadroom = shim_ds_SetMixBinHeadroom,
    .SetPosition = shim_ds_SetPosition,
    .SetVelocity = shim_ds_SetVelocity,
    .SetDistanceFactor = shim_ds_SetDistanceFactor,
    .SetRolloffFactor = shim_ds_SetRolloffFactor,
    .SetDopplerFactor = shim_ds_SetDopplerFactor,
    .SetOrientation = shim_ds_SetOrientation,
    .CommitDeferredSettings = shim_ds_CommitDeferredSettings,
};

HRESULT Audio_DirectSoundCreate(void *pGuid, IDirectSound8 **ppDS, void *pUnkOuter)
{
    (void)pGuid; (void)pUnkOuter;
    if (!ppDS) return E_INVALIDARG;

    ShimDirectSound *ds = (ShimDirectSound *)calloc(1, sizeof(ShimDirectSound));
    if (!ds) return E_OUTOFMEMORY;

    ds->iface.lpVtbl = &g_shim_dsound_vtbl;
    ds->ref_count = 1;
    *ppDS = &ds->iface;

    fprintf(stderr, "[AUDIO] DirectSound object created.\n");
    return S_OK;
}
