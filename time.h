#ifndef EFMM_SHIM_time_H
#define EFMM_SHIM_time_H
#ifdef _WIN32
struct timeval { long tv_sec; long tv_usec; };
#else
#include <sys/time.h>
#endif
#endif
