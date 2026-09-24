#ifndef BISEN_HARVEST_LOG_CONTROL_H
#define BISEN_HARVEST_LOG_CONTROL_H

// Include after am_util.h. Validation builds keep SWO; low-overhead builds
// suppress application prints at compile time without touching the workload.
#ifndef BISEN_ENABLE_SWO_LOGGING
#define BISEN_ENABLE_SWO_LOGGING 1
#endif
#if !BISEN_ENABLE_SWO_LOGGING
#define am_util_stdio_printf(...) ((void)0)
#endif

#endif
