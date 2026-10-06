#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Python's str and bytes methods on latin-1 text (the Python t4ff decodes scripts and menu strings
// as latin-1: its str methods follow Unicode on those 256 characters, its bytes methods ASCII).
namespace t4ff::py
{
// str.isspace / bytes.isspace
inline bool str_space(uint8_t c)
{
    return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 133 || c == 160;
}
inline bool bytes_space(uint8_t c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}
inline bool ascii_alnum(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// str.lower / bytes.lower
std::string lower(std::string_view s);
std::string lower_ascii(std::string_view s);
// str.upper (ß becomes SS; µ and ÿ, whose capitals are not latin-1, stay)
std::string upper(std::string_view s);
// str.title
std::string title(std::string_view s);

// str.strip and its kin (Unicode whitespace), bytes.strip and its kin (ASCII), strip(chars)
std::string_view strip(std::string_view s);
std::string_view rstrip(std::string_view s);
std::string_view bstrip(std::string_view s);
std::string_view brstrip(std::string_view s);
std::string_view strip_chars(std::string_view s, std::string_view chars);
std::string_view rstrip_chars(std::string_view s, std::string_view chars);

// str.split() (runs of whitespace) and str.split(sep)
std::vector<std::string> split(std::string_view s);
std::vector<std::string> split(std::string_view s, std::string_view sep);
std::string join(const std::vector<std::string> &parts, std::string_view sep);
// str.replace (every occurrence)
std::string replace(std::string_view s, std::string_view from, std::string_view to);

inline bool contains(std::string_view s, std::string_view sub)
{
    return s.find(sub) != std::string_view::npos;
}
inline bool starts_with(std::string_view s, std::string_view prefix)
{
    return s.substr(0, prefix.size()) == prefix;
}
inline bool ends_with(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
} // namespace t4ff::py
