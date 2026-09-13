#ifndef DUKE_PAPP_OPL_MIDI_H
#define DUKE_PAPP_OPL_MIDI_H

#include <stddef.h>
#include <stdint.h>

/* All calls except render/stop are made before the audio task starts.  The
 * render and stop calls are made by the Duke audio task. */
int duke_opl_midi_init(unsigned sample_rate);
void duke_opl_midi_shutdown(void);
void duke_opl_midi_set_volume(int volume);
void duke_opl_midi_set_timbres(const unsigned char *data, size_t length);
int duke_opl_midi_play(const void *data, size_t length, int looping);
void duke_opl_midi_stop(void);
void duke_opl_midi_pause(int paused);
int duke_opl_midi_is_playing(void);
void duke_opl_midi_render(int16_t *mono, int frames);

#endif
