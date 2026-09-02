#include "Arduino.h"
#include "device_allocator.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

class StringProbe : public String {
public:
    using String::String;
    bool sso() const { return isSSO(); }
    unsigned int exposedCapacity() const { return capacity(); }
};

static int fail(const char* message)
{
    std::fprintf(stderr, "String compatibility failure: %s\n", message);
    return 1;
}

int main()
{
    if (!mpDeviceAllocatorReset(MpDeviceState::RadiosOff)) {
        return fail("heap reset");
    }
    mpDeviceSetPhase(MpAllocationPhase::Parse);
    mpDeviceSetAllocationActive(true);

    void* zeroSizeNew = ::operator new(0, std::nothrow);
    if (!zeroSizeNew) return fail("operator new(0)");
    ::operator delete(zeroSizeNew);

    StringProbe empty;
    if (empty.c_str() == nullptr || empty.length() != 0 || !empty.sso()) {
        return fail("default empty SSO state");
    }
    StringProbe invalid(static_cast<const char*>(nullptr));
    if (invalid.c_str() != nullptr || invalid.sso()) {
        return fail("explicit null invalid state");
    }
    if (!invalid.reserve(0) || invalid.c_str() == nullptr || !invalid.sso()) {
        return fail("reserve(0) validation");
    }

    StringProbe inlineText("1234567890123");
    if (!inlineText.sso() || inlineText.length() != 13) {
        return fail("13-byte SSO boundary");
    }

    MpAllocationTelemetry beforeHeapString = mpDeviceTelemetry();
    StringProbe heapText("12345678901234");
    MpAllocationTelemetry afterHeapString = mpDeviceTelemetry();
    if (heapText.sso() || heapText.exposedCapacity() != 15 ||
        afterHeapString.allocationCalls != beforeHeapString.allocationCalls + 1) {
        return fail("14-byte heap boundary");
    }
    if (!heapText.reserve(16) || heapText.exposedCapacity() != 31) {
        return fail("16-byte rounded growth");
    }

    StringProbe copy(heapText);
    if (std::strcmp(copy.c_str(), heapText.c_str()) != 0 ||
        copy.c_str() == heapText.c_str()) {
        return fail("deep copy");
    }
    const char* movedPointer = copy.c_str();
    StringProbe moved(std::move(copy));
    if (moved.c_str() != movedPointer || copy.c_str() != nullptr) {
        return fail("move ownership");
    }

#if defined(MP_DEVICE_WATCHY)
    const unsigned int largeRequest = 60000;
#else
    const unsigned int largeRequest = 3000000;
#endif
    StringProbe firstLarge;
    StringProbe secondLarge;
    if (!firstLarge.reserve(largeRequest)) {
        return fail("first large reservation");
    }
#if defined(MP_DEVICE_WATCHY)
    StringProbe thirdLarge;
    if (!secondLarge.reserve(largeRequest)) {
        return fail("second large reservation");
    }
    if (thirdLarge.reserve(largeRequest)) {
#else
    if (secondLarge.reserve(largeRequest)) {
#endif
        return fail("OOM was not reproduced");
    }
    MpAllocationTelemetry oom = mpDeviceTelemetry();
    if (!oom.failure.valid) {
        return fail("OOM telemetry");
    }

    std::printf(
        "profile=%s arduino_string_size=%zu sso_max=13 rounded_capacity=%u "
        "oom_request=%zu oom_phase=%u\n",
        mpDeviceProfileName(), sizeof(String), heapText.exposedCapacity(),
        oom.failure.requested, (unsigned)oom.failure.phase);
    mpDeviceSetAllocationActive(false);
    return 0;
}
