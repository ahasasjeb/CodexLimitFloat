#define NOMINMAX

#include "file_utils.h"

#include <windows.h>

namespace codexlimit
{

    std::wstring GetEnvVar(const wchar_t *name)
    {
        DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size == 0)
            return L"";
        std::wstring value(size - 1, L'\0');
        GetEnvironmentVariableW(name, value.data(), size);
        return value;
    }

    std::wstring JoinPath(std::wstring base, std::wstring_view leaf)
    {
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
            base.push_back(L'\\');
        base.append(leaf);
        return base;
    }

    std::wstring GetCurrentDirectoryPath()
    {
        DWORD size = GetCurrentDirectoryW(0, nullptr);
        if (size == 0)
            return L"";
        std::wstring path(size, L'\0');
        GetCurrentDirectoryW(size, path.data());
        path.resize(size - 1);
        return path;
    }

    std::wstring GetExecutableDirectoryPath()
    {
        wchar_t path[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        if (length == 0 || length >= ARRAYSIZE(path))
            return GetCurrentDirectoryPath();
        std::wstring exe_path(path, length);
        size_t slash = exe_path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return GetCurrentDirectoryPath();
        return exe_path.substr(0, slash);
    }

    std::optional<std::string> ReadFileUtf8(const std::wstring &path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return std::nullopt;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart > 8 * 1024 * 1024)
        {
            CloseHandle(file);
            return std::nullopt;
        }
        std::string data(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok)
            return std::nullopt;
        data.resize(read);
        return data;
    }

    bool WriteFileUtf8(const std::wstring &path, const std::string &data)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(file);
        return ok && written == data.size();
    }

    std::wstring GetCodexHome()
    {
        std::wstring home = GetEnvVar(L"CODEX_HOME");
        if (!home.empty())
            return home;
        std::wstring user = GetEnvVar(L"USERPROFILE");
        return JoinPath(user, L".codex");
    }

} // namespace codexlimit
