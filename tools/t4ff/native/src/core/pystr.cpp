#include "core/pystr.h"

namespace t4ff::py
{
namespace
{
bool is_upper(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 192 && c <= 222 && c != 215);
}
bool is_lower(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 223 && c != 247) || c == 170 || c == 181 || c == 186;
}
// one character's capital ("" when it is not latin-1: left as it is by the callers)
std::string upper_of(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 224 && c <= 254 && c != 247))
        return std::string(1, static_cast<char>(c - 32));
    if (c == 223)
        return "SS";
    return std::string(1, static_cast<char>(c));
}
uint8_t lower_of(uint8_t c)
{
    return is_upper(c) ? static_cast<uint8_t>(c + 32) : c;
}
} // namespace

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = static_cast<char>(lower_of(static_cast<uint8_t>(c)));
    return out;
}

std::string lower_ascii(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 32);
    return out;
}

std::string upper(std::string_view s)
{
    std::string out;
    for (char c : s)
        out += upper_of(static_cast<uint8_t>(c));
    return out;
}

std::string title(std::string_view s)
{
    std::string out;
    bool previous_cased = false;
    for (char ch : s)
    {
        uint8_t c = static_cast<uint8_t>(ch);
        if (previous_cased)
            out += static_cast<char>(lower_of(c));
        else if (c == 223)
            out += "Ss";
        else
            out += upper_of(c);
        previous_cased = is_upper(c) || is_lower(c);
    }
    return out;
}

namespace
{
template <typename Pred> std::string_view strip_if(std::string_view s, Pred pred, bool left, bool right)
{
    size_t a = 0, b = s.size();
    while (left && a < b && pred(static_cast<uint8_t>(s[a])))
        ++a;
    while (right && b > a && pred(static_cast<uint8_t>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}
} // namespace

std::string_view strip(std::string_view s)
{
    return strip_if(s, str_space, true, true);
}
std::string_view rstrip(std::string_view s)
{
    return strip_if(s, str_space, false, true);
}
std::string_view bstrip(std::string_view s)
{
    return strip_if(s, bytes_space, true, true);
}
std::string_view brstrip(std::string_view s)
{
    return strip_if(s, bytes_space, false, true);
}
std::string_view strip_chars(std::string_view s, std::string_view chars)
{
    return strip_if(s, [&](uint8_t c) { return chars.find(static_cast<char>(c)) != std::string_view::npos; }, true, true);
}
std::string_view rstrip_chars(std::string_view s, std::string_view chars)
{
    return strip_if(s, [&](uint8_t c) { return chars.find(static_cast<char>(c)) != std::string_view::npos; }, false, true);
}

std::vector<std::string> split(std::string_view s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && str_space(static_cast<uint8_t>(s[i])))
            ++i;
        size_t start = i;
        while (i < s.size() && !str_space(static_cast<uint8_t>(s[i])))
            ++i;
        if (i > start)
            out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split(std::string_view s, std::string_view sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;)
    {
        size_t at = s.find(sep, start);
        if (at == std::string_view::npos)
        {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, at - start));
        start = at + sep.size();
    }
}

std::string join(const std::vector<std::string> &parts, std::string_view sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

std::string replace(std::string_view s, std::string_view from, std::string_view to)
{
    std::string out;
    size_t start = 0;
    for (;;)
    {
        size_t at = s.find(from, start);
        if (at == std::string_view::npos || from.empty())
        {
            out.append(s.substr(start));
            return out;
        }
        out.append(s.substr(start, at - start));
        out.append(to);
        start = at + from.size();
    }
}
} // namespace t4ff::py
