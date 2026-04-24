#include "re_tools.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>

#include <cstring>

#include <core/Functions.h>  // KenshiLib

#include "kmp_log.h"

#pragma comment(lib, "Psapi.lib")

namespace kmp {

namespace {

static uintptr_t s_base = 0;
static size_t    s_size = 0;
static bool      s_inited = false;

} // namespace

void re_tools_init() {
    if (s_inited) return;
    s_inited = true;
    HMODULE h = GetModuleHandleA("kenshi_x64.exe");
    if (!h) {
        KMP_LOG("[KenshiMP] re_tools: kenshi_x64.exe handle not found");
        return;
    }
    MODULEINFO info;
    std::memset(&info, 0, sizeof(info));
    if (!GetModuleInformation(GetCurrentProcess(), h, &info, sizeof(info))) {
        KMP_LOG("[KenshiMP] re_tools: GetModuleInformation failed");
        return;
    }
    s_base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    s_size = static_cast<size_t>(info.SizeOfImage);
    char buf[128];
    _snprintf(buf, sizeof(buf),
        "[KenshiMP] re_tools: kenshi_x64.exe base=0x%llx size=0x%llx",
        static_cast<unsigned long long>(s_base),
        static_cast<unsigned long long>(s_size));
    KMP_LOG(buf);
}

uintptr_t re_kenshi_base() { return s_base; }
size_t    re_kenshi_size() { return s_size; }

uintptr_t re_pattern_scan(const uint8_t* pattern, const char* mask, size_t len) {
    if (!s_base || !s_size || !pattern || !mask || len == 0) return 0;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(s_base);
    // Scan .text only — the PE header's first section is usually .text;
    // but for simplicity we scan the whole image. Fast enough for ad-hoc
    // RE sessions.
    for (size_t i = 0; i + len <= s_size; ++i) {
        bool match = true;
        for (size_t j = 0; j < len; ++j) {
            if (mask[j] == '?') continue;
            if (base[i + j] != pattern[j]) { match = false; break; }
        }
        if (match) return s_base + i;
    }
    return 0;
}

bool re_hook(uintptr_t addr, void* detour, void** orig) {
    if (!addr || !detour) return false;
    KenshiLib::HookStatus rc = KenshiLib::AddHook(
        static_cast<intptr_t>(addr), detour, orig);
    if (rc != KenshiLib::SUCCESS) {
        char buf[96];
        _snprintf(buf, sizeof(buf),
            "[KenshiMP] re_tools: AddHook failed at 0x%llx status=%d",
            static_cast<unsigned long long>(addr),
            static_cast<int>(rc));
        KMP_LOG(buf);
        return false;
    }
    return true;
}

uintptr_t re_resolve_symbol(const char* mangled) {
    HMODULE klib = GetModuleHandleA("KenshiLib.dll");
    if (!klib || !mangled) return 0;
    void* stub = reinterpret_cast<void*>(GetProcAddress(klib, mangled));
    if (!stub) return 0;
    return static_cast<uintptr_t>(KenshiLib::GetRealAddress(stub));
}

bool re_write_bytes(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, src, n);
    DWORD ignored = 0;
    VirtualProtect(dst, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

} // namespace kmp
