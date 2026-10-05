#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace t4ff::json
{
// A parsed JSON value (enough for the generated layout files: objects keep their order).
struct Value
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value *get(std::string_view key) const;
    const Value &at(std::string_view key) const; // throws when missing
    int64_t integer() const
    {
        return static_cast<int64_t>(number);
    }
    int64_t integer_or(std::string_view key, int64_t fallback) const;
    std::string string_or(std::string_view key, std::string_view fallback) const;
};

Value parse(std::string_view text);
} // namespace t4ff::json
