#include "core/json.h"

#include <cctype>
#include <charconv>
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
} // namespace t4ff::json
