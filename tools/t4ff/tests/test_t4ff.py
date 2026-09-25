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


class DepsTests(unittest.TestCase):
    """Finding, installing and testing xma2encode.exe (with stand-ins for the real encoder)."""

    def test_install_from_zip_in_downloads(self):
        from unittest import mock

        from t4ff import deps

        with tempfile.TemporaryDirectory() as tmp:
            home = os.path.join(tmp, "home")
            os.makedirs(os.path.join(home, "Downloads"))
            with zipfile.ZipFile(os.path.join(home, "Downloads", "xdk tools.zip"), "w") as z:
                z.writestr("xdk/bin/win32/xma2encode.exe", b"exe")
                z.writestr("xdk/bin/win32/xmaencoder.dll", b"dll")
                z.writestr("xdk/readme.txt", b"")
            bin_dir = os.path.join(tmp, "bin")
            env = {"HOME": home, "USERPROFILE": home, "XMA2ENCODE": "", "XEDK": "", "PATH": tmp}
            with mock.patch.dict(os.environ, env), mock.patch.object(deps, "BIN_DIR", bin_dir), mock.patch.object(deps, "TOOL_DIR", tmp):
                found = deps.find_xma2encode(search_zips=True)
                self.assertTrue(found.endswith("::xdk/bin/win32/xma2encode.exe"))
                path = deps.install_xma2encode(found, log=lambda msg: None)
                self.assertEqual(path, os.path.join(bin_dir, "xma2encode.exe"))
                self.assertEqual(sorted(os.listdir(bin_dir)), ["xma2encode.exe", "xmaencoder.dll"])
                self.assertEqual(deps.find_xma2encode(), path)  # found there from now on

    def test_encoder_of_developer_kit_used_in_place(self):
        from unittest import mock

        from t4ff import deps

        with tempfile.TemporaryDirectory() as tmp:
            program_files = os.path.join(tmp, "Program Files (x86)")
            kit = os.path.join(program_files, "Microsoft Xbox 360 SDK")
            os.makedirs(os.path.join(kit, "bin", "win32"))
            exe = os.path.join(kit, "bin", "win32", "xma2encode.exe")
            for name in ("xma2encode.exe", "other.dll"):
                with open(os.path.join(kit, "bin", "win32", name), "wb") as f:
                    f.write(b"x")
            home, tool_dir, bin_dir = (os.path.join(tmp, d) for d in ("home", "tool", "bin"))
            os.makedirs(home)
            os.makedirs(tool_dir)
            env = {"HOME": home, "USERPROFILE": home, "XMA2ENCODE": "", "XEDK": "", "PATH": tool_dir, "ProgramFiles(x86)": program_files, "ProgramFiles": "", "ProgramW6432": ""}
            with mock.patch.dict(os.environ, env), mock.patch.object(deps, "BIN_DIR", bin_dir), mock.patch.object(deps, "TOOL_DIR", tool_dir), mock.patch.object(sys, "platform", "linux"):
                self.assertEqual(deps.find_xma2encode(), exe)
                self.assertEqual(deps.ensure_xma2encode(log=lambda msg: None), exe)
                self.assertFalse(os.path.exists(bin_dir))  # nothing copied
                # the XEDK variable the kit's installer sets
                with mock.patch.dict(os.environ, {"ProgramFiles(x86)": "", "XEDK": kit + os.sep}):
                    self.assertEqual(deps.find_xma2encode(), exe)

    @unittest.skipIf(sys.platform.startswith("win"), "uses a stand-in for wine")
    def test_encoder_self_test(self):
        from unittest import mock

        from t4ff import deps

        tool_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        with tempfile.TemporaryDirectory() as tmp:

            def stand_in(name, rejects):
                # a stand-in encoder (a Python script) writing a one packet XMA2 file, run by a
                # stand-in wine; it fails with a usage text when it sees one of ``rejects``
                exe = os.path.join(tmp, name)
                with open(exe, "w") as f:
                    f.write(textwrap.dedent(f"""
                        import sys
                        if any(arg in sys.argv for arg in {rejects!r}):
                            print("usage: xma2encode <input.wav> /TargetFile <output.xma>")
                            sys.exit(1)
                        sys.path.insert(0, {tool_dir!r})
                        from t4ff import audio
                        pcm = audio.read_wav(open(sys.argv[1], "rb").read())
                        packet = bytearray(audio.XMA_PACKET_SIZE)
                        packet[0:4] = ((1 << 26) | (1 << 8)).to_bytes(4, "big")
                        stream = audio.XmaStream(pcm.rate, pcm.channels, 512, bytes(packet))
                        open(sys.argv[sys.argv.index("/TargetFile") + 1], "wb").write(audio.xma2_wav(stream))
                    """))
                return exe

            wine = os.path.join(tmp, "wine")
            with open(wine, "w") as f:
                f.write(f'#!/bin/sh\nexec "{sys.executable}" "$@"\n')
            os.chmod(wine, os.stat(wine).st_mode | stat.S_IEXEC)
            logged = []
            with mock.patch.dict(os.environ, {"PATH": tmp + os.pathsep + os.environ.get("PATH", "")}):
                self.assertTrue(deps.test_xma2encode(stand_in("xma2encode.exe", []), log=logged.append))
                # an encoder without the quality option still encodes
                self.assertTrue(deps.test_xma2encode(stand_in("noquality.exe", ["/Quality"]), log=logged.append))
                self.assertFalse(deps.test_xma2encode(os.path.join(tmp, "missing.exe"), log=logged.append))
                # a failing encoder: what it prints is shown, for the report
                del logged[:]
                self.assertFalse(deps.test_xma2encode(stand_in("broken.exe", ["/TargetFile", "/?"]), log=logged.append))
                self.assertTrue(any("usage: xma2encode" in line for line in logged))


class ProgressTests(unittest.TestCase):
    def test_progress_lines(self):
        import contextlib
        import io

        from t4ff import progress

        out = io.StringIO()
        try:
            progress.use_lines(True)
            with contextlib.redirect_stdout(out):
                for i in range(1001):
                    progress.step("Converting mod.ff (file 3 of 3)", i, 1000)
                progress.step("Writing nazi_zombie_aztec.ff")
        finally:
            progress.use_lines(False)
        reports = [progress.parse(line) for line in out.getvalue().splitlines()]
        self.assertEqual(reports[0], (0, 1000, "Converting mod.ff (file 3 of 3)"))
        self.assertEqual(reports[-2:], [(1000, 1000, "Converting mod.ff (file 3 of 3)"), (0, 0, "Writing nazi_zombie_aztec.ff")])
        self.assertLess(len(reports), 20)  # throttled
        self.assertIsNone(progress.parse("warning: @progress 1 2 x"))

    def test_progress_in_a_terminal(self):
        import contextlib
        import io

        from t4ff import progress

        out = io.StringIO()
        progress.use_lines(False)
        with contextlib.redirect_stdout(out):
            for i in range(9):
                progress.step("Encoding streamed sounds", i, 8)
        self.assertEqual(out.getvalue().splitlines(), [f"  Encoding streamed sounds: {p}% ({n}/8)" for p, n in ((25, 2), (50, 4), (75, 6))])


class UsermapTests(unittest.TestCase):
    def test_find_usermap(self):
        """The folder or any fastfile of the map finds the map, its _patch and _load, and mod.ff
        in the same folder or in mods/<map> next to usermaps/<map>."""
        from t4ff.__main__ import find_usermap

        with tempfile.TemporaryDirectory() as tmp:
            maps = os.path.join(tmp, "usermaps", "nazi_zombie_aztec")
            mods = os.path.join(tmp, "mods", "nazi_zombie_aztec")
            os.makedirs(maps)
            os.makedirs(mods)
            for folder, names in ((maps, ["nazi_zombie_aztec.ff", "nazi_zombie_aztec_patch.ff", "nazi_zombie_aztec_load.ff", "nazi_zombie_aztec.iwd"]), (mods, ["mod.ff", "mod.iwd"])):
                for n in names:
                    open(os.path.join(folder, n), "wb").close()
            expected = {
                "map": os.path.join(maps, "nazi_zombie_aztec.ff"),
                "patch": os.path.join(maps, "nazi_zombie_aztec_patch.ff"),
                "load": os.path.join(maps, "nazi_zombie_aztec_load.ff"),
                "mod": os.path.join(mods, "mod.ff"),
            }
            for choice in (maps, expected["map"], expected["patch"]):
                name, files, iwds = find_usermap(choice)
                self.assertEqual(name, "nazi_zombie_aztec")
                self.assertEqual(files, expected)
                self.assertEqual(iwds, [os.path.join(maps, "nazi_zombie_aztec.iwd"), os.path.join(mods, "mod.iwd")])
            # everything in one folder
            os.rename(os.path.join(mods, "mod.ff"), os.path.join(maps, "mod.ff"))
            self.assertEqual(find_usermap(os.path.join(maps, "mod.ff"))[1]["mod"], os.path.join(maps, "mod.ff"))


class GuiTests(unittest.TestCase):
    def test_cli_runs_in_a_separate_process(self):
        """The window runs the converter as a child process (it keeps the window responsive)."""
        import subprocess

        from t4ff import gui

        command = gui.cli_command(["convert", "--help"])
        self.assertEqual(command[1:5], ["-u", "-m", "t4ff", "--progress-lines"])
        with tempfile.TemporaryDirectory() as tmp:
            options = gui.process_options()
            options["cwd"] = tmp  # found through PYTHONPATH wherever it runs
            with subprocess.Popen(command, **options) as process:
                output = process.stdout.read()
            self.assertEqual(process.returncode, 0, output)
        self.assertIn("usage: t4ff convert", output)

    """The window's settings and the command line it runs (no display needed)."""

    def test_convert_args(self):
        from t4ff import gui

        s = gui.Settings(input="in", output="out", console_zones=["a.ff", "b.ff"], texture_budget=64, sound_rate=32000, mono_sounds=True)
        self.assertEqual(
            gui.convert_args(s),
            ["convert", "in", "-o", "out", "--console-zone", "a.ff", "--console-zone", "b.ff", "--texture-budget", "64", "--sound-rate", "32000", "--mono-sounds"],
        )
        # the command line parses
        from t4ff.__main__ import main

        with self.assertRaises(SystemExit):  # missing usermap folder
            with open(os.devnull, "w") as devnull, __import__("contextlib").redirect_stdout(devnull):
                main(gui.convert_args(s))

    def test_settings_roundtrip(self):
        from t4ff import gui

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "t4ff", "gui.json")
            s = gui.Settings(input="x", iwds=["y"], no_mips=True)
            s.save(path)
            self.assertEqual(gui.Settings.load(path), s)
            self.assertEqual(gui.Settings.load(os.path.join(tmp, "missing.json")), gui.Settings())
            self.assertTrue(gui.check_settings(gui.Settings()))


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


def xma2_packets(lengths, block_packets=2, seed=1):
    """XMA2 packets holding frames of the given bit lengths laid out as xma2encode does: frames do
    not cross block boundaries (the rest of a block is padded with ones), the last bit of a frame is
    0 when no other frame starts in its packet, packet headers give the frame count and the offset
    of the first frame starting in the packet."""
    rng = np.random.default_rng(seed)
    payload_bits = (audio.XMA_PACKET_SIZE - 4) * 8
    block_bits = block_packets * payload_bits
    starts, pos = [], 0
    for length in lengths:
        if pos % block_bits + length > block_bits:
            pos += block_bits - pos % block_bits
        starts.append(pos)
        pos += length
    total = -(-pos // block_bits) * block_bits
    bits = np.ones(total, dtype=np.uint8)
    for start, length in zip(starts, lengths):
        bits[start : start + length] = rng.integers(0, 2, length)
        bits[start : start + 15] = [(length >> (14 - i)) & 1 for i in range(15)]
    packet_of = [start // payload_bits for start in starts]
    for i, (start, length) in enumerate(zip(starts, lengths)):
        bits[start + length - 1] = 0 if i + 1 == len(starts) or packet_of[i + 1] != packet_of[i] else 1
    data = bytearray()
    for k in range(total // payload_bits):
        mine = [start for start, packet in zip(starts, packet_of) if packet == k]
        offset = mine[0] - k * payload_bits if mine else 0x7FFF
        data += ((len(mine) << 26) | (offset << 11)).to_bytes(4, "big")
        data += np.packbits(bits[k * payload_bits : (k + 1) * payload_bits]).tobytes()
    return bytes(data), [((start // payload_bits) * audio.XMA_PACKET_SIZE * 8 + 32 + start % payload_bits, length) for start, length in zip(starts, lengths)]


class AudioTests(unittest.TestCase):
    def test_xma_frames_across_blocks(self):
        """Frames are found past the padding at the end of every block and across packets."""
        lengths = [3000 + 97 * i % 2000 for i in range(40)]
        lengths[7] = 30000  # fills most of a block: a packet without a frame start
        data, expected = xma2_packets(lengths)
        self.assertGreater(len(data) // audio.XMA_PACKET_SIZE, 8)
        self.assertEqual(audio.xma_frames(data), expected)
        self.assertEqual(audio.xma_frames(audio.xma2_to_xma1(data)), expected)
        self.assertEqual(audio.xma_frame_count(data), len(lengths))

    def test_loaded_sound_loop_region(self):
        """The loop region is laid out as in CoD Xenon's loaded sounds: from 3 subframes into the
        first frame to the subframe of decoded sample length + 383."""
        lengths = [3000 + 97 * i % 2000 for i in range(40)]
        data, expected = xma2_packets(lengths)
        for valid, frame, subframe in ((40 * 512 - 384, 39, 3), (40 * 512, 39, 3), (1000, 2, 2), (20 * 512 - 380, 20, 0)):
            stream = audio.XmaStream(48000, 1, 40 * 512, data, valid)
            sound = audio.loaded_sound(stream, 1234)
            self.assertEqual(sound.format[0], expected[0][0])
            self.assertEqual(sound.format[1], expected[frame][0])
            self.assertEqual(sound.format[2], (subframe << 24) | (3 << 16))
            self.assertEqual((sound.format[33], sound.format[34]), (1234, len(sound.seek_table) + 2))
        # decoded samples before every packet (frames starting in the packets before it)
        packet_starts = [bit // (audio.XMA_PACKET_SIZE * 8) for bit, _ in expected]
        self.assertEqual(sound.seek_table, [512 * sum(p < k for p in packet_starts) for k in range(len(data) // audio.XMA_PACKET_SIZE)])

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

    def test_long_loaded_sound_decodes(self):
        """xma2encode data longer than one 64 KiB block (CoD Xenon's para_egg stream) makes a whole
        XMA1 loaded sound, which decodes to the PC sound."""
        if audio.ffmpeg_exe() is None:
            self.skipTest("FFmpeg not available")
        with open(sample("x360", "sounds", "para_egg.xma"), "rb") as f:
            stream = audio.read_sdns(f.read())
        source = audio.read_wav(zipfile.ZipFile(sample("pc", "nazi_zombie_aztec.iwd")).read("sound/eggs/para_egg.wav"))
        stream.valid_samples = source.frames
        sound = audio.loaded_sound(stream, source.frames * 1000 // source.rate)
        frames = audio.xma_frames(sound.data)
        self.assertEqual(len(frames), audio.xma_frame_count(stream.data))  # 11130 frames in 25 blocks
        self.assertEqual(sound.format[1], frames[-1][0])
        self.assertEqual(sound.seek_table[-1] + 512 * sum(1 for bit, _ in frames if bit // (audio.XMA_PACKET_SIZE * 8) == len(sound.seek_table) - 1), len(frames) * 512)
        decoded = audio.decode_loaded_sound(sound)
        self.assertEqual(decoded.frames, source.frames)
        tail = slice(source.frames - 200000, source.frames - 1000)
        a = source.samples[tail, 0].astype(np.float64)
        b = decoded.samples[tail, 0].astype(np.float64)
        self.assertGreater(float(a @ b / np.sqrt((a @ a) * (b @ b))), 0.99)


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
        import contextlib
        import io

        from t4ff import progress

        reports = io.StringIO()
        try:
            progress.use_lines(True)
            with contextlib.redirect_stdout(reports):
                out = Writer(x360()).write(ZoneConverter(zone, pc(), x360(), options).convert())
        finally:
            progress.use_lines(False)
        steps = [progress.parse(line) for line in reports.getvalue().splitlines()]
        count = len(zone.assets_node.children)
        self.assertIn((0, count, "Converting assets"), steps)
        self.assertEqual(steps[-1], (count, count, "Converting assets"))
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
        self.assertEqual(len(frames), audio.xma_frame_count(stream.data))
        self.assertEqual(fmt[1], frames[-1][0])  # loop end: the last frame, holding the last sample
        self.assertEqual(fmt[2] >> 24, 3)
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
        def script(zone, name):
            asset = next(a for a in zone.assets if a.type == "rawfile" and a.name == name and a.ptr is not None and a.ptr.kind == "follow")
            return bytes(next(c for c in asset.ptr.node.children if (c.extra.get("origin") or ("", "", ""))[-1] == "buffer").data)

        zones, expected = [], {}
        for name in ("nazi_zombie_aztec.ff", "nazi_zombie_aztec_patch.ff", "mod.ff"):
            _, _, data = read_fastfile(sample("pc", name))
            zone = Reader(pc(), data).load()
            for script_name in ("maps/_laststand.gsc", "animscripts/dog_init.gsc"):
                if any(a.name == script_name for a in zone.assets):
                    expected[script_name] = script(zone, script_name)  # the last zone's version
            zones.append(ZoneConverter(zone, pc(), x360(), options).convert())
        merged = merge_zones(x360(), zones, log=lambda msg: None)
        prune_references(x360(), merged, log=lambda msg: None)
        out = Writer(x360()).write(merged)
        converted = Reader(x360(), out).load()
        self.assertEqual(len(converted.assets), len(merged.assets))
        self.assertEqual(Writer(x360()).write(converted), out)

        # mod.ff overrides <map>_patch.ff, which overrides the map; patch only scripts are kept
        for script_name, data in expected.items():
            self.assertEqual(script(converted, script_name), data, script_name)


if __name__ == "__main__":
    unittest.main()
