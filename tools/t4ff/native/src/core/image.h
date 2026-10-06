#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/texture_format.h"
#include "core/xenos.h"

// PC image sources (IWI files in .iwd archives) and their conversion to Xbox 360 textures (the Python
// t4ff's images.py, plus stream.py's with_mips).
namespace t4ff
{
struct ImageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

constexpr int TS_NORMAL_MAP = 5; // GfxImage.semantic of normal maps: DXN on the console

constexpr int IWI_FLAG_NOPICMIP = 0x01;
constexpr int IWI_FLAG_NOMIPMAPS = 0x02;
constexpr int IWI_FLAG_CUBEMAP = 0x04;
constexpr int IWI_FLAG_VOLMAP = 0x08;

// A 2D texture or a cube map with its mip chain (largest first), linear PC layout. Each level of a
// cube map holds its six faces one after the other (+X, -X, +Y, -Y, +Z, -Z).
struct ImageData
{
    std::string name;
    Fmt format = Fmt::A8R8G8B8;
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;
    int flags = 0;
    std::string source;
    int faces = 1;

    bool mipped() const
    {
        return levels.size() > 1;
    }
    ImageData face(int index) const; // one face of a cube map, as a 2D texture
};

// A cube map of the faces (2D textures of one format, size and level count).
ImageData join_faces(const std::vector<ImageData> &faces);

ImageData parse_iwi(const std::string &name, const uint8_t *data, size_t size);

class ZipArchive;

// Looks up images/<name>.iwi and sound files in a list of .iwd archives and folders (the first one
// that has a name gives it).
class IwdLibrary
{
  public:
    IwdLibrary() = default;
    explicit IwdLibrary(const std::vector<std::filesystem::path> &paths);
    ~IwdLibrary();

    void add(const std::filesystem::path &path);
    std::optional<std::vector<uint8_t>> read(const std::string &rel) const;
    bool contains(const std::string &rel) const;
    std::vector<std::string> names(const std::string &prefix) const; // in the order they were added
    std::optional<ImageData> image(const std::string &name) const;

  private:
    struct Entry
    {
        const ZipArchive *archive = nullptr; // nullptr: a loose file
        size_t index = 0;
        std::filesystem::path file;
    };
    std::vector<std::unique_ptr<ZipArchive>> archives;
    std::unordered_map<std::string, Entry> entries;
    std::vector<std::string> order;

    void add_entry(std::string rel, Entry entry);
};

// -- console textures

struct ConsoleTexture
{
    const xenos::Format *format = nullptr;
    uint32_t width = 0, height = 0;
    int levels = 0;
    std::vector<uint8_t> header; // D3DBaseTexture360
    std::vector<uint8_t> pixels; // tiled data, base level followed by the mip levels
    int dropped_levels = 0;
    int faces = 1;
    uint32_t base_size = 0; // bytes of the base level (of all faces)
};

std::optional<Fmt> to_console_format(Fmt f);
ImageData convert_rgb24(const ImageData &image);
ImageData expand_a8(const ImageData &image);
ImageData compress_image(const ImageData &image);
ImageData reduce_image(const ImageData &image, int count);
ImageData normal_map_to_dxn(const ImageData &image);

// Whether a mip level of this size stays when the mip tail goes (see images.py).
inline bool keeps_level(uint32_t width, uint32_t height)
{
    return std::min(width, height) > 16;
}

struct TextureOptions
{
    uint32_t max_size = 0;   // limits the base level's sides
    bool keep_mips = true;
    int drop_levels = 0;     // top levels removed besides
    bool compress = true;    // uncompressed colour textures made DXT
    bool normal_map = false; // a PC normal map, made DXN
    bool mip_tail = true;    // false: no levels of 16 texels or less on a side
};

// image tiled for the Xbox 360 (see images.py's build_console_texture).
ConsoleTexture build_console_texture(const ImageData &image, const TextureOptions &options = {});

// Size of the console texture build_console_texture would make (without tiling). make_mips: of a
// single level image, as with the mips streaming makes for it.
uint32_t console_texture_size(const ImageData &image, const TextureOptions &options = {}, bool make_mips = false);

// (tiled pixels, level count) of a tiled single level 2D texture given the mip chain the PC's lacks,
// box filtered down to the last level of a whole block; the top level stays as it is (stream.py's
// with_mips). Nothing when it would have no mip level or the format is not encoded.
struct WithMips
{
    std::vector<uint8_t> pixels;
    int levels = 0;
};
std::optional<WithMips> with_mips(const uint8_t *pixels, size_t size, uint32_t width, uint32_t height, const xenos::Format &fmt,
                                  bool mip_tail = true);
} // namespace t4ff
