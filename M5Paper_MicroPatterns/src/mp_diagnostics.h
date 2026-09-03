#ifndef MP_DIAGNOSTICS_H
#define MP_DIAGNOSTICS_H

// A deliberately tiny engine-to-host diagnostic seam. Device firmware builds
// compile this to a no-op. Constrained WASM builds record the currently
// executing script line so an allocator failure can be attributed without the
// editor knowing anything about the engine's internal control flow.
#if defined(MP_DEVICE_CONSTRAINTS) && MP_DEVICE_CONSTRAINTS
#include "device_allocator.h"
static inline void mp_diagnostic_source_line(int line)
{
    mpDeviceSetSourceLine(line);
}
#else
static inline void mp_diagnostic_source_line(int) {}
#endif

#endif // MP_DIAGNOSTICS_H
