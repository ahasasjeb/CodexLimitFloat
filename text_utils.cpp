#define NOMINMAX

#include "text_utils.h"

#include <algorithm>
#include <cctype>
#include <windows.h>

namespace codexlimit
{

    std::wstring Utf8ToWide(std::string_view text)
    {
        if (text.empty())
            return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
        return out;
    }

    std::string WideToUtf8(std::wstring_view text)
    {
        if (text.empty())
            return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    std::string LowerAscii(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return out;
    }

} // namespace codexlimit
