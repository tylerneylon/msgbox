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

#ifndef true
#define true  1
#define false 0
#endif

#ifdef _WIN32

#include <windows.h>

// windows version
static double now() {
  static double counts_per_sec;

  static int is_initialized = false;
  if (!is_initialized) {
    LARGE_INTEGER counts_per_sec_int;
    QueryPerformanceFrequency(&counts_per_sec_int);
    counts_per_sec = (double)counts_per_sec_int.QuadPart;
    is_initialized = true;
  }

  LARGE_INTEGER counts;
  QueryPerformanceCounter(&counts);
  double seconds = (double)counts.QuadPart / counts_per_sec;
  return seconds;
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
