#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/zone.h"

// Mapping the bytes of PC structures to the console's (the Python t4ff's convert.py RecordMap,
// ScalarMap and _ArrayMap): fields are matched by name, scalars byte swapped (and cast when their
// types differ, as numpy's astype does), arrays truncated or zero extended, pointer slots left to
// the zone writer.
namespace t4ff
{
struct ConvertError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A scalar as numpy sees it: kind 'i', 'u' or 'f' and its size (size 0: raw bytes, numpy's None)
struct NumType
{
    char kind = 0;
    uint8_t size = 0;
    bool raw() const
    {
        return size == 0;
    }
    bool operator==(const NumType &o) const
    {
        return kind == o.kind && size == o.size;
    }
};
NumType np_type(const TypeRef &t);

class TypeMap
{
  public:
    virtual ~TypeMap() = default;
    uint32_t src_size = 0, dst_size = 0;
    // count elements of src_stride bytes (0: src_size) to the console's
    virtual std::vector<uint8_t> convert(const uint8_t *data, size_t available, uint32_t count, uint32_t src_stride = 0) const = 0;
    virtual uint32_t map_offset(uint32_t off) const = 0;
    virtual bool keeps_pointer(uint32_t off) const = 0;
};

// Rows of records being converted: row i of width bytes at base + i * stride.
struct Rows
{
    uint8_t *base;
    size_t stride;
    size_t width;
    size_t count;
    uint8_t *row(size_t i) const
    {
        return base + i * stride;
    }
};
using RowConverter = void (*)(const Rows &src, const Rows &dst);

class RecordMap : public TypeMap
{
  public:
    RecordMap(const Platform &src, const Platform &dst, const std::string &name, bool partial);

    std::vector<uint8_t> convert(const uint8_t *data, size_t available, uint32_t count, uint32_t src_stride = 0) const override;
    uint32_t map_offset(uint32_t off) const override;
    bool keeps_pointer(uint32_t off) const override
    {
        return pointers.count(off) != 0;
    }

    struct ScalarOp
    {
        uint32_t src;
        NumType src_type;
        uint32_t dst;
        NumType dst_type;
        uint32_t count;
    };
    struct Raw
    {
        uint32_t src, dst, size;
    };
    struct Custom
    {
        uint32_t src, dst, size;
        RowConverter fn; // writes the dst columns from the src ones
    };
    struct Fixup
    {
        uint32_t src, dst, src_size, dst_size;
        RowConverter fn;
    };

  private:
    const Platform &src_p, &dst_p;
    std::string name;
    std::vector<ScalarOp> ops;
    std::vector<Raw> raw;
    std::vector<Custom> custom;
    std::vector<Fixup> fixups;
    std::map<uint32_t, uint32_t> offsets; // src offset -> dst offset of every mapped field
    std::set<uint32_t> pointers;          // src offsets of pointer fields kept on console

    void map_record(const Record &src, const Record &dst, uint32_t so, uint32_t d_o, int64_t src_limit, int64_t dst_limit);
    void collect_pointers(const TypeRef &st, const TypeRef &dt, uint32_t so, uint32_t d_o);
    void map_field(const TypeRef &st, const TypeRef &dt, uint32_t so, uint32_t d_o);
    void merge();
};

class ScalarMap : public TypeMap
{
  public:
    explicit ScalarMap(const TypeRef &t);
    std::vector<uint8_t> convert(const uint8_t *data, size_t available, uint32_t count, uint32_t src_stride = 0) const override;
    uint32_t map_offset(uint32_t off) const override
    {
        return off;
    }
    bool keeps_pointer(uint32_t) const override
    {
        return true;
    }

  private:
    NumType type;
};

class ArrayMap : public TypeMap
{
  public:
    ArrayMap(const TypeMap &elem, uint32_t count);
    std::vector<uint8_t> convert(const uint8_t *data, size_t available, uint32_t count, uint32_t src_stride = 0) const override;
    uint32_t map_offset(uint32_t off) const override;
    bool keeps_pointer(uint32_t off) const override;

  private:
    const TypeMap &elem;
    uint32_t count;
};

// Converts one scalar from its PC (little endian) bytes to its console (big endian) ones, as numpy's
// astype does: the same type is the bytes reversed, others are cast.
void cast_scalar(const uint8_t *src, NumType st, uint8_t *dst, NumType dt);

// signed 10:10:10 packing of (x, y, z) in [-1, 1] (convert.pack_dec3n)
uint32_t pack_dec3n(double x, double y, double z);
} // namespace t4ff
