// msgbox_now.c
//
// https://github.com/tylerneylon/msgbox
//

#include "msgbox_now.h"

#ifdef _WIN32

// windows version
double now() {
  // TODO
}

#else

#include <sys/time.h>

// mac/linux version
double now() {
  struct timeval t;
  gettimeofday(&t, NULL);  // 2nd param = optional time zone pointer.
  return (double)t.tv_sec + 1e6 * (double)t.tv_usec;
}

#endif
