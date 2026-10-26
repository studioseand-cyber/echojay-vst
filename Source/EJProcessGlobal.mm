#include "EJProcessGlobal.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <cstdint>
#include <cstdlib>

namespace echojay
{

namespace
{
    // 'EJPG' + a layout generation. The header sits in front of the caller's block so a size
    // disagreement between two builds is detectable rather than silent corruption.
    constexpr std::uint64_t kMagic = 0x45'4A'50'47'00'00'00'01ull;

    struct Header
    {
        std::uint64_t magic;
        std::uint64_t bytes;
    };
}

void* processGlobal (const char* versionedKey, size_t bytes, bool* createdOut) noexcept
{
    if (createdOut != nullptr) *createdOut = false;
    if (versionedKey == nullptr || *versionedKey == 0 || bytes == 0) return nullptr;

    @try
    {
        // Uniqued by libobjc across every image in the process: this is the whole trick.
        const void* key = (const void*) sel_registerName (versionedKey);
        Class anchor = [NSNull class];     // Foundation; never unloaded; nothing of ours on it

        @synchronized (anchor)
        {
            if (id held = objc_getAssociatedObject (anchor, key))
            {
                auto* hdr = (Header*) [(NSValue*) held pointerValue];
                if (hdr == nullptr) return nullptr;
                // A different EchoJay build created it with a different shape: share nothing.
                if (hdr->magic != kMagic || hdr->bytes != (std::uint64_t) bytes) return nullptr;
                return (void*) (hdr + 1);
            }

            auto* hdr = (Header*) std::calloc (1, sizeof (Header) + bytes);
            if (hdr == nullptr) return nullptr;
            hdr->magic = kMagic;
            hdr->bytes = (std::uint64_t) bytes;
            // Deliberately never freed: the block outlives every bundle that uses it, which is the
            // point. One allocation per process, a few kB.
            objc_setAssociatedObject (anchor, key, [NSValue valueWithPointer: hdr],
                                      OBJC_ASSOCIATION_RETAIN);
            if (createdOut != nullptr) *createdOut = true;
            return (void*) (hdr + 1);
        }
    }
    @catch (NSException*) { return nullptr; }
}

} // namespace echojay
