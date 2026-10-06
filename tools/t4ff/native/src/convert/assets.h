#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/image.h"
#include "core/zone.h"

// Asset specific PC -> Xbox 360 conversion hooks (the Python t4ff's assets.py; loaded sounds are
// encoded in step 4).
namespace t4ff
{
class ZoneConverter;

constexpr int MAPTYPE_2D = 3;
constexpr int MAPTYPE_CUBE = 5;

// offset of an asset record's name pointer (throws std::out_of_range when it has none)
uint32_t name_offset(const Platform &p, const std::string &rec_name);

// nodes made outside a conversion (merges): in zone
Node *string_node(Zone &zone, const Platform &p, const std::string &text);
Ptr *follow(Zone &zone, Node *owner, uint32_t offset, Node *child);
Node *asset_header(Zone &zone, const Platform &dst, const std::string &rec_name, std::optional<uint32_t> size = std::nullopt);
// an asset header that only carries a (comma prefixed) name
Node *build_reference(Zone &zone, const Platform &dst, const std::string &asset_type, const Node &src_node, const std::string &ref_name);

// within a conversion
Node *build_reference(ZoneConverter &conv, const std::string &asset_type, const Node &src_node, const std::string &ref_name);
void map_name_string(ZoneConverter &conv, const Node &src_node, const std::string &asset_type, Node &new_asset);

void register_hooks(ZoneConverter &conv);
// how many top mip levels each image drops so the textures of all the zones fit one budget
void plan_textures_shared(const std::vector<ZoneConverter *> &convs);
// loaded sounds encoded before the zone is converted (step 4: nothing without an encoder)
void encode_loaded_sounds(ZoneConverter &conv);
void apply_string_edits(ZoneConverter &conv, Node *root);
Node *rebuild_console_image(ZoneConverter &conv, const std::string &name, Node *library_node);

// the PC pixel data of an image (shared: the caches keep them)
using ImageRef = std::shared_ptr<const ImageData>;
ImageRef image_source(ZoneConverter &conv, Node &node, const std::string &name);
ImageRef stock_image_source(ZoneConverter &conv, const std::string &name);
ImageRef console_image(ZoneConverter &conv, const std::string &name);
std::optional<ImageData> decode_console_image(const Platform &p, Node &node, const std::string &name);
std::pair<std::string, ImageRef> image_choice(ZoneConverter &conv, Node &node, const std::string &name);
// The PC pixel data of the images of the zones, read now on every processor (image_source then takes
// it, as it would have read it).
void prefetch_image_sources(const std::vector<ZoneConverter *> &convs);
// The console textures the image hooks of the zones will build, built now on every processor (the
// same function of the same data: the hooks then take them).
void prebuild_textures(const std::vector<ZoneConverter *> &convs);
bool pc_normal_map(const ImageData &source, int semantic);

// steps each image takes so their sizes fit budget (assets.choose_drops)
struct DropPlan
{
    std::map<std::string, int> drops;
    uint64_t total = 0;
    bool fits = true;
};
DropPlan choose_drops(const std::vector<std::string> &names, const std::function<uint64_t(const std::string &, int)> &size,
                      const std::function<bool(const std::string &, int)> &can_drop, uint64_t budget, const std::set<std::string> &last,
                      const std::function<int(const std::string &, int)> &tier);

// sounds
std::string console_sound_name(const std::string &name);
uint32_t stream_name_hash(const std::string &path);
uint32_t map_stream_hash(const std::string &directory, const std::string &name);
// 'sound/eggs/para_egg.wav' -> 'sounds/eggs/para_egg.xma' (audio.streamed_sound_target)
std::string streamed_sound_target(const std::string &rel);
// PC vertex layer data for the console, one 4 byte word at a time
std::vector<uint8_t> console_vertex_layer_data(const uint8_t *data, size_t size);
} // namespace t4ff
