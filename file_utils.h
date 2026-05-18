#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codexlimit
{

    std::wstring GetEnvVar(const wchar_t *name);
    std::wstring JoinPath(std::wstring base, std::wstring_view leaf);
    std::wstring GetCurrentDirectoryPath();
    std::wstring GetExecutableDirectoryPath();
    std::optional<std::string> ReadFileUtf8(const std::wstring &path);
    bool WriteFileUtf8(const std::wstring &path, const std::string &data);
    std::wstring GetCodexHome();

} // namespace codexlimit
