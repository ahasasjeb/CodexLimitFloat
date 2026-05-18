#define NOMINMAX

#include "relay_key.h"

#include "app_types.h"
#include "file_utils.h"

#include <windows.h>

namespace codexlimit
{

    namespace
    {

        bool EnsureRelayKeyFileAt(const std::wstring &dir)
        {
            if (dir.empty())
                return false;
            std::wstring path = JoinPath(dir, kRelayKeyFileName);
            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                return true;

            return WriteFileUtf8(path,
                                 "{\n"
                                 "  \"server_url\": \"https://your-domain.example/api/codex-relay\",\n"
                                 "  \"api_key\": \"填入你的转发服务器密钥\",\n"
                                 "  \"one_way_key\": \"填入单向加密密钥\"\n"
                                 "}\n");
        }

        void DeleteRelayKeyFileAt(const std::wstring &dir)
        {
            if (dir.empty())
                return;
            std::wstring path = JoinPath(dir, kRelayKeyFileName);
            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW(path.c_str());
        }

    } // namespace

    void DeleteRelayKeyFiles()
    {
        std::wstring exe_dir = GetExecutableDirectoryPath();
        std::wstring current_dir = GetCurrentDirectoryPath();
        DeleteRelayKeyFileAt(exe_dir);
        if (current_dir != exe_dir)
            DeleteRelayKeyFileAt(current_dir);
    }

    void EnsureRelayKeyFile()
    {
        EnsureRelayKeyFileAt(GetExecutableDirectoryPath());
        EnsureRelayKeyFileAt(GetCurrentDirectoryPath());
    }

} // namespace codexlimit
