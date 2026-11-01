// File names between the port's narrow strings and std::filesystem.
//
// The port keeps file names as narrow strings in the encoding fopen takes:
// the system code page on Windows (what getenv gives there), bytes elsewhere.
// On Windows libstdc++'s std::filesystem::path(std::string) and path::string()
// convert through the "C" locale, which takes ASCII alone and throws
// filesystem_error at anything else: a Windows account named in Cyrillic
// (%LOCALAPPDATA% holding it) ended the game as the texture pack's level
// cache folder was made. These convert through the code page instead; a name
// it cannot spell comes back with '?' in it, which fopen then fails to open.
#ifndef SMS_GX_PATH_H
#define SMS_GX_PATH_H

#include <filesystem>
#include <string>

namespace gx {

std::filesystem::path fsPath(const std::string& name);
std::string fsString(const std::filesystem::path& path);

}  // namespace gx

#endif
