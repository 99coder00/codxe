#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <tuple>
#include <utility>
#include <vector>

#include "core/layout.h"

// OpenAssetTools' zone code commands (the Python t4ff's commands.py): how each structure is streamed
// from a fastfile (which pointers are strings, array counts, blocks, conditions, reorders...).
namespace t4ff
{
// What an expression asks of the structures being loaded.
class EvalContext
{
  public:
    virtual ~EvalContext() = default;
    virtual int64_t lookup(const std::vector<std::string> &path, const std::vector<int64_t> &indices) = 0;
};

struct Expr
{
    enum class Op : uint8_t
    {
        Num,
        Var,
        Unary,
        Binary,
        Ternary,
        Never,
    };
    Op op = Op::Num;
    std::string oper; // unary and binary operators
    int64_t value = 0;
    std::vector<std::string> path; // a variable: ["GfxWorld", "dpvsPlanes", "cellCount"] or ["numBones"]
    std::vector<const Expr *> indices;
    const Expr *a = nullptr; // operand, left, condition
    const Expr *b = nullptr; // right, then
    const Expr *c = nullptr; // else

    int64_t eval(EvalContext *ctx) const;
    bool is_never() const
    {
        return op == Op::Never;
    }
};

struct Delayed
{
    std::string block;
    int64_t alignment = 1;
    const Expr *condition = nullptr;
};

struct MemberInfo
{
    std::string context; // the record its expressions are evaluated against
    bool string = false;
    bool scriptstring = false;
    bool reusable = false;
    const Expr *count = nullptr;
    std::map<std::vector<int64_t>, const Expr *> index_counts;
    const Expr *arraysize = nullptr;
    const Expr *condition = nullptr;
    std::optional<std::string> block;
    const Expr *allocalign = nullptr;
    std::optional<std::string> assetref;
    std::optional<Delayed> delayed;
};

struct TypeInfo
{
    std::optional<std::string> block;
    const Expr *allocalign = nullptr;
    std::optional<std::vector<std::string>> reorder;
};

// A member's directives, by the record they apply in (in the order they were first given).
using MemberInfos = std::vector<std::pair<std::string, MemberInfo>>;

struct Commands
{
    std::vector<std::pair<std::string, std::string>> assets; // record name -> asset enum name
    std::vector<std::tuple<std::string, std::string, bool>> blocks; // (kind, name, default)
    std::unordered_map<std::string, TypeInfo> types;
    std::map<std::pair<std::string, std::string>, MemberInfos> members; // (owner record, member)

    TypeInfo &type_info(const std::string &name)
    {
        return types[name];
    }
    const TypeInfo *find_type(const std::string &name) const
    {
        auto it = types.find(name);
        return it == types.end() ? nullptr : &it->second;
    }
    const MemberInfos *member_infos(const std::string &owner, const std::string &member) const
    {
        auto it = members.find({owner, member});
        return it == members.end() ? nullptr : &it->second;
    }

    std::deque<Expr> exprs; // owns every expression
};

const Expr *never_expr();

// Parses command text into a Commands (lenient: directives about members a layout lacks are skipped,
// as when the PC commands are applied to the console's layouts).
class CommandParser
{
  public:
    CommandParser(const Layout &layout, Commands &cmds, bool lenient = false) : layout(layout), cmds(cmds), lenient(lenient)
    {
    }

    void parse_text(std::string_view text);
    const std::vector<std::string> &skipped_statements() const
    {
        return skipped;
    }

    const Expr *parse_expr(std::string_view text);

  private:
    const Layout &layout;
    Commands &cmds;
    bool lenient;
    std::optional<std::string> use;
    std::vector<std::string> skipped;

    void statement(const std::string &s);
    void do_statement(const std::string &s);
    void set_statement(const std::string &s);
    bool resolve_is_type(const std::string &name) const;
    MemberInfo &member(const std::string &owner, const std::string &member_name, const std::string &ctx);
    struct Resolved
    {
        std::string owner, member;
        std::vector<int64_t> indices;
        std::string ctx;
    };
    Resolved resolve_member(const std::string &path);
};

// A directive names something the layout lacks (the Python KeyError).
struct MissingMember : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
} // namespace t4ff
