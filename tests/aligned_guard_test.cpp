// The exit free guard (vanilla_settings.cpp, r161) frees through the game's
// CRT only when the qword before the block points 8 to 0x10008 bytes below
// it. Checks that every block from the MSVC CRT's _aligned_malloc keeps its
// real block there, for every alignment up to 64 KB and several sizes, and
// that the values seen at the game's exit (0, 0x1FFFFFFFF, 0x200000000, a
// pointer above the block) fail the check.
#include <malloc.h>

#include <initializer_list>

#include <cstdint>
#include <cstdio>

namespace {

constexpr std::uintptr_t kAlignedSlack = 0x10000 + 8;

bool looks_like_crt_block(std::uintptr_t block, std::uintptr_t stored) {
    return stored && stored < block && block - stored <= kAlignedSlack;
}

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    bool all = true;
    int blocks = 0;
    for (std::size_t alignment = 1; alignment <= 0x10000; alignment <<= 1) {
        for (std::size_t size : {1u, 7u, 16u, 100u, 4096u, 70000u}) {
            void* p = _aligned_malloc(size, alignment);
            if (!p) { all = false; continue; }
            const auto block = reinterpret_cast<std::uintptr_t>(p);
            const auto stored = *reinterpret_cast<const std::uintptr_t*>((block & ~std::uintptr_t{7}) - 8);
            if (!looks_like_crt_block(block, stored)) all = false;
            _aligned_free(p);
            ++blocks;
        }
    }
    check(all && blocks == 17 * 6, "every _aligned_malloc block (alignment 1..64 KB) passes");
    const std::uintptr_t operator_block = 0x7FF441EC7700;
    check(!looks_like_crt_block(operator_block, 0), "stored 0 fails");
    check(!looks_like_crt_block(operator_block, 0x1FFFFFFFF), "stored 0x1FFFFFFFF fails");
    check(!looks_like_crt_block(operator_block, 0x200000000), "stored 0x200000000 fails");
    check(!looks_like_crt_block(0x7FF37586B540, 0x7FF37586B613), "a pointer above the block fails");
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
