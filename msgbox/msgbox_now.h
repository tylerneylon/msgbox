// msgbox_now.h
//
// https://github.com/tylerneylon/msgbox
//
// A cross-platform way to get a high resolution
// timestamp.
//
// This is a header-only file in order to support
// static linkage, which avoids polluting the
// global linkage namespace with the "now" symbol.
//
// The "now" function returns the number of seconds
// since the start of 1970 as a floating-point value.
//

#pragma once

#ifdef _WIN32

// windows version
static double now() {
  // TODO
}

#else

#include <sys/time.h>

// mac/linux version
static double now() {
  struct timeval t;
  gettimeofday(&t, NULL);  // 2nd param = optional time zone pointer.
  return (double)t.tv_sec + 1e-6 * (double)t.tv_usec;
}

#endif
