#include "json_utils.h"

#include <cctype>

namespace codexlimit
{

size_t FindJsonKey(std::string_view json, std::string_view key)
{
    size_t pos = 0;
    while ((pos = json.find('"', pos)) != std::string_view::npos)
    {
        size_t value = pos + 1;
        if (value + key.size() < json.size() &&
            json.compare(value, key.size(), key) == 0 &&
            json[value + key.size()] == '"')
        {
            return pos;
        }
        pos = value;
    }
    return std::string_view::npos;
}

std::string JsonStringValue(std::string_view json, std::string_view key)
{
    size_t pos = FindJsonKey(json, key);
    if (pos == std::string_view::npos)
        return "";
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string_view::npos)
        return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos)
        return "";
    std::string out;
    bool escape = false;
    for (size_t i = pos + 1; i < json.size(); ++i)
    {
        char c = json[i];
        if (escape)
        {
            switch (c)
            {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            default:
                out.push_back(c);
                break;
            }
            escape = false;
            continue;
        }
        if (c == '\\')
        {
            escape = true;
            continue;
        }
        if (c == '"')
            break;
        out.push_back(c);
    }
    return out;
}

std::optional<long long> JsonNumberValue(std::string_view json, std::string_view key)
{
    size_t pos = FindJsonKey(json, key);
    if (pos == std::string_view::npos)
        return std::nullopt;
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string_view::npos)
        return std::nullopt;
    ++pos;
    while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos])))
        ++pos;
    bool neg = pos < json.size() && json[pos] == '-';
    if (neg)
        ++pos;
    long long value = 0;
    bool any = false;
    while (pos < json.size() && isdigit(static_cast<unsigned char>(json[pos])))
    {
        any = true;
        value = value * 10 + (json[pos++] - '0');
    }
    if (!any)
        return std::nullopt;
    return neg ? -value : value;
}

std::string_view ObjectForKey(std::string_view json, std::string_view key)
{
    size_t pos = FindJsonKey(json, key);
    if (pos == std::string_view::npos)
        return {};
    pos = json.find('{', pos + key.size() + 2);
    if (pos == std::string_view::npos)
        return {};
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = pos; i < json.size(); ++i)
    {
        char c = json[i];
        if (in_string)
        {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '{')
            ++depth;
        else if (c == '}' && --depth == 0)
            return json.substr(pos, i - pos + 1);
    }
    return {};
}

std::string JsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

bool ReplaceJsonStringValue(std::string &json, std::string_view key, std::string_view value)
{
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    size_t begin = json.find('"', pos + 1);
    if (begin == std::string::npos)
        return false;

    bool escape = false;
    for (size_t end = begin + 1; end < json.size(); ++end)
    {
        char c = json[end];
        if (escape)
        {
            escape = false;
        }
        else if (c == '\\')
        {
            escape = true;
        }
        else if (c == '"')
        {
            json.replace(begin + 1, end - begin - 1, JsonEscape(value));
            return true;
        }
    }
    return false;
}

} // namespace codexlimit
