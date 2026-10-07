#include "core/json.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace t4ff::json
{
namespace
{
class Parser
{
  public:
    explicit Parser(std::string_view text) : text(text)
    {
    }

    Value document()
    {
        Value v = value();
        space();
        if (pos != text.size())
            fail("trailing characters");
        return v;
    }

  private:
    std::string_view text;
    size_t pos = 0;

    [[noreturn]] void fail(const char *what) const
    {
        throw std::runtime_error(std::string("json: ") + what + " at offset " + std::to_string(pos));
    }

    void space()
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
            ++pos;
    }

    char peek()
    {
        space();
        if (pos >= text.size())
            fail("unexpected end");
        return text[pos];
    }

    void expect(char c)
    {
        if (peek() != c)
            fail("unexpected character");
        ++pos;
    }

    bool literal(std::string_view word)
    {
        if (text.substr(pos, word.size()) == word)
        {
            pos += word.size();
            return true;
        }
        return false;
    }

    Value value()
    {
        Value v;
        char c = peek();
        if (c == '{')
        {
            v.kind = Value::Kind::Object;
            ++pos;
            if (peek() == '}')
            {
                ++pos;
                return v;
            }
            for (;;)
            {
                std::string key = string_token();
                expect(':');
                v.object.emplace_back(std::move(key), value());
                if (peek() == ',')
                {
                    ++pos;
                    continue;
                }
                expect('}');
                return v;
            }
        }
        if (c == '[')
        {
            v.kind = Value::Kind::Array;
            ++pos;
            if (peek() == ']')
            {
                ++pos;
                return v;
            }
            for (;;)
            {
                v.array.push_back(value());
                if (peek() == ',')
                {
                    ++pos;
                    continue;
                }
                expect(']');
                return v;
            }
        }
        if (c == '"')
        {
            v.kind = Value::Kind::String;
            v.string = string_token();
            return v;
        }
        if (literal("true"))
        {
            v.kind = Value::Kind::Bool;
            v.boolean = true;
            return v;
        }
        if (literal("false"))
        {
            v.kind = Value::Kind::Bool;
            return v;
        }
        if (literal("null"))
            return v;
        // a number
        size_t start = pos;
        while (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '-' || text[pos] == '+' ||
                                      text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'))
            ++pos;
        if (start == pos)
            fail("unexpected character");
        v.kind = Value::Kind::Number;
        auto result = std::from_chars(text.data() + start, text.data() + pos, v.number);
        if (result.ec != std::errc())
            fail("bad number");
        return v;
    }

    std::string string_token()
    {
        expect('"');
        std::string out;
        while (pos < text.size() && text[pos] != '"')
        {
            char c = text[pos++];
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (pos >= text.size())
                fail("unterminated escape");
            char e = text[pos++];
            switch (e)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (pos + 4 > text.size())
                    fail("bad unicode escape");
                unsigned code = 0;
                std::from_chars(text.data() + pos, text.data() + pos + 4, code, 16);
                pos += 4;
                // a character past U+FFFF: a surrogate pair
                if (code >= 0xD800 && code < 0xDC00 && pos + 6 <= text.size() && text[pos] == '\\' && text[pos + 1] == 'u')
                {
                    unsigned low = 0;
                    std::from_chars(text.data() + pos + 2, text.data() + pos + 6, low, 16);
                    if (low >= 0xDC00 && low < 0xE000)
                    {
                        pos += 6;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        out += static_cast<char>(0xF0 | (code >> 18));
                        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                        break;
                    }
                }
                // the layouts are ASCII; anything else is kept as UTF-8
                if (code < 0x80)
                    out += static_cast<char>(code);
                else if (code < 0x800)
                {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default: out += e; break;
            }
        }
        if (pos >= text.size())
            fail("unterminated string");
        ++pos;
        return out;
    }
};
} // namespace

const Value *Value::get(std::string_view key) const
{
    for (const auto &[k, v] : object)
        if (k == key)
            return &v;
    return nullptr;
}

const Value &Value::at(std::string_view key) const
{
    const Value *v = get(key);
    if (!v)
        throw std::runtime_error("json: missing key " + std::string(key));
    return *v;
}

int64_t Value::integer_or(std::string_view key, int64_t fallback) const
{
    const Value *v = get(key);
    return v ? v->integer() : fallback;
}

std::string Value::string_or(std::string_view key, std::string_view fallback) const
{
    const Value *v = get(key);
    return v ? v->string : std::string(fallback);
}

Value parse(std::string_view text)
{
    return Parser(text).document();
}

namespace
{
void quote(std::string &out, const std::string &s)
{
    out += '"';
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else
                out += c;
        }
    }
    out += '"';
}

void write(std::string &out, const Value &v, int depth, const std::string &newline)
{
    std::string pad(static_cast<size_t>(depth + 1) * 2, ' '), end_pad(static_cast<size_t>(depth) * 2, ' ');
    switch (v.kind)
    {
    case Value::Kind::Null: out += "null"; break;
    case Value::Kind::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Kind::Number: {
        char buf[32];
        if (v.number == static_cast<double>(static_cast<int64_t>(v.number)) && v.number > -1e15 && v.number < 1e15)
            snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.number));
        else
            snprintf(buf, sizeof buf, "%.17g", v.number);
        out += buf;
        break;
    }
    case Value::Kind::String: quote(out, v.string); break;
    case Value::Kind::Array:
        if (v.array.empty())
        {
            out += "[]";
            break;
        }
        out += "[" + newline;
        for (size_t i = 0; i < v.array.size(); ++i)
        {
            out += pad;
            write(out, v.array[i], depth + 1, newline);
            out += (i + 1 < v.array.size() ? "," : "") + newline;
        }
        out += end_pad + "]";
        break;
    case Value::Kind::Object:
        if (v.object.empty())
        {
            out += "{}";
            break;
        }
        out += "{" + newline;
        for (size_t i = 0; i < v.object.size(); ++i)
        {
            out += pad;
            quote(out, v.object[i].first);
            out += ": ";
            write(out, v.object[i].second, depth + 1, newline);
            out += (i + 1 < v.object.size() ? "," : "") + newline;
        }
        out += end_pad + "}";
        break;
    }
}
} // namespace

std::string dump(const Value &v, const std::string &newline)
{
    std::string out;
    write(out, v, 0, newline);
    return out + newline;
}

void Value::set(std::string_view key, Value value)
{
    for (auto &[k, v] : object)
        if (k == key)
        {
            v = std::move(value);
            return;
        }
    object.emplace_back(std::string(key), std::move(value));
}

bool Value::erase(std::string_view key)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it->first == key)
        {
            object.erase(it);
            return true;
        }
    return false;
}

Value Value::of(bool b)
{
    Value v;
    v.kind = Kind::Bool;
    v.boolean = b;
    return v;
}

Value Value::of(const std::string &s)
{
    Value v;
    v.kind = Kind::String;
    v.string = s;
    return v;
}
} // namespace t4ff::json
