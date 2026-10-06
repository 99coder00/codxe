#include "core/pyre.h"

#include <array>

namespace t4ff::pyre
{
namespace
{
using CharSet = std::bitset<256>;

// latin-1 characters as Python's str methods and re see them (bytes patterns: ASCII only)
const CharSet &table(char which, bool bytes)
{
    static const auto make = [](char w, bool b) {
        CharSet s;
        for (int c = 0; c < 128; ++c)
        {
            bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (w == 'w' && (alnum || c == '_'))
                s.set(c);
            if (w == 'd' && c >= '0' && c <= '9')
                s.set(c);
            if (w == 's' && (c == ' ' || (c >= 9 && c <= 13) || (!b && c >= 28 && c <= 31)))
                s.set(c);
        }
        if (!b)
        {
            if (w == 'w')
            {
                for (int c : {170, 178, 179, 181, 185, 186, 188, 189, 190})
                    s.set(c);
                for (int c = 192; c < 256; ++c)
                    if (c != 215 && c != 247)
                        s.set(c);
            }
            if (w == 's')
            {
                s.set(133);
                s.set(160);
            }
        }
        return s;
    };
    static const std::array<CharSet, 6> tables = {make('w', false), make('s', false), make('d', false),
                                                  make('w', true),  make('s', true),  make('d', true)};
    return tables[(which == 'w' ? 0 : which == 's' ? 1 : 2) + (bytes ? 3 : 0)];
}

enum AssertKind : int
{
    BOL,
    EOL,
    BOS,
    EOS,
    WORDB,
    NWORDB,
};

struct AstNode
{
    enum Kind
    {
        Empty,
        Set,
        Seq,
        Alt,
        Group,
        Repeat,
        Assert,
        Look,
    } kind = Empty;
    int set = -1;
    std::vector<int> kids;
    int kid = -1;
    int cap = -1; // capture group number (Group), -1: non capturing
    int min = 0, max = 0; // Repeat (max -1: no limit)
    bool greedy = true;
    int assert_kind = 0;
    bool behind = false, negate = false; // Look
};

enum Op : uint8_t
{
    OP_SET,
    OP_SPLIT,
    OP_JMP,
    OP_SAVE,
    OP_ASSERT,
    OP_LOOK,
    OP_REPSET,
    OP_MATCH,
};

struct Inst
{
    Op op;
    bool greedy = true;
    bool negate = false;
    bool behind = false;
    int a = 0, b = 0;
    int min = 0, max = 0;
};

using Prog = std::vector<Inst>;

struct Frame
{
    uint8_t kind; // 0 branch, 1 restore capture, 2 greedy repeat, 3 lazy repeat
    int pc;
    size_t sp;
    int64_t aux;
    size_t len;
    size_t lim;
};

struct Ctx
{
    const uint8_t *s;
    size_t end;
    const CharSet *word;
};
} // namespace

struct Regex::Impl
{
    std::string pattern;
    int flags = 0;
    int ngroups = 0; // capturing groups
    std::vector<AstNode> ast;
    std::vector<CharSet> sets;
    std::vector<Prog> progs; // [0] the pattern, then lookaround bodies
    CharSet first;           // characters a match can start with
    bool nullable = false;   // a match can be empty (no first character filter then)

    // -- parsing
    size_t at = 0;

    [[noreturn]] void error(const std::string &what) const
    {
        throw RegexError("regex " + pattern + ": " + what + " at " + std::to_string(at));
    }
    bool more() const
    {
        return at < pattern.size();
    }
    char peek(size_t k = 0) const
    {
        return at + k < pattern.size() ? pattern[at + k] : '\0';
    }
    int add(AstNode n)
    {
        ast.push_back(std::move(n));
        return static_cast<int>(ast.size() - 1);
    }
    // IGNORECASE: a letter of the set brings its other case (latin-1 ones too for str patterns)
    void fold(CharSet &s) const
    {
        if (!(flags & I))
            return;
        auto pair = [&](int lower) {
            if (s.test(lower) || s.test(lower - 32))
            {
                s.set(lower);
                s.set(lower - 32);
            }
        };
        for (int c = 'a'; c <= 'z'; ++c)
            pair(c);
        if (!bytes())
            for (int c = 0xE0; c <= 0xFE; ++c)
                if (c != 0xF7)
                    pair(c);
    }
    int add_set(CharSet s)
    {
        fold(s);
        sets.push_back(s);
        AstNode n;
        n.kind = AstNode::Set;
        n.set = static_cast<int>(sets.size() - 1);
        return add(std::move(n));
    }
    bool bytes() const
    {
        return flags & BYTES;
    }

    void inline_flags()
    {
        while (peek() == '(' && peek(1) == '?')
        {
            size_t k = at + 2;
            int f = 0;
            while (k < pattern.size() && std::string_view("aiLmsux").find(pattern[k]) != std::string_view::npos)
            {
                char c = pattern[k++];
                f |= c == 'i' ? I : c == 'm' ? M : c == 's' ? S : 0;
            }
            if (k == at + 2 || k >= pattern.size() || pattern[k] != ')')
                return;
            flags |= f;
            at = k + 1;
        }
    }

    int parse_alt()
    {
        std::vector<int> alts{parse_seq()};
        while (peek() == '|')
        {
            ++at;
            alts.push_back(parse_seq());
        }
        if (alts.size() == 1)
            return alts[0];
        AstNode n;
        n.kind = AstNode::Alt;
        n.kids = std::move(alts);
        return add(std::move(n));
    }

    int parse_seq()
    {
        AstNode n;
        n.kind = AstNode::Seq;
        while (more() && peek() != '|' && peek() != ')')
            n.kids.push_back(parse_item());
        return add(std::move(n));
    }

    bool parse_count(int &lo, int &hi)
    {
        // {m}, {m,}, {,n}, {m,n}: else '{' is a literal
        size_t k = at + 1;
        auto digits = [&](int &v) {
            size_t s = k;
            v = 0;
            while (k < pattern.size() && pattern[k] >= '0' && pattern[k] <= '9')
                v = v * 10 + (pattern[k++] - '0');
            return k > s;
        };
        int a = 0, b = 0;
        bool has_a = digits(a);
        if (k < pattern.size() && pattern[k] == '}')
        {
            if (!has_a)
                return false;
            lo = hi = a;
            at = k + 1;
            return true;
        }
        if (k >= pattern.size() || pattern[k] != ',')
            return false;
        ++k;
        bool has_b = digits(b);
        if (k >= pattern.size() || pattern[k] != '}')
            return false;
        lo = has_a ? a : 0;
        hi = has_b ? b : -1;
        if (hi >= 0 && hi < lo)
            error("min repeat greater than max repeat");
        at = k + 1;
        return true;
    }

    int parse_item()
    {
        int atom = parse_atom();
        for (;;)
        {
            int lo, hi;
            char c = peek();
            if (c == '*')
                lo = 0, hi = -1, ++at;
            else if (c == '+')
                lo = 1, hi = -1, ++at;
            else if (c == '?')
                lo = 0, hi = 1, ++at;
            else if (c == '{' && parse_count(lo, hi))
            {
            }
            else
                return atom;
            const AstNode &target = ast[atom];
            if (target.kind == AstNode::Assert || target.kind == AstNode::Look || target.kind == AstNode::Repeat ||
                (target.kind == AstNode::Seq && target.kids.empty()))
                error("nothing to repeat");
            AstNode n;
            n.kind = AstNode::Repeat;
            n.kid = atom;
            n.min = lo;
            n.max = hi;
            if (peek() == '?')
            {
                n.greedy = false;
                ++at;
            }
            atom = add(std::move(n));
        }
    }

    int parse_atom()
    {
        char c = pattern[at++];
        switch (c)
        {
        case '(':
            return parse_group();
        case '[':
            return parse_class();
        case '.': {
            CharSet s;
            s.set();
            if (!(flags & S))
                s.reset('\n');
            return add_set(s);
        }
        case '^':
            return add_assert(BOL);
        case '$':
            return add_assert(EOL);
        case '\\':
            return parse_escape();
        case '*':
        case '+':
        case '?':
            error("nothing to repeat");
        default: {
            CharSet s;
            s.set(static_cast<uint8_t>(c));
            return add_set(s);
        }
        }
    }

    int add_assert(int kind)
    {
        AstNode n;
        n.kind = AstNode::Assert;
        n.assert_kind = kind;
        return add(std::move(n));
    }

    int parse_group()
    {
        AstNode n;
        if (peek() == '?')
        {
            char k = peek(1);
            if (k == ':')
            {
                at += 2;
                n.kind = AstNode::Group;
            }
            else if (k == '=' || k == '!')
            {
                at += 2;
                n.kind = AstNode::Look;
                n.negate = k == '!';
            }
            else if (k == '<' && (peek(2) == '=' || peek(2) == '!'))
            {
                n.kind = AstNode::Look;
                n.behind = true;
                n.negate = peek(2) == '!';
                at += 3;
            }
            else
                error("unsupported group");
        }
        else
        {
            n.kind = AstNode::Group;
            n.cap = ++ngroups;
        }
        n.kid = parse_alt();
        if (peek() != ')')
            error("missing )");
        ++at;
        return add(std::move(n));
    }

    // a class escape (\w, \s, \d and negations) into s; false when it is not one
    bool class_escape(char e, CharSet &s)
    {
        char lower = static_cast<char>(e | 0x20);
        if (lower != 'w' && lower != 's' && lower != 'd')
            return false;
        CharSet t = table(lower, bytes());
        s |= e == lower ? t : ~t;
        return true;
    }

    // a character escape: its code, -1 when it is not one
    int char_escape(char e, bool in_class)
    {
        switch (e)
        {
        case 'n':
            return '\n';
        case 't':
            return '\t';
        case 'r':
            return '\r';
        case 'f':
            return '\f';
        case 'v':
            return '\v';
        case 'a':
            return '\a';
        case 'b':
            return in_class ? '\b' : -1;
        case '0':
            return 0;
        case 'x': {
            int v = 0;
            for (int k = 0; k < 2; ++k)
            {
                char h = peek();
                int d = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
                if (d < 0)
                    error("bad \\x escape");
                v = v * 16 + d;
                ++at;
            }
            return v;
        }
        default:
            if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || (e >= '1' && e <= '9'))
                return -1;
            return static_cast<uint8_t>(e);
        }
    }

    int parse_escape()
    {
        if (!more())
            error("bad escape (end of pattern)");
        char e = pattern[at++];
        CharSet s;
        if (class_escape(e, s))
            return add_set(s);
        switch (e)
        {
        case 'b':
            return add_assert(WORDB);
        case 'B':
            return add_assert(NWORDB);
        case 'A':
            return add_assert(BOS);
        case 'Z':
            return add_assert(EOS);
        default:
            break;
        }
        int c = char_escape(e, false);
        if (c < 0)
            error(std::string("unsupported escape \\") + e);
        s.set(static_cast<uint8_t>(c));
        return add_set(s);
    }

    int parse_class()
    {
        CharSet s;
        bool negate = false;
        if (peek() == '^')
        {
            negate = true;
            ++at;
        }
        bool opening = true;
        for (;;)
        {
            if (!more())
                error("unterminated character set");
            char c = pattern[at++];
            if (c == ']' && !opening)
                break;
            opening = false;
            int lo;
            if (c == '\\')
            {
                if (!more())
                    error("bad escape");
                char e = pattern[at++];
                if (class_escape(e, s))
                    continue;
                lo = char_escape(e, true);
                if (lo < 0)
                    error(std::string("unsupported escape \\") + e);
            }
            else
                lo = static_cast<uint8_t>(c);
            if (peek() == '-' && peek(1) != ']' && at + 1 < pattern.size())
            {
                ++at;
                char d = pattern[at++];
                int hi;
                if (d == '\\')
                {
                    char e = pattern[at++];
                    hi = char_escape(e, true);
                    if (hi < 0)
                        error("bad character range");
                }
                else
                    hi = static_cast<uint8_t>(d);
                if (hi < lo)
                    error("bad character range");
                for (int k = lo; k <= hi; ++k)
                    s.set(k);
            }
            else
                s.set(lo);
        }
        fold(s);
        if (negate)
            s.flip();
        sets.push_back(s);
        AstNode n;
        n.kind = AstNode::Set;
        n.set = static_cast<int>(sets.size() - 1);
        return add(std::move(n));
    }

    // -- analysis

    bool node_nullable(int i) const
    {
        const AstNode &n = ast[i];
        switch (n.kind)
        {
        case AstNode::Set:
            return false;
        case AstNode::Seq:
            for (int k : n.kids)
                if (!node_nullable(k))
                    return false;
            return true;
        case AstNode::Alt:
            for (int k : n.kids)
                if (node_nullable(k))
                    return true;
            return false;
        case AstNode::Group:
            return node_nullable(n.kid);
        case AstNode::Repeat:
            return n.min == 0 || node_nullable(n.kid);
        default:
            return true;
        }
    }

    // the characters a match of node i can start with (a superset); its nullability is node_nullable
    CharSet node_first(int i) const
    {
        const AstNode &n = ast[i];
        CharSet s;
        switch (n.kind)
        {
        case AstNode::Set:
            return sets[n.set];
        case AstNode::Seq:
            for (int k : n.kids)
            {
                s |= node_first(k);
                if (!node_nullable(k))
                    break;
            }
            return s;
        case AstNode::Alt:
            for (int k : n.kids)
                s |= node_first(k);
            return s;
        case AstNode::Group:
        case AstNode::Repeat:
            return node_first(n.kid);
        default:
            return s;
        }
    }

    bool has_capture(int i) const
    {
        const AstNode &n = ast[i];
        if (n.kind == AstNode::Group && n.cap > 0)
            return true;
        for (int k : n.kids)
            if (has_capture(k))
                return true;
        return n.kid >= 0 && has_capture(n.kid);
    }

    // the fixed width of node i, -1 when it varies
    int width(int i) const
    {
        const AstNode &n = ast[i];
        switch (n.kind)
        {
        case AstNode::Set:
            return 1;
        case AstNode::Seq: {
            int w = 0;
            for (int k : n.kids)
            {
                int x = width(k);
                if (x < 0)
                    return -1;
                w += x;
            }
            return w;
        }
        case AstNode::Alt: {
            int w = -2;
            for (int k : n.kids)
            {
                int x = width(k);
                if (x < 0 || (w != -2 && x != w))
                    return -1;
                w = x;
            }
            return w < 0 ? -1 : w;
        }
        case AstNode::Group:
            return width(n.kid);
        case AstNode::Repeat: {
            int x = width(n.kid);
            return x >= 0 && n.min == n.max ? x * n.min : -1;
        }
        default:
            return 0;
        }
    }

    // -- compiling

    void emit(Prog &p, int i)
    {
        const AstNode &n = ast[i];
        switch (n.kind)
        {
        case AstNode::Empty:
            return;
        case AstNode::Set: {
            Inst in{OP_SET};
            in.a = n.set;
            p.push_back(in);
            return;
        }
        case AstNode::Seq:
            for (int k : n.kids)
                emit(p, k);
            return;
        case AstNode::Alt: {
            // SPLIT next, alternative 2; ... JMP end
            std::vector<size_t> jumps;
            for (size_t k = 0; k < n.kids.size(); ++k)
            {
                size_t split = 0;
                bool last = k + 1 == n.kids.size();
                if (!last)
                {
                    split = p.size();
                    p.push_back(Inst{OP_SPLIT});
                    p[split].a = static_cast<int>(p.size());
                }
                emit(p, n.kids[k]);
                if (!last)
                {
                    jumps.push_back(p.size());
                    p.push_back(Inst{OP_JMP});
                    p[split].b = static_cast<int>(p.size());
                }
            }
            for (size_t j : jumps)
                p[j].a = static_cast<int>(p.size());
            return;
        }
        case AstNode::Group: {
            if (n.cap > 0)
            {
                Inst s{OP_SAVE};
                s.a = 2 * n.cap;
                p.push_back(s);
            }
            emit(p, n.kid);
            if (n.cap > 0)
            {
                Inst s{OP_SAVE};
                s.a = 2 * n.cap + 1;
                p.push_back(s);
            }
            return;
        }
        case AstNode::Repeat: {
            const AstNode &kid = ast[n.kid];
            if (kid.kind == AstNode::Set)
            {
                Inst r{OP_REPSET};
                r.a = kid.set;
                r.min = n.min;
                r.max = n.max;
                r.greedy = n.greedy;
                p.push_back(r);
                return;
            }
            if (n.max < 0 && node_nullable(n.kid))
                error("unsupported: an unbounded repeat of what can match nothing");
            for (int k = 0; k < n.min; ++k)
                emit(p, n.kid);
            if (n.max < 0)
            {
                size_t loop = p.size();
                p.push_back(Inst{OP_SPLIT});
                size_t body = p.size();
                emit(p, n.kid);
                Inst j{OP_JMP};
                j.a = static_cast<int>(loop);
                p.push_back(j);
                size_t out = p.size();
                p[loop].a = static_cast<int>(n.greedy ? body : out);
                p[loop].b = static_cast<int>(n.greedy ? out : body);
                return;
            }
            std::vector<size_t> splits;
            for (int k = n.min; k < n.max; ++k)
            {
                splits.push_back(p.size());
                p.push_back(Inst{OP_SPLIT});
                p.back().a = static_cast<int>(p.size()); // filled below for lazy
                emit(p, n.kid);
            }
            size_t out = p.size();
            for (size_t s : splits)
            {
                int body = static_cast<int>(s + 1);
                p[s].a = n.greedy ? body : static_cast<int>(out);
                p[s].b = n.greedy ? static_cast<int>(out) : body;
            }
            return;
        }
        case AstNode::Assert: {
            Inst in{OP_ASSERT};
            in.a = n.assert_kind;
            p.push_back(in);
            return;
        }
        case AstNode::Look: {
            if (has_capture(n.kid))
                error("unsupported: a group inside a lookaround");
            int w = 0;
            if (n.behind)
            {
                w = width(n.kid);
                if (w < 0)
                    error("look-behind requires fixed-width pattern");
            }
            Prog body;
            emit(body, n.kid);
            body.push_back(Inst{OP_MATCH});
            progs.push_back(std::move(body));
            Inst in{OP_LOOK};
            in.a = static_cast<int>(progs.size() - 1);
            in.b = w;
            in.negate = n.negate;
            in.behind = n.behind;
            p.push_back(in);
            return;
        }
        }
    }

    // -- matching

    bool check(int kind, const Ctx &c, size_t sp) const
    {
        switch (kind)
        {
        case BOL:
            return sp == 0 || ((flags & M) && c.s[sp - 1] == '\n');
        case EOL:
            if (flags & M)
                return sp == c.end || c.s[sp] == '\n';
            return sp == c.end || (sp + 1 == c.end && c.s[sp] == '\n');
        case BOS:
            return sp == 0;
        case EOS:
            return sp == c.end;
        default: {
            if (c.end == 0)
                return false; // as Python: no boundary (nor non-boundary) in an empty string
            bool before = sp > 0 && c.word->test(c.s[sp - 1]);
            bool here = sp < c.end && c.word->test(c.s[sp]);
            return (kind == WORDB) == (before != here);
        }
        }
    }

    // runs program prog from sp: the end of the match, or npos
    size_t exec(int prog_index, const Ctx &c, size_t start, std::vector<int64_t> &caps, std::vector<Frame> &stack, bool full,
                bool must_advance) const
    {
        const Prog &prog = progs[prog_index];
        size_t base = stack.size();
        int pc = 0;
        size_t sp = start;
        for (;;)
        {
            const Inst &in = prog[pc];
            bool ok = true;
            switch (in.op)
            {
            case OP_SET:
                if (sp < c.end && sets[in.a].test(c.s[sp]))
                {
                    ++sp;
                    ++pc;
                }
                else
                    ok = false;
                break;
            case OP_SPLIT:
                stack.push_back(Frame{0, in.b, sp, 0, 0, 0});
                pc = in.a;
                break;
            case OP_JMP:
                pc = in.a;
                break;
            case OP_SAVE:
                stack.push_back(Frame{1, in.a, 0, caps[in.a], 0, 0});
                caps[in.a] = static_cast<int64_t>(sp);
                ++pc;
                break;
            case OP_ASSERT:
                if (check(in.a, c, sp))
                    ++pc;
                else
                    ok = false;
                break;
            case OP_LOOK: {
                bool found = false;
                if (!in.behind || sp >= static_cast<size_t>(in.b))
                {
                    std::vector<Frame> sub;
                    found = exec(in.a, c, in.behind ? sp - in.b : sp, caps, sub, false, false) != npos;
                }
                if (found != in.negate)
                    ++pc;
                else
                    ok = false;
                break;
            }
            case OP_REPSET: {
                const CharSet &set = sets[in.a];
                size_t limit = in.max < 0 ? c.end : std::min(c.end, sp + static_cast<size_t>(in.max));
                size_t n = 0;
                if (in.greedy || in.min > 0)
                {
                    size_t stop = in.greedy ? limit : std::min(limit, sp + in.min);
                    while (sp + n < stop && set.test(c.s[sp + n]))
                        ++n;
                }
                if (n < static_cast<size_t>(in.min))
                {
                    ok = false;
                    break;
                }
                if (in.greedy)
                {
                    if (n > static_cast<size_t>(in.min))
                        stack.push_back(Frame{2, pc + 1, sp, 0, n - 1, static_cast<size_t>(in.min)});
                }
                else if (sp + n < limit)
                    stack.push_back(Frame{3, pc + 1, sp, in.a, n + 1, limit - sp});
                sp += n;
                ++pc;
                break;
            }
            case OP_MATCH:
                if ((full && sp != c.end) || (must_advance && sp == start))
                    ok = false;
                else
                {
                    // the captures stay as they are; drop this run's frames
                    stack.resize(base);
                    return sp;
                }
                break;
            }
            if (ok)
                continue;
            // backtrack
            for (;;)
            {
                if (stack.size() == base)
                    return npos;
                Frame f = stack.back();
                stack.pop_back();
                if (f.kind == 1)
                {
                    caps[f.pc] = f.aux;
                    continue;
                }
                if (f.kind == 0)
                {
                    pc = f.pc;
                    sp = f.sp;
                    break;
                }
                if (f.kind == 2)
                {
                    // greedy: one character less
                    if (f.len > f.lim)
                        stack.push_back(Frame{2, f.pc, f.sp, 0, f.len - 1, f.lim});
                    pc = f.pc;
                    sp = f.sp + f.len;
                    break;
                }
                // lazy: one character more, when it is in the set
                if (!sets[static_cast<size_t>(f.aux)].test(c.s[f.sp + f.len - 1]))
                    continue;
                if (f.len < f.lim)
                    stack.push_back(Frame{3, f.pc, f.sp, f.aux, f.len + 1, f.lim});
                pc = f.pc;
                sp = f.sp + f.len;
                break;
            }
        }
    }
};

Regex::Regex(std::string_view pattern, int flags) : impl(std::make_unique<Impl>())
{
    Impl &r = *impl;
    r.pattern = std::string(pattern);
    r.flags = flags;
    r.inline_flags();
    int root = r.parse_alt();
    if (r.more())
        r.error("unbalanced parenthesis");
    // the whole match is group 0
    Prog main;
    Inst s0{OP_SAVE};
    s0.a = 0;
    main.push_back(s0);
    r.progs.emplace_back(); // the main program is progs[0]
    r.emit(main, root);
    Inst s1{OP_SAVE};
    s1.a = 1;
    main.push_back(s1);
    main.push_back(Inst{OP_MATCH});
    r.progs[0] = std::move(main);
    r.nullable = r.node_nullable(root);
    r.first = r.node_first(root);
}

Regex::~Regex() = default;
Regex::Regex(Regex &&) noexcept = default;
Regex &Regex::operator=(Regex &&) noexcept = default;

int Regex::groups() const
{
    return impl->ngroups;
}

std::optional<Match> Regex::run(std::string_view s, size_t pos, size_t endpos, bool anchored, bool full, bool must_advance) const
{
    const Impl &r = *impl;
    // as Python: pos past the end is the end, and an endpos before pos finds nothing
    pos = std::min(pos, s.size());
    size_t end = std::min(endpos, s.size());
    if (pos > end)
        return std::nullopt;
    Ctx c{reinterpret_cast<const uint8_t *>(s.data()), end, &table('w', (r.flags & BYTES) != 0)};
    std::vector<int64_t> caps(2 * (r.ngroups + 1), -1);
    std::vector<Frame> stack;
    for (size_t start = pos; start <= end; ++start)
    {
        bool advance = must_advance && start == pos;
        if (!anchored && !r.nullable)
        {
            // skip to a character a match can start with
            while (start < end && !r.first.test(c.s[start]))
                ++start;
            if (start >= end)
                return std::nullopt;
        }
        std::fill(caps.begin(), caps.end(), -1);
        stack.clear();
        if (r.exec(0, c, start, caps, stack, full, advance) != npos)
        {
            Match m;
            m.subject = s;
            m.spans = std::move(caps);
            return m;
        }
        if (anchored)
            break;
    }
    return std::nullopt;
}

std::optional<Match> Regex::search(std::string_view s, size_t pos, size_t endpos) const
{
    return run(s, pos, endpos, false, false, false);
}

std::optional<Match> Regex::match(std::string_view s, size_t pos, size_t endpos) const
{
    return run(s, pos, endpos, true, false, false);
}

std::optional<Match> Regex::fullmatch(std::string_view s, size_t pos, size_t endpos) const
{
    return run(s, pos, endpos, true, true, false);
}

void Regex::for_each(std::string_view s, const std::function<void(const Match &)> &f, size_t pos, size_t endpos) const
{
    size_t end = std::min(endpos, s.size());
    bool advance = false;
    while (pos <= end)
    {
        auto m = run(s, pos, endpos, false, false, advance);
        if (!m)
            break;
        f(*m);
        advance = m->end() == m->start();
        pos = m->end();
    }
}

std::vector<Match> Regex::finditer(std::string_view s, size_t pos, size_t endpos) const
{
    std::vector<Match> out;
    for_each(s, [&](const Match &m) { out.push_back(m); }, pos, endpos);
    return out;
}

std::string Regex::sub(std::string_view s, const std::function<std::string(const Match &)> &repl, int count, int *n) const
{
    std::string out;
    size_t last = 0, pos = 0;
    int done = 0;
    bool advance = false;
    while (pos <= s.size() && (count == 0 || done < count))
    {
        auto m = run(s, pos, npos, false, false, advance);
        if (!m)
            break;
        out.append(s.substr(last, m->start() - last));
        out += repl(*m);
        last = m->end();
        ++done;
        advance = m->end() == m->start();
        pos = m->end();
    }
    out.append(s.substr(last));
    if (n)
        *n = done;
    return out;
}

std::string Regex::sub(std::string_view s, std::string_view tmpl, int count, int *n) const
{
    return sub(s, [&](const Match &m) { return m.expand(tmpl); }, count, n);
}

std::string Match::expand(std::string_view t) const
{
    std::string out;
    int groups = static_cast<int>(spans.size() / 2) - 1;
    for (size_t i = 0; i < t.size(); ++i)
    {
        char c = t[i];
        if (c != '\\' || i + 1 >= t.size())
        {
            out += c;
            continue;
        }
        char e = t[++i];
        int g = -1;
        if (e == 'g' && i + 1 < t.size() && t[i + 1] == '<')
        {
            size_t close = t.find('>', i + 2);
            if (close == std::string_view::npos)
                throw RegexError("missing > in a template");
            g = std::stoi(std::string(t.substr(i + 2, close - i - 2)));
            i = close;
        }
        else if (e >= '1' && e <= '9')
        {
            g = e - '0';
            if (i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9')
                g = g * 10 + (t[++i] - '0');
        }
        if (g >= 0)
        {
            if (g > groups)
                throw RegexError("invalid group reference " + std::to_string(g));
            out.append(group(g));
            continue;
        }
        switch (e)
        {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case 'f':
            out += '\f';
            break;
        case 'v':
            out += '\v';
            break;
        case 'a':
            out += '\a';
            break;
        case 'b':
            out += '\b';
            break;
        case '\\':
            out += '\\';
            break;
        default:
            if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z'))
                throw RegexError(std::string("bad escape \\") + e + " in a template");
            out += '\\';
            out += e;
        }
    }
    return out;
}

std::string escape(std::string_view s)
{
    static const std::string_view special = "()[]{}?*+-|^$\\.&~# \t\n\r\v\f";
    std::string out;
    for (char c : s)
    {
        if (special.find(c) != std::string_view::npos)
            out += '\\';
        out += c;
    }
    return out;
}
} // namespace t4ff::pyre
