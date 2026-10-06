#include "convert/record_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

#include "core/platforms.h"

namespace t4ff
{
namespace
{
// -- numbers

uint64_t read_le(const uint8_t *p, int size)
{
    uint64_t v = 0;
    for (int i = 0; i < size; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}

void write_be(uint8_t *p, int size, uint64_t v)
{
    for (int i = 0; i < size; ++i)
        p[i] = uint8_t(v >> (8 * (size - 1 - i)));
}

uint32_t field_offset(const Platform &p, const std::string &rec, std::initializer_list<const char *> path)
{
    uint32_t offset = 0;
    const Record *r = &p.record(rec);
    for (const char *part : path)
    {
        const Field *f = r->field(part);
        if (!f)
            throw ConvertError(r->name + " has no field " + part);
        offset += f->offset;
        r = f->type->record;
    }
    return offset;
}

void put_be32(uint8_t *p, uint32_t v)
{
    write_be(p, 4, v);
}

float get_le_f32(const uint8_t *p)
{
    uint32_t bits = static_cast<uint32_t>(read_le(p, 4));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void put_be_f32(uint8_t *p, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    write_be(p, 4, bits);
}

// -- record converters and fixups (convert.RECORD_CONVERTERS, RECORD_FIXUPS)

// PC PackedUnitVec (3 biased bytes and a scale byte) -> console 10:10:10 signed normalized, the vector
// renormalized before packing
void pack_unit_vec(const Rows &src, const Rows &dst)
{
    for (size_t i = 0; i < src.count; ++i)
    {
        const uint8_t *b = src.row(i);
        double scale = (double(b[3]) + 192.0) / 32385.0;
        double v[3] = {(double(b[0]) - 127.0) * scale, (double(b[1]) - 127.0) * scale, (double(b[2]) - 127.0) * scale};
        double length = std::sqrt((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
        for (double &c : v)
            c = length > 0 ? c / length : 0.0;
        put_be32(dst.row(i), pack_dec3n(v[0], v[1], v[2]));
    }
}

// the console keeps a copy of the surface bounds right after the triangle info
void fix_gfx_surface(const Rows &, const Rows &dst)
{
    static const uint32_t b = field_offset(x360(), "GfxSurface", {"bounds"});
    static const uint32_t c = field_offset(x360(), "GfxSurface", {"boundsCopy"});
    for (size_t i = 0; i < dst.count; ++i)
        std::memmove(dst.row(i) + c, dst.row(i) + b, 24);
}

// PC placement (origin, 3x3 axis, scale) -> console origin, DEC3N packed axis, scale
void fix_static_model_inst(const Rows &src, const Rows &dst)
{
    static const uint32_t so = field_offset(pc(), "GfxStaticModelDrawInst", {"placement"});
    static const uint32_t o = field_offset(x360(), "GfxStaticModelDrawInst", {"origin"});
    static const uint32_t a = field_offset(x360(), "GfxStaticModelDrawInst", {"axis"});
    static const uint32_t sc = field_offset(x360(), "GfxStaticModelDrawInst", {"scale"});
    static const uint32_t fs = field_offset(pc(), "GfxStaticModelDrawInst", {"flags"});
    static const uint32_t fd = field_offset(x360(), "GfxStaticModelDrawInst", {"flags"});
    for (size_t i = 0; i < src.count; ++i)
    {
        const uint8_t *s = src.row(i);
        uint8_t *d = dst.row(i);
        float placement[13];
        for (int k = 0; k < 13; ++k)
            placement[k] = get_le_f32(s + so + 4 * k);
        for (int k = 0; k < 3; ++k)
            put_be_f32(d + o + 4 * k, placement[k]);
        for (int k = 0; k < 3; ++k)
            put_be32(d + a + 4 * k, pack_dec3n(placement[3 + 3 * k], placement[4 + 3 * k], placement[5 + 3 * k]));
        put_be_f32(d + sc, placement[12]);
        // the flags are byte flags (0x80 on PC is 0x80000000 on console)
        std::memcpy(d + fd, s + fs, 4);
    }
}

// leaf trees have no children offset on console (the PC linker leaves a stale value)
void fix_aabb_tree(const Rows &, const Rows &dst)
{
    static const uint32_t cc = field_offset(x360(), "GfxAabbTree", {"childCount"});
    static const uint32_t co = field_offset(x360(), "GfxAabbTree", {"childrenOffset"});
    for (size_t i = 0; i < dst.count; ++i)
    {
        uint8_t *d = dst.row(i);
        if (d[cc] == 0 && d[cc + 1] == 0)
            std::memset(d + co, 0, 4);
    }
}

RowConverter record_converter(const std::string &name)
{
    return name == "PackedUnitVec" ? pack_unit_vec : nullptr;
}

RowConverter record_fixup(const std::string &name)
{
    if (name == "GfxSurface")
        return fix_gfx_surface;
    if (name == "GfxStaticModelDrawInst")
        return fix_static_model_inst;
    if (name == "GfxAabbTree")
        return fix_aabb_tree;
    return nullptr;
}

// runtime fields left zeroed on console (the PC linker leaves garbage in them)
bool zero_field(const std::string &rec, const std::string &field)
{
    return (rec == "XSurface" && field == "zoneHandle") || (rec == "GfxSurface" && field == "pad");
}

// byte arrays the console stores as one 32-bit value (byte order reversed)
bool swap32_field(const std::string &rec, const std::string &field)
{
    return rec == "FxElemVisualState" && field == "color";
}

// bytes of non pointer scalar data in a type
uint32_t scalar_bytes(const TypeRef &t)
{
    if (t.kind == TypeKind::Scalar || t.kind == TypeKind::Enum)
        return t.size;
    if (t.kind == TypeKind::Array)
        return t.count * scalar_bytes(*t.elem);
    if (t.kind == TypeKind::Record && t.record)
    {
        uint32_t total = 0, largest = 0;
        bool any = false;
        for (const Field &f : t.record->fields)
        {
            if (f.name.empty())
                continue;
            uint32_t s = scalar_bytes(*f.type);
            total += s;
            largest = std::max(largest, s);
            any = true;
        }
        return any ? (t.record->is_union ? largest : total) : 0;
    }
    return 0;
}

uint32_t elem_size(const TypeRef &t)
{
    const TypeRef *e = &t;
    while (e->kind == TypeKind::Array)
        e = e->elem;
    return e->size;
}
} // namespace

uint32_t pack_dec3n(double x, double y, double z)
{
    uint32_t out = 0;
    const double v[3] = {x, y, z};
    for (int i = 0; i < 3; ++i)
    {
        double q = std::clamp(std::floor(v[i] * 511.0 + 0.5), -511.0, 511.0);
        out |= (static_cast<uint32_t>(static_cast<int64_t>(q)) & 0x3FF) << (10 * i);
    }
    return out;
}

NumType np_type(const TypeRef &t)
{
    if (t.kind == TypeKind::Enum)
        return t.size == 1 ? NumType{'u', 1} : t.size == 2 ? NumType{'u', 2} : NumType{'i', 4};
    if (t.kind == TypeKind::Pointer)
        return {'u', 4};
    if (t.kind != TypeKind::Scalar || t.size == 1)
        return {}; // single bytes are copied verbatim
    switch (t.scalar)
    {
    case Scalar::Short: return {'i', 2};
    case Scalar::UShort: return {'u', 2};
    case Scalar::Int:
    case Scalar::Long: return {'i', 4};
    case Scalar::UInt:
    case Scalar::ULong: return {'u', 4};
    case Scalar::LongLong: return {'i', 8};
    case Scalar::ULongLong: return {'u', 8};
    case Scalar::Float: return {'f', 4};
    case Scalar::Double: return {'f', 8};
    default: return {};
    }
}

void cast_scalar(const uint8_t *src, NumType st, uint8_t *dst, NumType dt)
{
    if (st == dt)
    {
        for (int i = 0; i < st.size; ++i)
            dst[i] = src[st.size - 1 - i];
        return;
    }
    uint64_t raw = read_le(src, st.size);
    double f = 0;
    int64_t si = 0;
    bool is_float = st.kind == 'f';
    if (st.kind == 'f')
    {
        if (st.size == 4)
        {
            float v;
            uint32_t bits = static_cast<uint32_t>(raw);
            std::memcpy(&v, &bits, 4);
            f = v;
        }
        else
            std::memcpy(&f, &raw, 8);
    }
    else if (st.kind == 'i')
    {
        int shift = 64 - 8 * st.size;
        si = static_cast<int64_t>(raw << shift) >> shift; // sign extension
    }
    else
        si = static_cast<int64_t>(raw);
    if (dt.kind == 'f')
    {
        double v = is_float ? f : (st.kind == 'u' ? static_cast<double>(raw) : static_cast<double>(si));
        if (dt.size == 4)
        {
            float g = static_cast<float>(v);
            uint32_t bits;
            std::memcpy(&bits, &g, 4);
            write_be(dst, 4, bits);
        }
        else
        {
            uint64_t bits;
            std::memcpy(&bits, &v, 8);
            write_be(dst, 8, bits);
        }
        return;
    }
    uint64_t v = is_float ? static_cast<uint64_t>(std::isfinite(f) && std::abs(f) < 9.2e18 ? static_cast<int64_t>(f) : INT64_MIN)
                          : static_cast<uint64_t>(si);
    write_be(dst, dt.size, v); // C conversion: the low bytes
}

// -- RecordMap

RecordMap::RecordMap(const Platform &src, const Platform &dst, const std::string &name_, bool partial)
    : src_p(src), dst_p(dst), name(name_)
{
    const Record &src_rec = src.record(name);
    const Record &dst_rec = dst.record(name);
    src_size = src_rec.size;
    dst_size = dst_rec.size;
    int64_t src_limit = -1, dst_limit = -1;
    if (partial)
    {
        const Field *sdyn = src.dynamic_member(src_rec);
        const Field *ddyn = dst.dynamic_member(dst_rec);
        src_limit = sdyn ? sdyn->offset : src_rec.size;
        dst_limit = ddyn ? ddyn->offset : dst_rec.size;
        src_size = static_cast<uint32_t>(src_limit);
        dst_size = static_cast<uint32_t>(dst_limit);
    }
    map_record(src_rec, dst_rec, 0, 0, src_limit, dst_limit);
    if (RowConverter fix = record_fixup(name); fix && !partial)
        fixups.push_back({0, 0, src_rec.size, dst_rec.size, fix});
    merge();
}

void RecordMap::map_record(const Record &src, const Record &dst, uint32_t so, uint32_t d_o, int64_t src_limit, int64_t dst_limit)
{
    if (dst.is_union)
    {
        // pointer members all live at the union offset
        offsets.emplace(so, d_o);
        bool dst_ptr = std::any_of(dst.fields.begin(), dst.fields.end(), [](const Field &f) { return f.type->kind == TypeKind::Pointer; });
        bool src_ptr = std::any_of(src.fields.begin(), src.fields.end(), [](const Field &f) { return f.type->kind == TypeKind::Pointer; });
        if (dst_ptr && src_ptr)
            pointers.insert(so);
        std::vector<const Field *> candidates;
        for (const Field &f : dst.fields)
            if (!f.name.empty() && src.field(f.name))
                candidates.push_back(&f);
        if (candidates.empty())
            return;
        // map the member carrying the most plain data (a leaf count over child pointers): pointer
        // slots are rewritten by the zone writer anyway; pointers of every member are kept
        auto key = [](const Field *f) {
            return std::make_tuple(f->type->kind != TypeKind::Pointer, scalar_bytes(*f->type), elem_size(*f->type), f->type->size);
        };
        const Field *chosen = candidates[0];
        for (const Field *f : candidates)
            if (key(f) > key(chosen))
                chosen = f; // max(): the first of the largest
        for (const Field *f : candidates)
            collect_pointers(*src.field(f->name)->type, *f->type, so, d_o);
        map_field(*src.field(chosen->name)->type, *chosen->type, so, d_o);
        return;
    }

    std::set<uint32_t> seen_storage;
    for (const Field &f : dst.fields)
    {
        if (f.name.empty())
            continue;
        const Field *sf = src.field(f.name);
        if (!sf)
            continue;
        if (src_limit >= 0 && sf->offset >= src_limit)
            continue;
        if (dst_limit >= 0 && f.offset >= dst_limit)
            continue;
        if (zero_field(dst.name, f.name))
            continue;
        if (swap32_field(dst.name, f.name) && sf->type->size == f.type->size && f.type->size % 4 == 0)
        {
            offsets.emplace(so + sf->offset, d_o + f.offset);
            ops.push_back({so + sf->offset, {'u', 4}, d_o + f.offset, {'u', 4}, f.type->size / 4});
            continue;
        }
        if (f.bit_width)
        {
            if (!seen_storage.insert(f.offset).second)
                continue;
        }
        map_field(*sf->type, *f.type, so + sf->offset, d_o + f.offset);
    }
}

void RecordMap::collect_pointers(const TypeRef &st, const TypeRef &dt, uint32_t so, uint32_t d_o)
{
    if (dt.kind == TypeKind::Pointer && st.kind == TypeKind::Pointer)
        pointers.insert(so);
    else if (dt.kind == TypeKind::Array && st.kind == TypeKind::Array)
    {
        for (uint32_t i = 0; i < std::min(st.count, dt.count); ++i)
            collect_pointers(*st.elem, *dt.elem, so + i * st.elem->size, d_o + i * dt.elem->size);
    }
    else if (dt.kind == TypeKind::Record && st.kind == TypeKind::Record)
    {
        const Record &src = src_p.record(st.name), &dst = dst_p.record(dt.name);
        for (const Field &f : dst.fields)
        {
            const Field *sf = f.name.empty() ? nullptr : src.field(f.name);
            if (sf)
                collect_pointers(*sf->type, *f.type, so + sf->offset, d_o + f.offset);
        }
    }
}

void RecordMap::map_field(const TypeRef &st, const TypeRef &dt, uint32_t so, uint32_t d_o)
{
    offsets.emplace(so, d_o);
    if (dt.kind == TypeKind::Record)
    {
        if (RowConverter conv = record_converter(dt.name); conv && st.kind == TypeKind::Record && st.name == dt.name)
        {
            custom.push_back({so, d_o, st.size, conv});
            return;
        }
        if (st.kind == TypeKind::Pointer || st.kind == TypeKind::Scalar)
            return; // a PC runtime object pointer that is an embedded structure on console: zeroed
        if (st.kind != TypeKind::Record)
            throw ConvertError(name + ": type mismatch " + st.repr() + " -> " + dt.repr());
        map_record(src_p.record(st.name), dst_p.record(dt.name), so, d_o, -1, -1);
        if (RowConverter fix = record_fixup(dt.name))
            fixups.push_back({so, d_o, st.size, dt.size, fix});
        return;
    }
    if (dt.kind == TypeKind::Array)
    {
        if (st.kind != TypeKind::Array)
        {
            map_field(st, *dt.elem, so, d_o); // e.g. int -> int[4]: into the first element
            return;
        }
        uint32_t n = std::min(st.count, dt.count);
        const TypeRef &se = *st.elem, &de = *dt.elem;
        bool scalar_de = de.kind == TypeKind::Scalar || de.kind == TypeKind::Enum;
        bool scalar_se = se.kind == TypeKind::Scalar || se.kind == TypeKind::Enum;
        if (scalar_de && scalar_se)
        {
            NumType s_np = np_type(se), d_np = np_type(de);
            if (s_np.raw() || d_np.raw())
            {
                uint32_t size = std::min(se.size, de.size);
                if (se.size == de.size)
                    raw.push_back({so, d_o, n * se.size});
                else
                    for (uint32_t i = 0; i < n; ++i)
                        raw.push_back({so + i * se.size, d_o + i * de.size, size});
            }
            else
                ops.push_back({so, s_np, d_o, d_np, n});
            for (uint32_t i = 0; i < n; ++i)
                offsets.emplace(so + i * se.size, d_o + i * de.size);
            return;
        }
        for (uint32_t i = 0; i < n; ++i)
            map_field(se, de, so + i * se.size, d_o + i * de.size);
        return;
    }
    if (dt.kind == TypeKind::Pointer)
    {
        if (st.kind == TypeKind::Pointer)
            pointers.insert(so);
        return; // pointer values are written by the zone writer
    }
    if (dt.kind == TypeKind::Scalar || dt.kind == TypeKind::Enum)
    {
        if (st.kind != TypeKind::Scalar && st.kind != TypeKind::Enum && st.kind != TypeKind::Pointer)
            throw ConvertError(name + ": type mismatch " + st.repr() + " -> " + dt.repr());
        NumType s_np = np_type(st), d_np = np_type(dt);
        if (s_np.raw() && d_np.raw())
            raw.push_back({so, d_o, 1});
        else
            ops.push_back({so, s_np.raw() ? NumType{'u', 1} : s_np, d_o, d_np.raw() ? NumType{'u', 1} : d_np, 1});
        return;
    }
    if (dt.kind == TypeKind::Void)
        return;
    throw ConvertError(name + ": unsupported field type " + dt.repr());
}

void RecordMap::merge()
{
    // adjacent scalar runs of the same types
    std::stable_sort(ops.begin(), ops.end(), [](const ScalarOp &a, const ScalarOp &b) { return a.src < b.src; });
    std::vector<ScalarOp> merged;
    for (const ScalarOp &op : ops)
    {
        if (!merged.empty())
        {
            ScalarOp &last = merged.back();
            if (last.src_type == op.src_type && last.dst_type == op.dst_type && last.src + last.count * last.src_type.size == op.src &&
                last.dst + last.count * last.dst_type.size == op.dst)
            {
                last.count += op.count;
                continue;
            }
        }
        merged.push_back(op);
    }
    ops = std::move(merged);
}

std::vector<uint8_t> RecordMap::convert(const uint8_t *data, size_t available, uint32_t count, uint32_t src_stride) const
{
    if (!src_stride)
        src_stride = src_size;
    if (count == 0)
        return {};
    if (available < static_cast<size_t>(count) * src_stride)
        throw ConvertError(name + ": not enough data for " + std::to_string(count) + " records");
    std::vector<uint8_t> out(static_cast<size_t>(count) * dst_size);
    Rows src{const_cast<uint8_t *>(data), src_stride, src_stride, count};
    // numpy column slices end at the row: a field past the end of a partly streamed record is an empty
    // slice, assigned to an empty slice (nothing happens); widths that differ after clipping are an error
    auto clipped = [](uint32_t start, uint32_t width, uint32_t row) -> uint32_t {
        return start >= row ? 0 : std::min(width, row - start);
    };
    for (const ScalarOp &op : ops)
    {
        uint32_t ws = clipped(op.src, op.count * op.src_type.size, src_stride);
        uint32_t wd = clipped(op.dst, op.count * op.dst_type.size, dst_size);
        if (ws % op.src_type.size || ws / op.src_type.size * op.dst_type.size != wd)
            throw ConvertError(name + ": field outside the record");
        uint32_t n = ws / op.src_type.size;
        for (uint32_t i = 0; i < count && n; ++i)
        {
            const uint8_t *s = src.row(i) + op.src;
            uint8_t *d = out.data() + static_cast<size_t>(i) * dst_size + op.dst;
            for (uint32_t k = 0; k < n; ++k)
                cast_scalar(s + k * op.src_type.size, op.src_type, d + k * op.dst_type.size, op.dst_type);
        }
    }
    for (const Raw &r : raw)
    {
        uint32_t ws = clipped(r.src, r.size, src_stride), wd = clipped(r.dst, r.size, dst_size);
        if (ws != wd)
            throw ConvertError(name + ": field outside the record");
        for (uint32_t i = 0; i < count && ws; ++i)
            std::memcpy(out.data() + static_cast<size_t>(i) * dst_size + r.dst, src.row(i) + r.src, ws);
    }
    for (const Custom &c : custom)
        c.fn(Rows{src.base + c.src, src_stride, c.size, count}, Rows{out.data() + c.dst, dst_size, 4, count});
    for (const Fixup &fx : fixups)
        fx.fn(Rows{src.base + fx.src, src_stride, fx.src_size, count}, Rows{out.data() + fx.dst, dst_size, fx.dst_size, count});
    return out;
}

uint32_t RecordMap::map_offset(uint32_t off) const
{
    auto it = offsets.upper_bound(off);
    if (it == offsets.begin())
        throw ConvertError(name + ": cannot map offset " + std::to_string(off));
    --it; // the closest preceding mapped offset
    return it->second + (off - it->first);
}

// -- ScalarMap and ArrayMap

ScalarMap::ScalarMap(const TypeRef &t) : type(np_type(t))
{
    src_size = dst_size = std::max(t.size, 1u);
}

std::vector<uint8_t> ScalarMap::convert(const uint8_t *data, size_t available, uint32_t count, uint32_t) const
{
    size_t size = std::min(available, static_cast<size_t>(count) * src_size);
    if (type.raw())
        return std::vector<uint8_t>(data, data + size);
    if (size % type.size)
        throw ConvertError("scalar data of a size not a multiple of its type's");
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; i += type.size)
        cast_scalar(data + i, type, out.data() + i, type);
    return out;
}

ArrayMap::ArrayMap(const TypeMap &elem_, uint32_t count_) : elem(elem_), count(count_)
{
    src_size = elem.src_size * count;
    dst_size = elem.dst_size * count;
}

std::vector<uint8_t> ArrayMap::convert(const uint8_t *data, size_t available, uint32_t n, uint32_t) const
{
    return elem.convert(data, available, n * count);
}

uint32_t ArrayMap::map_offset(uint32_t off) const
{
    if (!elem.src_size)
        throw ConvertError("array of empty elements");
    return off / elem.src_size * elem.dst_size + elem.map_offset(off % elem.src_size);
}

bool ArrayMap::keeps_pointer(uint32_t off) const
{
    return elem.src_size ? elem.keeps_pointer(off % elem.src_size) : true;
}
} // namespace t4ff
