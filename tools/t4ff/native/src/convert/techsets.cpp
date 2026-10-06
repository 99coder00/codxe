#include "convert/techsets.h"

#include <algorithm>
#include <array>
#include <unordered_map>

#include "convert/library.h"

namespace t4ff
{
namespace
{
constexpr int PER_PRIM = 0, PER_OBJECT = 1, STABLE = 2;
constexpr uint32_t MTL_ARG_CODE_VERTEX_CONST = 3;
constexpr uint32_t MTL_ARG_CODE_PIXEL_SAMPLER = 4;
constexpr uint32_t MTL_ARG_CODE_PIXEL_CONST = 5;

using SectionTable = std::unordered_map<uint32_t, int>;

SectionTable make_table(std::initializer_list<uint32_t> per_prim, std::initializer_list<uint32_t> per_object,
                        std::initializer_list<uint32_t> stable)
{
    SectionTable t;
    for (uint32_t i : stable)
        t[i] = STABLE;
    for (uint32_t i : per_object)
        t[i] = PER_OBJECT;
    for (uint32_t i : per_prim)
        t[i] = PER_PRIM;
    return t;
}

// the section of each code argument in the console game's technique sets (techsets.SECTIONS)
const SectionTable *section_table(uint32_t arg_type)
{
    static const SectionTable vertex_const = make_table(
        {0x3B, 0x3D, 0x64, 0x6B, 0x6C, 0x76, 0x77, 0x78, 0x7F, 0x87}, {0x35, 0x36, 0x37, 0x39, 0x3A, 0x3C, 0x6F, 0x70, 0x73, 0x7B, 0x7C, 0x83},
        {0x05, 0x06, 0x07, 0x0A, 0x12, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x24, 0x2A, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x47, 0x4C,
         0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x61, 0x62, 0x63, 0x8B});
    static const SectionTable pixel_sampler = make_table(
        {}, {0x09, 0x11, 0x12, 0x1A, 0x1B, 0x1C, 0x1D, 0x1F, 0x20, 0x21, 0x22, 0x23},
        {0x01, 0x03, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x13, 0x14, 0x16, 0x17, 0x18});
    static const SectionTable pixel_const = make_table(
        {}, {},
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
         0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D,
         0x2E, 0x2F, 0x30, 0x31, 0x32, 0x44, 0x45, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x60, 0x63, 0x65, 0x66, 0x67, 0x68});
    switch (arg_type)
    {
    case MTL_ARG_CODE_VERTEX_CONST: return &vertex_const;
    case MTL_ARG_CODE_PIXEL_SAMPLER: return &pixel_sampler;
    case MTL_ARG_CODE_PIXEL_CONST: return &pixel_const;
    }
    return nullptr;
}

uint16_t u16(const Platform &p, const uint8_t *d)
{
    return p.big_endian ? uint16_t(d[0] << 8 | d[1]) : uint16_t(d[1] << 8 | d[0]);
}

std::optional<int> argument_section(const Platform &p, const std::vector<uint8_t> &arg)
{
    uint32_t arg_type = u16(p, arg.data());
    const SectionTable *table = section_table(arg_type);
    if (!table)
        return std::nullopt;
    // u.codeSampler (a texture index) or u.codeConst.index
    uint32_t index = arg_type == MTL_ARG_CODE_PIXEL_SAMPLER ? p.u32(arg.data() + 4) : u16(p, arg.data() + 4);
    auto it = table->find(index);
    if (it == table->end())
        return std::nullopt;
    return it->second;
}

using Args = std::vector<std::vector<uint8_t>>;

// the arguments of a pass (counts per section) with the code arguments moved to the sections the game
// uses; nothing when they already are
std::optional<std::pair<Args, std::array<int, 3>>> sort_sections(const Platform &p, const Args &args, const std::array<int, 3> &counts)
{
    std::array<Args, 3> sections;
    std::vector<std::pair<int, std::vector<uint8_t>>> moved;
    size_t start = 0;
    for (int section = 0; section < 3; ++section)
    {
        for (size_t i = start; i < start + counts[section]; ++i)
        {
            auto want = argument_section(p, args[i]);
            if (want && *want != section)
                moved.emplace_back(*want, args[i]);
            else
                sections[section].push_back(args[i]);
        }
        start += counts[section];
    }
    if (moved.empty())
        return std::nullopt;
    auto key = [&](const std::vector<uint8_t> &arg) { return std::make_pair(u16(p, arg.data()), u16(p, arg.data() + 2)); };
    for (auto &[section, arg] : moved)
    {
        Args &items = sections[section];
        size_t position = items.size();
        for (size_t i = 0; i < items.size(); ++i)
            if (key(items[i]) > key(arg))
            {
                position = i;
                break;
            }
        items.insert(items.begin() + position, arg);
    }
    Args all;
    std::array<int, 3> new_counts{};
    for (int s = 0; s < 3; ++s)
    {
        new_counts[s] = static_cast<int>(sections[s].size());
        all.insert(all.end(), sections[s].begin(), sections[s].end());
    }
    return std::make_pair(std::move(all), new_counts);
}

std::string technique_name(const Platform &p, const Node &node)
{
    Ptr *ptr = node.relocs.get(p.record("MaterialTechnique").field("name")->offset);
    Node *target = ptr ? ptr->target() : nullptr;
    if (target && target->string && !target->data.empty())
        return std::string(target->data.begin(), target->data.end() - 1);
    return "?";
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            return out;
        start = end + 1;
    }
}
} // namespace

int fix_argument_sections(const Platform &p, Zone &zone, const std::function<void(const std::string &)> &log)
{
    const Record &tech_rec = p.record("MaterialTechnique"), &pass_rec = p.record("MaterialPass");
    uint32_t pass_count_off = tech_rec.field("passCount")->offset;
    uint32_t pass_array_off = tech_rec.field("passArray")->offset;
    const char *count_fields[3] = {"perPrimArgCount", "perObjArgCount", "stableArgCount"};
    uint32_t count_offs[3];
    for (int i = 0; i < 3; ++i)
        count_offs[i] = pass_rec.field(count_fields[i])->offset;
    uint32_t args_off = pass_rec.field("args")->offset;
    uint32_t arg_size = p.record("MaterialShaderArgument").size;

    // passes by the arguments they read (an argument array could be shared by passes)
    struct Passes
    {
        Node *args_node;
        uint32_t start;
        std::vector<std::pair<Node *, uint32_t>> users;
        std::string name;
    };
    std::vector<Passes> passes;
    std::map<std::pair<const Node *, uint32_t>, size_t> by_key;
    zone.root->walk([&](Node *node) {
        if (node->string || !node->is_record("MaterialTechnique"))
            return;
        uint32_t count = u16(p, node->data.data() + pass_count_off);
        for (uint32_t k = 0; k < count; ++k)
        {
            uint32_t po = pass_array_off + k * pass_rec.size;
            Ptr *ptr = node->relocs.get(po + args_off);
            Node *target = ptr ? ptr->target() : nullptr;
            if (!target)
                continue;
            uint32_t start = ptr->kind == Ptr::Kind::Ref ? ptr->index * target->elem_size() + ptr->inner : 0;
            auto key = std::make_pair(static_cast<const Node *>(target), start);
            auto it = by_key.find(key);
            if (it == by_key.end())
            {
                it = by_key.emplace(key, passes.size()).first;
                passes.push_back({target, start, {}, technique_name(p, *node)});
            }
            passes[it->second].users.emplace_back(node, po);
        }
    });

    int changed = 0;
    std::vector<std::string> examples;
    for (Passes &ps : passes)
    {
        std::set<std::array<int, 3>> layouts;
        for (auto &[node, po] : ps.users)
            layouts.insert({node->data[po + count_offs[0]], node->data[po + count_offs[1]], node->data[po + count_offs[2]]});
        if (layouts.size() != 1)
        {
            if (log)
                log("warning: technique '" + ps.name + "': passes sharing arguments count them differently, left as they are");
            continue;
        }
        std::array<int, 3> counts = *layouts.begin();
        uint32_t total = counts[0] + counts[1] + counts[2];
        if (ps.start + total * arg_size > ps.args_node->data.size())
            continue;
        Args args;
        for (uint32_t i = 0; i < total; ++i)
        {
            const uint8_t *a = ps.args_node->data.data() + ps.start + i * arg_size;
            args.emplace_back(a, a + arg_size);
        }
        auto result = sort_sections(p, args, counts);
        if (!result)
            continue;
        uint8_t *w = ps.args_node->data.mutable_data() + ps.start;
        for (const auto &a : result->first)
        {
            std::copy(a.begin(), a.end(), w);
            w += a.size();
        }
        for (auto &[node, po] : ps.users)
            for (int i = 0; i < 3; ++i)
                node->data.mutable_data()[po + count_offs[i]] = static_cast<uint8_t>(result->second[i]);
        changed += static_cast<int>(ps.users.size());
        if (examples.size() < 3 && std::find(examples.begin(), examples.end(), ps.name) == examples.end())
            examples.push_back(ps.name);
    }
    if (changed && log)
    {
        std::string list;
        for (const auto &e : examples)
            list += (list.empty() ? "" : ", ") + e;
        log("technique sets: shader arguments of " + std::to_string(changed) +
            " passes moved to the sections the game reads them from (e.g. " + list + ")");
    }
    return changed;
}

TechsetInfo techset_info(const Platform &p, const Node &node)
{
    const Record &ts_rec = p.record("MaterialTechniqueSet"), &tech_rec = p.record("MaterialTechnique"), &pass_rec = p.record("MaterialPass");
    uint32_t techs = ts_rec.field("techniques")->offset;
    uint32_t shaders = pass_rec.field("vertexShaderArray")->offset;
    uint32_t pass_array = tech_rec.field("passArray")->offset;
    uint32_t count_off = tech_rec.field("passCount")->offset;
    int wvf = node.data[ts_rec.field("worldVertFormat")->offset];
    bool world = true, model = true, lit = false;
    // the lit techniques (sun, spot and omni lights, with and without shadows): 8 to 21
    for (uint32_t t = 8; t < 22; ++t)
    {
        Ptr *ptr = node.relocs.get(techs + 4 * t);
        Node *tech = ptr ? ptr->target() : nullptr;
        if (!tech)
            continue;
        uint32_t count = u16(p, tech->data.data() + count_off);
        for (uint32_t k = 0; k < count; ++k)
        {
            lit = true;
            uint32_t base = pass_array + k * pass_rec.size + shaders;
            auto has = [&](uint32_t slot) {
                Ptr *sp = tech->relocs.get(base + 4 * slot);
                return sp && sp->kind != Ptr::Kind::Null;
            };
            world = world && has(2 + wvf); // vertexShaderArray of world vertex format wvf
            model = model && has(1);       // models (packed vertices)
        }
    }
    return {wvf, lit && world, lit && model};
}

TechsetFeatures techset_features(const std::string &name)
{
    TechsetFeatures out;
    std::string n = lower_latin1(name);
    n = n.substr(std::min(n.find_first_not_of(','), n.size()));
    for (const std::string &word : split(n, '_'))
    {
        // a word made of letter-digit pairs (c0n0s0): texture maps and blend modes of layers
        bool pairs = !word.empty() && word.size() % 2 == 0;
        for (size_t i = 0; pairs && i < word.size(); i += 2)
            pairs = word[i] >= 'a' && word[i] <= 'z' && word[i + 1] >= '0' && word[i + 1] <= '9';
        if (pairs)
        {
            for (size_t i = 0; i < word.size(); i += 2)
            {
                std::string f = word.substr(i, 2);
                (std::string("cnsd").find(word[i]) != std::string::npos ? out.textures : out.modes).insert(f);
            }
        }
        else
            out.words.push_back(word);
    }
    return out;
}

std::optional<std::string> closest_techset(const std::string &name, int wvf, const std::map<std::string, TechsetInfo> &candidates)
{
    TechsetFeatures f = techset_features(name);
    std::set<char> layers;
    for (const auto &t : f.textures)
        layers.insert(t[1]);
    for (const auto &m : f.modes)
        layers.insert(m[1]);
    bool model = !f.words.empty() && f.words[0] == "mc";
    std::set<std::string> word_set(f.words.begin(), f.words.end());
    std::optional<std::string> best;
    std::tuple<int, int, int> best_key;
    for (const auto &[candidate, info] : candidates) // sorted by name
    {
        auto [c_wvf, world_ok, model_ok] = info;
        if (c_wvf != wvf || !(model ? model_ok : world_ok))
            continue;
        TechsetFeatures c = techset_features(candidate);
        if (c.words.empty() || f.words.empty() || c.words[0] != f.words[0] ||
            !std::includes(f.textures.begin(), f.textures.end(), c.textures.begin(), c.textures.end()))
            continue;
        std::set<char> c_layers;
        for (const auto &t : c.textures)
            c_layers.insert(t[1]);
        for (const auto &m : c.modes)
            c_layers.insert(m[1]);
        if (c_layers != layers)
            continue;
        std::set<std::string> c_words(c.words.begin(), c.words.end());
        std::vector<std::string> word_diff, mode_diff;
        std::set_symmetric_difference(c_words.begin(), c_words.end(), word_set.begin(), word_set.end(), std::back_inserter(word_diff));
        std::set_symmetric_difference(c.modes.begin(), c.modes.end(), f.modes.begin(), f.modes.end(), std::back_inserter(mode_diff));
        auto key = std::make_tuple(static_cast<int>(c.textures.size()), -static_cast<int>(word_diff.size()), -static_cast<int>(mode_diff.size()));
        if (!best || key > best_key)
        {
            best = candidate;
            best_key = key;
        }
    }
    return best;
}
} // namespace t4ff
