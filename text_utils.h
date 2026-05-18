#pragma once

#include <string>
#include <string_view>

namespace codexlimit
{

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
std::string LowerAscii(std::string_view text);

} // namespace codexlimit
