// dbgcheck.h
//
// https://github.com/tylerneylon/msgbox
//
// The items below are placeholders for actual debug-checking code.
// In the future I may make this code public, but for now the
// actual implementations remain private and not part of the
// official msgbox library.
//

#pragma once

#define dbgcheck__same_thread()
#define dbgcheck__start_sync_block(name)
#define dbgcheck__end_sync_block(name)
#define dbgcheck__in_sync_block(name)

#define dbgcheck__lock(m) pthread_mutex_lock(m)
#define dbgcheck__unlock(m) pthread_mutex_unlock(m)

#define dbgcheck__malloc(size, set_name) malloc(size)
#define dbgcheck__calloc(size, set_name) calloc(1, size)
#define dbgcheck__free(ptr, set_name) free(ptr)
#define dbgcheck__ptr(root_ptr, set_name)
#define dbgcheck__ptr_size(root_ptr, set_name, size)
#define dbgcheck__inner_ptr(inner_ptr, root_ptr, set_name)
#define dbgcheck__inner_ptr_size(inner_ptr, root_ptr, set_name, size)
#define dbgcheck__fail_if(cond, fmt, ...)
#define dbgcheck__warn_if(cond, fmt, ...)

