#ifndef PS_TIMEBASE_H
#define PS_TIMEBASE_H

#include "playsync2.h"

/*
 * Local monotonic clock only. This is NOT a clock synchronizer:
 * no timestamp produced here ever crosses the network (invariant A3).
 */
double tb_now(void);

#endif
