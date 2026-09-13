/* The shared MIDI parser was written for PrBoom's logging layer.  Duke's
 * PAPP already has its own launcher logger, and parser warnings are not
 * allowed to call into an unavailable Doom subsystem. */
#ifndef DUKE_PAPP_LPRINTF_H
#define DUKE_PAPP_LPRINTF_H

#define LO_WARN 0
#define lprintf(...) ((void)0)

#endif
