#ifndef AUDIO_SHIM_H
#define AUDIO_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>    /* the Windows SDL2 build: HRESULT, DWORD, ... */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Windows / COM Basic Types ── */
#ifndef _WIN32
#ifndef HRESULT
typedef int32_t HRESULT;
#endif
#ifndef ULONG
typedef uint32_t ULONG;
#endif
#ifndef LONG
typedef int32_t LONG;
#endif
#ifndef DWORD
typedef uint32_t DWORD;
#endif
#ifndef WORD
typedef uint16_t WORD;
#endif
#ifndef BYTE
typedef uint8_t BYTE;
#endif
#ifndef BOOL
typedef int BOOL;
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef S_OK
#define S_OK ((HRESULT)0x00000000L)
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif
#ifndef E_INVALIDARG
#define E_INVALIDARG ((HRESULT)0x80070057L)
#endif
#ifndef E_OUTOFMEMORY
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#endif
#ifndef __stdcall
#define __stdcall
#endif
#endif /* !_WIN32 */

/* ── Lifecycle functions called from main.c ── */
void Audio_Init(void);
void Audio_InitOffline(void);
void Audio_RenderOffline(int16_t *out_samples, int num_frames);
void Audio_Update(void);
void Audio_Shutdown(void);

/* ── Forward Declarations ── */
typedef struct IDirectSound8 IDirectSound8;
typedef struct IDirectSoundBuffer8 IDirectSoundBuffer8;

/* ── High-Level Audio Playback API ── */
int   Audio_PlayMusic(const char *rel_path, int loop);
void  Audio_StopMusic(void);
void  Audio_SetMusicVolume(float volume);
void  Audio_SetMusicVolumeMillibels(int32_t mb);
float Audio_GetMusicVolume(void);

void  Audio_SetSfxVolume(float volume);
void  Audio_SetSfxVolumeMillibels(int32_t mb);
float Audio_GetSfxVolume(void);

int   Audio_PlaySound(const char *rel_path, float volume);
int   Audio_PlaySoundMemory(const void *data, size_t size, float volume, int loop);
void  Audio_StopSound(int voice_slot);
void  Audio_SetVoiceVolume(int voice_slot, float volume);
void  Audio_SetVoiceVolumeMillibels(int voice_slot, int32_t mb);
void  Audio_SetVoicePan(int voice_slot, float pan_l, float pan_r);
void  Audio_SetVoiceSpatial(int voice_slot, float pan_l, float pan_r, int itd_delay);
HRESULT Audio_SetBufferPan(IDirectSoundBuffer8 *self, LONG lPan);
int   Audio_GetActiveSfxVoiceCount(void);

/* ── Decibel / Millibel Utilities ── */
/* DirectSound volume is in millibels (hundredths of a dB): -10000 (mute) to 0 (max).
 * Conversion: linear = pow(10.0, mb / 2000.0) */
float   Audio_MillibelsToLinear(int32_t mb);
int32_t Audio_LinearToMillibels(float linear);

/* ── Xbox ADPCM (0x0069) Decoder ── */
int Audio_DecodeXboxADPCM(const uint8_t *src, size_t src_size,
                          int16_t **out_pcm, size_t *out_samples,
                          int channels, int block_align);

/* ── DirectSound8 / XACT Compatibility Types & Bridges ── */
typedef struct IDirectSound8 IDirectSound8;
typedef struct IDirectSoundBuffer8 IDirectSoundBuffer8;

#ifndef _WAVEFORMATEX_
#define _WAVEFORMATEX_
typedef struct tWAVEFORMATEX {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
} WAVEFORMATEX;
#endif

#define WAVE_FORMAT_XBOX_ADPCM 0x0069

#ifndef DSBPLAY_LOOPING
#define DSBPLAY_LOOPING 0x00000001
#endif
#ifndef DSBSTATUS_PLAYING
#define DSBSTATUS_PLAYING 0x00000001
#endif
#ifndef DSBSTATUS_LOOPING
#define DSBSTATUS_LOOPING 0x00000002
#endif

typedef struct DSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    void *lpwfxFormat;
    DWORD dwMixBinMask;
    DWORD dwInputMixBin;
} DSBUFFERDESC;

typedef struct DSMIXBINS {
    DWORD dwMixBinCount;
    void *lpMixBinVolumePairs;
} DSMIXBINS;

typedef struct DSMIXBINVOLUMEPAIR {
    DWORD dwMixBin;
    LONG  lVolume;
} DSMIXBINVOLUMEPAIR;

typedef struct IDirectSoundBuffer8Vtbl {
    HRESULT (__stdcall *QueryInterface)(IDirectSoundBuffer8 *self, const void *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirectSoundBuffer8 *self);
    ULONG   (__stdcall *Release)(IDirectSoundBuffer8 *self);
    HRESULT (__stdcall *SetBufferData)(IDirectSoundBuffer8 *self, const void *pvBufferData, DWORD dwBufferBytes);
    HRESULT (__stdcall *SetPlayRegion)(IDirectSoundBuffer8 *self, DWORD dwPlayStart, DWORD dwPlayLength);
    HRESULT (__stdcall *Lock)(IDirectSoundBuffer8 *self, DWORD dwOffset, DWORD dwBytes,
                              void **ppvAudioPtr1, DWORD *pdwAudioBytes1,
                              void **ppvAudioPtr2, DWORD *pdwAudioBytes2, DWORD dwFlags);
    HRESULT (__stdcall *Unlock)(IDirectSoundBuffer8 *self, void *pvAudioPtr1, DWORD dwAudioBytes1,
                                void *pvAudioPtr2, DWORD dwAudioBytes2);
    HRESULT (__stdcall *SetCurrentPosition)(IDirectSoundBuffer8 *self, DWORD dwNewPosition);
    HRESULT (__stdcall *GetCurrentPosition)(IDirectSoundBuffer8 *self, DWORD *pdwCurrentPlayCursor, DWORD *pdwCurrentWriteCursor);
    HRESULT (__stdcall *GetStatus)(IDirectSoundBuffer8 *self, DWORD *pdwStatus);
    HRESULT (__stdcall *Play)(IDirectSoundBuffer8 *self, DWORD dwReserved1, DWORD dwReserved2, DWORD dwFlags);
    HRESULT (__stdcall *Stop)(IDirectSoundBuffer8 *self);
    HRESULT (__stdcall *SetVolume)(IDirectSoundBuffer8 *self, LONG lVolume);
    HRESULT (__stdcall *SetFrequency)(IDirectSoundBuffer8 *self, DWORD dwFrequency);
    HRESULT (__stdcall *SetMaxDistance)(IDirectSoundBuffer8 *self, float flMaxDistance, DWORD dwApply);
    HRESULT (__stdcall *SetMinDistance)(IDirectSoundBuffer8 *self, float flMinDistance, DWORD dwApply);
    HRESULT (__stdcall *SetPosition)(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetVelocity)(IDirectSoundBuffer8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetConeAngles)(IDirectSoundBuffer8 *self, DWORD dwInside, DWORD dwOutside, DWORD dwApply);
    HRESULT (__stdcall *SetConeOutsideVolume)(IDirectSoundBuffer8 *self, LONG lConeOutsideVolume, DWORD dwApply);
    HRESULT (__stdcall *SetMixBins)(IDirectSoundBuffer8 *self, const void *pMixBins);
} IDirectSoundBuffer8Vtbl;

struct IDirectSoundBuffer8 {
    const IDirectSoundBuffer8Vtbl *lpVtbl;
};

typedef struct IDirectSound8Vtbl {
    HRESULT (__stdcall *QueryInterface)(IDirectSound8 *self, const void *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirectSound8 *self);
    ULONG   (__stdcall *Release)(IDirectSound8 *self);
    HRESULT (__stdcall *CreateSoundBuffer)(IDirectSound8 *self, const DSBUFFERDESC *desc, IDirectSoundBuffer8 **ppBuffer, void *pUnkOuter);
    HRESULT (__stdcall *CreateSoundStream)(IDirectSound8 *self, const void *desc, void **ppStream, void *pUnkOuter);
    HRESULT (__stdcall *SetMixBinHeadroom)(IDirectSound8 *self, DWORD dwMixBin, DWORD dwHeadroom);
    HRESULT (__stdcall *SetPosition)(IDirectSound8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetVelocity)(IDirectSound8 *self, float x, float y, float z, DWORD dwApply);
    HRESULT (__stdcall *SetDistanceFactor)(IDirectSound8 *self, float f, DWORD dwApply);
    HRESULT (__stdcall *SetRolloffFactor)(IDirectSound8 *self, float f, DWORD dwApply);
    HRESULT (__stdcall *SetDopplerFactor)(IDirectSound8 *self, float f, DWORD dwApply);
    HRESULT (__stdcall *SetOrientation)(IDirectSound8 *self, float xf, float yf, float zf, float xt, float yt, float zt, DWORD dwApply);
    HRESULT (__stdcall *CommitDeferredSettings)(IDirectSound8 *self);
} IDirectSound8Vtbl;

struct IDirectSound8 {
    const IDirectSound8Vtbl *lpVtbl;
};

/* DirectSound creation functions */
HRESULT Audio_DirectSoundCreate(void *pGuid, IDirectSound8 **ppDS, void *pUnkOuter);
HRESULT xbox_DirectSoundCreate(void *pGuid, IDirectSound8 **ppDS, void *pUnkOuter);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SHIM_H */
