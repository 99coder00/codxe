#include "audio/audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <windows.h>

#include "core/fastfile.h"
#include "core/image.h"
#include "core/process.h"
#include "core/sha256.h"
#include "core/threads.h"

namespace t4ff
{
namespace fs = std::filesystem;

namespace
{
constexpr uint32_t SDNS_HEADER_SIZE = 0x1000;
constexpr uint32_t SDNS_TABLE = 0x18;
constexpr uint32_t SDNS_TABLE_ENTRIES = (SDNS_HEADER_SIZE - SDNS_TABLE) / 4;
constexpr uint32_t STREAM_BLOCK_PACKETS = 2;
constexpr uint32_t XMA_MAX_PACKET_FRAMES = 63;
constexpr uint32_t XMA_SUBFRAME_SAMPLES = 128;
constexpr uint32_t PAYLOAD_BITS = (XMA_PACKET_SIZE - 4) * 8;
constexpr uint32_t PACKET_BITS = XMA_PACKET_SIZE * 8;
constexpr uint32_t WAVE_FORMAT_XMA2 = 0x166;
constexpr uint32_t XAUDIO_SAMPLE_TYPE_XMA = 3;
constexpr uint32_t LOOP_SUBFRAME_SKIP = 3;

uint16_t le16(const uint8_t *p)
{
    return uint16_t(p[0] | p[1] << 8);
}
uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t be32(const uint8_t *p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void put_le16(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}
void put_le32(std::vector<uint8_t> &out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}
void put_be32(std::vector<uint8_t> &out, uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        out.push_back(uint8_t(v >> (8 * i)));
}
void put_be32_at(uint8_t *p, uint32_t v)
{
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void append(std::vector<uint8_t> &out, const char *s)
{
    out.insert(out.end(), s, s + std::strlen(s));
}

using Chunks = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

Chunks riff_chunks(const std::vector<uint8_t> &data)
{
    if (data.size() < 4 || std::memcmp(data.data(), "RIFF", 4) != 0)
        throw AudioError("not a RIFF file");
    Chunks chunks;
    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        std::string id(data.begin() + pos, data.begin() + pos + 4);
        uint32_t size = le32(data.data() + pos + 4);
        size_t start = pos + 8, end = std::min<size_t>(data.size(), start + size);
        chunks.emplace_back(id, std::vector<uint8_t>(data.begin() + start, data.begin() + end));
        pos += 8 + size + (size & 1);
    }
    return chunks;
}

// dict(riff_chunks(data)): the last chunk of each id
const std::vector<uint8_t> *chunk(const Chunks &chunks, const char *id)
{
    const std::vector<uint8_t> *found = nullptr;
    for (const auto &[cid, body] : chunks)
        if (cid == id)
            found = &body;
    return found;
}

// numpy's float64 sin and cos are the C runtime's (ucrtbase.dll) on processors without AVX-512: the
// resampler uses those, not the static library's or the compiler's
struct Trig
{
    double (*sin)(double) = nullptr;
    double (*cos)(double) = nullptr;
    Trig()
    {
        if (HMODULE m = LoadLibraryW(L"ucrtbase.dll"))
        {
            sin = reinterpret_cast<double (*)(double)>(GetProcAddress(m, "sin"));
            cos = reinterpret_cast<double (*)(double)>(GetProcAddress(m, "cos"));
        }
        if (!sin || !cos)
        {
            sin = [](double x) { return std::sin(x); };
            cos = [](double x) { return std::cos(x); };
        }
    }
};
const Trig &trig()
{
    static const Trig t;
    return t;
}

std::mutex ffmpeg_lock;
fs::path ffmpeg_override;

std::string ffmpeg_message(const std::string &stderr_text)
{
    std::vector<std::string> lines;
    std::istringstream in(stderr_text);
    for (std::string line; std::getline(in, line);)
    {
        size_t a = line.find_first_not_of(" \t\r\n"), b = line.find_last_not_of(" \t\r\n");
        if (a != std::string::npos)
            lines.push_back(line.substr(a, b - a + 1));
    }
    if (lines.empty())
        return "unknown error";
    std::string first = lines[0].size() <= 200 ? lines[0] : lines[0].substr(0, 200) + "...";
    return first + (lines.size() > 1 ? " (and " + std::to_string(lines.size() - 1) + " more messages)" : "");
}

std::string strip(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::pair<Pcm, std::string> ffmpeg_decode(const std::vector<uint8_t> &data, std::optional<int> channels, std::optional<int> rate)
{
    fs::path exe = ffmpeg_exe();
    if (exe.empty())
        throw AudioError("FFmpeg is required to decode this sound (pip install imageio-ffmpeg)");
    TempDir tmp;
    fs::path src = tmp.path() / "in.bin";
    write_file(src, data);
    if (!rate || !channels)
    {
        ProcessResult probe = run_process({exe.wstring(), L"-hide_banner", L"-i", src.wstring()});
        int probed_rate = 44100, probed_channels = 1;
        std::istringstream in(probe.err);
        for (std::string line; std::getline(in, line);)
        {
            if (line.find("Audio:") == std::string::npos)
                continue;
            std::istringstream parts(line);
            for (std::string part; std::getline(parts, part, ',');)
            {
                part = strip(part);
                auto ends = [&](const char *s) {
                    size_t n = std::strlen(s);
                    return part.size() >= n && part.compare(part.size() - n, n, s) == 0;
                };
                if (ends(" Hz"))
                    probed_rate = std::stoi(part.substr(0, part.find(' ')));
                else if (part == "mono")
                    probed_channels = 1;
                else if (part == "stereo")
                    probed_channels = 2;
                else if (ends(" channels"))
                    probed_channels = std::stoi(part.substr(0, part.find(' ')));
            }
            break;
        }
        if (!rate || !*rate)
            rate = probed_rate;
        if (!channels || !*channels)
            channels = probed_channels;
    }
    ProcessResult out = run_process({exe.wstring(), L"-hide_banner", L"-loglevel", L"error", L"-i", src.wstring(), L"-f", L"s16le", L"-ac",
                                     std::to_wstring(*channels), L"-"});
    if (out.exit_code != 0)
        throw AudioError("FFmpeg failed: " + ffmpeg_message(out.err));
    if (*channels <= 0)
        throw AudioError("FFmpeg decoded a sound without channels");
    Pcm pcm;
    pcm.rate = *rate;
    pcm.channels = *channels;
    size_t frames = out.out.size() / 2 / *channels;
    pcm.samples.resize(frames * *channels);
    std::memcpy(pcm.samples.data(), out.out.data(), pcm.samples.size() * 2);
    return {std::move(pcm), strip(out.err)};
}

// PC loaded sounds are often xWMA, which FFmpeg decodes with the right bit rate and codec options
// only: these are tried in turn (audio.XWMA_RETRIES)
std::vector<uint8_t> xwma_file(const std::vector<uint8_t> &fmt, const std::vector<uint8_t> &dpds, const std::vector<uint8_t> &body,
                               int bitrate, int options)
{
    uint16_t tag = le16(fmt.data()), channels = le16(fmt.data() + 2), align = le16(fmt.data() + 12), bits = le16(fmt.data() + 14);
    uint32_t rate = le32(fmt.data() + 4), avg_bytes = le32(fmt.data() + 8);
    std::vector<uint8_t> extra;
    if (options >= 0)
    {
        put_le32(extra, 0);
        put_le16(extra, static_cast<uint16_t>(options));
    }
    std::vector<uint8_t> f;
    put_le16(f, tag);
    put_le16(f, channels);
    put_le32(f, rate);
    put_le32(f, bitrate ? static_cast<uint32_t>(bitrate / 8) : avg_bytes);
    put_le16(f, align);
    put_le16(f, bits);
    put_le16(f, static_cast<uint16_t>(extra.size()));
    f.insert(f.end(), extra.begin(), extra.end());
    std::vector<uint8_t> out;
    auto add = [&](const char *id, const std::vector<uint8_t> &c) {
        append(out, id);
        put_le32(out, static_cast<uint32_t>(c.size()));
        out.insert(out.end(), c.begin(), c.end());
        if (c.size() & 1)
            out.push_back(0);
    };
    add("fmt ", f);
    add("dpds", dpds);
    add("data", body);
    std::vector<uint8_t> file;
    append(file, "RIFF");
    put_le32(file, static_cast<uint32_t>(4 + out.size()));
    append(file, "XWMA");
    file.insert(file.end(), out.begin(), out.end());
    return file;
}

Pcm decode_xwma(const std::vector<uint8_t> &data)
{
    Chunks chunks = riff_chunks(data);
    const auto &fmt = *chunk(chunks, "fmt "), &dpds = *chunk(chunks, "dpds"), &body = *chunk(chunks, "data");
    int channels = le16(fmt.data() + 2);
    int rate = static_cast<int>(le32(fmt.data() + 4));
    int64_t expected = dpds.size() >= 4 ? le32(dpds.data() + dpds.size() - 4) / (2 * channels) : 0;
    const std::pair<int, int> attempts[] = {{0, -1}, {20000, 0x17}, {20000, -1}, {32000, 0x17}, {32000, -1}, {48000, 0x17}};
    std::vector<Pcm> decoded;
    std::optional<AudioError> first_error;
    for (auto [bitrate, options] : attempts)
    {
        std::pair<Pcm, std::string> result;
        try
        {
            result = ffmpeg_decode(xwma_file(fmt, dpds, body, bitrate, options), channels, rate);
        }
        catch (const AudioError &e)
        {
            if (!first_error)
                first_error = e;
            continue;
        }
        bool complete = !expected || std::llabs(static_cast<int64_t>(result.first.frames()) - expected) <= 2048;
        if (complete && result.second.empty())
            return std::move(result.first);
        if (complete)
            decoded.push_back(std::move(result.first));
    }
    if (!decoded.empty())
        return std::move(decoded[0]); // complete, with some damaged packets
    if (first_error)
        throw *first_error;
    throw AudioError("xWMA sound could not be decoded");
}

std::vector<uint8_t> unpack_bits(const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> bits(data.size() * 8);
    for (size_t i = 0; i < data.size(); ++i)
        for (int b = 0; b < 8; ++b)
            bits[8 * i + b] = (data[i] >> (7 - b)) & 1;
    return bits;
}

void pack_bits(const uint8_t *bits, size_t count, std::vector<uint8_t> &out)
{
    for (size_t i = 0; i < count; i += 8)
    {
        uint8_t v = 0;
        for (int b = 0; b < 8 && i + b < count; ++b)
            v |= bits[i + b] << (7 - b);
        out.push_back(v);
    }
}

// the frame's bits without the packet headers it spans
void copy_frame(const std::vector<uint8_t> &source, uint64_t bit, uint32_t length, uint8_t *dst)
{
    uint64_t at = bit;
    uint32_t remaining = length;
    while (remaining)
    {
        uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(remaining, PACKET_BITS - at % PACKET_BITS));
        std::memcpy(dst, source.data() + at, take);
        dst += take;
        remaining -= take;
        at += take + 32; // the next packet's header
    }
}

std::vector<uint32_t> seek_table(const std::vector<uint8_t> &data, const std::vector<std::pair<uint64_t, uint32_t>> &frames)
{
    size_t packets = data.size() / XMA_PACKET_SIZE;
    std::vector<uint32_t> starts(packets);
    for (const auto &[bit, len] : frames)
        ++starts[std::min<uint64_t>(bit / PACKET_BITS, packets - 1)];
    std::vector<uint32_t> table;
    uint32_t total = 0;
    for (uint32_t n : starts)
    {
        table.push_back(total);
        total += n * XMA_FRAME_SAMPLES;
    }
    return table;
}

std::vector<uint8_t> xma2_to_xma1(const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> out = data;
    for (size_t i = 0, o = 0; o + 3 < out.size(); ++i, o += XMA_PACKET_SIZE)
    {
        uint32_t offset = (be32(out.data() + o) >> 11) & 0x7FFF;
        put_be32_at(out.data() + o, uint32_t((i & 0xF) << 28) | (0x2u << 26) | (offset << 11));
    }
    return out;
}
} // namespace

// -- PCM

Pcm read_wav(const std::vector<uint8_t> &data)
{
    Chunks chunks = riff_chunks(data);
    const std::vector<uint8_t> *fmt = chunk(chunks, "fmt "), *pcm = chunk(chunks, "data");
    if (!fmt || !pcm || data.size() < 12 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        return decode_with_ffmpeg(data);
    if (fmt->size() < 16)
        throw AudioError("WAV format chunk too short");
    int tag = le16(fmt->data()), channels = le16(fmt->data() + 2), bits = le16(fmt->data() + 14);
    int rate = static_cast<int>(le32(fmt->data() + 4));
    if (tag == 0xFFFE && fmt->size() >= 26)
        tag = le16(fmt->data() + 24);
    if ((tag != 1 && tag != 3) || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return decode_with_ffmpeg(data, channels, rate); // compressed: FFmpeg, told the rate and channels
    std::vector<int16_t> s;
    const std::vector<uint8_t> &p = *pcm;
    if (tag == 1 && bits == 8)
        for (uint8_t v : p)
            s.push_back(static_cast<int16_t>(static_cast<int16_t>(v - 128) << 8));
    else if (tag == 1 && bits == 16)
        for (size_t i = 0; i + 1 < p.size(); i += 2)
            s.push_back(static_cast<int16_t>(le16(p.data() + i)));
    else if (tag == 1 && bits == 24)
        for (size_t i = 0; i + 2 < p.size(); i += 3)
        {
            int32_t v = static_cast<int32_t>(uint32_t(p[i + 2]) << 24 | uint32_t(p[i + 1]) << 16 | uint32_t(p[i]) << 8);
            s.push_back(static_cast<int16_t>(v >> 16));
        }
    else if (tag == 1 && bits == 32)
    {
        if (p.size() % 4)
            throw std::runtime_error("32 bit PCM data of a size not a multiple of 4");
        for (size_t i = 0; i < p.size(); i += 4)
            s.push_back(static_cast<int16_t>(static_cast<int32_t>(le32(p.data() + i)) >> 16));
    }
    else if (tag == 3 && bits == 32)
    {
        if (p.size() % 4)
            throw std::runtime_error("float data of a size not a multiple of 4");
        for (size_t i = 0; i + 3 < p.size(); i += 4)
        {
            uint32_t b = le32(p.data() + i);
            float f;
            std::memcpy(&f, &b, 4);
            float v = std::clamp(f, -1.0f, 1.0f) * 32767.0f; // float32 arithmetic, as numpy's
            s.push_back(static_cast<int16_t>(v));
        }
    }
    else
        return decode_with_ffmpeg(data);
    if (channels <= 0)
        throw AudioError("WAV without channels");
    Pcm out;
    out.rate = rate;
    out.channels = channels;
    s.resize(s.size() / channels * channels);
    out.samples = std::move(s);
    return out;
}

std::vector<uint8_t> write_wav(const Pcm &pcm)
{
    std::vector<uint8_t> out;
    uint32_t body = static_cast<uint32_t>(pcm.samples.size() * 2);
    append(out, "RIFF");
    put_le32(out, 4 + 8 + 16 + 8 + body);
    append(out, "WAVEfmt ");
    put_le32(out, 16);
    put_le16(out, 1);
    put_le16(out, static_cast<uint16_t>(pcm.channels));
    put_le32(out, static_cast<uint32_t>(pcm.rate));
    put_le32(out, static_cast<uint32_t>(pcm.rate * pcm.channels * 2));
    put_le16(out, static_cast<uint16_t>(pcm.channels * 2));
    put_le16(out, 16);
    append(out, "data");
    put_le32(out, body);
    size_t at = out.size();
    out.resize(at + body);
    std::memcpy(out.data() + at, pcm.samples.data(), body);
    return out;
}

Pcm downmix_mono(const Pcm &pcm)
{
    if (pcm.channels == 1)
        return pcm;
    Pcm out;
    out.rate = pcm.rate;
    out.channels = 1;
    size_t frames = pcm.frames();
    out.samples.resize(frames);
    for (size_t i = 0; i < frames; ++i)
    {
        int64_t sum = 0;
        for (int c = 0; c < pcm.channels; ++c)
            sum += pcm.samples[i * pcm.channels + c];
        double mean = static_cast<double>(sum) / pcm.channels;
        out.samples[i] = static_cast<int16_t>(std::clamp(mean, -32768.0, 32767.0)); // astype: toward zero
    }
    return out;
}

Pcm resample(const Pcm &pcm, int rate)
{
    // band limited resampling (windowed sinc), numpy's float64 expression term for term
    if (rate == pcm.rate || pcm.frames() == 0)
        return pcm;
    const Trig &tr = trig();
    const double pi = 3.141592653589793;
    double ratio = static_cast<double>(rate) / pcm.rate;
    size_t out_frames = static_cast<size_t>(static_cast<double>(pcm.frames()) * ratio);
    const int taps = 32;
    double cutoff = std::min(ratio, 1.0) * 0.95;
    const int ch = pcm.channels;
    const int64_t frames = static_cast<int64_t>(pcm.frames());
    Pcm out;
    out.rate = rate;
    out.channels = ch;
    out.samples.resize(out_frames * ch);
    std::vector<double> acc(ch);
    for (size_t i = 0; i < out_frames; ++i)
    {
        double t = static_cast<double>(i) / ratio;
        int64_t base = static_cast<int64_t>(std::floor(t));
        double frac = t - static_cast<double>(base);
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int k = -taps + 1; k <= taps; ++k)
        {
            double x = static_cast<double>(k) - frac;
            double z = cutoff * x;
            double y = pi * (z == 0 ? 1.0e-20 : z);
            double sinc = tr.sin(y) / y;
            double w = (cutoff * sinc) * (0.5 + 0.5 * tr.cos(pi * x / taps));
            int64_t src = base + k; // padded[base + k + taps]: taps zeros before, taps + 1 after
            for (int c = 0; c < ch; ++c)
            {
                double v = (src >= 0 && src < frames) ? static_cast<double>(pcm.samples[src * ch + c]) : 0.0;
                acc[c] = acc[c] + v * w;
            }
        }
        for (int c = 0; c < ch; ++c)
            out.samples[i * ch + c] = static_cast<int16_t>(std::clamp(std::nearbyint(acc[c]), -32768.0, 32767.0));
    }
    return out;
}

bool is_xwma(const std::vector<uint8_t> &data)
{
    if (data.size() < 4 || std::memcmp(data.data(), "RIFF", 4) != 0)
        return false;
    Chunks chunks;
    try
    {
        chunks = riff_chunks(data);
    }
    catch (const AudioError &)
    {
        return false;
    }
    const auto *fmt = chunk(chunks, "fmt ");
    return fmt && fmt->size() >= 16 && le16(fmt->data()) == 0x161 && chunk(chunks, "dpds") && chunk(chunks, "data");
}

Pcm decode_with_ffmpeg(const std::vector<uint8_t> &data, int channels, int rate)
{
    if (is_xwma(data))
        return decode_xwma(data);
    // read_wav passes the header's values (given even when 0); the others pass none
    std::optional<int> c, r;
    if (channels || rate)
        c = channels, r = rate;
    return ffmpeg_decode(data, c, r).first;
}

void set_ffmpeg_path(const fs::path &path)
{
    std::lock_guard guard(ffmpeg_lock);
    ffmpeg_override = path;
}

fs::path ffmpeg_exe()
{
    static std::once_flag once;
    static fs::path found;
    {
        std::lock_guard guard(ffmpeg_lock);
        if (!ffmpeg_override.empty())
            return ffmpeg_override;
    }
    std::call_once(once, [] {
        if (const wchar_t *env = _wgetenv(L"T4FF_FFMPEG"); env && *env)
        {
            found = env;
            return;
        }
        found = find_on_path(L"ffmpeg");
        if (!found.empty())
            return;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::path dir = fs::path(exe).parent_path();
        for (const fs::path &p : {dir / "ffmpeg.exe", dir / "bin" / "ffmpeg.exe"})
            if (fs::is_regular_file(p))
            {
                found = p;
                return;
            }
        // imageio-ffmpeg's, as the Python t4ff uses
        std::vector<fs::path> roots;
        for (const wchar_t *var : {L"LOCALAPPDATA", L"APPDATA"})
            if (const wchar_t *v = _wgetenv(var))
                roots.push_back(fs::path(v) / (std::wstring(var) == L"LOCALAPPDATA" ? L"Programs\\Python" : L"Python"));
        for (const fs::path &root : roots)
        {
            std::error_code ec;
            if (!fs::is_directory(root, ec))
                continue;
            for (const auto &py : fs::directory_iterator(root, ec))
            {
                for (const fs::path &site : {py.path() / "Lib" / "site-packages", py.path() / "site-packages"})
                {
                    fs::path bins = site / "imageio_ffmpeg" / "binaries";
                    if (!fs::is_directory(bins, ec))
                        continue;
                    for (const auto &f : fs::directory_iterator(bins, ec))
                    {
                        std::wstring n = f.path().filename().wstring();
                        if (n.rfind(L"ffmpeg", 0) == 0 && f.path().extension() == L".exe")
                        {
                            found = f.path();
                            return;
                        }
                    }
                }
            }
        }
    });
    return found;
}

// -- XMA

std::vector<std::pair<uint64_t, uint32_t>> xma_frames(const std::vector<uint8_t> &data)
{
    // (absolute bit offset, bit length) of every XMA frame in decoding order: a frame can continue in
    // the next packet; its last bit is 0 when no other frame starts in its packet (decoding goes on at
    // the first frame of the next packet that has one)
    size_t packets = data.size() / XMA_PACKET_SIZE;
    if (!packets)
        return {};
    std::vector<uint8_t> payload;
    payload.reserve(packets * (XMA_PACKET_SIZE - 4));
    std::vector<uint32_t> offsets(packets);
    for (size_t i = 0; i < packets; ++i)
    {
        payload.insert(payload.end(), data.begin() + i * XMA_PACKET_SIZE + 4, data.begin() + (i + 1) * XMA_PACKET_SIZE);
        offsets[i] = (be32(data.data() + i * XMA_PACKET_SIZE) >> 11) & 0x7FFF;
    }
    const uint64_t total = payload.size() * 8;
    auto bits = [&](uint64_t pos, uint32_t count) -> uint32_t {
        uint64_t first = pos >> 3, last = (pos + count + 7) >> 3;
        uint64_t v = 0;
        for (uint64_t b = first; b < last; ++b)
            v = (v << 8) | payload[b];
        return static_cast<uint32_t>((v >> (last * 8 - pos - count)) & ((1u << count) - 1));
    };
    auto first_frame = [&](size_t packet) -> std::optional<uint64_t> {
        for (size_t k = packet; k < packets; ++k)
            if (offsets[k] < PAYLOAD_BITS)
                return k * PAYLOAD_BITS + offsets[k];
        return std::nullopt;
    };
    std::vector<std::pair<uint64_t, uint32_t>> frames;
    std::optional<uint64_t> pos = first_frame(0);
    while (pos && *pos + 15 <= total)
    {
        uint32_t length = bits(*pos, 15);
        uint64_t end = *pos + length;
        if (length == 0x7FFF || length <= 15 || end > total)
        {
            pos = first_frame(*pos / PAYLOAD_BITS + 1); // padding, or a damaged packet
            continue;
        }
        frames.emplace_back((*pos / PAYLOAD_BITS) * PACKET_BITS + 32 + *pos % PAYLOAD_BITS, length);
        if (bits(end - 1, 1))
            pos = end;
        else
        {
            pos = first_frame(*pos / PAYLOAD_BITS + 1);
            if (pos && *pos < end)
                break;
        }
    }
    return frames;
}

uint32_t xma_frame_count(const std::vector<uint8_t> &data)
{
    uint32_t n = 0;
    for (size_t o = 0; o + 3 < data.size(); o += XMA_PACKET_SIZE)
        n += be32(data.data() + o) >> 26;
    return n;
}

std::pair<std::vector<uint8_t>, std::vector<uint32_t>> xma2_reblock(const std::vector<uint8_t> &data, uint32_t block_packets)
{
    auto frames = xma_frames(data);
    if (frames.empty())
        throw AudioError("XMA stream without frames");
    const uint64_t block_bits = uint64_t(block_packets) * PAYLOAD_BITS;
    std::vector<uint64_t> starts;
    std::map<uint64_t, uint32_t> per_packet;
    uint64_t pos = 0;
    for (const auto &[bit, length] : frames)
    {
        if (length > block_bits)
            throw AudioError("XMA frame of " + std::to_string(length) + " bits, larger than a block");
        if (per_packet[pos / PAYLOAD_BITS] >= XMA_MAX_PACKET_FRAMES)
            pos += PAYLOAD_BITS - pos % PAYLOAD_BITS;
        if (pos % block_bits + length > block_bits)
            pos += block_bits - pos % block_bits;
        starts.push_back(pos);
        ++per_packet[pos / PAYLOAD_BITS];
        pos += length;
    }
    uint64_t packets = (pos + PAYLOAD_BITS - 1) / PAYLOAD_BITS;
    std::vector<uint8_t> source = unpack_bits(data);
    std::vector<uint8_t> bits(packets * PAYLOAD_BITS, 1);
    for (size_t i = 0; i < frames.size(); ++i)
        copy_frame(source, frames[i].first, frames[i].second, bits.data() + starts[i]);
    std::map<uint64_t, uint32_t> first;
    for (size_t i = 0; i < starts.size(); ++i)
    {
        bool last_in_packet = i + 1 == starts.size() || starts[i + 1] / PAYLOAD_BITS != starts[i] / PAYLOAD_BITS;
        bits[starts[i] + frames[i].second - 1] = last_in_packet ? 0 : 1;
        first.emplace(starts[i] / PAYLOAD_BITS, static_cast<uint32_t>(starts[i] % PAYLOAD_BITS));
    }
    std::vector<uint8_t> out;
    for (uint64_t k = 0; k < packets; ++k)
    {
        // frame count, offset of the first frame starting in the packet (0x7FFF: none), metadata 1
        auto pp = per_packet.find(k);
        auto ff = first.find(k);
        uint32_t count = pp == per_packet.end() ? 0 : pp->second;
        uint32_t offset = ff == first.end() ? 0x7FFF : ff->second;
        put_be32(out, (count << 26) | (offset << 11) | (1u << 8));
        pack_bits(bits.data() + k * PAYLOAD_BITS, PAYLOAD_BITS, out);
    }
    std::vector<uint32_t> block_frames((packets + block_packets - 1) / block_packets);
    for (uint64_t s : starts)
        ++block_frames[s / block_bits];
    return {std::move(out), std::move(block_frames)};
}

std::vector<uint8_t> xma1_repack(const std::vector<uint8_t> &data)
{
    // XMA1 packets holding the frames back to back (XMA1 streams run their frames on from packet to
    // packet; the last bit of each frame follows the new packets)
    auto frames = xma_frames(data);
    if (frames.empty())
        return xma2_to_xma1(data);
    uint64_t total = 0;
    for (const auto &f : frames)
        total += f.second;
    uint64_t packets = (total + PAYLOAD_BITS - 1) / PAYLOAD_BITS;
    std::vector<uint8_t> source = unpack_bits(data);
    std::vector<uint8_t> bits(packets * PAYLOAD_BITS, 1);
    std::vector<uint64_t> starts;
    uint64_t pos = 0;
    for (const auto &[bit, length] : frames)
    {
        copy_frame(source, bit, length, bits.data() + pos);
        starts.push_back(pos);
        pos += length;
    }
    for (size_t i = 0; i < starts.size(); ++i)
    {
        bool last_in_packet = i + 1 == starts.size() || starts[i + 1] / PAYLOAD_BITS != starts[i] / PAYLOAD_BITS;
        bits[starts[i] + frames[i].second - 1] = last_in_packet ? 0 : 1;
    }
    std::map<uint64_t, uint32_t> first;
    for (uint64_t s : starts)
        first.emplace(s / PAYLOAD_BITS, static_cast<uint32_t>(s % PAYLOAD_BITS));
    std::vector<uint8_t> out;
    for (uint64_t k = 0; k < packets; ++k)
    {
        auto ff = first.find(k);
        uint32_t offset = ff == first.end() ? 0x7FFF : ff->second;
        put_be32(out, uint32_t((k & 0xF) << 28) | (0x2u << 26) | (offset << 11));
        pack_bits(bits.data() + k * PAYLOAD_BITS, PAYLOAD_BITS, out);
    }
    return out;
}

XmaStream read_xma2_wav(const std::vector<uint8_t> &data)
{
    Chunks chunks = riff_chunks(data);
    const std::vector<uint8_t> *fmt = nullptr, *xma2 = nullptr, *body = nullptr;
    for (const auto &[cid, c] : chunks)
    {
        if (cid == "fmt ")
            fmt = &c;
        else if (cid == "XMA2")
            xma2 = &c;
        else if (cid == "data")
            body = &c;
    }
    if (!body)
        throw AudioError("XMA file without data chunk");
    XmaStream s;
    s.data = *body;
    if (fmt && fmt->size() >= 2 && le16(fmt->data()) == WAVE_FORMAT_XMA2)
    {
        if (fmt->size() < 28)
            throw AudioError("XMA2 format chunk too short");
        s.channels = le16(fmt->data() + 2);
        s.rate = le32(fmt->data() + 4);
        s.samples = le32(fmt->data() + 24);
        if (le16(fmt->data() + 18) != 1)
            throw AudioError("multi stream XMA is not supported, encode mono or stereo sounds");
        uint32_t play_length = fmt->size() >= 40 ? le32(fmt->data() + 36) : 0;
        s.valid_samples = play_length ? play_length : s.samples;
        return s;
    }
    if (xma2)
    {
        std::vector<uint8_t> x = *xma2;
        x.resize(x.size() + 64);
        s.samples = be32(x.data() + 16);
        s.rate = be32(x.data() + 20);
        s.channels = uint32_t(x[38]) << 8 | x[39];
        s.valid_samples = s.samples;
        return s;
    }
    throw AudioError("not an XMA2 file");
}

XmaStream read_sdns(const std::vector<uint8_t> &data)
{
    if (data.size() < 0x18 || std::memcmp(data.data(), "SDNS", 4) != 0)
        throw AudioError("not an SDNS stream");
    XmaStream s;
    s.rate = be32(data.data() + 8);
    s.channels = be32(data.data() + 12);
    s.samples = be32(data.data() + 16);
    uint32_t size = be32(data.data() + 20);
    size_t start = std::min<size_t>(SDNS_HEADER_SIZE, data.size()), end = std::min<size_t>(data.size(), size_t(SDNS_HEADER_SIZE) + size);
    s.data.assign(data.begin() + start, data.begin() + std::max(start, end));
    return s;
}

std::vector<uint8_t> write_sdns(const XmaStream &stream)
{
    auto [data, block_frames] = xma2_reblock(stream.data);
    uint64_t frames = 0;
    for (uint32_t f : block_frames)
        frames += f;
    std::vector<uint32_t> table{0, 0};
    uint32_t total = 0;
    for (size_t i = 0; i < block_frames.size() && i < SDNS_TABLE_ENTRIES - 2; ++i)
    {
        total += block_frames[i] * XMA_FRAME_SAMPLES;
        table.push_back(total);
    }
    uint64_t wanted = stream.valid_samples ? stream.valid_samples : stream.samples;
    uint32_t samples = static_cast<uint32_t>(std::min<uint64_t>(wanted, frames * XMA_FRAME_SAMPLES));
    if (!samples)
        samples = stream.samples;
    std::vector<uint8_t> out;
    append(out, "SDNS");
    for (uint32_t v : {0u, stream.rate, stream.channels, samples, static_cast<uint32_t>(data.size())})
        put_be32(out, v);
    for (uint32_t v : table)
        put_be32(out, v);
    out.resize(SDNS_HEADER_SIZE, 0);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

bool sdns_has_game_layout(const std::vector<uint8_t> &data)
{
    if (data.size() < SDNS_HEADER_SIZE || std::memcmp(data.data(), "SDNS", 4) != 0)
        return false;
    uint32_t size = be32(data.data() + 0x14);
    size_t packets = std::min<size_t>(size, data.size() - SDNS_HEADER_SIZE) / XMA_PACKET_SIZE;
    if (!packets)
        return false;
    auto table = [&](size_t i) { return be32(data.data() + SDNS_TABLE + 4 * i); };
    size_t blocks = (packets + STREAM_BLOCK_PACKETS - 1) / STREAM_BLOCK_PACKETS;
    uint64_t total = 0;
    for (size_t k = 0; k < packets; ++k)
    {
        uint32_t header = be32(data.data() + SDNS_HEADER_SIZE + k * XMA_PACKET_SIZE);
        if (k % STREAM_BLOCK_PACKETS == 0 && ((header >> 11) & 0x7FFF))
            return false; // a block that does not start with a frame
        total += uint64_t(header >> 26) * XMA_FRAME_SAMPLES;
        size_t block = k / STREAM_BLOCK_PACKETS;
        if ((k % STREAM_BLOCK_PACKETS == STREAM_BLOCK_PACKETS - 1 || k == packets - 1) && block < SDNS_TABLE_ENTRIES - 2)
            if (table(2 + block) != total)
                return false;
    }
    if (blocks >= SDNS_TABLE_ENTRIES - 2)
        return true;
    for (size_t i = 2 + blocks; i < SDNS_TABLE_ENTRIES; ++i)
        if (table(i))
            return false;
    return true;
}

std::optional<std::vector<uint8_t>> upgrade_sdns(const std::vector<uint8_t> &data)
{
    if (sdns_has_game_layout(data))
        return std::nullopt;
    XmaStream stream = read_sdns(data);
    stream.valid_samples = stream.samples;
    std::vector<uint8_t> nw = write_sdns(stream);
    std::copy(data.begin() + SDNS_TABLE, data.begin() + SDNS_TABLE + 8, nw.begin() + SDNS_TABLE); // a loop start stays
    if (nw.size() <= data.size() && std::equal(nw.begin(), nw.end(), data.begin()))
        return std::nullopt;
    return nw;
}

UpgradeStats upgrade_stream_files(const fs::path &folder, const std::function<void(const std::string &)> &log)
{
    std::vector<fs::path> paths;
    std::error_code ec;
    for (const auto &e : fs::recursive_directory_iterator(folder, ec))
    {
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (e.is_regular_file() && ext == L".xma")
            paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end());
    UpgradeStats stats;
    stats.files = static_cast<int>(paths.size());
    for (const fs::path &path : paths)
    {
        try
        {
            if (auto nw = upgrade_sdns(read_file(path)))
            {
                write_file(path, *nw);
                ++stats.upgraded;
            }
        }
        catch (const std::exception &e)
        {
            if (log)
                log("warning: " + path.string() + ": " + e.what());
            ++stats.failed;
        }
    }
    return stats;
}

// -- the encoder

XmaEncoder::XmaEncoder(fs::path path, int quality, fs::path cache_dir) : path_(std::move(path)), quality_(quality), cache_dir_(std::move(cache_dir))
{
    std::error_code ec;
    if (available())
    {
        auto size = fs::file_size(path_, ec);
        auto time = fs::last_write_time(path_, ec).time_since_epoch().count();
        identity_ = std::to_string(size) + "-" + std::to_string(time);
    }
}

bool XmaEncoder::available() const
{
    std::error_code ec;
    return !path_.empty() && fs::is_regular_file(path_, ec);
}

std::vector<uint8_t> XmaEncoder::run(const std::vector<uint8_t> &wav)
{
    TempDir tmp;
    fs::path src = tmp.path() / "in.wav", dst = tmp.path() / "out.xma";
    write_file(src, wav);
    // the documented options first, then without the quality option for a build that knows none
    std::vector<std::vector<std::wstring>> variants = {
        {path_.wstring(), src.wstring(), L"/TargetFile", dst.wstring(), L"/Quality", std::to_wstring(quality_)},
        {path_.wstring(), src.wstring(), L"/TargetFile", dst.wstring()}};
    int known = variant_.load();
    std::string failures;
    for (int index = 0; index < 2; ++index)
    {
        if (known >= 0 && index != known)
            continue;
        ProcessResult r = run_process(variants[index], 1800);
        if (r.timed_out)
            throw AudioError(path_.string() + " did not finish in 1800 s");
        if (r.exit_code == 0 && fs::exists(dst))
        {
            int expected = -1;
            variant_.compare_exchange_strong(expected, index);
            return read_file(dst);
        }
        std::string text = std::string(r.out.begin(), r.out.end()) + r.err;
        text = strip(text);
        if (text.size() > 600)
            text = text.substr(text.size() - 600);
        failures += "\n  " + std::string(index == 0 ? "in.wav /TargetFile out.xma /Quality " + std::to_string(quality_) : "in.wav /TargetFile out.xma") +
                    ": " + text;
    }
    throw AudioError("xma2encode failed:" + failures);
}

XmaStream XmaEncoder::encode(Pcm pcm)
{
    if (!available())
        throw AudioError("xma2encode.exe not found (run python -m t4ff setup, or use --xma-encoder)");
    if (pcm.channels > 2)
        pcm = downmix_mono(pcm);
    std::vector<uint8_t> wav = write_wav(pcm);
    std::vector<uint8_t> out;
    fs::path cached;
    if (!cache_dir_.empty())
    {
        Sha256 h;
        h.update(wav.data(), wav.size());
        h.update("|q" + std::to_string(quality_) + "|" + identity_);
        cached = cache_dir_ / (h.hex() + ".xma");
        std::error_code ec;
        if (fs::is_regular_file(cached, ec))
        {
            try
            {
                out = read_file(cached);
                ++hits_;
            }
            catch (const std::exception &)
            {
                out.clear();
            }
        }
    }
    if (out.empty())
    {
        out = run(wav);
        if (!cached.empty())
        {
            std::error_code ec;
            fs::create_directories(cache_dir_, ec);
            fs::path tmp = cached;
            tmp += L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
            try
            {
                write_file(tmp, out);
                fs::rename(tmp, cached, ec);
                if (ec)
                    fs::remove(tmp, ec);
            }
            catch (const std::exception &)
            {
            }
        }
    }
    XmaStream stream = read_xma2_wav(out);
    // the SDNS sample count is the number of XMA frames in the packets times 512
    stream.valid_samples = static_cast<uint32_t>(pcm.frames());
    stream.samples = xma_frame_count(stream.data) * XMA_FRAME_SAMPLES;
    return stream;
}

fs::path default_sound_cache_dir()
{
    if (const wchar_t *v = _wgetenv(L"LOCALAPPDATA"))
        return fs::path(v) / "t4ff" / "sound_cache";
    return fs::temp_directory_path() / "t4ff" / "sound_cache";
}

fs::path find_xma2encode()
{
    const std::wstring name = L"xma2encode.exe";
    fs::path on_path = find_on_path(L"xma2encode");
    if (!on_path.empty())
        return on_path;
    std::vector<fs::path> places;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path dir = fs::path(exe).parent_path();
    places.push_back(dir / name);
    places.push_back(dir / "bin" / name);
    for (const wchar_t *var : {L"XMA2ENCODE", L"XEDK", L"DurangoXDK", L"GXDKLatest", L"GameDKLatest", L"GameDKXboxLatest", L"GameDK"})
        if (const wchar_t *v = _wgetenv(var); v && *v)
            places.push_back(v);
    std::vector<fs::path> roots;
    for (const wchar_t *var : {L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432"})
        if (const wchar_t *v = _wgetenv(var); v && *v)
            roots.push_back(v);
    roots.push_back(L"C:\\Program Files (x86)");
    roots.push_back(L"C:\\Program Files");
    for (const fs::path &root : roots)
        for (const wchar_t *kit : {L"Microsoft Xbox 360 SDK", L"Microsoft Durango XDK", L"Microsoft GDK"})
            places.push_back(root / kit);
    if (const wchar_t *home = _wgetenv(L"USERPROFILE"))
    {
        places.push_back(fs::path(home) / "Downloads");
        places.push_back(fs::path(home) / "Desktop");
    }
    std::error_code ec;
    for (const fs::path &place : places)
    {
        if (fs::is_regular_file(place, ec))
        {
            std::wstring n = place.filename().wstring();
            std::transform(n.begin(), n.end(), n.begin(), ::towlower);
            if (n == name)
                return place;
            continue;
        }
        if (!fs::is_directory(place, ec))
            continue;
        for (const wchar_t *sub : {L"bin\\win32", L"bin\\x64", L"bin\\x86", L"bin", L""})
        {
            fs::path p = place / sub / name;
            if (fs::is_regular_file(p, ec))
                return p;
        }
        // at most 5 folders deep
        for (auto it = fs::recursive_directory_iterator(place, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (it.depth() >= 5)
                it.disable_recursion_pending();
            std::wstring n = it->path().filename().wstring();
            std::transform(n.begin(), n.end(), n.begin(), ::towlower);
            if (n == name && it->is_regular_file(ec))
                return it->path();
        }
    }
    return {};
}

// -- loaded sounds

uint32_t xma1_rate(int rate)
{
    for (uint32_t r : {24000u, 32000u, 44100u, 48000u})
        if (rate <= r * 1.001)
            return r;
    return 48000;
}

LoadedXma loaded_sound(const XmaStream &stream, uint32_t duration_ms)
{
    LoadedXma out;
    out.data = xma1_repack(stream.data);
    auto frames = xma_frames(out.data);
    if (frames.empty())
        throw AudioError("XMA stream without frames");
    out.seek_table = seek_table(out.data, frames);
    uint64_t valid = std::max<uint64_t>(1, stream.valid_samples ? stream.valid_samples : stream.samples);
    uint64_t last_sample = std::min<uint64_t>(valid + LOOP_SUBFRAME_SKIP * XMA_SUBFRAME_SAMPLES, frames.size() * XMA_FRAME_SAMPLES) - 1;
    uint64_t last = last_sample / XMA_FRAME_SAMPLES;
    uint32_t subframe_end = static_cast<uint32_t>((last_sample % XMA_FRAME_SAMPLES) / XMA_SUBFRAME_SAMPLES);
    out.format[0] = static_cast<uint32_t>(frames[0].first);    // loop start: bit offset of the first frame
    out.format[1] = static_cast<uint32_t>(frames[last].first); // loop end: of the frame holding the last sample
    out.format[2] = (subframe_end << 24) | (LOOP_SUBFRAME_SKIP << 16);
    // XAUDIOSOURCEFORMAT: sample type, stream count, then per stream sample rate and channel count
    out.format[19] = XAUDIO_SAMPLE_TYPE_XMA << 24;
    out.format[20] = 1u << 24;
    out.format[21] = stream.rate;
    out.format[22] = stream.channels << 24;
    out.format[33] = duration_ms;
    out.format[34] = static_cast<uint32_t>(out.seek_table.size() + 2); // size of the seek table in dwords
    out.format[35] = 0xFFFFFFFF;
    out.rate = stream.rate;
    out.channels = stream.channels;
    out.stream = std::make_shared<XmaStream>(stream);
    return out;
}

LoadedXma encode_loaded_sound(const std::vector<uint8_t> &wav, XmaEncoder &encoder, int max_rate, bool mono)
{
    Pcm pcm = read_wav(wav);
    uint32_t duration_ms = pcm.rate ? static_cast<uint32_t>(static_cast<uint64_t>(pcm.frames()) * 1000 / pcm.rate) : 0;
    if (mono && pcm.channels > 1)
        pcm = downmix_mono(pcm);
    uint32_t target = xma1_rate(max_rate ? std::min(pcm.rate, max_rate) : pcm.rate);
    if (static_cast<uint32_t>(pcm.rate) != target)
        pcm = resample(pcm, static_cast<int>(target));
    return loaded_sound(encoder.encode(std::move(pcm)), duration_ms);
}

// -- streamed sounds

std::string streamed_sound_target_path(const std::string &rel)
{
    std::string s = rel;
    std::replace(s.begin(), s.end(), '\\', '/');
    size_t slash = s.find('/');
    std::string first = s.substr(0, slash);
    std::transform(first.begin(), first.end(), first.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    if (first == "sound")
        s = slash == std::string::npos ? "" : s.substr(slash + 1);
    // os.path.splitext's root
    size_t sep = s.find_last_of('/');
    size_t base = sep == std::string::npos ? 0 : sep + 1;
    size_t first_char = s.find_first_not_of('.', base);
    size_t dot = s.rfind('.');
    if (first_char != std::string::npos && dot != std::string::npos && dot > first_char)
        s = s.substr(0, dot);
    return "sounds/" + s + ".xma";
}

StreamStats convert_streamed_sounds(const IwdLibrary &library, const fs::path &out_dir, XmaEncoder &encoder, int max_rate, bool mono,
                                    const std::function<void(const std::string &)> &log, int jobs, const std::vector<std::string> *names_in,
                                    bool skip_existing)
{
    auto target_of = [&](const std::string &name) {
        fs::path p = out_dir;
        std::string t = streamed_sound_target_path(name);
        size_t start = 0;
        while (true)
        {
            size_t end = t.find('/', start);
            p /= t.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (end == std::string::npos)
                return p;
            start = end + 1;
        }
    };
    std::vector<std::string> names;
    if (names_in)
        names = *names_in;
    else
        for (const std::string &n : library.names("sound/"))
            for (const char *ext : {".wav", ".mp3", ".ogg", ".flac"})
                if (n.size() >= std::strlen(ext) && n.compare(n.size() - std::strlen(ext), std::strlen(ext), ext) == 0)
                    names.push_back(n);
    StreamStats stats;
    stats.sounds = static_cast<int>(names.size());
    if (skip_existing)
    {
        std::vector<std::string> todo;
        for (const std::string &n : names)
            if (!fs::exists(target_of(n)))
                todo.push_back(n);
        stats.kept = static_cast<int>(names.size() - todo.size());
        names = std::move(todo);
    }
    if (names.empty())
        return stats;
    if (!encoder.available())
    {
        if (log)
            log("warning: " + std::to_string(names.size()) + " streamed sounds need xma2encode.exe (Xbox 360 XDK), skipped. Use --xma-encoder.");
        stats.failed = static_cast<int>(names.size());
        return stats;
    }
    struct Result
    {
        uint64_t in = 0, out = 0;
        std::string error;
    };
    std::vector<Result> results(names.size());
    parallel_for(names.size(), jobs, [&](size_t i) {
        const std::string &name = names[i];
        auto data = library.read(name);
        if (!data)
        {
            results[i].error = "not found";
            return;
        }
        results[i].in = data->size();
        try
        {
            bool wav = name.size() >= 4 && name.compare(name.size() - 4, 4, ".wav") == 0;
            Pcm pcm = wav ? read_wav(*data) : decode_with_ffmpeg(*data);
            if (mono)
                pcm = downmix_mono(pcm);
            if (max_rate && pcm.rate > max_rate)
                pcm = resample(pcm, max_rate);
            std::vector<uint8_t> out = write_sdns(encoder.encode(std::move(pcm)));
            fs::path target = target_of(name);
            fs::create_directories(target.parent_path());
            write_file(target, out);
            results[i].out = out.size();
        }
        catch (const AudioError &e)
        {
            results[i].error = e.what();
        }
    });
    for (size_t i = 0; i < names.size(); ++i)
    {
        stats.input_bytes += results[i].in;
        if (!results[i].error.empty())
        {
            if (log)
                log("warning: " + names[i] + ": " + results[i].error);
            ++stats.failed;
        }
        else
        {
            ++stats.converted;
            stats.output_bytes += results[i].out;
        }
    }
    return stats;
}
} // namespace t4ff
