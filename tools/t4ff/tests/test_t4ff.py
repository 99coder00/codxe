"""Tests for t4ff.

Sample based tests use the fastfiles of a folder given by the T4FF_SAMPLES
environment variable and are skipped otherwise:

    $T4FF_SAMPLES/pc/nazi_zombie_aztec_load.ff, nazi_zombie_aztec_patch.ff, nazi_zombie_aztec.iwd
    $T4FF_SAMPLES/x360/patch.ff, patch_ui.ff, sounds/para_egg.xma

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

from t4ff import audio, images, xenos  # noqa: E402
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


if __name__ == "__main__":
    unittest.main()
