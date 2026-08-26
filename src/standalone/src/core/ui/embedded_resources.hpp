#pragma once


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <string>

namespace embedded_resources {

namespace detail {


inline bool write_resource_to_file(const void* data, size_t size,
                                   const std::wstring& path)
{
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY,
                            nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hf, data, static_cast<DWORD>(size), &written, nullptr);
    FlushFileBuffers(hf);
    CloseHandle(hf);
    return ok && written == static_cast<DWORD>(size);
}


inline std::wstring make_temp_dir()
{
    wchar_t tmp_dir[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp_dir))
        return {};
    wchar_t tmp_file[MAX_PATH] = {};
    if (!GetTempFileNameW(tmp_dir, L"gsp", 0, tmp_file))
        return {};

    DeleteFileW(tmp_file);
    std::wstring dir(tmp_file);
    dir += L"_d";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

}

std::string extract_ghidra_specs();

std::size_t ghidra_spec_resource_count(std::size_t& total_bytes);


inline void delete_specs_dir(const std::string& dir)
{
    if (dir.empty()) return;

    wchar_t wide[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, dir.c_str(), -1, wide, MAX_PATH);

    static const wchar_t* filenames[] = {
        L"x86-64.sla", L"x86-64.pspec", L"x86-64-win.cspec", L"x86.ldefs"
    };
    for (auto* fn : filenames) {
        std::wstring path = std::wstring(wide) + L"\\" + fn;
        DeleteFileW(path.c_str());
    }
    RemoveDirectoryW(wide);
}

}
