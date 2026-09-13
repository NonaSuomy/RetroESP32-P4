#ifndef _GB_CONFIG_H
#define _GB_CONFIG_H

/* PAPP build configuration. The upstream game stores its compressed banks
 * in the generated data headers, so the SD/dataflash reader is not needed. */
#define use_lib_dataflash
#define use_lib_width 320
#define use_lib_height 200
#define use_lib_offset_x 0
#define use_lib_offset_y 0
#define use_lib_video 0
#define use_lib_log_serial

#endif
