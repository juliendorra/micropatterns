#include "device_allocator.h"

#include <cstdlib>
#include <new>

namespace {

void* allocate(size_t size, bool mayThrow)
{
    // A replaceable C++ allocation function must return a non-null pointer for
    // a zero-size request when it succeeds. The underlying ESP allocator may
    // return null for malloc(0), so preserve operator new's contract here.
    if (size == 0) size = 1;
    void* ptr = mpDeviceAllocationActive()
        ? mpDeviceMalloc(size, MpMemoryCapability::Default,
                         MpAllocationSource::CppNew)
        : std::malloc(size);
    if (!ptr && mayThrow) throw std::bad_alloc();
    return ptr;
}

void release(void* ptr) noexcept
{
    if (!ptr) return;
    if (mpDeviceOwns(ptr)) mpDeviceFree(ptr);
    else std::free(ptr);
}

} // namespace

void* operator new(size_t size) { return allocate(size, true); }
void* operator new[](size_t size) { return allocate(size, true); }
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate(size, false); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate(size, false); } catch (...) { return nullptr; }
}
void operator delete(void* ptr) noexcept { release(ptr); }
void operator delete[](void* ptr) noexcept { release(ptr); }
void operator delete(void* ptr, size_t) noexcept { release(ptr); }
void operator delete[](void* ptr, size_t) noexcept { release(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { release(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { release(ptr); }

extern "C" void* mpArduinoStringMalloc(size_t size)
{
    return mpDeviceAllocationActive()
        ? mpDeviceMalloc(size, MpMemoryCapability::Default,
                         MpAllocationSource::ArduinoString)
        : std::malloc(size);
}

extern "C" void* mpArduinoStringRealloc(void* ptr, size_t size)
{
    if (ptr && mpDeviceOwns(ptr)) {
        return mpDeviceRealloc(ptr, size, MpMemoryCapability::Default,
                               MpAllocationSource::ArduinoString);
    }
    if (!ptr && mpDeviceAllocationActive()) {
        return mpDeviceMalloc(size, MpMemoryCapability::Default,
                              MpAllocationSource::ArduinoString);
    }
    return std::realloc(ptr, size);
}

extern "C" void mpArduinoStringFree(void* ptr)
{
    release(ptr);
}
