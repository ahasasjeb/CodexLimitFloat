#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codexlimit
{

size_t FindJsonKey(std::string_view json, std::string_view key);
std::string JsonStringValue(std::string_view json, std::string_view key);
std::optional<long long> JsonNumberValue(std::string_view json, std::string_view key);
std::string_view ObjectForKey(std::string_view json, std::string_view key);
std::string JsonEscape(std::string_view text);
bool ReplaceJsonStringValue(std::string &json, std::string_view key, std::string_view value);

} // namespace codexlimit
