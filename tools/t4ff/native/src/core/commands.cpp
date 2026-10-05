#include "core/commands.h"

#include <cctype>
#include <stdexcept>

namespace t4ff
{
namespace
{
const Expr NEVER_EXPR = [] {
    Expr e;
    e.op = Expr::Op::Never;
    return e;
}();

// Python's int(text, 0): decimal, or 0x / 0o / 0b prefixed.
int64_t parse_int(const std::string &text)
{
    std::string t = text;
    int sign = 1;
    size_t i = 0;
    if (i < t.size() && (t[i] == '-' || t[i] == '+'))
    {
        sign = t[i] == '-' ? -1 : 1;
        ++i;
    }
    int base = 10;
    if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X'))
    {
        base = 16;
        i += 2;
    }
    else if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'o' || t[i + 1] == 'O'))
    {
        base = 8;
        i += 2;
    }
    else if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'b' || t[i + 1] == 'B'))
    {
        base = 2;
        i += 2;
    }
    if (i >= t.size())
        throw std::runtime_error("invalid integer " + text);
    int64_t value = 0;
    for (; i < t.size(); ++i)
    {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(t[i])));
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99;
        if (digit >= base)
            throw std::runtime_error("invalid integer " + text);
        value = value * base + digit;
    }
    return sign * value;
}

std::vector<std::string> split_ws(const std::string &s, size_t max_parts = 0)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size())
            break;
        if (max_parts && out.size() + 1 == max_parts)
        {
            std::string rest = s.substr(i);
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
                rest.pop_back();
            out.push_back(rest);
            break;
        }
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])))
            ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string strip(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

bool starts_with(const std::string &s, std::string_view prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool is_word(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The Python _TOKEN regex, by hand.
std::vector<std::string> tokenize(std::string_view text)
{
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < text.size())
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos >= text.size())
            break;
        char c = text[pos];
        if (c == '0' && pos + 2 < text.size() + 0 && (text[pos + 1] == 'x' || text[pos + 1] == 'X') && pos + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[pos + 2])))
        {
            size_t j = pos + 2;
            while (j < text.size() && std::isxdigit(static_cast<unsigned char>(text[j])))
                ++j;
            tokens.emplace_back(text.substr(pos, j - pos));
            pos = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            size_t j = pos;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])))
                ++j;
            tokens.emplace_back(text.substr(pos, j - pos));
            pos = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t j = pos;
            while (j < text.size() && is_word(text[j]))
                ++j;
            // (?:::[A-Za-z_]\w*)*
            while (j + 2 < text.size() && text[j] == ':' && text[j + 1] == ':' &&
                   (std::isalpha(static_cast<unsigned char>(text[j + 2])) || text[j + 2] == '_'))
            {
                j += 2;
                while (j < text.size() && is_word(text[j]))
                    ++j;
            }
            tokens.emplace_back(text.substr(pos, j - pos));
            pos = j;
            continue;
        }
        static const char *TWO[] = {"&&", "||", "==", "!=", "<=", ">=", "<<", ">>"};
        bool matched = false;
        for (const char *two : TWO)
        {
            if (text.substr(pos, 2) == two)
            {
                tokens.emplace_back(two);
                pos += 2;
                matched = true;
                break;
            }
        }
        if (matched)
            continue;
        if (std::string_view("-+*/%()<>!~&|^?:[]").find(c) != std::string_view::npos)
        {
            tokens.emplace_back(1, c);
            ++pos;
            continue;
        }
        throw std::runtime_error("bad expression near " + std::string(text.substr(pos)));
    }
    return tokens;
}

class ExprParser
{
  public:
    ExprParser(std::string_view text, const std::unordered_map<std::string, int64_t> &enums, std::deque<Expr> &store)
        : tokens(tokenize(strip(text))), enums(enums), store(store)
    {
    }

    const Expr *parse()
    {
        const Expr *e = ternary();
        if (pos < tokens.size())
            throw std::runtime_error("trailing tokens in expression");
        return e;
    }

  private:
    std::vector<std::string> tokens;
    size_t pos = 0;
    const std::unordered_map<std::string, int64_t> &enums;
    std::deque<Expr> &store;

    const std::string *peek() const
    {
        return pos < tokens.size() ? &tokens[pos] : nullptr;
    }
    bool peek_is(std::string_view s) const
    {
        return pos < tokens.size() && tokens[pos] == s;
    }
    std::string take(const char *expected = nullptr)
    {
        if (expected && !peek_is(expected))
            throw std::runtime_error(std::string("expected ") + expected + " in expression");
        if (pos >= tokens.size())
            throw std::runtime_error("unexpected end of expression");
        return tokens[pos++];
    }
    Expr *make(Expr::Op op)
    {
        Expr &e = store.emplace_back();
        e.op = op;
        return &e;
    }

    const Expr *ternary()
    {
        const Expr *cond = binary(0);
        if (peek_is("?"))
        {
            take();
            const Expr *a = ternary();
            take(":");
            const Expr *b = ternary();
            Expr *e = make(Expr::Op::Ternary);
            e->a = cond;
            e->b = a;
            e->c = b;
            return e;
        }
        return cond;
    }

    static const std::vector<std::vector<std::string>> &levels()
    {
        static const std::vector<std::vector<std::string>> PRECEDENCE = {
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", "<=", ">", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"},
        };
        return PRECEDENCE;
    }

    bool at_level(size_t level) const
    {
        const std::string *tok = peek();
        if (!tok)
            return false;
        for (const std::string &op : levels()[level])
            if (*tok == op)
                return true;
        return false;
    }

    const Expr *binary(size_t level)
    {
        if (level == levels().size())
            return unary();
        const Expr *lhs = binary(level + 1);
        while (at_level(level))
        {
            std::string op = take();
            const Expr *rhs = binary(level + 1);
            Expr *e = make(Expr::Op::Binary);
            e->oper = op;
            e->a = lhs;
            e->b = rhs;
            lhs = e;
        }
        return lhs;
    }

    const Expr *unary()
    {
        if (peek_is("-") || peek_is("!") || peek_is("~"))
        {
            std::string op = take();
            Expr *e = make(Expr::Op::Unary);
            e->oper = op;
            e->a = unary();
            return e;
        }
        if (peek_is("+"))
        {
            take();
            return unary();
        }
        return primary();
    }

    const Expr *primary()
    {
        if (!peek())
            throw std::runtime_error("unexpected end of expression");
        std::string tok = take();
        if (tok == "(")
        {
            const Expr *e = ternary();
            take(")");
            return e;
        }
        if (std::isdigit(static_cast<unsigned char>(tok[0])))
        {
            Expr *e = make(Expr::Op::Num);
            e->value = parse_int(tok);
            return e;
        }
        if (auto it = enums.find(tok); it != enums.end())
        {
            Expr *e = make(Expr::Op::Num);
            e->value = it->second;
            return e;
        }
        if (tok == "true" || tok == "false")
        {
            Expr *e = make(Expr::Op::Num);
            e->value = tok == "true";
            return e;
        }
        Expr *var = make(Expr::Op::Var);
        size_t start = 0;
        for (;;)
        {
            size_t sep = tok.find("::", start);
            var->path.push_back(tok.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
            if (sep == std::string::npos)
                break;
            start = sep + 2;
        }
        while (peek_is("["))
        {
            take();
            var->indices.push_back(ternary());
            take("]");
        }
        return var;
    }
};

// Python's % and // for negative operands.
int64_t py_mod(int64_t a, int64_t b)
{
    if (b == 0)
        throw std::runtime_error("modulo by zero in an expression");
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}
} // namespace

const Expr *never_expr()
{
    return &NEVER_EXPR;
}

int64_t Expr::eval(EvalContext *ctx) const
{
    switch (op)
    {
    case Op::Num: return value;
    case Op::Never: return 0;
    case Op::Var: {
        std::vector<int64_t> idx;
        idx.reserve(indices.size());
        for (const Expr *i : indices)
            idx.push_back(i->eval(ctx));
        return ctx->lookup(path, idx);
    }
    case Op::Unary: {
        int64_t v = a->eval(ctx);
        if (oper == "-")
            return -v;
        if (oper == "!")
            return v == 0;
        return ~v;
    }
    case Op::Ternary: return a->eval(ctx) ? b->eval(ctx) : c->eval(ctx);
    case Op::Binary: {
        if (oper == "&&")
            return a->eval(ctx) != 0 && b->eval(ctx) != 0;
        if (oper == "||")
            return a->eval(ctx) != 0 || b->eval(ctx) != 0;
        int64_t x = a->eval(ctx);
        int64_t y = b->eval(ctx);
        const std::string &o = oper;
        if (o == "+") return x + y;
        if (o == "-") return x - y;
        if (o == "*") return x * y;
        if (o == "/") return y ? static_cast<int64_t>(static_cast<double>(x) / static_cast<double>(y)) : 0;
        if (o == "%") return py_mod(x, y);
        if (o == "<<") return x << y;
        if (o == ">>") return x >> y;
        if (o == "&") return x & y;
        if (o == "|") return x | y;
        if (o == "^") return x ^ y;
        if (o == "==") return x == y;
        if (o == "!=") return x != y;
        if (o == "<") return x < y;
        if (o == "<=") return x <= y;
        if (o == ">") return x > y;
        if (o == ">=") return x >= y;
        throw std::runtime_error("unknown operator " + o);
    }
    }
    return 0;
}

const Expr *CommandParser::parse_expr(std::string_view text)
{
    if (strip(text) == "never")
        return never_expr();
    return ExprParser(text, layout.enums, cmds.exprs).parse();
}

void CommandParser::parse_text(std::string_view text)
{
    // comments go, then statements are split at ';' with their whitespace collapsed
    std::string joined;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        size_t comment = line.find("//");
        if (comment != std::string_view::npos)
            line = line.substr(0, comment);
        joined.append(line);
        joined.push_back('\n');
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    size_t pos = 0;
    while (pos <= joined.size())
    {
        size_t semi = joined.find(';', pos);
        std::string raw = joined.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        std::string collapsed;
        for (const std::string &word : split_ws(raw))
        {
            if (!collapsed.empty())
                collapsed.push_back(' ');
            collapsed += word;
        }
        if (!collapsed.empty())
            statement(collapsed);
        if (semi == std::string::npos)
            break;
        pos = semi + 1;
    }
}

void CommandParser::statement(const std::string &s)
{
    try
    {
        do_statement(s);
    }
    catch (const MissingMember &)
    {
        if (!lenient)
            throw;
        skipped.push_back(s);
    }
}

void CommandParser::do_statement(const std::string &s)
{
    if (starts_with(s, "game ") || starts_with(s, "wordsize ") || starts_with(s, "architecture "))
        return;
    if (starts_with(s, "asset "))
    {
        auto parts = split_ws(s);
        if (parts.size() != 3)
            throw std::runtime_error("bad asset command " + s);
        for (auto &[rec, e] : cmds.assets)
        {
            if (rec == parts[1])
            {
                e = parts[2];
                return;
            }
        }
        cmds.assets.emplace_back(parts[1], parts[2]);
        return;
    }
    if (starts_with(s, "block "))
    {
        auto parts = split_ws(s);
        cmds.blocks.emplace_back(parts[1], parts[2], parts.size() > 3 && parts[3] == "default");
        return;
    }
    if (starts_with(s, "use "))
    {
        use = split_ws(s)[1];
        if (!layout.find(*use))
            throw MissingMember("use of unknown type " + *use);
        return;
    }
    if (starts_with(s, "reorder"))
    {
        // reorder\s*(\w+)?\s*:\s*(.*)
        size_t i = 7;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        size_t name_start = i;
        while (i < s.size() && is_word(s[i]))
            ++i;
        std::string target = s.substr(name_start, i - name_start);
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i >= s.size() || s[i] != ':')
            throw std::runtime_error("bad reorder " + s);
        if (target.empty())
            target = use.value_or("");
        cmds.type_info(target).reorder = split_ws(s.substr(i + 1));
        return;
    }
    if (starts_with(s, "set "))
    {
        set_statement(s.substr(4));
        return;
    }
    throw std::runtime_error("unknown command " + s);
}

bool CommandParser::resolve_is_type(const std::string &name) const
{
    return !use || layout.record(*use).field(name) == nullptr;
}

MemberInfo &CommandParser::member(const std::string &owner, const std::string &member_name, const std::string &ctx)
{
    MemberInfos &infos = cmds.members[{owner, member_name}];
    for (auto &[c, info] : infos)
        if (c == ctx)
            return info;
    MemberInfo info;
    info.context = ctx;
    infos.emplace_back(ctx, std::move(info));
    return infos.back().second;
}

CommandParser::Resolved CommandParser::resolve_member(const std::string &path_in)
{
    Resolved r;
    std::string path;
    for (size_t i = 0; i < path_in.size();)
    {
        if (path_in[i] == '[')
        {
            size_t close = path_in.find(']', i);
            r.indices.push_back(parse_int(path_in.substr(i + 1, close - i - 1)));
            i = close + 1;
            continue;
        }
        path.push_back(path_in[i++]);
    }
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;)
    {
        size_t sep = path.find("::", start);
        parts.push_back(path.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos)
            break;
        start = sep + 2;
    }

    if (parts.size() > 1 && layout.find(parts[0]) && (!use || layout.record(*use).field(parts[0]) == nullptr))
    {
        r.ctx = parts[0];
        parts.erase(parts.begin());
    }
    else
    {
        if (!use)
            throw MissingMember("no type in use for " + path_in);
        r.ctx = *use;
    }

    const Record *owner = layout.find(r.ctx);
    if (!owner)
        throw MissingMember("unknown type " + r.ctx);
    for (size_t i = 0; i + 1 < parts.size(); ++i)
    {
        const Field *f = owner->field(parts[i]);
        if (!f)
            throw MissingMember(owner->name + " has no member " + parts[i] + " (" + path_in + ")");
        const TypeRef *t = f->type;
        while (t->kind == TypeKind::Array)
            t = t->elem;
        if (t->kind != TypeKind::Record)
            throw MissingMember(owner->name + "::" + parts[i] + " is not a record (" + path_in + ")");
        owner = layout.find(t->name);
        if (!owner)
            throw MissingMember("unknown type " + t->name);
    }
    if (!owner->field(parts.back()))
        throw MissingMember(owner->name + " has no member " + parts.back() + " (" + path_in + ")");
    r.owner = owner->name;
    r.member = parts.back();
    return r;
}

void CommandParser::set_statement(const std::string &s)
{
    size_t space = s.find(' ');
    std::string kind = s.substr(0, space);
    std::string rest = space == std::string::npos ? std::string() : s.substr(space + 1);
    if (kind == "action")
        return;

    if (kind == "string" || kind == "scriptstring" || kind == "reusable")
    {
        Resolved r = resolve_member(strip(rest));
        MemberInfo &info = member(r.owner, r.member, r.ctx);
        if (kind == "string")
            info.string = true;
        else if (kind == "scriptstring")
            info.scriptstring = true;
        else
            info.reusable = true;
        return;
    }

    size_t sp = rest.find(' ');
    std::string path = rest.substr(0, sp);
    std::string arg = sp == std::string::npos ? std::string() : strip(rest.substr(sp + 1));

    if (kind == "block")
    {
        if (arg.empty())
        {
            cmds.type_info(use.value_or("")).block = path;
            return;
        }
        Resolved r = resolve_member(path);
        member(r.owner, r.member, r.ctx).block = arg;
        return;
    }
    if (kind == "allocalign")
    {
        if (layout.find(path) && resolve_is_type(path))
        {
            cmds.type_info(path).allocalign = parse_expr(arg);
            return;
        }
        Resolved r = resolve_member(path);
        member(r.owner, r.member, r.ctx).allocalign = parse_expr(arg);
        return;
    }
    if (kind == "delayed")
    {
        Resolved r = resolve_member(path);
        auto parts = split_ws(arg, 3);
        MemberInfo &info = member(r.owner, r.member, r.ctx);
        Delayed d;
        d.block = parts.at(0);
        d.alignment = parts.size() > 1 ? parse_int(parts[1]) : 1;
        d.condition = parts.size() > 2 ? parse_expr(parts[2]) : nullptr;
        info.delayed = d;
        return;
    }
    if (kind == "assetref")
    {
        Resolved r = resolve_member(path);
        member(r.owner, r.member, r.ctx).assetref = arg;
        return;
    }

    Resolved r = resolve_member(path);
    MemberInfo &info = member(r.owner, r.member, r.ctx);
    const Expr *expr = parse_expr(arg);
    if (kind == "count")
    {
        if (!r.indices.empty())
            info.index_counts[r.indices] = expr;
        else
            info.count = expr;
    }
    else if (kind == "arraysize")
        info.arraysize = expr;
    else if (kind == "condition")
        info.condition = expr;
    else
        throw std::runtime_error("unknown set command " + kind);
}
} // namespace t4ff
