// re_tools.h — Low-level reverse-engineering utilities.
//
// Wraps pattern scanning in kenshi_x64.exe, hooks at arbitrary
// addresses via KenshiLib::AddHook, and page-permission patching for
// poking bytes directly. Used to instrument native Kenshi code paths
// we don't have symbols for (e.g., CharacterEditWindow internals).
#pragma once

#include <cstddef>
#include <cstdint>

namespace kmp {

// One-time init (idempotent). Resolves the base + size of kenshi_x64.exe
// and caches them. Call before any other function here.
void re_tools_init();

// Byte-pattern scan across kenshi_x64.exe's .text region. `pattern` is a
// sequence of byte values; `mask` is the same length, '?' = wildcard, 'x'
// = exact match. Returns absolute address of the first match, or 0 if not
// found.
//
// Example:
//   uint8_t  pat[]  = { 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00 };
//   const char* m = "xxx????";
//   uintptr_t a = re_pattern_scan(pat, m, sizeof(pat));
uintptr_t re_pattern_scan(const uint8_t* pattern, const char* mask, size_t len);

// Install a detour at an absolute address via KenshiLib::AddHook. `orig`
// receives the trampoline for calling through to the original. Returns
// true on success.
bool re_hook(uintptr_t addr, void* detour, void** orig);

// Resolve a KenshiLib-exported mangled symbol to its real address in
// kenshi_x64.exe (handles the stub → real redirect). Returns 0 if the
// symbol isn't exported.
uintptr_t re_resolve_symbol(const char* mangled);

// Write raw bytes to protected memory. Flips VirtualProtect, writes,
// restores. Returns true on success. Useful for NOP patching.
bool re_write_bytes(void* dst, const void* src, size_t n);

// Absolute base + size of kenshi_x64.exe (0 before re_tools_init).
uintptr_t re_kenshi_base();
size_t    re_kenshi_size();

} // namespace kmp
