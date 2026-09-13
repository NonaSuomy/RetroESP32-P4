#ifndef SDL_audio_h_
#define SDL_audio_h_

#include "SDL_stdinc.h"
#include "SDL_rwops.h"
#include "SDL_endian.h"

#define SAMPLECOUNT 512
#define SAMPLERATE 32000
#define SAMPLESIZE 2

typedef struct SDL_AudioCVT {
    int needed;
    Uint16 src_format;
    Uint16 dst_format;
    double rate_incr;
    Uint8 *buf;
    int len;
    int len_cvt;
    int len_mult;
    double len_ratio;
    void (SDLCALL *filters[10])(struct SDL_AudioCVT *cvt, Uint16 format);
    int filter_index;
} SDL_AudioCVT;

typedef struct {
    int freq;
    Uint16 format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint32 size;
    void (*callback)(void *userdata, Uint8 *stream, int len);
    void *userdata;
} SDL_AudioSpec;

typedef Uint16 SDL_AudioFormat;

#define SDL_AUDIO_MASK_BITSIZE       0xFF
#define SDL_AUDIO_MASK_DATATYPE      (1 << 8)
#define SDL_AUDIO_MASK_ENDIAN        (1 << 12)
#define SDL_AUDIO_MASK_SIGNED        (1 << 15)
#define AUDIO_U8        0x0008
#define AUDIO_S8        0x8008
#define AUDIO_U16LSB    0x0010
#define AUDIO_S16LSB    0x8010
#define AUDIO_U16MSB    0x1010
#define AUDIO_S16MSB    0x9010
#define AUDIO_U16       AUDIO_U16LSB
#define AUDIO_S16       AUDIO_S16LSB
#define AUDIO_S16SYS    AUDIO_S16LSB
#define AUDIO_U16SYS    AUDIO_U16LSB

#define SDL_MIX_MAXVOLUME 128

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained);
void SDL_PauseAudio(int pause_on);
void SDL_CloseAudio(void);
int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, Uint16 src_format,
                      Uint8 src_channels, int src_rate,
                      Uint16 dst_format, Uint8 dst_channels, int dst_rate);
int SDL_ConvertAudio(SDL_AudioCVT *cvt);
void SDL_LockAudio(void);
void SDL_UnlockAudio(void);
void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume);
SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc,
                              SDL_AudioSpec *spec, Uint8 **audio_buf,
                              Uint32 *audio_len);

#endif
