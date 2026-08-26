#include "embedded_resources.hpp"

#include <QByteArray>
#include <QResource>

#include <utility>
#include <vector>

namespace embedded_resources {

std::string extract_ghidra_specs()
{
    std::wstring dir = detail::make_temp_dir();
    if (dir.empty())
        return {};

    struct spec_file {
        const char* resource_path;
        const wchar_t* filename;
    };

    static const spec_file specs[] = {
        { ":/ghidra/x86-64.sla",       L"x86-64.sla" },
        { ":/ghidra/x86-64.pspec",     L"x86-64.pspec" },
        { ":/ghidra/x86-64-win.cspec", L"x86-64-win.cspec" },
        { ":/ghidra/x86.ldefs",        L"x86.ldefs" },
    };

    std::vector<std::wstring> written;
    written.reserve(sizeof(specs) / sizeof(specs[0]));

    auto cleanup_on_failure = [&]() {
        for (const auto& w : written)
            DeleteFileW(w.c_str());
        RemoveDirectoryW(dir.c_str());
    };

    for (auto& s : specs) {
        const QResource resource(QString::fromLatin1(s.resource_path));
        if (!resource.isValid()) {
            OutputDebugStringA("embedded_resources: ghidra spec resource not found\n");
            cleanup_on_failure();
            return {};
        }
        const QByteArray payload = resource.uncompressedData();
        if (payload.isEmpty()) {
            OutputDebugStringA("embedded_resources: failed to decompress ghidra spec resource\n");
            cleanup_on_failure();
            return {};
        }

        std::wstring file_path = dir + L"\\" + s.filename;
        if (!detail::write_resource_to_file(payload.constData(),
                                            static_cast<size_t>(payload.size()),
                                            file_path)) {
            OutputDebugStringA("embedded_resources: failed to write ghidra spec file\n");
            cleanup_on_failure();
            return {};
        }
        written.push_back(std::move(file_path));
    }


    char narrow[MAX_PATH] = {};
    WideCharToMultiByte(CP_ACP, 0, dir.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
    return std::string(narrow);
}

std::size_t ghidra_spec_resource_count(std::size_t& total_bytes)
{
    total_bytes = 0;
    static const char* const paths[] = {
        ":/ghidra/x86-64.sla",
        ":/ghidra/x86-64.pspec",
        ":/ghidra/x86-64-win.cspec",
        ":/ghidra/x86.ldefs"
    };
    std::size_t count = 0;
    for (const char* path : paths) {
        const QResource resource(QString::fromLatin1(path));
        if (!resource.isValid())
            continue;
        const qint64 size = resource.uncompressedSize();
        if (size <= 0)
            continue;
        ++count;
        total_bytes += static_cast<std::size_t>(size);
    }
    return count;
}

}
