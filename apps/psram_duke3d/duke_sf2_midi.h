#ifndef DUKE_SF2_MIDI_H
#define DUKE_SF2_MIDI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int duke_sf2_midi_init(int sample_rate);
void duke_sf2_midi_shutdown(void);
int duke_sf2_midi_play(const void *midi_data, size_t midi_length, int looping);
void duke_sf2_midi_stop(void);
void duke_sf2_midi_pause(int paused);
int duke_sf2_midi_is_playing(void);
void duke_sf2_midi_set_volume(int volume);
void duke_sf2_midi_render(int16_t *mono, int frames);

#ifdef __cplusplus
}
#endif

#endif
