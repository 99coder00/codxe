#include "convert/xanim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "convert/converter.h"

namespace t4ff
{
namespace
{
const char *PC_QUAT_TYPES[] = {"none", "half", "full", "halfns", "fullns"};
const char *PC_TRANS_TYPES[] = {"small", "full", "nosize", "none"};
const char *X360_QUAT_TYPES[] = {"none", "half", "fullc", "fullp", "halfns", "fullnsc", "fullnsp"};
constexpr int PC_PART_TYPE_COUNT = 10;
constexpr int X360_PART_TYPE_COUNT = 12;
constexpr int ASSET_TYPE_VIEWMODEL = 1;

// bones whose full quaternions keep the precise 48-bit packing in non viewmodel animations
const std::unordered_set<std::string> PRECISE_BONES = {
    "j_mainroot",   "j_hip_le",       "j_hip_ri",       "j_knee_le",     "j_knee_ri",     "j_ankle_le",
    "j_ankle_ri",   "j_spinelower",   "j_spineupper",   "j_spine4",      "j_clavicle_le", "j_clavicle_ri",
    "j_shoulder_le", "j_shoulder_ri", "j_elbow_le",     "j_elbow_ri",    "j_wrist_le",    "j_wrist_ri",
};

// stream order of the XAnimParts members
const char *MEMBER_ORDER[] = {"name",    "names",           "notify",         "deltaPart",     "dataByte", "dataShort",
                              "dataInt", "randomDataShort", "randomDataByte", "randomDataInt", "indices"};

int member_rank(const std::string &m)
{
    for (int i = 0; i < 11; ++i)
        if (m == MEMBER_ORDER[i])
            return i;
    throw XAnimError("unknown XAnimParts member " + m);
}

struct Indices
{
    std::string kind = "none"; // 'byte' (dataByte), 'short' (dataShort) or 'pool' (indices + checkpoints)
    std::vector<int64_t> values;
    std::vector<int64_t> checkpoints;
};

struct QuatTrack
{
    std::string kind;
    int64_t stored_size = 0;
    Indices indices;
    std::vector<int64_t> frames; // (n, 4) or (n, 2) int16 values
};

struct TransTrack
{
    std::string kind;
    int64_t bone = 0;
    int64_t stored_size = 0;
    std::vector<int64_t> mins, size, frames, constant;
    Indices indices;
};

class Cursor
{
  public:
    std::map<std::string, std::vector<int64_t>> arrays;
    std::map<std::string, size_t> pos;

    std::vector<int64_t> pop(const std::string &key, size_t n = 1)
    {
        auto &a = arrays[key];
        size_t i = pos[key];
        if (i + n > a.size())
            throw XAnimError(key + " exhausted (need " + std::to_string(n) + " at " + std::to_string(i) + " of " + std::to_string(a.size()) + ")");
        pos[key] = i + n;
        return std::vector<int64_t>(a.begin() + i, a.begin() + i + n);
    }
    bool remaining() const
    {
        for (const auto &[k, v] : arrays)
            if (v.size() > pos.at(k))
                return true;
        return false;
    }
};

Indices read_indices(Cursor &cur, int64_t stored, bool byte_indices)
{
    size_t n = static_cast<size_t>(stored + 1);
    Indices idx;
    if (byte_indices)
    {
        idx.kind = "byte";
        idx.values = cur.pop("dataByte", n);
    }
    else if (stored >= 64)
    {
        idx.kind = "pool";
        idx.values = cur.pop("indices", n);
        idx.checkpoints = cur.pop("dataShort", (n - 2) / 256 + 2);
    }
    else
    {
        idx.kind = "short";
        idx.values = cur.pop("dataShort", n);
    }
    return idx;
}

void read_pc(const std::vector<int> &bone_counts, Cursor &cur, std::vector<QuatTrack> &quats, std::vector<TransTrack> &trans, bool byte_indices)
{
    for (int k = 0; k < 5; ++k)
    {
        std::string kind = PC_QUAT_TYPES[k];
        for (int i = 0; i < bone_counts[k]; ++i)
        {
            QuatTrack t;
            t.kind = kind;
            if (kind == "half" || kind == "full")
            {
                t.stored_size = cur.pop("dataShort")[0] & 0xFFFF;
                t.indices = read_indices(cur, t.stored_size, byte_indices);
                size_t width = kind == "half" ? 2 : 4;
                t.frames = cur.pop("randomDataShort", static_cast<size_t>(t.stored_size + 1) * width);
            }
            else if (kind == "halfns")
                t.frames = cur.pop("dataShort", 2);
            else if (kind == "fullns")
                t.frames = cur.pop("dataShort", 4);
            quats.push_back(std::move(t));
        }
    }
    for (int k = 0; k < 4; ++k)
    {
        std::string kind = PC_TRANS_TYPES[k];
        for (int i = 0; i < bone_counts[5 + k]; ++i)
        {
            TransTrack t;
            t.kind = kind;
            t.bone = cur.pop("dataByte")[0];
            if (kind == "small" || kind == "full")
            {
                t.stored_size = cur.pop("dataShort")[0] & 0xFFFF;
                t.mins = cur.pop("dataInt", 3);
                t.size = cur.pop("dataInt", 3);
                t.indices = read_indices(cur, t.stored_size, byte_indices);
                size_t n = static_cast<size_t>(t.stored_size + 1) * 3;
                t.frames = kind == "small" ? cur.pop("randomDataByte", n) : cur.pop("randomDataShort", n);
            }
            else if (kind == "nosize")
                t.constant = cur.pop("dataInt", 3);
            trans.push_back(std::move(t));
        }
    }
    if (cur.remaining())
        throw XAnimError("unread animation data");
}

struct ConsoleAnim
{
    std::vector<int> bone_order;
    std::vector<int> bone_counts;
    std::vector<int64_t> data_byte, data_short, data_int, random_short, random_byte, random_int, indices;
};

void put_indices(ConsoleAnim &w, const Indices &idx)
{
    if (idx.kind == "byte")
        w.data_byte.insert(w.data_byte.end(), idx.values.begin(), idx.values.end());
    else if (idx.kind == "short")
        w.data_short.insert(w.data_short.end(), idx.values.begin(), idx.values.end());
    else if (idx.kind == "pool")
    {
        w.indices.insert(w.indices.end(), idx.values.begin(), idx.values.end());
        w.data_short.insert(w.data_short.end(), idx.checkpoints.begin(), idx.checkpoints.end());
    }
}

void append_quats48(std::vector<int64_t> &out, const std::vector<int64_t> &frames)
{
    static const int widths[3] = {15, 15, 15};
    for (uint64_t v : pack_quats(frames, widths))
    {
        out.push_back(static_cast<int64_t>((v >> 32) & 0xFFFF));
        out.push_back(static_cast<int64_t>((v >> 16) & 0xFFFF));
        out.push_back(static_cast<int64_t>(v & 0xFFFF));
    }
}

void append_quats32(std::vector<int64_t> &out, const std::vector<int64_t> &frames)
{
    static const int widths[3] = {9, 10, 10};
    for (uint64_t v : pack_quats(frames, widths))
        out.push_back(static_cast<int64_t>(static_cast<uint32_t>(v)));
}

ConsoleAnim to_console(const std::vector<std::string> &bone_names, const std::vector<QuatTrack> &quats, const std::vector<TransTrack> &trans,
                       int asset_type)
{
    bool viewmodel = asset_type == ASSET_TYPE_VIEWMODEL;
    std::vector<std::string> kinds;
    for (size_t i = 0; i < quats.size(); ++i)
    {
        bool precise = viewmodel || PRECISE_BONES.count(bone_names[i]);
        const std::string &k = quats[i].kind;
        kinds.push_back(k == "full" ? (precise ? "fullp" : "fullc") : k == "fullns" ? (precise ? "fullnsp" : "fullnsc") : k);
    }
    ConsoleAnim w;
    for (const char *kind : X360_QUAT_TYPES)
        for (size_t i = 0; i < kinds.size(); ++i)
            if (kinds[i] == kind)
                w.bone_order.push_back(static_cast<int>(i));
    std::unordered_map<int64_t, int64_t> new_index;
    for (size_t n = 0; n < w.bone_order.size(); ++n)
        new_index[w.bone_order[n]] = static_cast<int64_t>(n);

    for (int i : w.bone_order)
    {
        const QuatTrack &track = quats[i];
        const std::string &kind = kinds[i];
        if (kind == "half" || kind == "fullc" || kind == "fullp")
        {
            w.data_short.push_back(track.stored_size);
            put_indices(w, track.indices);
            if (kind == "half")
                for (uint16_t v : pack_half_quats(track.frames))
                    w.random_short.push_back(v);
            else if (kind == "fullc")
                append_quats32(w.random_int, track.frames);
            else
                append_quats48(w.random_short, track.frames);
        }
        else if (kind == "halfns")
            for (uint16_t v : pack_half_quats(track.frames))
                w.data_short.push_back(v);
        else if (kind == "fullnsc")
            append_quats32(w.data_int, track.frames);
        else if (kind == "fullnsp")
            append_quats48(w.data_short, track.frames);
    }

    std::vector<int> trans_counts;
    for (const char *kind : PC_TRANS_TYPES)
    {
        std::vector<const TransTrack *> tracks;
        for (const TransTrack &t : trans)
            if (t.kind == kind)
                tracks.push_back(&t);
        auto index_of = [&](int64_t bone) {
            auto it = new_index.find(bone);
            if (it == new_index.end())
                throw XAnimError("translation of a bone without a rotation track");
            return it->second;
        };
        std::stable_sort(tracks.begin(), tracks.end(), [&](const TransTrack *a, const TransTrack *b) { return index_of(a->bone) < index_of(b->bone); });
        trans_counts.push_back(static_cast<int>(tracks.size()));
        for (const TransTrack *t : tracks)
        {
            w.data_byte.push_back(index_of(t->bone));
            if (t->kind == "small" || t->kind == "full")
            {
                w.data_short.push_back(t->stored_size);
                w.data_int.insert(w.data_int.end(), t->mins.begin(), t->mins.end());
                w.data_int.insert(w.data_int.end(), t->size.begin(), t->size.end());
                put_indices(w, t->indices);
                auto &dst = t->kind == "small" ? w.random_byte : w.random_short;
                dst.insert(dst.end(), t->frames.begin(), t->frames.end());
            }
            else if (t->kind == "nosize")
                w.data_int.insert(w.data_int.end(), t->constant.begin(), t->constant.end());
        }
    }
    for (const char *kind : X360_QUAT_TYPES)
        w.bone_counts.push_back(static_cast<int>(std::count(kinds.begin(), kinds.end(), kind)));
    w.bone_counts.insert(w.bone_counts.end(), trans_counts.begin(), trans_counts.end());
    w.bone_counts.push_back(static_cast<int>(quats.size()));
    return w;
}

std::string member_of(const Node &node)
{
    if (node.origin != Origin::Member)
        return "";
    return *node.origin_record == "XAnimIndices" ? "indices" : *node.origin_field;
}

uint64_t read_uint(const Platform &p, const uint8_t *d, uint32_t size)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; ++i)
        v |= uint64_t(d[p.big_endian ? i : size - 1 - i]) << (8 * (size - 1 - i));
    return v;
}

void write_uint(const Platform &p, uint8_t *d, uint32_t size, uint64_t v)
{
    for (uint32_t i = 0; i < size; ++i)
        d[p.big_endian ? i : size - 1 - i] = uint8_t(v >> (8 * (size - 1 - i)));
}
} // namespace

std::vector<uint64_t> pack_quats(const std::vector<int64_t> &q, const int widths[3])
{
    size_t n = q.size() / 4;
    std::vector<uint64_t> out(n);
    for (size_t r = 0; r < n; ++r)
    {
        const int64_t *v = &q[4 * r];
        int comp = 0;
        for (int c = 1; c < 4; ++c)
            if (std::llabs(v[c]) > std::llabs(v[comp]))
                comp = c; // argmax: the first largest
        double largest = static_cast<double>(v[comp]);
        if (largest == 0)
            largest = 1.0;
        uint64_t o = 0;
        int shift = 0;
        for (int k = 0; k < 3; ++k)
        {
            int w = widths[k];
            double other = static_cast<double>(v[(comp + 1 + k) % 4]);
            double scale = static_cast<double>((int64_t(1) << (w - 1)) - 1);
            double f = std::floor(other / largest * scale + 0.5);
            f = std::clamp(f, -static_cast<double>(int64_t(1) << (w - 1)), static_cast<double>((int64_t(1) << (w - 1)) - 1));
            int64_t iv = static_cast<int64_t>(f);
            o |= static_cast<uint64_t>(iv & ((int64_t(1) << w) - 1)) << shift;
            shift += w;
        }
        o |= static_cast<uint64_t>(3 - comp) << shift;
        o |= static_cast<uint64_t>(v[comp] < 0) << (shift + 2);
        if (!v[0] && !v[1] && !v[2] && !v[3])
            o = 0;
        out[r] = o;
    }
    return out;
}

std::vector<uint16_t> pack_half_quats(const std::vector<int64_t> &q)
{
    size_t n = q.size() / 2;
    std::vector<uint16_t> out(n);
    for (size_t r = 0; r < n; ++r)
    {
        int64_t z = q[2 * r], w = q[2 * r + 1];
        bool z_largest = std::llabs(z) >= std::llabs(w);
        double largest = static_cast<double>(z_largest ? z : w);
        double other = static_cast<double>(z_largest ? w : z);
        double ratio = largest != 0 ? other / largest : 0.0;
        double f = std::clamp(std::floor(ratio * 8191 + 0.5), -8192.0, 8191.0);
        int64_t v = static_cast<int64_t>(f);
        int64_t o = (v & 0x3FFF) | (int64_t(z_largest) << 14) | (int64_t(largest < 0) << 15);
        if (z == 0 && w == 0)
            o = 0;
        out[r] = static_cast<uint16_t>(o);
    }
    return out;
}

void convert_xanim_parts(ZoneConverter &conv, Node &src, Node &dst, const std::string &name)
{
    const Platform &sp = conv.src, &dp = conv.dst;
    const Record &srec = sp.record("XAnimParts"), &drec = dp.record("XAnimParts");
    auto get = [&](const char *f) {
        const Field *field = srec.field(f);
        return read_uint(sp, src.data.data() + field->offset, field->type->size);
    };
    auto put = [&](const char *f, uint64_t value) {
        const Field *field = drec.field(f);
        write_uint(dp, dst.data.mutable_data() + field->offset, field->type->size, value);
    };
    uint64_t numframes = get("numframes");
    int asset_type = static_cast<int>(get("assetType"));
    uint32_t bc = srec.field("boneCount")->offset;
    std::vector<int> bone_counts(src.data.begin() + bc, src.data.begin() + bc + PC_PART_TYPE_COUNT);

    std::map<std::string, Node *> src_children; // last wins, as a dict
    for (Node *c : src.children)
        src_children[member_of(*c)] = c;
    Cursor cur;
    struct ArrayType
    {
        int size;
        bool is_signed;
    };
    const std::map<std::string, ArrayType> pc_arrays = {{"dataByte", {1, false}},       {"dataShort", {2, true}},      {"dataInt", {4, true}},
                                                        {"randomDataShort", {2, true}}, {"randomDataByte", {1, false}}, {"indices", {2, false}}};
    bool byte_indices = numframes < 256;
    for (const auto &[member, type] : pc_arrays)
    {
        if (member == "indices" && byte_indices)
            continue; // the byte index pool is carried over as it is
        std::vector<int64_t> values;
        auto it = src_children.find(member);
        if (it != src_children.end())
        {
            const Bytes &d = it->second->data;
            for (size_t i = 0; i + type.size <= d.size(); i += type.size)
            {
                uint64_t raw = read_uint(sp, d.data() + i, type.size);
                int shift = 64 - 8 * type.size;
                values.push_back(type.is_signed ? static_cast<int64_t>(raw << shift) >> shift : static_cast<int64_t>(raw));
            }
        }
        cur.arrays[member] = std::move(values);
        cur.pos[member] = 0;
    }
    std::vector<uint16_t> name_ids;
    if (auto it = src_children.find("names"); it != src_children.end())
        for (size_t i = 0; i + 1 < it->second->data.size(); i += 2)
            name_ids.push_back(static_cast<uint16_t>(read_uint(sp, it->second->data.data() + i, 2)));
    const auto &strings = conv.zone.script_strings;
    std::vector<std::string> bone_names;
    for (uint16_t i : name_ids)
        bone_names.push_back(i < strings.size() && strings[i] ? *strings[i] : "");
    if (static_cast<int>(bone_names.size()) != bone_counts[9])
        throw XAnimError("xanim '" + name + "': " + std::to_string(bone_names.size()) + " bone names for " + std::to_string(bone_counts[9]) +
                         " bones");

    std::vector<QuatTrack> quats;
    std::vector<TransTrack> trans;
    read_pc(bone_counts, cur, quats, trans, byte_indices);
    ConsoleAnim anim = to_console(bone_names, quats, trans, asset_type);

    uint32_t dbc = drec.field("boneCount")->offset;
    for (int i = 0; i < X360_PART_TYPE_COUNT; ++i)
    {
        if (anim.bone_counts[i] < 0 || anim.bone_counts[i] > 255)
            throw XAnimError("xanim '" + name + "': bone count out of range");
        dst.data.mutable_data()[dbc + i] = static_cast<uint8_t>(anim.bone_counts[i]);
    }
    put("dataByteCount", anim.data_byte.size());
    put("dataShortCount", anim.data_short.size());
    put("dataIntCount", anim.data_int.size());
    put("randomDataByteCount", anim.random_byte.size());
    put("randomDataIntCount", anim.random_int.size());
    put("randomDataShortCount", anim.random_short.size());

    std::vector<int64_t> names;
    for (int i : anim.bone_order)
        names.push_back(name_ids[i]);
    // member, values, bytes of a value, the scalar type of the array
    struct NewArray
    {
        const char *member;
        const std::vector<int64_t> *values;
        int size;
        Scalar scalar;
    };
    std::vector<NewArray> arrays = {{"names", &names, 2, Scalar::UShort},
                                    {"dataByte", &anim.data_byte, 1, Scalar::UChar},
                                    {"dataShort", &anim.data_short, 2, Scalar::Short},
                                    {"dataInt", &anim.data_int, 4, Scalar::Int},
                                    {"randomDataShort", &anim.random_short, 2, Scalar::Short},
                                    {"randomDataByte", &anim.random_byte, 1, Scalar::UChar},
                                    {"randomDataInt", &anim.random_int, 4, Scalar::Int}};
    if (numframes >= 256)
    {
        arrays.push_back({"indices", &anim.indices, 2, Scalar::UShort});
        put("indexCount", anim.indices.size());
    }
    for (const auto &value : anim.data_byte)
        if (value < 0 || value > 255)
            throw XAnimError("xanim '" + name + "': byte data out of range");

    std::map<std::string, Node *> dst_children;
    for (Node *c : dst.children)
        dst_children[member_of(*c)] = c;
    for (const NewArray &a : arrays)
    {
        const std::vector<int64_t> &values = *a.values;
        std::vector<uint8_t> data(values.size() * a.size);
        for (size_t i = 0; i < values.size(); ++i)
            write_uint(dp, data.data() + i * a.size, a.size, static_cast<uint64_t>(values[i]));
        uint32_t count = static_cast<uint32_t>(values.size());
        uint32_t offset = drec.field(a.member)->offset;
        Ptr *ptr = dst.relocs.get(offset);
        auto found = dst_children.find(a.member);
        Node *child = found == dst_children.end() ? nullptr : found->second;
        if (!count)
        {
            if (child)
            {
                auto it = std::find(dst.children.begin(), dst.children.end(), child);
                if (it != dst.children.end())
                    dst.children.erase(it);
            }
            if (ptr)
            {
                ptr->kind = Ptr::Kind::Null;
                ptr->node = nullptr;
            }
            continue;
        }
        if (!child)
        {
            child = conv.new_node(dp.layout->scalar(a.scalar), count, BLOCK_VIRTUAL);
            child->align = a.size;
            child->origin = Origin::Member;
            bool indices = std::string(a.member) == "indices";
            child->origin_record = intern(indices ? "XAnimIndices" : "XAnimParts");
            child->origin_field = intern(indices ? "_2" : a.member);
            int rank = member_rank(a.member);
            size_t pos = dst.children.size();
            for (size_t i = 0; i < dst.children.size(); ++i)
            {
                std::string m = member_of(*dst.children[i]);
                if (member_rank(m.empty() ? "name" : m) > rank)
                {
                    pos = i;
                    break;
                }
            }
            dst.children.insert(dst.children.begin() + pos, child);
            Ptr *np = conv.new_ptr(Ptr::Kind::Follow);
            np->node = child;
            np->owner = &dst;
            np->offset = offset;
            dst.relocs.set(offset, np);
        }
        child->count = count;
        child->segments = {{child->type, count, static_cast<uint32_t>(data.size()), false}};
        child->data.assign(std::move(data));
    }
}
} // namespace t4ff
