#ifndef SDL_system_h_
#define SDL_system_h_

#include "SDL_stdinc.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

typedef struct {
    Uint8 major;
    Uint8 minor;
    Uint8 patch;
} SDL_version;

#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 0
#define SDL_PATCHLEVEL 9
#define SDL_VERSION(x) do { \
    (x)->major = SDL_MAJOR_VERSION; \
    (x)->minor = SDL_MINOR_VERSION; \
    (x)->patch = SDL_PATCHLEVEL; \
} while (0)

const SDL_version *SDL_Linked_Version(void);
int SDL_Init(Uint32 flags);
void SDL_Quit(void);
void SDL_Delay(Uint32 ms);
void SDL_InitSD(void);
void Check(const char *str);

struct _reent;
FILE *__fopen(const char *path, const char *mode);
int __fclose(FILE *stream);
size_t __fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t __fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int __fseek(FILE *stream, long offset, int whence);
long __ftell(FILE *stream);
int __open(const char *path, int flags, ...);
int __close(int fd);
ssize_t __read(int fd, void *buf, size_t count);
ssize_t __write(int fd, const void *buf, size_t count);
off_t __lseek(int fd, off_t offset, int whence);
int __unlink(const char *path);
int __stat(const char *path, struct stat *st);
int __mkdir(const char *path, mode_t mode);

struct SDL_mutex;
typedef struct SDL_mutex SDL_mutex;
SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);

#define SDL_mutexP(mutex) SDL_LockMutex(mutex)
#define SDL_mutexV(mutex) SDL_UnlockMutex(mutex)

/* The original port uses these two FreeRTOS calls only as cooperative
 * scheduler hints. A PAPP runs on the launcher's task, so a short delay is
 * the portable equivalent and also gives the watchdog time to run. */
#define taskYIELD() SDL_Delay(1)
#define vTaskDelay(ms) SDL_Delay((Uint32)(ms))

#endif
