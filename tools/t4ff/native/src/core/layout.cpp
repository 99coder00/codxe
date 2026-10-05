#include "core/layout.h"

#include <stdexcept>

#include "core/json.h"

namespace t4ff
{
namespace
{
struct ScalarInfo
{
    Scalar scalar;
    const char *name;
    uint32_t size;
};

constexpr ScalarInfo SCALARS[] = {
    {Scalar::Char, "char", 1},   {Scalar::SChar, "schar", 1},        {Scalar::UChar, "uchar", 1},         {Scalar::Bool, "bool", 1},
    {Scalar::Short, "short", 2}, {Scalar::UShort, "ushort", 2},      {Scalar::Int, "int", 4},             {Scalar::UInt, "uint", 4},
    {Scalar::Long, "long", 4},   {Scalar::ULong, "ulong", 4},        {Scalar::LongLong, "longlong", 8},   {Scalar::ULongLong, "ulonglong", 8},
    {Scalar::Float, "float", 4}, {Scalar::Double, "double", 8},
};

TypeKind kind_from_name(const std::string &kind)
{
    if (kind == "scalar")
        return TypeKind::Scalar;
    if (kind == "enum")
        return TypeKind::Enum;
    if (kind == "pointer")
        return TypeKind::Pointer;
    if (kind == "array")
        return TypeKind::Array;
    if (kind == "record")
        return TypeKind::Record;
    if (kind == "void")
        return TypeKind::Void;
    throw std::runtime_error("layout: unknown type kind " + kind);
}

const TypeRef *read_type(Layout &layout, const json::Value &v)
{
    TypeRef *t = layout.new_type();
    t->kind = kind_from_name(v.at("kind").string);
    t->name = v.string_or("name", "");
    t->size = static_cast<uint32_t>(v.integer_or("size", 0));
    t->align = static_cast<uint32_t>(v.integer_or("align", 1));
    if (const json::Value *to = v.get("to"))
        t->to = read_type(layout, *to);
    if (const json::Value *elem = v.get("elem"))
    {
        t->elem = read_type(layout, *elem);
        t->count = static_cast<uint32_t>(v.integer_or("count", 0));
    }
    if (t->kind == TypeKind::Scalar)
        t->scalar = scalar_from_name(t->name);
    return t;
}

void link(const Layout &layout, const TypeRef *t)
{
    for (; t; t = t->to ? t->to : t->elem)
    {
        if (t->kind == TypeKind::Record)
            const_cast<TypeRef *>(t)->record = layout.find(t->name);
    }
}
} // namespace

const char *scalar_name(Scalar s)
{
    for (const auto &info : SCALARS)
        if (info.scalar == s)
            return info.name;
    return "";
}

Scalar scalar_from_name(std::string_view name)
{
    for (const auto &info : SCALARS)
        if (name == info.name)
            return info.scalar;
    return Scalar::None;
}

uint32_t scalar_size(Scalar s)
{
    for (const auto &info : SCALARS)
        if (info.scalar == s)
            return info.size;
    return 0;
}

std::string TypeRef::repr() const
{
    if (kind == TypeKind::Pointer)
        return (to ? to->repr() : std::string("void")) + "*";
    if (kind == TypeKind::Array)
        return (elem ? elem->repr() : std::string("?")) + "[" + std::to_string(count) + "]";
    if (!name.empty())
        return name;
    switch (kind)
    {
    case TypeKind::Scalar: return "scalar";
    case TypeKind::Enum: return "enum";
    case TypeKind::Record: return "record";
    default: return "void";
    }
}

const Field *Record::field(std::string_view field_name) const
{
    for (const Field &f : fields)
        if (f.name == field_name)
            return &f;
    return nullptr;
}

const Record *Layout::find(std::string_view name) const
{
    auto it = records.find(std::string(name));
    return it == records.end() ? nullptr : it->second.get();
}

const Record &Layout::record(std::string_view name) const
{
    const Record *r = find(name);
    if (!r)
        throw std::out_of_range("layout: no record " + std::string(name));
    return *r;
}

const TypeRef *Layout::scalar(Scalar s) const
{
    return scalars[static_cast<int>(s)];
}

std::unique_ptr<Layout> Layout::from_json(std::string_view text, std::string platform)
{
    json::Value doc = json::parse(text);
    auto layout = std::make_unique<Layout>();
    layout->platform = std::move(platform);

    for (const auto &[name, v] : doc.at("enums").object)
        layout->enums[name] = v.integer();
    for (const auto &[name, v] : doc.at("enum_sizes").object)
        layout->enum_sizes[name] = static_cast<int>(v.integer());

    for (const auto &[name, v] : doc.at("records").object)
    {
        auto rec = std::make_unique<Record>();
        rec->name = v.at("name").string;
        rec->is_union = v.at("union").boolean;
        rec->size = static_cast<uint32_t>(v.at("size").integer());
        rec->align = static_cast<uint32_t>(v.at("align").integer());
        for (const json::Value &f : v.at("fields").array)
        {
            Field field;
            field.name = f.at("name").string;
            field.offset = static_cast<uint32_t>(f.at("offset").integer());
            field.type = read_type(*layout, f.at("type"));
            field.bit_offset = static_cast<int>(f.integer_or("bit_offset", -1));
            field.bit_width = static_cast<int>(f.integer_or("bit_width", 0));
            rec->fields.push_back(std::move(field));
        }
        TypeRef *self = layout->new_type();
        self->kind = TypeKind::Record;
        self->name = rec->name;
        self->size = rec->size;
        self->align = rec->align;
        rec->self = self;
        layout->order.push_back(rec.get());
        layout->records[name] = std::move(rec);
    }

    for (Record *rec : layout->order)
    {
        const_cast<TypeRef *>(rec->self)->record = rec;
        for (const Field &f : rec->fields)
            link(*layout, f.type);
    }

    for (const auto &info : SCALARS)
    {
        TypeRef *t = layout->new_type();
        t->kind = TypeKind::Scalar;
        t->name = info.name;
        t->size = info.size;
        t->align = info.size;
        t->scalar = info.scalar;
        layout->scalars[static_cast<int>(info.scalar)] = t;
    }
    return layout;
}
} // namespace t4ff
