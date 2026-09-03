#include "device_allocator.h"

#include <algorithm>
#include <cstring>

#include <multi_heap.h>

namespace {

// The arrays are browser support memory which multi_heap subdivides exactly as
// ESP-IDF would subdivide physical regions. They are BSS in WASM, not part of
// the simulated free-byte budget until registered below.
#if defined(MP_DEVICE_WATCHY)
#define MP_HAS_SECOND_INTERNAL_REGION 1
#define MP_HAS_PSRAM_REGION 0
constexpr size_t INTERNAL_REGION_0_BYTES = 114688;
constexpr size_t INTERNAL_REGION_1_BYTES = 64488;
constexpr size_t PSRAM_REGION_BYTES = 0;
constexpr size_t DEFAULT_INTERNAL_LIMIT = SIZE_MAX;
// multi_heap's light-poisoning metadata costs 16 bytes for this allocation, so
// reserve 89,792 payload bytes to reproduce the measured 89,808-byte free-heap
// drop (177,476 -> 87,668) when Watchy BLE is active.
constexpr size_t BLE_RESERVATION_BYTES = 89792;
constexpr size_t WIFI_TLS_RESERVATION_BYTES = 76000;
const char* const PROFILE_NAME = "watchy2";
const char* const ARDUINO_VERSION = "3.1.3";
const char* const IDF_VERSION = "5.3.2";
constexpr bool PROFILE_CALIBRATED = true;
#elif defined(MP_DEVICE_M5PAPER)
#define MP_HAS_SECOND_INTERNAL_REGION 0
#define MP_HAS_PSRAM_REGION 1
// Provisional raw-region sizes. Replace these with captured block maps; see the
// development journal. The allocator behavior is exact, while initial layout
// fidelity is explicitly reported as uncalibrated until then.
constexpr size_t INTERNAL_REGION_0_BYTES = 196608;
constexpr size_t INTERNAL_REGION_1_BYTES = 0;
constexpr size_t PSRAM_REGION_BYTES = 3800000;
constexpr size_t DEFAULT_INTERNAL_LIMIT = 4096;
constexpr size_t BLE_RESERVATION_BYTES = 90000;
constexpr size_t WIFI_TLS_RESERVATION_BYTES = 85000;
const char* const PROFILE_NAME = "m5paper";
const char* const ARDUINO_VERSION = "2.0.4";
const char* const IDF_VERSION = "4.4.1";
constexpr bool PROFILE_CALIBRATED = false;
#else
#error "Build with MP_DEVICE_WATCHY or MP_DEVICE_M5PAPER"
#endif

alignas(16) unsigned char g_internal0[INTERNAL_REGION_0_BYTES];
#if MP_HAS_SECOND_INTERNAL_REGION
alignas(16) unsigned char g_internal1[INTERNAL_REGION_1_BYTES];
#endif
#if MP_HAS_PSRAM_REGION
alignas(16) unsigned char g_psram[PSRAM_REGION_BYTES];
#endif

struct Region {
    unsigned char* base = nullptr;
    size_t bytes = 0;
    multi_heap_handle_t heap = nullptr;
};

Region g_internal[2];
size_t g_internalCount = 0;
Region g_external;
bool g_active = false;
MpAllocationPhase g_phase = MpAllocationPhase::Idle;
int g_sourceLine = 0;
MpDeviceState g_requestedState = MpDeviceState::RadiosOff;
MpAllocationTelemetry g_telemetry;
void* g_radioReservation = nullptr;

bool contains(const Region& region, const void* ptr)
{
    if (!region.base || !ptr) return false;
    const auto* p = static_cast<const unsigned char*>(ptr);
    return p >= region.base && p < region.base + region.bytes;
}

MpHeapSnapshot snapshotRegion(const Region& region)
{
    MpHeapSnapshot out;
    if (!region.heap) return out;
    multi_heap_info_t info{};
    multi_heap_get_info(region.heap, &info);
    out.totalFree = info.total_free_bytes;
    out.largestFree = info.largest_free_block;
    out.minimumFree = info.minimum_free_bytes;
    out.allocatedBlocks = info.allocated_blocks;
    out.freeBlocks = info.free_blocks;
    return out;
}

MpHeapSnapshot snapshotInternal()
{
    MpHeapSnapshot out;
    for (size_t i = 0; i < g_internalCount; ++i) {
        MpHeapSnapshot part = snapshotRegion(g_internal[i]);
        out.totalFree += part.totalFree;
        out.largestFree = std::max(out.largestFree, part.largestFree);
        out.minimumFree += part.minimumFree;
        out.allocatedBlocks += part.allocatedBlocks;
        out.freeBlocks += part.freeBlocks;
    }
    return out;
}

void updateTelemetry()
{
    g_telemetry.currentInternal = snapshotInternal();
    g_telemetry.currentPsram = snapshotRegion(g_external);
    const size_t internalUsed = g_telemetry.initialInternal.totalFree >=
            g_telemetry.currentInternal.totalFree
        ? g_telemetry.initialInternal.totalFree - g_telemetry.currentInternal.totalFree
        : 0;
    const size_t psramUsed = g_telemetry.initialPsram.totalFree >=
            g_telemetry.currentPsram.totalFree
        ? g_telemetry.initialPsram.totalFree - g_telemetry.currentPsram.totalFree
        : 0;
    if (internalUsed > g_telemetry.peakInternalUsed) {
        g_telemetry.peakInternalUsed = internalUsed;
        g_telemetry.peakInternalSourceLine = g_sourceLine;
    }
    if (psramUsed > g_telemetry.peakPsramUsed) {
        g_telemetry.peakPsramUsed = psramUsed;
        g_telemetry.peakPsramSourceLine = g_sourceLine;
    }
}

void recordFailure(size_t size, MpMemoryCapability cap, MpAllocationSource source)
{
    if (g_telemetry.failure.valid) return;
    updateTelemetry();
    g_telemetry.failure.valid = true;
    g_telemetry.failure.sourceLine = g_sourceLine;
    g_telemetry.failure.requested = size;
    g_telemetry.failure.capability = cap;
    g_telemetry.failure.phase = g_phase;
    g_telemetry.failure.source = source;
    g_telemetry.failure.internal = g_telemetry.currentInternal;
    g_telemetry.failure.psram = g_telemetry.currentPsram;
}

void* allocIn(Region* regions, size_t count, size_t size)
{
    for (size_t i = 0; i < count; ++i) {
        if (void* p = multi_heap_malloc(regions[i].heap, size)) return p;
    }
    return nullptr;
}

void* allocDefault(size_t size)
{
#if defined(MP_DEVICE_M5PAPER)
    if (size <= DEFAULT_INTERNAL_LIMIT) {
        if (void* p = allocIn(g_internal, g_internalCount, size)) return p;
        if (g_external.heap) return multi_heap_malloc(g_external.heap, size);
    } else {
        if (g_external.heap) {
            if (void* p = multi_heap_malloc(g_external.heap, size)) return p;
        }
        return allocIn(g_internal, g_internalCount, size);
    }
    return nullptr;
#else
    return allocIn(g_internal, g_internalCount, size);
#endif
}

Region* owningRegion(const void* ptr)
{
    for (size_t i = 0; i < g_internalCount; ++i) {
        if (contains(g_internal[i], ptr)) return &g_internal[i];
    }
    if (contains(g_external, ptr)) return &g_external;
    return nullptr;
}

bool registerRegion(Region& region, unsigned char* base, size_t bytes)
{
    std::memset(base, 0, bytes);
    region.base = base;
    region.bytes = bytes;
    region.heap = multi_heap_register(base, bytes);
    return region.heap != nullptr;
}

} // namespace

bool mpDeviceAllocatorReset(MpDeviceState state)
{
    g_active = false;
    g_phase = MpAllocationPhase::Idle;
    g_sourceLine = 0;
    g_telemetry = MpAllocationTelemetry{};
    g_radioReservation = nullptr;
    g_internalCount = 0;
    g_external = Region{};

    if (!registerRegion(g_internal[g_internalCount++], g_internal0,
                        INTERNAL_REGION_0_BYTES)) return false;
#if MP_HAS_SECOND_INTERNAL_REGION
    if (!registerRegion(g_internal[g_internalCount++], g_internal1,
                        INTERNAL_REGION_1_BYTES)) return false;
#endif
#if MP_HAS_PSRAM_REGION
    if (!registerRegion(g_external, g_psram, PSRAM_REGION_BYTES)) return false;
#endif

    g_telemetry.initialInternal = snapshotInternal();
    g_telemetry.initialPsram = snapshotRegion(g_external);
    g_telemetry.currentInternal = g_telemetry.initialInternal;
    g_telemetry.currentPsram = g_telemetry.initialPsram;

    const size_t reservation = state == MpDeviceState::Ble
        ? BLE_RESERVATION_BYTES
        : (state == MpDeviceState::WifiTls ? WIFI_TLS_RESERVATION_BYTES : 0);
    if (reservation) {
        g_phase = MpAllocationPhase::Idle;
        g_radioReservation = allocIn(g_internal, g_internalCount, reservation);
        if (!g_radioReservation) {
            recordFailure(reservation, MpMemoryCapability::Internal,
                          MpAllocationSource::RadioReservation);
            return false;
        }
        updateTelemetry();
        // The profile's initial state is after radio reservations.
        g_telemetry.initialInternal = g_telemetry.currentInternal;
        g_telemetry.peakInternalUsed = 0;
        g_telemetry.peakInternalSourceLine = 0;
    }
    return true;
}

void mpDeviceBeginRenderTelemetry()
{
    g_sourceLine = 0;
    updateTelemetry();
    g_telemetry.failure = MpAllocationFailure{};
    g_telemetry.peakInternalUsed = g_telemetry.initialInternal.totalFree >=
            g_telemetry.currentInternal.totalFree
        ? g_telemetry.initialInternal.totalFree - g_telemetry.currentInternal.totalFree
        : 0;
    g_telemetry.peakPsramUsed = g_telemetry.initialPsram.totalFree >=
            g_telemetry.currentPsram.totalFree
        ? g_telemetry.initialPsram.totalFree - g_telemetry.currentPsram.totalFree
        : 0;
    g_telemetry.peakInternalSourceLine = 0;
    g_telemetry.peakPsramSourceLine = 0;
}

void mpDeviceSetRequestedState(MpDeviceState state) { g_requestedState = state; }
MpDeviceState mpDeviceRequestedState() { return g_requestedState; }

void mpDeviceSetPhase(MpAllocationPhase phase) { g_phase = phase; }
MpAllocationPhase mpDevicePhase() { return g_phase; }
void mpDeviceSetSourceLine(int line) { g_sourceLine = line > 0 ? line : 0; }
int mpDeviceSourceLine() { return g_sourceLine; }
bool mpDeviceAllocationActive() { return g_active; }
void mpDeviceSetAllocationActive(bool active) { g_active = active; }

void* mpDeviceMalloc(size_t size, MpMemoryCapability capability,
                     MpAllocationSource source)
{
    if (size == 0) return nullptr;
    ++g_telemetry.allocationCalls;
    void* result = nullptr;
    switch (capability) {
        case MpMemoryCapability::Internal:
            result = allocIn(g_internal, g_internalCount, size);
            break;
        case MpMemoryCapability::Psram:
            if (g_external.heap) result = multi_heap_malloc(g_external.heap, size);
            break;
        case MpMemoryCapability::Default:
            result = allocDefault(size);
            break;
    }
    updateTelemetry();
    if (!result) recordFailure(size, capability, source);
    return result;
}

void* mpDeviceCalloc(size_t count, size_t size, MpMemoryCapability capability)
{
    if (count && size > SIZE_MAX / count) {
        recordFailure(SIZE_MAX, capability, MpAllocationSource::Explicit);
        return nullptr;
    }
    const size_t bytes = count * size;
    void* ptr = mpDeviceMalloc(bytes, capability);
    if (ptr) std::memset(ptr, 0, bytes);
    return ptr;
}

void* mpDeviceRealloc(void* ptr, size_t size, MpMemoryCapability capability,
                      MpAllocationSource source)
{
    ++g_telemetry.reallocCalls;
    if (!ptr) return mpDeviceMalloc(size, capability, source);
    if (size == 0) {
        mpDeviceFree(ptr);
        return nullptr;
    }
    Region* owner = owningRegion(ptr);
    if (!owner) return nullptr;
    if (void* result = multi_heap_realloc(owner->heap, ptr, size)) {
        updateTelemetry();
        return result;
    }
    // heap_caps_realloc can migrate a default allocation to another compatible
    // heap when in-place/same-region growth fails.
    void* result = mpDeviceMalloc(size, capability, source);
    if (!result) return nullptr;
    const size_t oldSize = multi_heap_get_allocated_size(owner->heap, ptr);
    std::memcpy(result, ptr, std::min(oldSize, size));
    multi_heap_free(owner->heap, ptr);
    updateTelemetry();
    return result;
}

void mpDeviceFree(void* ptr)
{
    if (!ptr) return;
    Region* owner = owningRegion(ptr);
    if (!owner) return;
    ++g_telemetry.freeCalls;
    multi_heap_free(owner->heap, ptr);
    updateTelemetry();
}

bool mpDeviceOwns(const void* ptr) { return owningRegion(ptr) != nullptr; }
MpAllocationTelemetry mpDeviceTelemetry() { updateTelemetry(); return g_telemetry; }
const char* mpDeviceProfileName() { return PROFILE_NAME; }
const char* mpDeviceArduinoVersion() { return ARDUINO_VERSION; }
const char* mpDeviceIdfVersion() { return IDF_VERSION; }
bool mpDeviceProfileCalibrated() { return PROFILE_CALIBRATED; }
bool mpDeviceStateCalibrated()
{
#if defined(MP_DEVICE_WATCHY)
    return g_requestedState != MpDeviceState::WifiTls;
#else
    return false;
#endif
}

extern "C" size_t heap_caps_get_largest_free_block(uint32_t)
{
    updateTelemetry();
    return std::max(g_telemetry.currentInternal.largestFree,
                    g_telemetry.currentPsram.largestFree);
}
