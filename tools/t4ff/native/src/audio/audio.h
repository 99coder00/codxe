#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Audio conversion: PC sounds -> Xbox 360 XMA (the Python t4ff's audio.py).
//
// Streamed sounds go next to the converted fastfile as sounds/<path>.xma in the container of the
// game's own streams (SDNS: a 4 KiB header with a block table, then XMA2 packets in 4 KiB blocks).
// Loaded sounds are XMA1 with a seek table and the format block of CoD Xenon's. Encoding runs
// xma2encode.exe from Microsoft's Xbox developer kits; sounds other than PCM WAV are decoded with
// FFmpeg, as the Python does.
namespace t4ff
{
class IwdLibrary;

struct AudioError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Pcm
{
    int rate = 0;
    int channels = 1;
    std::vector<int16_t> samples; // interleaved
    size_t frames() const
    {
        return channels ? samples.size() / channels : 0;
    }
};

struct XmaStream
{
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint32_t samples = 0;
    std::vector<uint8_t> data; // XMA2 packets
    uint32_t valid_samples = 0; // decoded samples that belong to the sound
};

constexpr uint32_t XMA_PACKET_SIZE = 2048;
constexpr uint32_t XMA_FRAME_SAMPLES = 512;

// -- PCM
Pcm read_wav(const std::vector<uint8_t> &data);
std::vector<uint8_t> write_wav(const Pcm &pcm);
Pcm downmix_mono(const Pcm &pcm);
Pcm resample(const Pcm &pcm, int rate);
// any sound FFmpeg knows (channels and rate, when given, spare asking FFmpeg for them)
Pcm decode_with_ffmpeg(const std::vector<uint8_t> &data, int channels = 0, int rate = 0);
bool is_xwma(const std::vector<uint8_t> &data);

// FFmpeg: --ffmpeg, T4FF_FFMPEG, PATH, next to the program, then imageio-ffmpeg's of a Python install
void set_ffmpeg_path(const std::filesystem::path &path);
std::filesystem::path ffmpeg_exe();

// -- XMA
std::vector<std::pair<uint64_t, uint32_t>> xma_frames(const std::vector<uint8_t> &data); // (bit offset, bit length)
uint32_t xma_frame_count(const std::vector<uint8_t> &data);
std::pair<std::vector<uint8_t>, std::vector<uint32_t>> xma2_reblock(const std::vector<uint8_t> &data, uint32_t block_packets = 2);
std::vector<uint8_t> xma1_repack(const std::vector<uint8_t> &data);
XmaStream read_xma2_wav(const std::vector<uint8_t> &data);
XmaStream read_sdns(const std::vector<uint8_t> &data);
std::vector<uint8_t> write_sdns(const XmaStream &stream);
bool sdns_has_game_layout(const std::vector<uint8_t> &data);
std::optional<std::vector<uint8_t>> upgrade_sdns(const std::vector<uint8_t> &data);
struct UpgradeStats
{
    int files = 0, upgraded = 0, failed = 0;
};
UpgradeStats upgrade_stream_files(const std::filesystem::path &folder, const std::function<void(const std::string &)> &log);

// Runs xma2encode.exe. Its output for the same input is the same (checked), so encodings are kept in a
// cache folder by the SHA-256 of their input.
class XmaEncoder
{
  public:
    XmaEncoder(std::filesystem::path path, int quality = 60, std::filesystem::path cache_dir = {});
    bool available() const;
    const std::filesystem::path &path() const
    {
        return path_;
    }
    XmaStream encode(Pcm pcm);
    int cache_hits() const
    {
        return hits_;
    }

  private:
    std::filesystem::path path_;
    int quality_;
    std::filesystem::path cache_dir_;
    std::string identity_; // of the encoder (size and date), part of the cache keys
    std::atomic<int> variant_{-1};
    std::atomic<int> hits_{0};
    std::vector<uint8_t> run(const std::vector<uint8_t> &wav);
};

// xma2encode.exe where the Xbox developer kits and t4ff's setup put it (deps.find_xma2encode)
std::filesystem::path find_xma2encode();
// %LOCALAPPDATA%\t4ff\sound_cache
std::filesystem::path default_sound_cache_dir();

// -- loaded sounds
uint32_t xma1_rate(int rate);
struct LoadedXma
{
    uint32_t rate = 0, channels = 0;
    std::vector<uint8_t> data; // XMA1 packets
    std::vector<uint32_t> seek_table;
    std::array<uint32_t, 36> format{}; // the console snd_asset format block
    std::shared_ptr<XmaStream> stream; // the xma2encode stream, should the sound be streamed instead
};
LoadedXma loaded_sound(const XmaStream &stream, uint32_t duration_ms);
LoadedXma encode_loaded_sound(const std::vector<uint8_t> &wav, XmaEncoder &encoder, int max_rate = 0, bool mono = false);

// -- streamed sounds
std::string streamed_sound_target_path(const std::string &rel);
struct StreamStats
{
    int sounds = 0, converted = 0, failed = 0, kept = 0;
    uint64_t input_bytes = 0, output_bytes = 0;
};
// the sound/ files of the library (names: these, default all) as sounds/<path>.xma in out_dir, on
// every processor; skip_existing keeps the files already there
StreamStats convert_streamed_sounds(const IwdLibrary &library, const std::filesystem::path &out_dir, XmaEncoder &encoder, int max_rate,
                                    bool mono, const std::function<void(const std::string &)> &log, int jobs,
                                    const std::vector<std::string> *names = nullptr, bool skip_existing = false);
} // namespace t4ff
