/* Win32 kernel objects: handle table, events, mutexes, threads, modules. */
#ifndef HANDLES_H
#define HANDLES_H

#include "../rt.h"
#include <pthread.h>

/* Win32 error codes (GetLastError) */
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_ALREADY_EXISTS 183
#define ERROR_CALL_NOT_IMPLEMENTED 120

enum { H_FREE, H_FILE, H_FIND, H_EVENT, H_MUTEX, H_THREAD, H_TIMER };

void handles_init(void);
uint32_t handle_new(int type);
int handle_type(uint32_t h);
bool handle_close(uint32_t h);

uint32_t handle_new_file(int fd, int oflags, const char *winpath);
int handle_fd(uint32_t h);

uint32_t handle_new_event(bool manual, bool initial);
void handle_event_set(uint32_t h, bool signaled);

uint32_t thread_current_id(void);
/* Returns WAIT_OBJECT_0+i, WAIT_TIMEOUT, WAIT_FAILED, or WAIT_RESTART when a
   savestate was loaded while waiting: the caller must re-read its guest
   arguments and call again. */
#define WAIT_RESTART 0xfffffffeu
uint32_t wait_handles(const uint32_t *hs, uint32_t n, bool all, uint32_t ms, bool (*wake)(void));
/* Run fn on a new host thread with its own guest CPU/stack.  If out is NULL
   the thread is detached. */
bool host_thread_start(void (*fn)(CPU *c, void *arg), void *arg, pthread_t *out);
uint32_t win_tick_ms(void);

/* Modules for LoadLibrary/GetProcAddress. */
uint32_t module_handle_for(const char *name);
uint32_t module_proc(uint32_t module, const char *name);
void module_register_export(const char *dll, const char *name, GuestFn fn);
GuestFn module_export_fn(const char *name);

#endif
