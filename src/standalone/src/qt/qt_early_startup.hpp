#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aida_early_startup {

extern std::atomic<const char*> g_phase;

void install();
void mark(const char* phase);
void mark_normal_diagnostics_reached();
void wide_to_utf8(const wchar_t* in, char* out, size_t cap);

}
