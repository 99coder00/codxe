"""Tests for t4ff.

Sample based tests use the fastfiles of a folder given by the T4FF_SAMPLES
environment variable and are skipped otherwise:

    $T4FF_SAMPLES/pc/nazi_zombie_aztec.ff, mod.ff, nazi_zombie_aztec_load.ff, nazi_zombie_aztec_patch.ff,
                    nazi_zombie_aztec.iwd
    $T4FF_SAMPLES/x360/patch.ff, patch_ui.ff, sounds/para_egg.xma, nazi_zombie_aztec.ff (CoD Xenon's)

Run with ``python -m unittest discover -s tests`` from tools/t4ff.
"""

import os
import stat
import sys
import tempfile
import textwrap
import unittest
import zipfile

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from t4ff import audio, images, xanim, xenos  # noqa: E402
from t4ff.commands import parse_expr  # noqa: E402

SAMPLES = os.environ.get("T4FF_SAMPLES", "")


def sample(*parts):
    path = os.path.join(SAMPLES, *parts)
    if not SAMPLES or not os.path.exists(path):
        raise unittest.SkipTest(f"sample {os.path.join(*parts)} not available (set T4FF_SAMPLES)")
    return path


class ExpressionTests(unittest.TestCase):
    def test_precedence(self):
        self.assertEqual(parse_expr("(3 * 4 + 31) / 32 * 4", {}).eval(None), 4)
        self.assertEqual(parse_expr("1 || 0 && 0", {}).eval(None), 1)
        self.assertEqual(parse_expr("2 + 3 * 4 == 14", {}).eval(None), 1)


class EncodingTests(unittest.TestCase):
    """Console encodings recovered from CoD Xenon's converted nazi_zombie_aztec (values taken from it)."""

    def test_quaternion_packing(self):
        self.assertEqual(int(xanim.pack_quats32(np.array([[-367, -589, -11824, 30551]]))[0]), 0x19D7EDFD)
        self.assertEqual(xanim.pack_quats48(np.array([[-22262, -5135, -23215, 3574]])).tolist(), [[41866, 7855, 30246]])
        self.assertEqual(xanim.pack_half_quats(np.array([[27532, 17767], [2968, -32632]])).tolist(), [21670, 48407])
        self.assertEqual(int(xanim.pack_quats32(np.zeros((1, 4)))[0]), 0)

    def test_anim_part_types(self):
        # two bones: a precise (main skeleton) and a compressed full quaternion without frames
        quats = [xanim.QuatTrack("fullns", frames=np.array([[0, 0, 9680, 31305]])), xanim.QuatTrack("fullns", frames=np.array([[0, 0, 9680, 31305]]))]
        trans = [xanim.TransTrack("none", 1), xanim.TransTrack("none", 0)]
        anim = xanim.to_console(["tag_weapon", "j_mainroot"], quats, trans, asset_type=2)
        self.assertEqual(anim.bone_counts, [0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 2, 2])
        self.assertEqual(anim.bone_order, [0, 1])
        self.assertEqual(anim.data_int.tolist(), [0x04F00000])
        self.assertEqual(len(anim.data_short), 3)
        self.assertEqual(anim.data_byte, bytes([0, 1]))  # translation tracks sorted by console bone

    def test_normal_packing(self):
        from t4ff.convert import _pack_unit_vec

        self.assertEqual(_pack_unit_vec(np.array([[254, 132, 122, 63]], dtype=np.uint8)).tobytes(), bytes.fromhex("3ec051fe"))

    def test_stream_name_hash(self):
        from t4ff.assets import console_sound_name, stream_name_hash

        self.assertEqual(stream_name_hash("sfx\\levels\\nazi_zombie_factory\\air_raid\\air_raid_loop_03"), 1667193692)
        self.assertEqual(console_sound_name("sfx/a/b.wav"), "sfx/a/b")
        self.assertEqual(console_sound_name(",b.WAV"), ",b")

    def test_technique_mapping(self):
        from t4ff.assets import PC_TECHNIQUE_TO_X360, X360_TECHNIQUE_COUNT

        self.assertEqual(PC_TECHNIQUE_TO_X360[0x23], 0x23)
        self.assertNotIn(0x24, PC_TECHNIQUE_TO_X360)  # TECHNIQUE_LIT_INSTANCED
        self.assertEqual(PC_TECHNIQUE_TO_X360[0x2B], 0x24)  # TECHNIQUE_LIGHT_SPOT
        self.assertEqual(PC_TECHNIQUE_TO_X360[0x39], 50)  # TECHNIQUE_DEBUG_BUMPMAP
        self.assertEqual(sorted(PC_TECHNIQUE_TO_X360.values()), list(range(X360_TECHNIQUE_COUNT)))

    def test_xma1_packet_headers(self):
        packet = bytearray(audio.XMA_PACKET_SIZE)
        packet[0:4] = ((5 << 26) | (123 << 11) | (1 << 8)).to_bytes(4, "big")  # XMA2: 5 frames, offset 123
        data = audio.xma2_to_xma1(bytes(packet) * 3)
        headers = [int.from_bytes(data[i : i + 4], "big") for i in range(0, len(data), audio.XMA_PACKET_SIZE)]
        self.assertEqual([h >> 28 for h in headers], [0, 1, 2])  # sequence numbers
        self.assertTrue(all((h >> 26) & 3 == 2 and (h >> 11) & 0x7FFF == 123 and h & 0x7FF == 0 for h in headers))
        self.assertEqual(audio.xma1_rate(22050), 24000)
        self.assertEqual(audio.xma1_rate(44094), 44100)


class XenosTests(unittest.TestCase):
    def test_tiling_roundtrip(self):
        rng = np.random.default_rng(1)
        for fmt_name, (w, h) in [("DXT1", (256, 64)), ("DXT5", (512, 512)), ("A8R8G8B8", (64, 32)), ("A8L8", (32, 32))]:
            fmt = xenos.FORMATS[fmt_name]
            size = images.level_size(fmt_name, w, h)
            data = rng.integers(0, 256, size, dtype=np.uint8).tobytes()
            tiled = xenos.tile_level(data, w, h, 0, fmt)
            self.assertEqual(xenos.untile_level(tiled, w, h, 0, fmt), data, fmt_name)

    def test_mip_chain_roundtrip(self):
        rng = np.random.default_rng(2)
        fmt = xenos.FORMATS["DXT1"]
        w, h = 256, 128
        levels = []
        lw, lh = w, h
        while min(lw, lh) >= 4:
            levels.append(rng.integers(0, 256, images.level_size("DXT1", lw, lh), dtype=np.uint8).tobytes())
            lw, lh = lw >> 1, lh >> 1
        tiled = xenos.tile_mip_chain(levels, w, h, fmt)
        self.assertEqual(xenos.untile_mip_chain(tiled, w, h, fmt, len(levels)), levels)


class AudioTests(unittest.TestCase):
    def test_sdns_roundtrip(self):
        stream = audio.XmaStream(44100, 1, 1024, bytes(range(256)) * 16)
        self.assertEqual(audio.read_sdns(audio.write_sdns(stream)).data, stream.data)

    def test_streamed_sound_target(self):
        self.assertEqual(audio.streamed_sound_target("sound/eggs/para_egg.wav"), "sounds/eggs/para_egg.xma")

    def test_encoder_pipeline_matches_cod_xenon(self):
        """An encoder producing CoD Xenon's XMA packets must yield CoD Xenon's SDNS file."""
        reference_path = sample("x360", "sounds", "para_egg.xma")
        with open(reference_path, "rb") as f:
            reference = f.read()
        with tempfile.TemporaryDirectory() as tmp:
            fake = os.path.join(tmp, "xma2encode")
            with open(fake, "w") as f:
                f.write(
                    textwrap.dedent(
                        f"""\
                        #!{sys.executable}
                        import sys
                        sys.path.insert(0, {os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')!r})
                        from t4ff import audio
                        stream = audio.read_sdns(open({reference_path!r}, "rb").read())
                        out = sys.argv[sys.argv.index("/TargetFile") + 1]
                        open(out, "wb").write(audio.xma2_wav(stream))
                        """
                    )
                )
            os.chmod(fake, os.stat(fake).st_mode | stat.S_IEXEC)
            encoder = audio.XmaEncoder(fake)
            pcm = audio.Pcm(44100, np.zeros((1000, 1), dtype=np.int16))
            self.assertEqual(audio.write_sdns(encoder.encode(pcm)), reference)


class SampleZoneTests(unittest.TestCase):
    def roundtrip(self, *parts):
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import for_endian
        from t4ff.zone import Reader, Writer

        endian, _, data = read_fastfile(sample(*parts))
        platform = for_endian(endian)
        zone = Reader(platform, data).load()
        self.assertEqual(Writer(platform).write(zone), data)
        return zone

    def test_pc_roundtrip(self):
        self.roundtrip("pc", "nazi_zombie_aztec_patch.ff")

    def test_console_roundtrip(self):
        self.roundtrip("x360", "patch.ff")
        self.roundtrip("x360", "patch_ui.ff")

    def test_texture_matches_cod_xenon(self):
        """The PC loadscreen converts to the exact texture CoD Xenon produced."""
        iwd = zipfile.ZipFile(sample("pc", "nazi_zombie_aztec.iwd"))
        source = images.parse_iwi("loadscreen_nazi_zombie_aztec", iwd.read("images/loadscreen_nazi_zombie_aztec.iwi"))
        tex = images.build_console_texture(source)
        zone = self.roundtrip("x360", "patch_ui.ff")
        image = next(a.node for a in zone.assets if a.type == "image" and a.name == "loadscreen_nazi_zombie_aztec")
        pixels = next(c for c in image.children if c.extra.get("delayed"))
        load_def = next(c for c in image.children if c.type.name == "GfxImageLoadDef")
        self.assertEqual(tex.pixels, bytes(pixels.data))
        self.assertEqual(tex.header, bytes(load_def.children[0].data))

    def test_convert_load_zone(self):
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, Writer

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec_load.ff"))
        zone = Reader(pc(), data).load()
        options = ConvertOptions(iwd_paths=[sample("pc", "nazi_zombie_aztec.iwd")], log=lambda msg: None)
        out = Writer(x360()).write(ZoneConverter(zone, pc(), x360(), options).convert())
        converted = Reader(x360(), out).load()
        self.assertEqual([a.type for a in converted.assets], [a.type for a in zone.assets])
        self.assertEqual(Writer(x360()).write(converted), out)

    def test_loaded_sound(self):
        """A PC loaded sound becomes an XMA1 console loaded sound (the encoder is replaced by CoD
        Xenon's XMA2 data of another sound, xma2encode is not needed)."""
        import struct

        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, Writer, asset_name

        with open(sample("x360", "sounds", "para_egg.xma"), "rb") as f:
            stream = audio.read_sdns(f.read())

        class FakeEncoder:
            available = True

            def encode(self, pcm):
                return audio.XmaStream(stream.rate, stream.channels, stream.samples, stream.data, stream.samples - 256)

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        zone = Reader(pc(), data).load()
        options = ConvertOptions(xma_encoder=FakeEncoder(), log=lambda msg: None)
        conv = ZoneConverter(zone, pc(), x360(), options)
        node = next(n for n in zone.extra_root.walk() if (n.extra.get("origin") or ("",))[0] == "asset" and n.type.name == "LoadedSound" and not asset_name(pc(), n).startswith(","))
        sound = conv.convert_asset_node("loaded_sound", node)
        self.assertFalse(asset_name(x360(), sound).endswith(".wav"))
        data_node = next(c for c in sound.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("snd_asset", "data"))
        seek = next(c for c in sound.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("snd_asset", "seekTable"))
        packets = len(data_node.data) // audio.XMA_PACKET_SIZE
        self.assertEqual(struct.unpack_from(">II", seek.data), (1, packets))
        fmt = struct.unpack_from(">36I", sound.data, 16)
        self.assertEqual(fmt[0], 32)  # loop start: the first frame
        self.assertEqual(fmt[34], packets + 2)
        self.assertEqual(fmt[21], stream.rate)
        self.assertEqual(int.from_bytes(data_node.data[:4], "big") >> 28, 0)  # XMA1 sequence numbers
        self.assertEqual(int.from_bytes(data_node.data[audio.XMA_PACKET_SIZE : audio.XMA_PACKET_SIZE + 4], "big") >> 28, 1)
        frames = audio.xma_frames(bytes(data_node.data))
        self.assertTrue(any(bit + length == fmt[1] for bit, length in frames))  # loop end on a frame end
        # a zone with the sound reads back with the console rules
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Zone, ZoneAsset
        from t4ff.layout import TypeRef

        conv.fix_pointers()
        assets = Node(TypeRef("scalar", "uint", 4, 4), 2, BLOCK_VIRTUAL)
        assets.data = bytearray(struct.pack(">I", x360().asset_type_index["loaded_sound"]) + bytes(4))
        assets.segments = [(assets.type, 2, 8, False)]
        assets.extra["align"] = 4
        ptr = Ptr("follow", sound)
        ptr.owner, ptr.offset = assets, 4
        assets.relocs[4] = ptr
        assets.children = [sound]
        root = Node(TypeRef("scalar", "uint", 4, 4), 4, -1)
        root.data = bytearray(16)
        root.children = [assets]
        out_zone = Zone(x360().name, [], [ZoneAsset("loaded_sound", ptr, asset_name(x360(), sound))], [], 0, 0, None, assets)
        out_zone.extra_root = root
        out = Writer(x360()).write(out_zone)
        self.assertEqual(Writer(x360()).write(Reader(x360(), out).load()), out)

    def test_convert_map(self):
        """The whole usermap (map + mod, merged) converts to a zone the console loader reads."""
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.merge import merge_zones, prune_references
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, Writer

        library = os.path.join(SAMPLES, "x360", "nazi_zombie_aztec.ff")
        options = ConvertOptions(
            iwd_paths=[sample("pc", "nazi_zombie_aztec.iwd")],
            console_zones=[library] if os.path.exists(library) else [],
            log=lambda msg: None,
        )
        zones = []
        for name in ("nazi_zombie_aztec.ff", "mod.ff"):
            _, _, data = read_fastfile(sample("pc", name))
            zones.append(ZoneConverter(Reader(pc(), data).load(), pc(), x360(), options).convert())
        merged = merge_zones(x360(), zones, log=lambda msg: None)
        prune_references(x360(), merged, log=lambda msg: None)
        out = Writer(x360()).write(merged)
        converted = Reader(x360(), out).load()
        self.assertEqual(len(converted.assets), len(merged.assets))
        self.assertEqual(Writer(x360()).write(converted), out)


if __name__ == "__main__":
    unittest.main()
