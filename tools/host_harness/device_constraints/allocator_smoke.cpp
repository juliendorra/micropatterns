#include "device_allocator.h"

#include <cassert>
#include <cstdio>

int main()
{
    assert(mpDeviceAllocatorReset(MpDeviceState::RadiosOff));
    MpAllocationTelemetry initial = mpDeviceTelemetry();
    assert(initial.initialInternal.totalFree > 0);
    assert(initial.initialInternal.largestFree > 0);

    mpDeviceSetPhase(MpAllocationPhase::Parse);
    void* a = mpDeviceMalloc(1024);
    void* b = mpDeviceMalloc(4096);
    void* c = mpDeviceMalloc(2048);
    assert(a && b && c);
    assert(mpDeviceOwns(a) && mpDeviceOwns(b) && mpDeviceOwns(c));

    mpDeviceFree(b);
    MpAllocationTelemetry fragmented = mpDeviceTelemetry();
    assert(fragmented.currentInternal.totalFree < initial.initialInternal.totalFree);
    assert(fragmented.currentInternal.freeBlocks >= initial.initialInternal.freeBlocks);

    void* tooLarge = mpDeviceMalloc(initial.initialInternal.totalFree + 1,
                                    MpMemoryCapability::Internal);
    assert(!tooLarge);
    MpAllocationTelemetry failed = mpDeviceTelemetry();
    assert(failed.failure.valid);
    assert(failed.failure.phase == MpAllocationPhase::Parse);

    mpDeviceFree(a);
    mpDeviceFree(c);
    std::printf("profile=%s idf=%s initial_free=%zu largest=%zu "
                "fragmented_free=%zu fragmented_largest=%zu failure=%zu\n",
                mpDeviceProfileName(), mpDeviceIdfVersion(),
                initial.initialInternal.totalFree,
                initial.initialInternal.largestFree,
                fragmented.currentInternal.totalFree,
                fragmented.currentInternal.largestFree,
                failed.failure.requested);
    return 0;
}
