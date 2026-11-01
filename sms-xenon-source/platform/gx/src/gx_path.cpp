// File names between narrow strings and std::filesystem (gx_path.h).
#include "gx_path.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gx {

std::filesystem::path fsPath(const std::string& name) {
#ifdef _WIN32
    if (name.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, name.data(), int(name.size()), nullptr, 0);
    std::wstring wide(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, name.data(), int(name.size()), &wide[0], n);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(name);
#endif
}

std::string fsString(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& wide = path.native();
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, wide.data(), int(wide.size()), &out[0], n, nullptr, nullptr);
    return out;
#else
    return path.string();
#endif
}

}  // namespace gx
