#include "EJProcessGlobal.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Off-Mac counterpart of EJProcessGlobal.mm, in the shape NativeClipWin.cpp has: the same surface,
// one table per image. EchoJay does not ship on Windows and the real-editor road is AU/VST3 on
// macOS, so this keeps the tree compiling without pretending to a cross-image rendezvous (the
// Windows one would be a named section or a shared mapping; it is not needed and so is not guessed).
namespace echojay
{

void* processGlobal (const char* versionedKey, size_t bytes, bool* createdOut) noexcept
{
    if (createdOut != nullptr) *createdOut = false;
    if (versionedKey == nullptr || *versionedKey == 0 || bytes == 0) return nullptr;

    struct Entry { std::string key; size_t bytes; void* mem; };
    static std::mutex mx;
    static std::vector<Entry> table;

    std::lock_guard<std::mutex> lock (mx);
    for (const auto& e : table)
        if (e.key == versionedKey)
            return e.bytes == bytes ? e.mem : nullptr;

    void* mem = std::calloc (1, bytes);
    if (mem == nullptr) return nullptr;
    table.push_back ({ versionedKey, bytes, mem });
    if (createdOut != nullptr) *createdOut = true;
    return mem;
}

} // namespace echojay
