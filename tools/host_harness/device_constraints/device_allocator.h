#ifndef MP_DEVICE_ALLOCATOR_H
#define MP_DEVICE_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

// This layer deliberately exposes only the capabilities used by the two
// firmware builds. Allocation placement inside each region is delegated to the
// version-matched ESP-IDF multi_heap/TLSF source selected by the build.
enum class MpMemoryCapability : uint8_t {
    Default = 0,
    Internal = 1,
    Psram = 2,
};

enum class MpAllocationPhase : uint8_t {
    Idle = 0,
    Source = 1,
    Parse = 2,
    DisplayList = 3,
    Rasterize = 4,
    Output = 5,
};

enum class MpDeviceState : uint8_t {
    RadiosOff = 0,
    Ble = 1,
    WifiTls = 2,
};

enum class MpAllocationSource : uint8_t {
    Explicit = 0,
    CppNew = 1,
    ArduinoString = 2,
    RadioReservation = 3,
};

struct MpHeapSnapshot {
    size_t totalFree = 0;
    size_t largestFree = 0;
    size_t minimumFree = 0;
    size_t allocatedBlocks = 0;
    size_t freeBlocks = 0;
};

struct MpAllocationFailure {
    bool valid = false;
    int sourceLine = 0;
    size_t requested = 0;
    MpMemoryCapability capability = MpMemoryCapability::Default;
    MpAllocationPhase phase = MpAllocationPhase::Idle;
    MpAllocationSource source = MpAllocationSource::Explicit;
    MpHeapSnapshot internal;
    MpHeapSnapshot psram;
};

struct MpAllocationTelemetry {
    uint32_t allocationCalls = 0;
    uint32_t freeCalls = 0;
    uint32_t reallocCalls = 0;
    size_t peakInternalUsed = 0;
    size_t peakPsramUsed = 0;
    int peakInternalSourceLine = 0;
    int peakPsramSourceLine = 0;
    MpHeapSnapshot initialInternal;
    MpHeapSnapshot initialPsram;
    MpHeapSnapshot currentInternal;
    MpHeapSnapshot currentPsram;
    MpAllocationFailure failure;
};

// Reset the simulator to the compile-time device profile and requested radio
// state. All prior simulated pointers become invalid.
bool mpDeviceAllocatorReset(MpDeviceState state);
// Starts a new user-visible render measurement without rebuilding a persistent
// device heap. This keeps M5Paper's cross-render allocations realistic while
// ensuring peak/failure source lines belong to the current script.
void mpDeviceBeginRenderTelemetry();
void mpDeviceSetRequestedState(MpDeviceState state);
MpDeviceState mpDeviceRequestedState();

void mpDeviceSetPhase(MpAllocationPhase phase);
MpAllocationPhase mpDevicePhase();
void mpDeviceSetSourceLine(int line);
int mpDeviceSourceLine();

void* mpDeviceMalloc(size_t size,
                     MpMemoryCapability capability = MpMemoryCapability::Default,
                     MpAllocationSource source = MpAllocationSource::Explicit);
void* mpDeviceCalloc(size_t count, size_t size,
                     MpMemoryCapability capability = MpMemoryCapability::Default);
void* mpDeviceRealloc(void* ptr, size_t size,
                      MpMemoryCapability capability = MpMemoryCapability::Default,
                      MpAllocationSource source = MpAllocationSource::Explicit);
void mpDeviceFree(void* ptr);

bool mpDeviceOwns(const void* ptr);
bool mpDeviceAllocationActive();
void mpDeviceSetAllocationActive(bool active);

MpAllocationTelemetry mpDeviceTelemetry();
const char* mpDeviceProfileName();
const char* mpDeviceArduinoVersion();
const char* mpDeviceIdfVersion();
bool mpDeviceProfileCalibrated();
bool mpDeviceStateCalibrated();

#endif
