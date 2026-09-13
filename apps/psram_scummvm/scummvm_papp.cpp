/*
 * ScummVM AGI/SCI PAPP backend.
 *
 * The ScummVM engine libraries are built once for the P4 and this file is
 * the small platform layer that connects them to the launcher's append-only
 * PAPP ABI.  The same source is used for both scummvm-agi.papp and
 * scummvm-sci.papp; only the selected ScummVM engine archive changes.
 */
#define PAPP_APP_SIDE 1
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "psram_app.h"
#include "config.h"
#include "base/main.h"
#include "common/system.h"
#include "common/config-manager.h"
#include "common/fs.h"
#include "common/textconsole.h"
#include "common/rect.h"
#include "common/mutex.h"
#include "common/memstream.h"
#include "backends/modular-backend.h"
#include "backends/graphics/graphics.h"
#include "backends/fs/fs-factory.h"
#include "backends/fs/posix/posix-fs.h"
#include "backends/mixer/mixer.h"
#include "audio/mixer_intern.h"
#include "graphics/surface.h"
#include "backends/events/default/default-events.h"
#include "backends/timer/default/default-timer.h"
#include "backends/saves/default/default-saves.h"
#include "backends/keymapper/hardware-input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

const app_services_t *_papp_svc = NULL;

/* The launcher selects the storage volume at runtime.  Keep the PAPP's
 * historical /sd namespace available through the service resolver, but use
 * the actual selected mount for ScummVM's own filesystem nodes and path
 * settings.  This makes the paths shown in Global Settings match the volume
 * that is really being used and avoids depending on /sd being mounted. */
static char s_scummvm_storage_root[8] = "/sd";
static bool s_scummvm_storage_root_ready = false;

static const char *scummvm_storage_root() {
	if (s_scummvm_storage_root_ready)
		return s_scummvm_storage_root;

	const char *root = "/sd";
	if (_papp_svc && _papp_svc->file_stat) {
		int is_dir = 0;
		long size = 0;
		if (_papp_svc->file_stat("/usb0/roms", &is_dir, &size) == 0 && is_dir)
			root = "/usb0";
	}
	strncpy(s_scummvm_storage_root, root, sizeof(s_scummvm_storage_root) - 1);
	s_scummvm_storage_root[sizeof(s_scummvm_storage_root) - 1] = '\0';
	s_scummvm_storage_root_ready = true;
	if (_papp_svc && _papp_svc->log_printf)
		_papp_svc->log_printf("ScummVM storage root selected: %s\n",
			s_scummvm_storage_root);
	return s_scummvm_storage_root;
}

static bool scummvm_storage_path(char *out, size_t out_size, const char *suffix) {
	if (!out || out_size == 0 || !suffix)
		return false;
	int written = snprintf(out, out_size, "%s%s%s", scummvm_storage_root(),
		suffix[0] == '/' ? "" : "/", suffix);
	return written >= 0 && (size_t)written < out_size;
}

static bool scummvm_rebase_path(const char *input, char *out, size_t out_size) {
	if (!input || !out || out_size == 0)
		return false;

	const char *old_roots[] = { "/sd", "/usb0" };
	for (const char *old_root : old_roots) {
		size_t root_len = strlen(old_root);
		if (strncmp(input, old_root, root_len) != 0 ||
			(input[root_len] != '\0' && input[root_len] != '/'))
			continue;
		int written = snprintf(out, out_size, "%s%s", scummvm_storage_root(),
			input + root_len);
		return written >= 0 && (size_t)written < out_size;
	}
	return false;
}

static void scummvm_rebase_domain_path(Common::ConfigManager::Domain *domain,
		const char *key, const char *domain_name) {
	if (!domain || !key || !domain->contains(key))
		return;
	Common::Path old_path = Common::Path::fromConfig(domain->getVal(key));
	Common::String old_string = old_path.toString('/');
	char new_path[256];
	if (!scummvm_rebase_path(old_string.c_str(), new_path, sizeof(new_path)) ||
		old_string == new_path)
		return;
	domain->setVal(key, Common::Path(new_path).toConfig());
	if (_papp_svc && _papp_svc->log_printf)
		_papp_svc->log_printf("ScummVM path rebased %s/%s: %s -> %s\n",
			domain_name ? domain_name : "?", key, old_string.c_str(), new_path);
}

static void scummvm_rebase_config_paths() {
	static const char *const application_keys[] = {
		"path", "savepath", "extrapath", "iconspath", "pluginspath",
		"themepath", "browser_lastpath"
	};
	Common::ConfigManager::Domain *application =
		ConfMan.getDomain(Common::ConfigManager::kApplicationDomain);
	for (const char *key : application_keys)
		scummvm_rebase_domain_path(application, key,
			Common::ConfigManager::kApplicationDomain);

	for (Common::ConfigManager::DomainMap::iterator domain = ConfMan.beginGameDomains();
		 domain != ConfMan.endGameDomains(); ++domain) {
		scummvm_rebase_domain_path(&domain->_value, "path", domain->_key.c_str());
	}
}

static jmp_buf s_exit_env;
static volatile int s_exit_requested;

extern "C" void app_return_to_launcher(void) {
	 s_exit_requested = 1;
	 longjmp(s_exit_env, 1);
}

/* ── C++ allocation and exit hooks ───────────────────────────────────── */

struct papp_alloc_header {
	size_t size;
	uint32_t magic;
	uint32_t reserved[2];
};

static const uint32_t kPappAllocMagic = 0x50415050U;

static void *papp_alloc(size_t size) {
	if (!_papp_svc) return NULL;
	/* ScummVM creates a very large number of small C++ objects while it
	 * builds the launcher and engine state. Keeping those small allocations
	 * on the internal heap exhausts it before a game starts, so use PSRAM for
	 * the complete ScummVM heap as well as its large buffers. */
	/* Keep the requested size beside each PSRAM allocation so realloc can
	 * copy only the bytes that actually belong to the old object.  The old
	 * wrapper copied the new size unconditionally, corrupting newer ScummVM
	 * containers as they grew. */
	size_t requested = size ? size : 1;
	if (requested > SIZE_MAX - sizeof(papp_alloc_header)) return NULL;
	papp_alloc_header *header = (papp_alloc_header *)_papp_svc->mem_caps_alloc(
		requested + sizeof(papp_alloc_header), PAPP_MEM_CAP_SPIRAM);
	if (!header) return NULL;
	header->size = requested;
	header->magic = kPappAllocMagic;
	header->reserved[0] = 0;
	header->reserved[1] = 0;
	return header + 1;
}

static bool papp_owned(void *ptr, size_t *size = NULL) {
	if (!ptr) return false;
	papp_alloc_header *header = ((papp_alloc_header *)ptr) - 1;
	if (header->magic != kPappAllocMagic) return false;
	if (size) *size = header->size;
	return true;
}

static void papp_free(void *ptr) {
	if (!ptr || !_papp_svc) return;
	if (papp_owned(ptr))
		_papp_svc->mem_free(((papp_alloc_header *)ptr) - 1);
	else
		_papp_svc->mem_free(ptr);
}

static void *papp_realloc(void *old, size_t size) {
	if (!old) return papp_alloc(size);
	if (size == 0) {
		papp_free(old);
		return NULL;
	}
	size_t old_size = 0;
	if (!papp_owned(old, &old_size))
		return _papp_svc ? _papp_svc->mem_realloc(old, size) : NULL;
	void *replacement = papp_alloc(size);
	if (!replacement) return NULL;
	memcpy(replacement, old, old_size < size ? old_size : size);
	papp_free(old);
	return replacement;
}

void *operator new(size_t size) noexcept { return papp_alloc(size); }
void *operator new[](size_t size) noexcept { return papp_alloc(size); }
void operator delete(void *p) noexcept { papp_free(p); }
void operator delete[](void *p) noexcept { papp_free(p); }
void operator delete(void *p, size_t) noexcept { papp_free(p); }
void operator delete[](void *p, size_t) noexcept { papp_free(p); }

extern "C" void *__wrap_malloc(size_t size) { return papp_alloc(size); }
extern "C" void *__wrap_calloc(size_t n, size_t size) {
	size_t total = n * size;
	void *p = papp_alloc(total);
	if (p) memset(p, 0, total);
	return p;
}
extern "C" void *__wrap_realloc(void *old, size_t size) {
	return papp_realloc(old, size);
}
extern "C" void __wrap_free(void *p) { papp_free(p); }
extern "C" void *__wrap__malloc_r(struct _reent *, size_t n) { return __wrap_malloc(n); }
extern "C" void *__wrap__calloc_r(struct _reent *, size_t n, size_t s) { return __wrap_calloc(n, s); }
extern "C" void *__wrap__realloc_r(struct _reent *, void *p, size_t n) { return __wrap_realloc(p, n); }
extern "C" void *__wrap__memalign_r(struct _reent *, size_t, size_t n) { return __wrap_malloc(n); }

extern "C" void _exit(int) { app_return_to_launcher(); for (;;) {} }
extern "C" void __wrap_exit(int status) { _exit(status); }
extern "C" void abort(void) {
	if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM PAPP abort()\n");
	_exit(1);
}

/* newlib's reentrancy/lock hooks are not useful inside one PAPP task. */
struct __lock;
extern "C" void __wrap___retarget_lock_init(struct __lock **p) { if (p) *p = NULL; }
extern "C" void __wrap___retarget_lock_init_recursive(struct __lock **p) { if (p) *p = NULL; }
extern "C" void __wrap___retarget_lock_close(struct __lock *) {}
extern "C" void __wrap___retarget_lock_close_recursive(struct __lock *) {}
extern "C" void __wrap___retarget_lock_acquire(struct __lock *) {}
extern "C" int __wrap___retarget_lock_try_acquire(struct __lock *) { return 0; }
extern "C" void __wrap___retarget_lock_acquire_recursive(struct __lock *) {}
extern "C" int __wrap___retarget_lock_try_acquire_recursive(struct __lock *) { return 0; }
extern "C" void __wrap___retarget_lock_release(struct __lock *) {}
extern "C" void __wrap___retarget_lock_release_recursive(struct __lock *) {}
extern "C" struct _reent *__getreent(void) { return _impure_ptr; }

/* ── stdio wrappers ─────────────────────────────────────────────────── */

/* The FILE objects belong to the launcher's VFS service, so ScummVM cannot
 * safely rely on the app-side libc's feof/ferror state.  Keep the standard
 * indicators at the PAPP boundary instead.  SCI's resource-map parser uses
 * eos() while scanning variable-length map directories; returning a false
 * EOF here makes a valid map look corrupt and eventually produces
 * "GfxDefaultDriver: Unknown view type". */
struct papp_file_state {
	FILE *file;
	unsigned eof : 1;
	unsigned error : 1;
};

static papp_file_state s_file_states[64];

static papp_file_state *papp_file_state_for(FILE *file, bool create) {
	if (!file)
		return NULL;
	papp_file_state *free_slot = NULL;
	for (unsigned i = 0; i < sizeof(s_file_states) / sizeof(s_file_states[0]); ++i) {
		if (s_file_states[i].file == file)
			return &s_file_states[i];
		if (!s_file_states[i].file && !free_slot)
			free_slot = &s_file_states[i];
	}
	if (!create || !free_slot)
		return NULL;
	free_slot->file = file;
	free_slot->eof = 0;
	free_slot->error = 0;
	return free_slot;
}

static void papp_file_state_remove(FILE *file) {
	papp_file_state *state = papp_file_state_for(file, false);
	if (state)
		memset(state, 0, sizeof(*state));
}

static bool scummvm_save_path(const char *path) {
	return path && (strstr(path, "/scummvm/saves/") != NULL ||
		strstr(path, "/scummvm.ini") != NULL);
}

extern "C" FILE *__wrap_fopen(const char *path, const char *mode) {
	FILE *file = _papp_svc && _papp_svc->file_open ?
		(FILE *)_papp_svc->file_open(path, mode) : NULL;
	if (file)
		papp_file_state_for(file, true);
	if (scummvm_save_path(path) && mode && strpbrk(mode, "wa+")) {
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM save open %s mode=%s -> %s errno=%d\n",
				path, mode, file ? "ok" : "failed", file ? 0 : errno);
	}
	return file;
}
extern "C" int __wrap_fclose(FILE *f) {
	int result = (_papp_svc && f) ? _papp_svc->file_close(f) : EOF;
	papp_file_state_remove(f);
	return result;
}
extern "C" size_t __wrap_fread(void *p, size_t s, size_t n, FILE *f) {
	if (!_papp_svc || !f) return 0;
	size_t got = _papp_svc->file_read(p, s, n, f);
	papp_file_state *state = papp_file_state_for(f, true);
	if (state && n > 0 && got < n)
		state->eof = 1;
	return got;
}
extern "C" size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f) {
	size_t written = (_papp_svc && f) ? _papp_svc->file_write(p, s, n, f) : 0;
	if (written != n) {
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM file write short: requested=%u written=%u\n",
				(unsigned)n, (unsigned)written);
	}
	return written;
}
extern "C" int __wrap_fseek(FILE *f, long off, int whence) {
	if (!_papp_svc || !f) return -1;
	int result = _papp_svc->file_seek(f, off, whence);
	papp_file_state *state = papp_file_state_for(f, true);
	if (state && result == 0) {
		state->eof = 0;
		state->error = 0;
	}
	static unsigned trace_seeks = 0;
	if (trace_seeks < 32 && (off < 0 || whence != SEEK_SET || result != 0)) {
		++trace_seeks;
		if (_papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM fseek off=%ld whence=%d result=%d\n",
				off, whence, result);
	}
	return result;
}
extern "C" long __wrap_ftell(FILE *f) {
	return (_papp_svc && f) ? _papp_svc->file_tell(f) : -1;
}
extern "C" int __wrap_getc(FILE *f) {
	unsigned char c = 0;
	if (!_papp_svc || !f)
		return EOF;
	if (_papp_svc->file_read(&c, 1, 1, f) == 1)
		return c;
	papp_file_state *state = papp_file_state_for(f, true);
	if (state)
		state->eof = 1;
	return EOF;
}
extern "C" int __wrap_fgetc(FILE *f) { return __wrap_getc(f); }
/* FILE handles returned by the PAPP service are launcher-owned opaque
 * objects, not the PAPP libc's FILE layout.  ScummVM's direct-launch
 * sidecar and a few POSIX helpers use fgets(), so letting newlib's native
 * implementation inspect this handle causes a load fault inside memchr().
 * Read through the service-backed getc wrapper instead. */
extern "C" char *__wrap_fgets(char *buffer, int size, FILE *f) {
	if (!buffer || size <= 0 || !_papp_svc || !f)
		return NULL;
	int length = 0;
	while (length < size - 1) {
		int c = __wrap_getc(f);
		if (c == EOF)
			break;
		buffer[length++] = (char)c;
		if (c == '\n')
			break;
	}
	if (length == 0)
		return NULL;
	buffer[length] = '\0';
	return buffer;
}
extern "C" int __wrap_feof(FILE *f) {
	papp_file_state *state = papp_file_state_for(f, false);
	return state ? (int)state->eof : 0;
}
extern "C" int __wrap_ferror(FILE *f) {
	papp_file_state *state = papp_file_state_for(f, false);
	return state ? (int)state->error : 0;
}
extern "C" void __wrap_clearerr(FILE *f) {
	papp_file_state *state = papp_file_state_for(f, false);
	if (state) {
		state->eof = 0;
		state->error = 0;
	}
}
extern "C" int __wrap_fflush(FILE *) { return 0; }

/* ── POSIX/newlib filesystem facade ─────────────────────────────────── */

typedef struct {
	void *backend;
	struct dirent entry;
} papp_dir_t;

extern "C" DIR *opendir(const char *path) {
	if (!_papp_svc || !_papp_svc->dir_open) return NULL;
	papp_dir_t *dir = (papp_dir_t *)papp_alloc(sizeof(papp_dir_t));
	if (!dir) return NULL;
	dir->backend = _papp_svc->dir_open(path);
	if (!dir->backend) { papp_free(dir); return NULL; }
	memset(&dir->entry, 0, sizeof(dir->entry));
	return (DIR *)dir;
}

extern "C" struct dirent *readdir(DIR *handle) {
	if (!_papp_svc || !handle) return NULL;
	papp_dir_t *dir = (papp_dir_t *)handle;
	char name[sizeof(dir->entry.d_name)];
	int is_dir = 0;
	if (!_papp_svc->dir_read(dir->backend, name, sizeof(name), &is_dir)) return NULL;
	memset(&dir->entry, 0, sizeof(dir->entry));
	strncpy(dir->entry.d_name, name, sizeof(dir->entry.d_name) - 1);
	#ifdef _DIRENT_HAVE_D_TYPE
	dir->entry.d_type = is_dir ? DT_DIR : DT_REG;
	#endif
	return &dir->entry;
}

extern "C" int closedir(DIR *handle) {
	if (!handle) return 0;
	papp_dir_t *dir = (papp_dir_t *)handle;
	int ret = _papp_svc->dir_close(dir->backend);
	papp_free(dir);
	return ret;
}
extern "C" void rewinddir(DIR *) {}
extern "C" long telldir(DIR *) { return 0; }
extern "C" void seekdir(DIR *, long) {}

static int papp_stat_path(const char *path, struct stat *st) {
	if (!_papp_svc || !_papp_svc->file_stat || !st) return -1;
	int is_dir = 0;
	long size = 0;
	if (_papp_svc->file_stat(path, &is_dir, &size) != 0) return -1;
	memset(st, 0, sizeof(*st));
	st->st_mode = is_dir ? S_IFDIR : S_IFREG;
	st->st_size = size;
	return 0;
}
extern "C" int stat(const char *path, struct stat *st) { return papp_stat_path(path, st); }
extern "C" int lstat(const char *path, struct stat *st) { return papp_stat_path(path, st); }
extern "C" int _stat(const char *path, struct stat *st) { return papp_stat_path(path, st); }
extern "C" int _stat_r(struct _reent *, const char *path, struct stat *st) { return papp_stat_path(path, st); }
extern "C" int access(const char *path, int) {
	struct stat st;
	return papp_stat_path(path, &st);
}
extern "C" int mkdir(const char *path, mode_t) {
	return (_papp_svc && _papp_svc->file_mkdir) ? _papp_svc->file_mkdir(path) : -1;
}
extern "C" int rmdir(const char *) { return 0; }
extern "C" int unlink(const char *path) {
	if (_papp_svc && _papp_svc->file_unlink)
		return _papp_svc->file_unlink(path);
	errno = EROFS;
	return -1;
}
extern "C" int _unlink(const char *p) { return unlink(p); }
extern "C" int _unlink_r(struct _reent *, const char *p) { return unlink(p); }
extern "C" int remove(const char *path) { return unlink(path); }
extern "C" int rename(const char *src, const char *dst) {
	if (_papp_svc && _papp_svc->file_rename)
		return _papp_svc->file_rename(src, dst);
	errno = EROFS;
	return -1;
}
extern "C" char *getcwd(char *buf, size_t size) {
	if (!buf || size < 5) return NULL;
	if (!scummvm_storage_path(buf, size, "/")) return NULL;
	return buf;
}

static int fd_flags_mode(int flags) {
	return (flags & O_ACCMODE) == O_WRONLY ? 1 :
	       ((flags & O_ACCMODE) == O_RDWR ? 2 : 0);
}
/* Keep the PAPP-side POSIX descriptor table ahead of the USB FAT VFS limit.
 * Current ScummVM builds can have launcher, theme, and game streams open at
 * the same time. */
static void *s_fd_table[32];
static int fd_index(int fd) { return fd - 3; }
extern "C" int _open(const char *path, int flags, int) {
	if (!_papp_svc || !_papp_svc->file_open) return -1;
	const char *mode = fd_flags_mode(flags) == 2 ? ((flags & O_CREAT) ? "wb+" : "rb+") :
	                   (fd_flags_mode(flags) == 1 ? "wb" : "rb");
	void *f = _papp_svc->file_open(path, mode);
	if (!f) return -1;
	for (int i = 0; i < 32; ++i) if (!s_fd_table[i]) { s_fd_table[i] = f; return i + 3; }
	_papp_svc->file_close(f);
	return -1;
}
extern "C" int _open_r(struct _reent *, const char *path, int flags, int mode) { return _open(path, flags, mode); }
extern "C" int _close(int fd) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return -1;
	int r = _papp_svc->file_close(s_fd_table[i]);
	s_fd_table[i] = NULL;
	return r;
}
extern "C" int _close_r(struct _reent *, int fd) { return _close(fd); }
extern "C" ssize_t _read(int fd, void *buf, size_t count) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return -1;
	return (ssize_t)_papp_svc->file_read(buf, 1, count, s_fd_table[i]);
}
extern "C" ssize_t _read_r(struct _reent *, int fd, void *buf, size_t count) { return _read(fd, buf, count); }
extern "C" ssize_t _write(int fd, const void *buf, size_t count) {
	if (fd == 1 || fd == 2) {
		if (_papp_svc && _papp_svc->log_printf) {
			char tmp[256]; size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
			memcpy(tmp, buf, n); tmp[n] = '\0'; _papp_svc->log_printf("%s", tmp);
		}
		return (ssize_t)count;
	}
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return -1;
	return (ssize_t)_papp_svc->file_write(buf, 1, count, s_fd_table[i]);
}
extern "C" ssize_t _write_r(struct _reent *, int fd, const void *buf, size_t count) { return _write(fd, buf, count); }
extern "C" off_t _lseek(int fd, off_t off, int whence) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i]) return (off_t)-1;
	if (_papp_svc->file_seek(s_fd_table[i], (long)off, whence) != 0) return (off_t)-1;
	return (off_t)_papp_svc->file_tell(s_fd_table[i]);
}
extern "C" off_t _lseek_r(struct _reent *, int fd, off_t off, int whence) { return _lseek(fd, off, whence); }
extern "C" int _fstat(int fd, struct stat *st) {
	int i = fd_index(fd);
	if (i < 0 || i >= 32 || !s_fd_table[i] || !st) return -1;
	memset(st, 0, sizeof(*st)); st->st_mode = S_IFREG;
	long cur = _papp_svc->file_tell(s_fd_table[i]);
	_papp_svc->file_seek(s_fd_table[i], 0, SEEK_END); st->st_size = _papp_svc->file_tell(s_fd_table[i]);
	_papp_svc->file_seek(s_fd_table[i], cur, SEEK_SET);
	return 0;
}
extern "C" int _fstat_r(struct _reent *, int fd, struct stat *st) { return _fstat(fd, st); }
extern "C" int _isatty(int fd) { return fd >= 0 && fd <= 2; }
extern "C" int _isatty_r(struct _reent *, int fd) { return _isatty(fd); }
extern "C" void *_sbrk(ptrdiff_t) { return (void *)-1; }
extern "C" void *_sbrk_r(struct _reent *, ptrdiff_t) { return (void *)-1; }
extern "C" int _getpid(void) { return 1; }
extern "C" int _kill(int, int) { return -1; }
extern "C" int _gettimeofday(struct timeval *tv, void *) {
	if (tv && _papp_svc) { int64_t us = _papp_svc->get_time_us(); tv->tv_sec = us / 1000000; tv->tv_usec = us % 1000000; }
	return 0;
}
extern "C" clock_t _times(struct tms *buf) {
	clock_t t = _papp_svc ? (clock_t)(_papp_svc->get_time_us() / 1000) : 0;
	if (buf) memset(buf, 0, sizeof(*buf));
	return t;
}
extern "C" char *getenv(const char *) { return NULL; }

static void scummvm_prepare_storage() {
	if (!_papp_svc || !_papp_svc->file_mkdir)
		return;
	/* file_mkdir intentionally mirrors POSIX mkdir (one component), so make
	 * the parent directories explicitly on the selected physical volume. */
	static const char *const suffixes[] = {
		"/scummvm",
		"/scummvm/saves",
		"/roms/scummvm"
	};
	for (const char *suffix : suffixes) {
		char dir[256];
		if (!scummvm_storage_path(dir, sizeof(dir), suffix)) continue;
		int result = _papp_svc->file_mkdir(dir);
		if (_papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM storage mkdir %s -> %d\n", dir, result);
	}
}

static bool scummvm_open_storage_file(const char *suffix, const char *mode,
		void **file_out, char *path_out, size_t path_out_size) {
	if (!file_out || !path_out || !mode ||
		!scummvm_storage_path(path_out, path_out_size, suffix) ||
		!_papp_svc || !_papp_svc->file_open)
		return false;
	*file_out = _papp_svc->file_open(path_out, mode);
	return *file_out != NULL;
}

class PappConfigReadStream final : public Common::SeekableReadStream {
public:
	explicit PappConfigReadStream(const char *path)
		: _file(NULL), _size(0), _eos(false), _error(false) {
		if (!_papp_svc || !_papp_svc->file_open) return;
		_file = _papp_svc->file_open(path, "rb");
		if (!_file) return;
		if (_papp_svc->file_seek(_file, 0, SEEK_END) != 0) {
			_error = true;
			return;
		}
		_size = _papp_svc->file_tell(_file);
		if (_size < 0 || _papp_svc->file_seek(_file, 0, SEEK_SET) != 0) {
			_error = true;
			_size = 0;
		}
	}

	~PappConfigReadStream() override {
		if (_file && _papp_svc && _papp_svc->file_close)
			_papp_svc->file_close(_file);
	}

	bool isOpen() const { return _file != NULL && !_error; }
	uint32 read(void *dataPtr, uint32 dataSize) override {
		if (!isOpen() || dataSize == 0) return 0;
		size_t got = _papp_svc->file_read(dataPtr, 1, dataSize, _file);
		if (got < dataSize) _eos = true;
		return (uint32)got;
	}
	bool eos() const override { return _eos; }
	bool err() const override { return _error; }
	void clearErr() override { _eos = false; _error = false; }
	int64 pos() const override {
		return isOpen() ? (int64)_papp_svc->file_tell(_file) : -1;
	}
	int64 size() const override { return _size; }
	bool seek(int64 offset, int whence = SEEK_SET) override {
		if (!isOpen() || offset < LONG_MIN || offset > LONG_MAX) {
			_error = true;
			return false;
		}
		if (_papp_svc->file_seek(_file, (long)offset, whence) != 0) {
			_error = true;
			return false;
		}
		_eos = false;
		return true;
	}

private:
	void *_file;
	int64 _size;
	bool _eos;
	bool _error;
};

class PappConfigWriteStream final : public Common::MemoryWriteStreamDynamic {
public:
	PappConfigWriteStream() : Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES) {}

	~PappConfigWriteStream() override {
		if (!_papp_svc || !_papp_svc->file_open || !_data) return;
		scummvm_prepare_storage();
		char config_path[256] = "";
		void *file = NULL;
		bool opened = scummvm_open_storage_file("/roms/scummvm/scummvm.ini", "wb",
			&file, config_path, sizeof(config_path));
		if (!opened) {
			if (_papp_svc->log_printf)
				_papp_svc->log_printf("ScummVM config open failed: %s\n", config_path);
			return;
		}
		size_t written = _papp_svc->file_write(_data, 1, _size, file);
		_papp_svc->file_close(file);
		if (_papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM config saved: %u/%u bytes\n",
				(unsigned)written, (unsigned)_size);
	}
};

/* The launcher writes this short record when the user chooses:
 *   ScummVM PAPP -> SCI32 Engine -> <game>
 *
 * Lines are: portable game path, engine id, game id, display description.
 * Keeping this outside scummvm.ini means the normal ScummVM game list and
 * key mappings remain persistent, while a dashboard launch is one-shot. */
static void scummvm_trim_line(char *line) {
	if (!line) return;
	line[strcspn(line, "\r\n")] = '\0';
}

static bool scummvm_load_direct_target() {
	char direct_path[256];
	void *direct_file = NULL;
	if (!scummvm_open_storage_file("/roms/papp/scummvm-sci.rom", "rb",
		&direct_file, direct_path, sizeof(direct_path)))
		return false;
	FILE *file = (FILE *)direct_file;
	if (!file) return false;

	char path[256] = "";
	char engine[32] = "";
	char game_id[64] = "";
	char description[96] = "";
	bool valid = fgets(path, sizeof(path), file) != NULL &&
	             fgets(engine, sizeof(engine), file) != NULL &&
	             fgets(game_id, sizeof(game_id), file) != NULL;
	if (valid) {
		scummvm_trim_line(path);
		scummvm_trim_line(engine);
		scummvm_trim_line(game_id);
		if (fgets(description, sizeof(description), file) != NULL)
			scummvm_trim_line(description);
	}
	fclose(file);

	if (!valid || !path[0] || !engine[0] || !game_id[0]) {
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM direct launch record is incomplete\n");
		return false;
	}

	const char *domain = "papp_direct";
	ConfMan.addGameDomain(domain);
	ConfMan.set("engineid", engine, domain);
	ConfMan.set("gameid", game_id, domain);
	ConfMan.setPath("path", Common::Path(path), domain);
	ConfMan.set("description", description[0] ? description : game_id, domain);
	ConfMan.setActiveDomain(domain);

	if (_papp_svc && _papp_svc->log_printf)
		_papp_svc->log_printf("ScummVM direct target: engine=%s game=%s path=%s\n",
			engine, game_id, path);

	/* Consume the dashboard request.  Opening with wb is supported by the
	 * launcher's file service and lets the next ordinary PAPP launch show the
	 * ScummVM launcher instead of replaying the previous game. */
	file = fopen(direct_path, "wb");
	if (file) fclose(file);
	return true;
}

/* ── PAPP graphics manager ──────────────────────────────────────────── */

class PappGraphicsManager final : public GraphicsManager {
public:
	PappGraphicsManager() : _width(400), _height(240), _format(Graphics::PixelFormat::createFormatCLUT8()),
		_screen(), _overlay(), _cursor(), _cursorMask(NULL), _rgb(NULL), _rgbPixels(0),
		_overlayVisible(false), _cursorVisible(true), _cursorX(0), _cursorY(0),
		_cursorAreaW(400), _cursorAreaH(240), _cursorHotX(0), _cursorHotY(0),
		_cursorKeyColor(0), _cursorDontScale(false), _lastUpdate(0) {
		memset(_palette, 0, sizeof(_palette));
		memset(_cursorPalette, 0, sizeof(_cursorPalette));
	}
	~PappGraphicsManager() override {
		_screen.free(); _overlay.free(); _cursor.free();
		if (_cursorMask && _papp_svc) _papp_svc->mem_free(_cursorMask);
		if (_rgb && _papp_svc) _papp_svc->mem_free(_rgb);
	}
	void init() {
		_overlay.create(400, 240, getOverlayFormat());
		memset(_overlay.getPixels(), 0, _overlay.pitch * _overlay.h);
	}
	bool hasFeature(OSystem::Feature f) const override {
		return f == OSystem::kFeatureTouchscreen || f == OSystem::kFeatureVirtualKeyboard || f == OSystem::kFeatureNoQuit;
	}
	void setFeatureState(OSystem::Feature, bool) override {}
	bool getFeatureState(OSystem::Feature f) const override { return hasFeature(f); }
	void initSize(uint width, uint height, const Graphics::PixelFormat *format = NULL) override {
		_screen.free();
		_width = width; _height = height;
		_format = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
		_screen.create(width, height, _format);
		if (_rgb && _papp_svc) _papp_svc->mem_free(_rgb);
		/* Keep enough room for ScummVM's 400x240 RGB565 overlay as well as
		 * the game framebuffer.  The GUI overlay is larger than most game
		 * surfaces (for example SCI is commonly 320x200). */
		_rgbPixels = width * height;
		if (_rgbPixels < 400u * 240u) _rgbPixels = 400u * 240u;
		_rgb = (uint16_t *)_papp_svc->mem_caps_alloc(_rgbPixels * sizeof(uint16_t), PAPP_MEM_CAP_SPIRAM | PAPP_MEM_CAP_DMA);
		memset(_palette, 0, sizeof(_palette));
		if (_rgb) memset(_rgb, 0, _rgbPixels * sizeof(uint16_t));
	}
	int getScreenChangeID() const override { return 0; }
	void beginGFXTransaction() override {}
	OSystem::TransactionError endGFXTransaction() override { return OSystem::kTransactionSuccess; }
	int16 getHeight() const override { return (int16)_height; }
	int16 getWidth() const override { return (int16)_width; }
#ifdef USE_RGB_COLOR
	Graphics::PixelFormat getScreenFormat() const override { return _format; }
	Common::List<Graphics::PixelFormat> getSupportedFormats() const override {
		Common::List<Graphics::PixelFormat> formats;
		formats.push_back(Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0));
		formats.push_back(Graphics::PixelFormat::createFormatCLUT8());
		return formats;
	}
#endif
	void setPalette(const byte *colors, uint start, uint num) override {
		if (!colors) return;
		if (start + num > 256) num = 256 - start;
		memcpy(_palette + start * 3, colors, num * 3);
	}
	void grabPalette(byte *colors, uint start, uint num) const override {
		if (colors && start + num <= 256) memcpy(colors, _palette + start * 3, num * 3);
	}
	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override { _screen.copyRectToSurface(buf, pitch, x, y, w, h); }
	Graphics::Surface *lockScreen() override { return &_screen; }
	void unlockScreen() override {}
	void updateScreen() override {
		if (!_papp_svc || !_papp_svc->display_write_frame_custom || !_rgb) return;
		int64_t now = _papp_svc->get_time_us();
		if (now - _lastUpdate < 25000) return;
		_lastUpdate = now;
		uint16_t *out = _rgb;
		uint16_t frame_w = (uint16_t)_width;
		uint16_t frame_h = (uint16_t)_height;
		if (_overlayVisible) {
			/* The overlay is a 400x240 Surface and may have padding in its
			 * pitch.  Repack it into the same tightly packed DMA buffer used
			 * by the game framebuffer, and pass its real dimensions to the
			 * display pipeline.  Treating it as _width x _height was the
			 * source of the chopped/green GUI frames. */
			const uint8_t *src = (const uint8_t *)_overlay.getPixels();
			uint8_t *dst = (uint8_t *)_rgb;
			frame_w = (uint16_t)_overlay.w;
			frame_h = (uint16_t)_overlay.h;
			for (uint y = 0; y < frame_h; ++y)
				memcpy(dst + y * frame_w * sizeof(uint16_t),
				       src + y * _overlay.pitch,
				       frame_w * sizeof(uint16_t));
		} else if (_format.bytesPerPixel == 2) {
			/* The display/PPA path requires a tightly packed RGB565 frame.
			 * Graphics::Surface may have a padded pitch, so repack each row
			 * instead of assuming width == pitch / bytes-per-pixel. */
			const uint8_t *src = (const uint8_t *)_screen.getPixels();
			uint8_t *dst = (uint8_t *)_rgb;
			for (uint y = 0; y < _height; ++y)
				memcpy(dst + y * _width * sizeof(uint16_t),
				       src + y * _screen.pitch,
				       _width * sizeof(uint16_t));
		} else {
			const uint8_t *src = (const uint8_t *)_screen.getPixels();
			for (uint y = 0; y < _height; ++y) {
				const uint8_t *src_row = src + y * _screen.pitch;
				for (uint x = 0; x < _width; ++x) {
					const uint8_t *p = _palette + src_row[x] * 3;
					_rgb[y * _width + x] = (uint16_t)(((p[0] >> 3) << 11) |
						((p[1] >> 2) << 5) | (p[2] >> 3));
				}
			}
		}
		drawCursor(out, frame_w, frame_h);
		float scale = 1.0f;
		if (frame_w && frame_h) {
			float sx = 800.0f / (float)frame_w, sy = 480.0f / (float)frame_h;
			scale = sx < sy ? sx : sy;
			if (scale > 2.4f) scale = 2.4f;
			if (scale < 1.0f) scale = 1.0f;
		}
		_papp_svc->display_write_frame_custom(out, frame_w, frame_h, scale, false);
	}
	void setShakePos(int, int) override {}
	void fillScreen(uint32 col) override { if (_screen.getPixels()) _screen.fillRect(Common::Rect(_screen.w, _screen.h), col); }
	void fillScreen(const Common::Rect &r, uint32 col) override { if (_screen.getPixels()) _screen.fillRect(r, col); }
	void setFocusRectangle(const Common::Rect &) override {}
	void clearFocusRectangle() override {}
	void showOverlay(bool visible) override { _overlayVisible = visible; }
	void hideOverlay() override { _overlayVisible = false; }
	bool isOverlayVisible() const override { return _overlayVisible; }
	uint overlayInputWidth() const { return _overlayVisible ? _overlay.w : _width; }
	uint overlayInputHeight() const { return _overlayVisible ? _overlay.h : _height; }
	Graphics::PixelFormat getOverlayFormat() const override { return Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0); }
	void clearOverlay() override { memset(_overlay.getPixels(), 0, _overlay.pitch * _overlay.h); }
	void grabOverlay(Graphics::Surface &surface) const override { surface.copyFrom(_overlay); }
	void copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) override { _overlay.copyRectToSurface(buf, pitch, x, y, w, h); }
	int16 getOverlayHeight() const override { return 240; }
	int16 getOverlayWidth() const override { return 400; }
	bool showMouse(bool visible) override {
		bool previous = _cursorVisible;
		_cursorVisible = visible;
		return previous;
	}
	void warpMouse(int x, int y) override {
		_cursorX = x;
		_cursorY = y;
		_cursorAreaW = _overlayVisible ? _overlay.w : _width;
		_cursorAreaH = _overlayVisible ? _overlay.h : _height;
	}
	void setMousePosition(int x, int y, uint areaW, uint areaH) {
		_cursorX = x;
		_cursorY = y;
		_cursorAreaW = areaW ? areaW : _width;
		_cursorAreaH = areaH ? areaH : _height;
	}
	void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
			uint32 keycolor, bool dontScale = false,
			const Graphics::PixelFormat *format = NULL,
			const byte *mask = NULL) override {
		_cursor.free();
		if (_cursorMask && _papp_svc) {
			_papp_svc->mem_free(_cursorMask);
			_cursorMask = NULL;
		}
		_cursorHotX = hotspotX;
		_cursorHotY = hotspotY;
		_cursorKeyColor = keycolor;
		_cursorDontScale = dontScale;
		if (!buf || w == 0 || h == 0)
			return;

		Graphics::PixelFormat actualFormat = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
		_cursor.create(w, h, actualFormat);
		_cursor.copyRectToSurface(buf, w * actualFormat.bytesPerPixel, 0, 0, w, h);
		if (mask) {
			size_t maskBytes = (size_t)w * h;
			_cursorMask = (byte *)_papp_svc->mem_caps_alloc(maskBytes, PAPP_MEM_CAP_SPIRAM);
			if (_cursorMask)
				memcpy(_cursorMask, mask, maskBytes);
		}
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM cursor installed: %ux%u hotspot=(%d,%d) bpp=%u mask=%s\n",
				(unsigned)w, (unsigned)h, hotspotX, hotspotY,
				(unsigned)actualFormat.bytesPerPixel, mask ? "yes" : "no");
	}
	void setCursorPalette(const byte *colors, uint start, uint num) override {
		if (!colors || start >= 256) return;
		if (num > 256 - start) num = 256 - start;
		memcpy(_cursorPalette + start * 3, colors, num * 3);
	}
private:
	static uint16_t cursorRgb565(uint8 r, uint8 g, uint8 b) {
		return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	}
	void drawCursor(uint16_t *frame, uint frameW, uint frameH) {
		if (!_cursorVisible || !frame || frameW == 0 || frameH == 0)
			return;

		int cx = _cursorAreaW ? (_cursorX * (int)frameW) / (int)_cursorAreaW : _cursorX;
		int cy = _cursorAreaH ? (_cursorY * (int)frameH) / (int)_cursorAreaH : _cursorY;
		int originX = cx - _cursorHotX;
		int originY = cy - _cursorHotY;
		if (_cursor.w > 0 && _cursor.h > 0 && _cursor.getPixels()) {
			const uint bpp = _cursor.format.bytesPerPixel;
			for (int y = 0; y < _cursor.h; ++y) {
				for (int x = 0; x < _cursor.w; ++x) {
					int dstX = originX + x;
					int dstY = originY + y;
					if (dstX < 0 || dstY < 0 || dstX >= (int)frameW || dstY >= (int)frameH)
						continue;
					size_t index = (size_t)y * _cursor.pitch + (size_t)x * bpp;
					const byte *pixel = (const byte *)_cursor.getPixels() + index;
					if (_cursorMask && _cursorMask[(size_t)y * _cursor.w + x] == kCursorMaskTransparent)
						continue;

					uint32 color = 0;
					if (bpp == 1) color = pixel[0];
					else if (bpp == 2) color = (uint32)pixel[0] | ((uint32)pixel[1] << 8);
					else if (bpp == 3) color = (uint32)pixel[0] | ((uint32)pixel[1] << 8) | ((uint32)pixel[2] << 16);
					else if (bpp >= 4) color = (uint32)pixel[0] | ((uint32)pixel[1] << 8) |
						((uint32)pixel[2] << 16) | ((uint32)pixel[3] << 24);

					uint8 a = 255, r = 255, g = 255, b = 255;
					if (_cursor.format == Graphics::PixelFormat::createFormatCLUT8()) {
						if (color == _cursorKeyColor) continue;
						r = _cursorPalette[color * 3 + 0];
						g = _cursorPalette[color * 3 + 1];
						b = _cursorPalette[color * 3 + 2];
					} else {
						if (color == _cursorKeyColor) continue;
						_cursor.format.colorToARGB(color, a, r, g, b);
						if (a == 0) continue;
					}
					frame[(size_t)dstY * frameW + dstX] = cursorRgb565(r, g, b);
				}
			}
			return;
		}

		/* Some engines do not install a cursor until their first input event.
		 * Keep a visible, high-contrast fallback so the ScummVM launcher remains
		 * usable on the first frame as well. */
		static const int shape[][2] = {
			{0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7},
			{1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6},
			{2, 2}, {2, 3}, {2, 4}, {2, 5},
			{3, 3}, {3, 4}, {3, 5}, {4, 4}, {4, 5}, {5, 5}
		};
		for (size_t i = 0; i < sizeof(shape) / sizeof(shape[0]); ++i) {
			int x = cx + shape[i][0], y = cy + shape[i][1];
			if (x >= 0 && y >= 0 && x < (int)frameW && y < (int)frameH)
				frame[(size_t)y * frameW + x] = cursorRgb565(255, 255, 255);
		}
	}
	uint _width, _height;
	Graphics::PixelFormat _format;
	Graphics::Surface _screen, _overlay, _cursor;
	byte *_cursorMask;
	uint16_t *_rgb;
	uint32_t _rgbPixels;
	byte _palette[256 * 3];
	byte _cursorPalette[256 * 3];
	bool _overlayVisible;
	bool _cursorVisible;
	int _cursorX, _cursorY;
	uint _cursorAreaW, _cursorAreaH;
	int _cursorHotX, _cursorHotY;
	uint32 _cursorKeyColor;
	bool _cursorDontScale;
	int64_t _lastUpdate;
};

/* ── PAPP mixer manager ─────────────────────────────────────────────── */

class PappMixerManager final : public MixerManager {
public:
	PappMixerManager(int freq, int bufSize) : _freq(freq), _bufSize(bufSize), _buffer(NULL), _task(NULL), _stop(false) {}
	~PappMixerManager() override {
		_stop = true;
		if (_task && _papp_svc) _papp_svc->task_delete(_task);
		if (_buffer && _papp_svc) _papp_svc->mem_free(_buffer);
	}
	void init() override {
		_mixer = new Audio::MixerImpl(_freq);
		if (_papp_svc && _papp_svc->audio_init) _papp_svc->audio_init(_freq);
		_buffer = (byte *)_papp_svc->mem_caps_alloc(_bufSize, PAPP_MEM_CAP_SPIRAM);
		if (_buffer && _papp_svc) _papp_svc->task_create(audioTaskStub, "scvm_audio", 16384, this, 5, &_task, 1);
		_mixer->setReady(true);
	}
	void suspendAudio() override { _audioSuspended = true; }
	int resumeAudio() override { if (!_audioSuspended) return -2; _audioSuspended = false; return 0; }
private:
	static void audioTaskStub(void *arg) { ((PappMixerManager *)arg)->audioTask(); }
	void audioTask() {
		while (!_stop) {
			if (_audioSuspended) {
				if (_papp_svc) _papp_svc->delay_ms(10);
				continue;
			}
			_mixer->mixCallback(_buffer, _bufSize);
			if (_papp_svc && _papp_svc->audio_submit)
				_papp_svc->audio_submit((short *)_buffer, _bufSize / 4);
		}
	}
	int _freq, _bufSize;
	byte *_buffer;
	void *_task;
	volatile bool _stop;
};

class PappFilesystemFactory final : public FilesystemFactory {
protected:
	AbstractFSNode *makeRootFileNode() const override {
		char path[32];
		scummvm_storage_path(path, sizeof(path), "/");
		return new POSIXFilesystemNode(path);
	}
	AbstractFSNode *makeCurrentDirectoryFileNode() const override {
		char path[32];
		scummvm_storage_path(path, sizeof(path), "/");
		return new POSIXFilesystemNode(path);
	}
	AbstractFSNode *makeFileNodePath(const Common::String &path) const override { return new POSIXFilesystemNode(path); }
};

class PappMutex final : public Common::MutexInternal {
public:
	bool lock() override { return true; }
	bool unlock() override { return true; }
};

/* ── ScummVM OSystem bridge ─────────────────────────────────────────── */

class OSystem_papp final : public ModularMixerBackend, public ModularGraphicsBackend, public Common::EventSource {
public:
	OSystem_papp() : _lastTouch(false), _lastMouseButtons(0), _mouseX(0), _mouseY(0), _head(0), _tail(0) {
		_fsFactory = new PappFilesystemFactory();
		memset(_padPrev, 0, sizeof(_padPrev));
	}
	~OSystem_papp() override {}
	Common::HardwareInputSet *getHardwareInputSet() override {
		/* USE_NULL_DRIVER keeps ScummVM's desktop input backend out of the PAPP,
		 * but the keymapper still needs a description of the inputs generated by
		 * this bridge. Without this, game-specific keymaps are never initialized
		 * and the engine reports "No hardware inputs were registered". The PAPP
		 * converts the physical controller to ScummVM keyboard events, so the
		 * standard keyboard set is also the correct representation for controller
		 * actions. */
		Common::CompositeHardwareInputSet *inputs = new Common::CompositeHardwareInputSet();
		inputs->addHardwareInputSet(new Common::MouseHardwareInputSet(Common::defaultMouseButtons));
		inputs->addHardwareInputSet(new Common::KeyboardHardwareInputSet(Common::defaultKeys, Common::defaultModifiers));
		return inputs;
	}
	Common::SeekableReadStream *createConfigReadStream() override {
		char config_path[256];
		scummvm_storage_path(config_path, sizeof(config_path), "/roms/scummvm/scummvm.ini");
		PappConfigReadStream *stream = new PappConfigReadStream(config_path);
		if (!stream->isOpen()) {
			delete stream;
			return NULL;
		}
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM config loaded: %s\n", config_path);
		return stream;
	}
	Common::WriteStream *createConfigWriteStream() override {
		return new PappConfigWriteStream();
	}
	void initBackend() override {
		if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM OSystem init begin\n");
		scummvm_prepare_storage();
		_timerManager = new DefaultTimerManager();
		_eventManager = new DefaultEventManager(this);
		_savefileManager = new DefaultSaveFileManager();
		PappGraphicsManager *graphics = new PappGraphicsManager();
		graphics->init();
		_graphicsManager = graphics;
		_mixerManager = new PappMixerManager(44100, 4096);
		_mixerManager->init();
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/scummvm/extras/");
		ConfMan.registerDefault("extrapath", Common::Path(path));
		/* The compact PAPP runtime bundle keeps the common ScummVM archives
		 * directly in the selected ScummVM data directory rather than duplicating them in per-type
		 * subdirectories. */
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		ConfMan.registerDefault("iconspath", Common::Path(path));
		scummvm_storage_path(path, sizeof(path), "/scummvm/plugins/");
		ConfMan.registerDefault("pluginspath", Common::Path(path));
		scummvm_storage_path(path, sizeof(path), "/scummvm/saves/");
		ConfMan.registerDefault("savepath", Common::Path(path));
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		ConfMan.registerDefault("themepath", Common::Path(path));
		BaseBackend::initBackend();
		/* Existing configs may contain paths from the other volume. Rebase only
		 * the built-in storage paths; user paths elsewhere remain untouched. */
		scummvm_rebase_config_paths();
		if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM OSystem init complete\n");
	}
	bool pollEvent(Common::Event &event) override {
		((DefaultTimerManager *)getTimerManager())->checkTimers();
		if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		if (_papp_svc && _papp_svc->input_l3_read && _papp_svc->input_l3_read()) { event.type = Common::EVENT_QUIT; return true; }
		if (_papp_svc && _papp_svc->input_gamepad_read) {
			papp_gamepad_state_t pad; memset(&pad, 0, sizeof(pad));
			/* USB keyboards have a separate text/event stream. Use the physical
			 * gamepad view here so Z/X/arrows are not delivered twice: once as
			 * virtual controller buttons and once as real keyboard keys. */
			if (_papp_svc->input_gamepad_read_physical)
				_papp_svc->input_gamepad_read_physical(&pad);
			else
				_papp_svc->input_gamepad_read(&pad);
			for (int i = 0; i < PAPP_INPUT_MAX; ++i) {
				if (pad.values[i] == _padPrev[i]) continue;
				int key = 0, ascii = 0;
				switch (i) {
				case PAPP_INPUT_UP: key = Common::KEYCODE_UP; break;
				case PAPP_INPUT_DOWN: key = Common::KEYCODE_DOWN; break;
				case PAPP_INPUT_LEFT: key = Common::KEYCODE_LEFT; break;
				case PAPP_INPUT_RIGHT: key = Common::KEYCODE_RIGHT; break;
				case PAPP_INPUT_A: key = Common::KEYCODE_RETURN; ascii = Common::ASCII_RETURN; break;
				case PAPP_INPUT_B: key = Common::KEYCODE_ESCAPE; ascii = Common::ASCII_ESCAPE; break;
				case PAPP_INPUT_START: key = Common::KEYCODE_RETURN; ascii = Common::ASCII_RETURN; break;
				default: break;
				}
				if (key) {
					if (_papp_svc && _papp_svc->log_printf)
						_papp_svc->log_printf("ScummVM gamepad event input=%d key=%d ascii=%d down=%d\n",
							i, key, ascii, pad.values[i] != 0 ? 1 : 0);
					queueKey(key, ascii, pad.values[i] != 0);
				}
				_padPrev[i] = pad.values[i];
			}
			if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		}
		if (_papp_svc && _papp_svc->input_keyboard_read) {
			papp_keyboard_event_t kev;
			if (_papp_svc->input_keyboard_read(&kev)) {
				if (_papp_svc->log_printf)
					_papp_svc->log_printf("ScummVM keyboard event key=%d down=%d\n", kev.key, kev.down ? 1 : 0);
				queueKeyboard(kev);
				if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
			}
		}
		if (_papp_svc && _papp_svc->touch_read) {
			int x = 0, y = 0; int touched = _papp_svc->touch_read(&x, &y);
			/* Touch coordinates are reported in the fixed 800x480 PAPP canvas.
			 * ScummVM keeps the game surface at (for example) 320x200, but
			 * its launcher/settings UI is drawn in the separate 400x240
			 * overlay. Use the active surface dimensions for hit testing. */
			uint touch_w = getWidth(), touch_h = getHeight();
			if (_graphicsManager) {
				PappGraphicsManager *graphics = static_cast<PappGraphicsManager *>(_graphicsManager);
				touch_w = graphics->overlayInputWidth();
				touch_h = graphics->overlayInputHeight();
			}
			if (touched) {
				/* The touch service reports coordinates in the physical LCD's
				 * landscape space. The custom display writer rotates both the
				 * ScummVM overlay and the game canvas by 180 degrees before
				 * presenting them. ScummVM receives source-space coordinates,
				 * so undo that display rotation for both paths. This keeps the
				 * launcher UI and in-game touch controls aligned. */
				bool overlay = _graphicsManager &&
					static_cast<PappGraphicsManager *>(_graphicsManager)->isOverlayVisible();
				(void)overlay;
				int event_x = 799 - x;
				int event_y = 479 - y;
				_mouseX = (int16)(event_x * touch_w / 800); _mouseY = (int16)(event_y * touch_h / 480);
				if (!_lastTouch && _papp_svc->log_printf) {
					_papp_svc->log_printf("ScummVM touch down raw=(%d,%d) event=(%d,%d) surface=%ux%u overlay=%d\n",
						x, y, _mouseX, _mouseY, (unsigned)touch_w,
						(unsigned)touch_h, overlay ? 1 : 0);
				}
			} else if (_lastTouch && _papp_svc->log_printf) {
				_papp_svc->log_printf("ScummVM touch up event=(%d,%d)\n", _mouseX, _mouseY);
			}
			if (touched) {
				Common::Event move; move.type = Common::EVENT_MOUSEMOVE; move.mouse = Common::Point(_mouseX, _mouseY); queue(move);
				if (_graphicsManager)
					static_cast<PappGraphicsManager *>(_graphicsManager)->setMousePosition(
						_mouseX, _mouseY, touch_w, touch_h);
				if (!_lastTouch) { Common::Event down = move; down.type = Common::EVENT_LBUTTONDOWN; queue(down); }
			} else if (_lastTouch) { Common::Event up; up.type = Common::EVENT_LBUTTONUP; up.mouse = Common::Point(_mouseX, _mouseY); queue(up); }
			_lastTouch = touched != 0;
			if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		}
		if (_papp_svc && _papp_svc->input_mouse_read) {
			int dx = 0, dy = 0, buttons = 0;
			if (_papp_svc->input_mouse_read(&dx, &dy, &buttons)) {
				uint mouse_w = getWidth(), mouse_h = getHeight();
				if (_graphicsManager) {
					PappGraphicsManager *graphics = static_cast<PappGraphicsManager *>(_graphicsManager);
					mouse_w = graphics->overlayInputWidth();
					mouse_h = graphics->overlayInputHeight();
				}
				_mouseX += dx; _mouseY += dy;
				if (_mouseX < 0) _mouseX = 0; if (_mouseY < 0) _mouseY = 0;
				if (_mouseX >= (int)mouse_w) _mouseX = (int)mouse_w - 1; if (_mouseY >= (int)mouse_h) _mouseY = (int)mouse_h - 1;
				if (_graphicsManager)
					static_cast<PappGraphicsManager *>(_graphicsManager)->setMousePosition(
						_mouseX, _mouseY, mouse_w, mouse_h);
				Common::Event move; move.type = Common::EVENT_MOUSEMOVE; move.mouse = Common::Point(_mouseX, _mouseY); queue(move);
				if ((buttons & 1) != (_lastMouseButtons & 1)) { Common::Event b = move; b.type = (buttons & 1) ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP; queue(b); }
				if ((buttons & 2) != (_lastMouseButtons & 2)) { Common::Event b = move; b.type = (buttons & 2) ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP; queue(b); }
				_lastMouseButtons = buttons;
			}
			if (_head != _tail) { event = _events[_head]; _head = (_head + 1) % 16; return true; }
		}
		event.type = Common::EVENT_INVALID;
		return false;
	}
	Common::MutexInternal *createMutex() override { return new PappMutex(); }
	uint32 getMillis(bool = false) override { return _papp_svc ? (uint32)(_papp_svc->get_time_us() / 1000) : 0; }
	void delayMillis(uint ms) override { if (_papp_svc) _papp_svc->delay_ms((int)ms); }
	void getTimeAndDate(TimeDate &td, bool = false) const override { memset(&td, 0, sizeof(td)); td.tm_mday = 1; td.tm_mon = 1; td.tm_year = 120; }
	void quit() override { app_return_to_launcher(); }
	void logMessage(LogMessageType::Type, const char *message) override { if (_papp_svc && _papp_svc->log_printf && message) _papp_svc->log_printf("%s", message); }
	void addSysArchivesToSearchSet(Common::SearchSet &s, int priority) override {
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/scummvm/");
		s.add("engine-data", new Common::FSDirectory(path, 4), priority);
		s.add("gui/themes", new Common::FSDirectory(path, 4), priority);
	}
	protected:
	Common::Path getDefaultConfigFileName() override {
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/roms/scummvm/scummvm.ini");
		return Common::Path(path);
	}
	Common::Path getDefaultLogFileName() override {
		char path[256];
		scummvm_storage_path(path, sizeof(path), "/scummvm/scummvm.log");
		return Common::Path(path);
	}
private:
	void queue(const Common::Event &event) { int next = (_tail + 1) % 16; if (next != _head) { _events[_tail] = event; _tail = next; } }
	void queueKey(int key, int ascii, bool down) { Common::Event event; event.type = down ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP; event.kbd.keycode = (Common::KeyCode)key; event.kbd.ascii = ascii; queue(event); }
	void queueKeyboard(const papp_keyboard_event_t &in) {
		int key = in.key, ascii = 0;
		if (key >= 273 && key <= 276) {
			static const int arrows[] = { Common::KEYCODE_UP, Common::KEYCODE_DOWN, Common::KEYCODE_RIGHT, Common::KEYCODE_LEFT };
			key = arrows[key - 273];
		} else if (key == 13) { key = Common::KEYCODE_RETURN; ascii = Common::ASCII_RETURN; }
		else if (key == 27) { key = Common::KEYCODE_ESCAPE; ascii = Common::ASCII_ESCAPE; }
		else if (key == 8) key = Common::KEYCODE_BACKSPACE;
		else if (key == 9) key = Common::KEYCODE_TAB;
		else if (key >= 'a' && key <= 'z') {
			/* ScummVM text widgets use the ASCII field for printable text.
			 * The HID decoder already gives us lowercase ASCII for an
			 * unshifted key, so preserve it before translating the keycode. */
			ascii = key;
			key = Common::KEYCODE_a + key - 'a';
		}
		else if (key >= 'A' && key <= 'Z') { ascii = key; key = Common::KEYCODE_a + key - 'A'; }
		else ascii = (key >= 32 && key < 127) ? key : 0;
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM translated key=%d ascii=%d down=%d\n", key, ascii, in.down ? 1 : 0);
		queueKey(key, ascii, in.down != 0);
	}
	bool _lastTouch;
	int _lastMouseButtons;
	int16 _mouseX, _mouseY;
	int _padPrev[PAPP_INPUT_MAX];
	Common::Event _events[16];
	int _head, _tail;
};

extern "C" OSystem *OSystem_esp32_create(bool) { return new OSystem_papp(); }

extern "C" int app_entry(const app_services_t *svc) {
	_papp_svc = svc;
	s_exit_requested = 0;
	/* Resolve the storage volume before ScummVM loads its config or creates
	 * any filesystem nodes. */
	scummvm_storage_root();
	/* The normal ESP32 ScummVM frontend assigns this global in app_main().
	 * PAPPs enter through the launcher instead, so do it here before
	 * scummvm_main() checks that the backend exists. */
	if (_papp_svc && _papp_svc->log_printf)
		_papp_svc->log_printf("ScummVM PAPP starting (AGI/SCI); constructing OSystem\n");
	g_system = new OSystem_papp();
	if (!g_system) {
		if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM OSystem allocation failed\n");
		return -1;
	}
	if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM OSystem constructed\n");
	if (setjmp(s_exit_env) == 0) {
		if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM entering scummvm_main\n");
		/* USE_NULL_DRIVER intentionally skips ScummVM's desktop config-file
		 * branch. Load the same VFS-backed config here, then optionally install
		 * the one-shot target selected by the dashboard before scummvm_main()
		 * decides whether to show its launcher dialog. */
		bool config_loaded = ConfMan.loadDefaultConfigFile(Common::Path());
		if (_papp_svc && _papp_svc->log_printf)
			_papp_svc->log_printf("ScummVM config load: %s\n",
				config_loaded ? "ok" : "missing or invalid");
		(void)scummvm_load_direct_target();
	#if defined(SCUMMVM_PAPP_AUTOSTART_KQ6)
		/* Test build: bypass the launcher and auto-detect the installed KQ6
		 * directory so the engine/data/input path can be verified end-to-end. */
		char autostart_path[256];
		scummvm_storage_path(autostart_path, sizeof(autostart_path), "/roms/scummvm/kq6");
		const char *argv[] = { "scummvm",
			"--path", autostart_path, NULL };
		scummvm_main(3, argv);
	#elif defined(SCUMMVM_PAPP_AUTOSTART_QFG4)
		/* Test build: bypass the launcher and auto-detect the installed QFG4
		 * directory so the engine/data/input path can be verified end-to-end. */
		char autostart_path[256];
		scummvm_storage_path(autostart_path, sizeof(autostart_path), "/roms/scummvm/qfg4");
		const char *argv[] = { "scummvm",
			"--path", autostart_path, NULL };
		scummvm_main(3, argv);
	#else
		const char *argv[] = { "scummvm", NULL };
		scummvm_main(1, argv);
	#endif
		if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM scummvm_main returned\n");
	}
	if (g_system) {
		OSystem *system = g_system;
		g_system = NULL;
		system->destroy();
	}
	if (_papp_svc && _papp_svc->display_clear) {
		_papp_svc->display_clear(0);
		_papp_svc->display_flush();
	}
	if (_papp_svc && _papp_svc->log_printf) _papp_svc->log_printf("ScummVM PAPP exited (%d)\n", s_exit_requested);
	return 0;
}
