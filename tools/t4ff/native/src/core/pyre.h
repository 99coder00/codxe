#pragma once

#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Regular expressions as Python's re module runs them (the Python t4ff's script and menu passes are
// written with them, and must find the same matches).
//
// A backtracking matcher over bytes: a pattern compiled with BYTES is a Python bytes pattern (ASCII
// \w, \s, \d, \b); without, a str pattern run on latin-1 text (the Python decodes scripts as
// latin-1, so its \w, \s and case folding are Unicode's on those 256 characters). Supported: literals
// and escapes, classes, . ^ $ \A \Z \b \B \w \s \d (and negations), groups ((...), (?:...)),
// alternation, greedy and lazy quantifiers (* + ? {m,n}), lookahead and fixed width lookbehind, and the
// flags I, M, S (also inline at the start: (?i)). The backtracking state is on the heap: long subjects
// do not overflow the stack.
namespace t4ff::pyre
{
enum Flags : int
{
    I = 1,     // IGNORECASE
    M = 2,     // MULTILINE
    S = 4,     // DOTALL
    BYTES = 8, // a bytes pattern
};

struct RegexError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Match
{
    std::string_view subject;
    std::vector<int64_t> spans; // start, end of each group (group 0 the match); -1: unset

    bool has(int g) const
    {
        return spans[2 * g] >= 0;
    }
    size_t start(int g = 0) const
    {
        return static_cast<size_t>(spans[2 * g]);
    }
    size_t end(int g = 0) const
    {
        return static_cast<size_t>(spans[2 * g + 1]);
    }
    // the group's text ("" when it did not take part: test has())
    std::string_view group(int g = 0) const
    {
        return has(g) ? subject.substr(start(g), end(g) - start(g)) : std::string_view();
    }
    std::string str(int g = 0) const
    {
        return std::string(group(g));
    }
    // Python's match.expand(template): \1, \g<1>, \n, \\ ...
    std::string expand(std::string_view tmpl) const;
};

constexpr size_t npos = static_cast<size_t>(-1);

class Regex
{
  public:
    Regex(std::string_view pattern, int flags = 0);
    ~Regex();
    Regex(Regex &&) noexcept;
    Regex &operator=(Regex &&) noexcept;

    // re.search / re.match / re.fullmatch, from pos to endpos (as Python's pattern methods)
    std::optional<Match> search(std::string_view s, size_t pos = 0, size_t endpos = npos) const;
    std::optional<Match> match(std::string_view s, size_t pos = 0, size_t endpos = npos) const;
    std::optional<Match> fullmatch(std::string_view s, size_t pos = 0, size_t endpos = npos) const;
    // re.finditer (and findall: take the groups of each)
    std::vector<Match> finditer(std::string_view s, size_t pos = 0, size_t endpos = npos) const;
    void for_each(std::string_view s, const std::function<void(const Match &)> &f, size_t pos = 0, size_t endpos = npos) const;
    // re.subn with a function / a template; count 0: every match
    std::string sub(std::string_view s, const std::function<std::string(const Match &)> &repl, int count = 0, int *n = nullptr) const;
    std::string sub(std::string_view s, std::string_view tmpl, int count = 0, int *n = nullptr) const;
    int groups() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::optional<Match> run(std::string_view s, size_t pos, size_t endpos, bool anchored, bool full, bool must_advance) const;
};

// re.escape
std::string escape(std::string_view s);
} // namespace t4ff::pyre
