"""Tests for t4ff.

Sample based tests use the fastfiles of a folder given by the T4FF_SAMPLES
environment variable and are skipped otherwise:

    $T4FF_SAMPLES/pc/nazi_zombie_aztec.ff, mod.ff, nazi_zombie_aztec_load.ff, nazi_zombie_aztec_patch.ff,
                    nazi_zombie_aztec.iwd
    $T4FF_SAMPLES/x360/patch.ff, patch_ui.ff, sounds/para_egg.xma, nazi_zombie_aztec.ff (CoD Xenon's)
    $T4FF_SAMPLES/v020/_codxe/t4/...: CoD Xenon's 0.2.0 fastfiles package

Run with ``python -m unittest discover -s tests`` from tools/t4ff.
"""

import collections
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


class FastfileTests(unittest.TestCase):
    def test_parallel_compression_is_one_zlib_stream(self):
        import zlib

        from t4ff import fastfile

        rng = np.random.default_rng(3)
        # compressible data with matches across the 1 MiB chunk boundaries
        words = rng.integers(0, 256, 4096, dtype=np.uint8).tobytes()
        data = b"".join(words[i : i + 64] for i in rng.integers(0, 4000, 90000))
        packed = fastfile.compress(data, 9, jobs=4)
        stream = zlib.decompressobj()
        self.assertEqual(stream.decompress(packed), data)
        self.assertTrue(stream.eof)
        self.assertEqual(stream.unused_data, b"")
        self.assertLess(len(packed), len(zlib.compress(data, 9)) * 1.01)


class ScriptTests(unittest.TestCase):
    def test_script_references(self):
        from t4ff.scripts import script_references

        text = b"""#include maps\\_utility;
#include common_scripts\\utility ;
init()
{
    // maps\\_commented_out::nothing();
    /* maps\\_block_comment::nothing(); */
    level thread maps\\_zombiemode_weapons_sumpf::init();
    self local_function();
    thread clientscripts\\_fx::main();
}
"""
        self.assertEqual(
            script_references("maps/_zombiemode.gsc", text),
            {"maps/_utility.gsc", "common_scripts/utility.gsc", "maps/_zombiemode_weapons_sumpf.gsc", "clientscripts/_fx.gsc"},
        )
        self.assertEqual(script_references("clientscripts/x.csc", b"#include clientscripts\\_utility;"), {"clientscripts/_utility.csc"})

    def test_named_assets(self):
        """What the game looks up by name: the animations of the player animation script and the
        shellshock files the scripts may name."""
        from t4ff.library import is_game_zone
        from t4ff.named import player_animations, shellshock_candidates

        script = b"""// both pb_commented_out
state combat
{
\tidle
\t{
\t\tplayerAnimType satchel
\t\t{
\t\t\tboth pb_hold_idle_satchel
\t\t}
\t\tDEFAULT
\t\t{
   \t\t\tboth PB_Hold_Idle   // the first match wins
\t\t\ttorso pt_hold_throw_satchel
\t\t\tboth pb_hold_idle_satchel
\t\t}
\t}
}

scriptevent
{
\tevent lvt_ride_player2
\t{
\t\tboth crew_lvt4_peleliu1_character4_player
\t}
}

death
{
\tdefault
\t{
\t\tboth pb_stand_death_legs
\t}
}
"""
        self.assertEqual(player_animations(script), ["pb_hold_idle_satchel", "pb_hold_idle", "pt_hold_throw_satchel", "pb_stand_death_legs"])
        scripts = [("maps/_zombiemode.gsc", b'init_shellshocks()\n{\n\tlevel.player_killed_shellshock = "zombie_death";\n\t// "commented"\n}\n')]
        self.assertEqual(shellshock_candidates(scripts), ["shock/zombie_death.shock"])
        for name, game in (("common.ff", True), ("D:/zone/code_post_gfx.ff", True), ("patch.ff", True), ("localized_common.ff", True),
                           ("patch_ui.ff", False), ("nazi_zombie_aztec_patch.ff", False), ("nazi_zombie_aztec.ff", False)):
            self.assertEqual(is_game_zone(name), game, name)


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


class LibraryTests(unittest.TestCase):
    def test_library_files(self):
        """A folder of CoD Xenon's maps is enough: the map being converted is read first (its own
        versions win), and a fastfile given twice is read once."""
        from t4ff.library import library_files

        with tempfile.TemporaryDirectory() as tmp:
            t4 = os.path.join(tmp, "_codxe", "t4")
            names = ["usermaps/mario/mario.ff", "usermaps/mario/mario_load.ff", "usermaps/nazi_zombie_aztec/nazi_zombie_aztec.ff",
                     "usermaps/nazi_zombie_aztec/nazi_zombie_aztec_load.ff", "usermaps/nazi_zombie_aztec/readme.txt", "zone/common.ff"]
            for n in names:
                path = os.path.join(t4, *n.split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                open(path, "wb").close()
            short = lambda files: [os.path.relpath(f, t4).replace(os.sep, "/") for f in files]  # noqa: E731
            everything = [n for n in names if n.endswith(".ff")]
            self.assertEqual(short(library_files([t4])), everything)
            self.assertEqual(short(library_files([t4], "NAZI_ZOMBIE_AZTEC")), [everything[2]] + everything[:2] + everything[3:])
            aztec = os.path.join(t4, "usermaps", "nazi_zombie_aztec", "nazi_zombie_aztec.ff")
            self.assertEqual(short(library_files([aztec, t4])), [everything[2]] + everything[:2] + everything[3:])
            self.assertEqual(short(library_files([t4], "zm_unknown")), everything)


class WorldTests(unittest.TestCase):
    def test_vertex_layer_data(self):
        """Layered vertices: (u, v) floats per layer, some with a packed RGBA value (ARGB on the
        console); 8 and 12 byte records mixed, as in The Simpsons."""
        import struct

        from t4ff.assets import console_vertex_layer_data

        pc = struct.pack("<2f", 1.101, 0.065) + struct.pack("<2f", 0.625, 0.514) + bytes.fromhex("ff8080ff") + struct.pack("<2f", 0.0, 3.5)
        console = console_vertex_layer_data(pc)
        self.assertEqual(console[:8], struct.pack(">2f", 1.101, 0.065))
        self.assertEqual(console[8:16], struct.pack(">2f", 0.625, 0.514))
        self.assertEqual(console[16:20], bytes.fromhex("ffff8080"))
        self.assertEqual(console[20:], struct.pack(">2f", 0.0, 3.5))


class MemoryTests(unittest.TestCase):
    def test_automatic_texture_budget(self):
        """Textures get what the memory target leaves: a map that fits is converted once, one over
        it again with less, down to a floor."""
        from t4ff.memory import MARGIN_MIB, MIB, MIN_TEXTURE_BUDGET_MIB, next_texture_budget

        # Zombie Woods: 48 MiB besides 96 MiB of textures fits 200 MiB
        self.assertIsNone(next_texture_budget(144 * MIB, 200 * MIB, 96 * MIB, 96 * MIB))
        # 130 MiB besides the textures: 26 MiB over, the textures lose it
        self.assertEqual(next_texture_budget(226 * MIB, 200 * MIB, 96 * MIB, 96 * MIB), (70 - MARGIN_MIB) * MIB)
        # textures smaller than the budget: from what they are
        self.assertEqual(next_texture_budget(210 * MIB, 200 * MIB, 60 * MIB, 96 * MIB), (50 - MARGIN_MIB) * MIB)
        # not below the floor, and no further once there
        self.assertEqual(next_texture_budget(300 * MIB, 200 * MIB, 96 * MIB, 96 * MIB), MIN_TEXTURE_BUDGET_MIB * MIB)
        self.assertIsNone(next_texture_budget(250 * MIB, 200 * MIB, 24 * MIB, MIN_TEXTURE_BUDGET_MIB * MIB))

    def test_block_sizes_of_a_written_zone(self):
        import struct

        from t4ff.memory import block_sizes

        self.assertEqual(block_sizes(struct.pack(">9I", 100, 0, 1, 2, 3, 4, 5, 6, 7) + b"data"), [1, 2, 3, 4, 5, 6, 7])

    def test_stock_textures_use_the_console_versions(self):
        """A texture from the map's own files is converted; a stock one (only in the PC game's
        files) is referenced when the game's zones have it, copied from the console fastfiles
        when they have it, converted from the PC game's files only otherwise."""
        from t4ff.assets import image_choice

        class Library:
            def in_game_zones(self, rec, name):
                return name == "in_common"

            def find(self, rec, name):
                return ("zone", "node") if name in ("in_common", "in_a_map") else None

        class Conv:
            pass

        conv = Conv()
        conv.console_library = Library()
        conv.stock_library = None
        own, stock = object(), object()
        conv._image_sources = {"custom": own, "in_common": None, "in_a_map": None, "pc_only": None, "nowhere": None}
        conv._stock_images = {"in_common": stock, "in_a_map": stock, "pc_only": stock, "nowhere": None}
        node = None
        self.assertEqual(image_choice(conv, node, "custom"), ("own", own))
        self.assertEqual(image_choice(conv, node, "in_common"), ("game", None))
        self.assertEqual(image_choice(conv, node, "in_a_map"), ("library", None))
        self.assertEqual(image_choice(conv, node, "pc_only"), ("stock", stock))
        self.assertEqual(image_choice(conv, node, "nowhere"), ("missing", None))


class LoadScreenTests(unittest.TestCase):
    def test_title_card(self):
        """A map without a loading screen picture gets its name in red on a dark picture."""
        from t4ff.loadscreen import title_card

        card = title_card("Zombie Woods", 1280, 720)
        self.assertEqual(card.shape, (720, 1280, 4))
        self.assertTrue((card[:, :, 3] == 255).all())
        self.assertLess(int(card[:40, :, :3].max()), 64)  # dark around the title
        red = (card[:, :, 0] > 150) & (card[:, :, 1] < 60)
        rows = np.nonzero(red.any(axis=1))[0]
        self.assertTrue(red.sum() > 1000 and 250 < rows.mean() < 470)  # the title, in the middle

    def test_map_title(self):
        from t4ff.images import IwdLibrary
        from t4ff.loadscreen import map_title

        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(map_title(IwdLibrary([tmp]), "nazi_zombie_wh"), "Nazi Zombie Wh")
            with open(os.path.join(tmp, "mod.arena"), "w") as f:
                f.write('{\n\tmap "nazi_zombie_wh"\n\tlongname "^1Zombie ^7Woods"\n}\n')
            self.assertEqual(map_title(IwdLibrary([tmp]), "nazi_zombie_wh"), "Zombie Woods")
            # an arena file listing other maps first (The Simpsons' lists the campaign's "mak")
            with open(os.path.join(tmp, "mod.arena"), "w") as f:
                f.write('{\n\tmap "mak"\n\tlongname "MENU_LEVEL_MAK"\n}\n{\n\tmap "simpsons"\n\tlongname "simpsons"\n}\n')
            self.assertEqual(map_title(IwdLibrary([tmp]), "simpsons"), "simpsons")
            self.assertEqual(map_title(IwdLibrary([tmp]), "other_map"), "Other Map")

    def test_load_zone_from_cod_xenon_template(self):
        """The load zone of a map is CoD Xenon's with the map's picture and names."""
        from t4ff import dxt
        from t4ff.assets import _decode_console_image
        from t4ff.fastfile import read_fastfile
        from t4ff.loadscreen import _picture_image, build_load_zone, read_template, title_card
        from t4ff.platforms import x360
        from t4ff.zone import Reader

        path = sample("v020", "_codxe", "t4", "usermaps", "mario", "mario_load.ff")
        p = x360()
        template = read_template(p, path)
        self.assertIsNotNone(template)
        card = title_card("Zombie Woods", 1280, 720)
        out = build_load_zone(p, template, "nazi_zombie_wh", card)
        zone = Reader(p, out).load()
        self.assertEqual(
            [a.name for a in zone.assets],
            [",2d", ",$victorybackdrop", "defeat", "$defeatbackdrop", "loadscreen_nazi_zombie_wh_codxe", "$levelbriefing", "nazi_zombie_wh_load"],
        )
        _, _, original = read_fastfile(path)
        sizes = Reader(p, original).load().block_sizes
        self.assertEqual(sizes[2], zone.block_sizes[2])  # the pictures: same size and format
        longer = 2 * (len("nazi_zombie_wh") - len("mario")) + len("_codxe")  # the image and raw file names
        self.assertTrue(longer - 4 <= zone.block_sizes[4] - sizes[4] <= longer + 4)
        image = _decode_console_image(p, _picture_image(p, zone), "loadscreen")
        rgba = dxt.decode(image.levels[0], image.width, image.height, image.format)
        self.assertLess(np.abs(rgba[:, :, :3].astype(int) - card[:, :, :3]).mean(), 3)


class MenuTests(unittest.TestCase):
    def test_frontend_menus_left_out(self):
        """The mod's own main menu and lobby (menu lists of the console's menus) are left out of
        the map's zone, also when one points into the other; a list of script menus stays, and so
        do lists it points into or whose menus the scripts open."""
        from unittest import mock

        import t4ff.scripts  # noqa: F401 (imported before asset_name is patched, it keeps the real one)
        from t4ff.layout import TypeRef
        from t4ff.merge import drop_frontend_menus
        from t4ff.platforms import x360
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Zone, ZoneAsset

        names = {}

        def node(type_name, *children, name=None):
            n = Node(TypeRef("record", type_name), 1, BLOCK_VIRTUAL)
            n.children = list(children)
            names[id(n)] = name
            return n

        def point(source, target, kind="ref"):
            ptr = Ptr(kind, target) if kind == "ref" else Ptr(kind, slot=target)
            ptr.owner, ptr.offset = source, 4 * len(source.relocs)
            source.relocs[ptr.offset] = ptr

        lists = {
            "ui/main.menu": node("MenuList", node("menuDef_t", name="main"), node("menuDef_t", name="main_text"), node("char")),
            "ui/xboxlive_lobby.menu": node("MenuList", node("menuDef_t", name="lobby"), node("menuDef_t", name="multi_popmenu"),
                                           node("menuDef_t", name="main_solo")),
            "ui/shared.menu": node("MenuList", node("menuDef_t", name="pausedmenu")),
            "ui/scriptmenus/music.menu": node("MenuList", node("menuDef_t", name="music_box")),
            "ui/briefing.menu": node("MenuList", node("menuDef_t", name="briefing")),
        }
        lobby = lists["ui/xboxlive_lobby.menu"]
        point(lobby.children[0], lists["ui/main.menu"].children[2])  # the lobby shares a string of the main menu
        item = node("itemDef_s")
        lists["ui/main.menu"].children[1].children.append(item)
        slot = Ptr("follow", item)
        slot.owner = lists["ui/main.menu"].children[1]
        point(lobby.children[1], slot, "alias")  # and an item
        point(lists["ui/scriptmenus/music.menu"].children[0], lists["ui/shared.menu"].children[0])
        rawfile = node("RawFile")
        assets_node = Node(TypeRef("scalar", "uint", 4, 4), 0, BLOCK_VIRTUAL)
        assets = []
        for name, target in list(lists.items()) + [("maps/zombie.gsc", rawfile)]:
            ptr = Ptr("follow", target)
            ptr.owner, ptr.offset = assets_node, 8 * len(assets) + 4
            assets_node.relocs[ptr.offset] = ptr
            assets_node.children.append(target)
            assets.append(ZoneAsset("rawfile" if name.endswith(".gsc") else "menulist", ptr, name))
        assets_node.data = bytearray(8 * len(assets))
        assets_node.count = 2 * len(assets)
        zone = Zone(x360().name, [], assets, [], 0, 0, None, assets_node)
        zone.extra_root = node("root", assets_node)
        stock = {"main", "main_text", "lobby", "main_solo", "pausedmenu", "briefing"}
        script = b'precacheMenu( "briefing" );\nself openMenu("music_box");'
        with mock.patch("t4ff.zone.asset_name", lambda p, n: names.get(id(n))), \
             mock.patch("t4ff.scripts._rawfiles", lambda p, z: [("maps/zombie.gsc", script)]), \
             mock.patch("t4ff.scripts.rawfile_text", lambda raw: raw):
            dropped = drop_frontend_menus(x360(), zone, lambda m: m in stock, log=lambda msg: None)
        self.assertEqual(dropped, ["ui/main.menu", "ui/xboxlive_lobby.menu"])
        self.assertEqual([a.name for a in zone.assets], ["ui/shared.menu", "ui/scriptmenus/music.menu", "ui/briefing.menu", "maps/zombie.gsc"])
        self.assertEqual(sorted(assets_node.relocs), [4, 12, 20, 28])
        self.assertEqual(assets_node.children, [lists["ui/shared.menu"], lists["ui/scriptmenus/music.menu"], lists["ui/briefing.menu"], rawfile])

    def test_dynamic_map_list(self):
        """The Nazi Zombies map list of CoD Xenon's patch_ui.ff: the stock rows stay, the 13 rows of
        their maps become 13 rows showing dvars (CoD Xe fills them from the usermaps folder), with
        scroll catchers, a counter, the preview of the focused map and LB / RB paging."""
        import contextlib
        import io
        import shutil
        import struct

        from t4ff.__main__ import main
        from t4ff.fastfile import read_fastfile
        from t4ff.menu import COUNTER_DX, PREVIEW_SLOT, MenuEditor
        from t4ff.platforms import x360
        from t4ff.zone import Reader, asset_name

        patch_ui = sample("v020", "_codxe", "t4", "zone", "patch_ui.ff")
        patch = sample("v020", "_codxe", "t4", "zone", "patch.ff")
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "zone"))
            for f in (patch_ui, patch):
                shutil.copy(f, os.path.join(tmp, "zone"))
            for name in ("nazi_zombie_aztec", "nazi_zombie_wh"):
                os.makedirs(os.path.join(tmp, "usermaps", name))
            # a converted map installed with its loading screen: its picture for the list comes from it
            from t4ff.loadscreen import build_load_zone, read_template, title_card

            template = read_template(x360(), sample("v020", "_codxe", "t4", "usermaps", "mario", "mario_load.ff"))
            card = title_card("Zombie Woods", 1280, 720)
            with open(os.path.join(tmp, "usermaps", "nazi_zombie_wh", "nazi_zombie_wh_load.ff"), "wb") as f:
                from t4ff.fastfile import write_fastfile

                write_fastfile(f.name, ">", build_load_zone(x360(), template, "nazi_zombie_wh", card))
            for _ in range(2):  # a second run starts again from CoD Xenon's menu
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(["--no-install", "menu", tmp]), 0)
            self.assertTrue(os.path.exists(os.path.join(tmp, "zone", "patch_ui.ff.orig")))
            with open(os.path.join(tmp, "usermaps", "nazi_zombie_aztec", "description.txt")) as f:
                self.assertEqual(f.read().splitlines()[0], "Aztec")
            with open(os.path.join(tmp, "usermaps", "nazi_zombie_aztec", "preview.txt")) as f:
                self.assertEqual(f.read().strip(), "loadscreen_nazi_zombie_aztec")
            self.assertFalse(os.path.exists(os.path.join(tmp, "usermaps", "nazi_zombie_wh", "description.txt")))
            self.assertFalse(os.path.exists(os.path.join(tmp, "usermaps", "nazi_zombie_aztec", "preview.bin")))  # has preview.txt
            with open(os.path.join(tmp, "usermaps", "nazi_zombie_wh", "preview.bin"), "rb") as f:
                preview = f.read()
            magic, (version, width, height, _, fmt, size) = preview[:4], struct.unpack(">HHHHII", preview[4:20])
            self.assertEqual((magic, version, width, height, fmt, size, len(preview)), (b"CXPV", 1, 512, 288, 0x12, 98304, 20 + 98304))

            p = x360()
            zone = Reader(p, read_fastfile(os.path.join(tmp, "zone", "patch_ui.ff"))[2]).load()
            editor = MenuEditor(p, zone)
            actions = [editor.string(item, "action") or "" for item in editor.items]
            self.assertEqual(sorted(a.split("devmap ")[1].split('"')[0] for a in actions if "devmap" in a), sorted(["nazi_zombie_asylum", "nazi_zombie_factory", "nazi_zombie_prototype", "nazi_zombie_sumpf"]))
            names = [editor.string(item, "window.name") for item in editor.items]
            rows = [names.index(f"codxe_map{k}") for k in range(13)]
            self.assertEqual(rows, sorted(rows))
            self.assertLess(names.index("codxe_map_up"), rows[0])
            self.assertGreater(names.index("codxe_map_down"), rows[-1])
            for k, index in enumerate(rows):
                item = editor.items[index]
                self.assertEqual(editor.expression(item, "textExp"), [("op", 31), ("str", f"ui_codxe_map{k}"), ("op", 1)])
                self.assertIn(f'"exec" "vstr ui_codxe_mapcmd{k}"', actions[index])
                self.assertIn(f'"setdvar" "ui_codxe_focus" "{k}"', editor.string(item, "onFocus"))
                self.assertAlmostEqual(editor.rect(item)[1], 134 + 20 * k)
            self.assertEqual(names.count("image_codxe_map"), 3)
            counter = next(item for item in editor.items if editor.expression(item, "textExp") == [("op", 31), ("str", "ui_codxe_maprange"), ("op", 1)])
            last = editor.items[rows[-1]]
            self.assertAlmostEqual(editor.rect(counter)[0], editor.rect(last)[0] + COUNTER_DX)
            self.assertAlmostEqual(editor.rect(counter)[1], editor.rect(last)[1] + 1)
            # the picture slot preview.bin is copied into: its size and format are those of preview.bin
            slot = zone.assets[-1]
            self.assertEqual((slot.type, slot.name), ("material", PREVIEW_SLOT))
            image = next(n for n in slot.ptr.node.walk() if n.type.name == "GfxImage")
            self.assertEqual(asset_name(p, image), PREVIEW_SLOT)
            rec = p.record("GfxImage")
            from t4ff.commands import find_field

            get = lambda f, fmt: struct.unpack_from(">" + fmt, image.data, find_field(rec, f).offset)[0]  # noqa: E731
            self.assertEqual((get("width", "H"), get("height", "H"), get("baseSize", "I")), (width, height, size))
            handlers = {}
            for node in zone.extra_root.walk():
                if node.type.name == "ItemKeyHandler":
                    handlers.setdefault(struct.unpack_from(">i", node.data, 0)[0], []).append(bytes(node.relocs[4].target().data))
            self.assertIn(b'"setdvar" "ui_codxe_scroll" "-13" ; \0', handlers[5])
            self.assertIn(b'"setdvar" "ui_codxe_scroll" "13" ; \0', handlers[6])


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
            # the name and the loading picture belong to one map: not kept for the next
            gui.Settings(input="x", map_name="Zombie Woods", loading_image="woods.png").save(path)
            loaded = gui.Settings.load(path)
            self.assertEqual((loaded.map_name, loaded.loading_image), ("", ""))
        args = gui.convert_args(gui.Settings(input="in", output="out", map_name="Zombie Woods", loading_image="woods.png"))
        self.assertEqual(args[args.index("--name") + 1], "Zombie Woods")
        self.assertEqual(args[args.index("--loading-image") + 1], "woods.png")

    def test_t4_layout_is_the_default(self):
        """CoD Xe reads _codxe\\t4 once it exists: the window and the command line write there
        unless told otherwise, and settings saved before that get it checked."""
        import json

        from t4ff import gui
        from t4ff.__main__ import main

        self.assertTrue(gui.Settings().t4_layout)
        self.assertNotIn("--no-t4-layout", gui.convert_args(gui.Settings(input="in", output="out")))
        self.assertIn("--no-t4-layout", gui.convert_args(gui.Settings(input="in", output="out", t4_layout=False)))
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "gui.json")
            with open(path, "w") as f:
                json.dump({"input": "x", "t4_layout": False}, f)  # an older settings file
            self.assertTrue(gui.Settings.load(path).t4_layout)
            gui.Settings(input="x", t4_layout=False).save(path)  # unchecked since
            self.assertFalse(gui.Settings.load(path).t4_layout)
            # the automatic texture budget replaces a fixed one saved before it existed
            with open(path, "w") as f:
                json.dump({"input": "x", "texture_budget": 48.0, "version": 2}, f)
            self.assertEqual(gui.Settings.load(path).texture_budget, "auto")
            gui.Settings(input="x", texture_budget="64").save(path)
            self.assertEqual(gui.Settings.load(path).texture_budget, "64")
        self.assertNotIn("--texture-budget", gui.convert_args(gui.Settings(input="in", output="out")))
        self.assertEqual(gui.convert_args(gui.Settings(input="in", output="out", texture_budget="0"))[-2:], ["--texture-budget", "0"])

        # the command line
        from t4ff import __main__ as cli

        seen = []
        original = cli.cmd_convert
        cli.cmd_convert = lambda args: seen.append((args.t4_layout, args.texture_budget))
        try:
            for extra in ([], ["--no-t4-layout"], ["--t4-layout", "--texture-budget", "48"]):
                main(["--no-install", "convert", "in", "-o", "out"] + extra)
            with self.assertRaises(SystemExit), open(os.devnull, "w") as devnull, __import__("contextlib").redirect_stderr(devnull):
                main(["--no-install", "convert", "in", "-o", "out", "--texture-budget", "lots"])
        finally:
            cli.cmd_convert = original
        self.assertEqual(seen, [(True, "auto"), (False, "auto"), (True, "48")])


class IwiTests(unittest.TestCase):
    def test_luminance_and_alpha_formats(self):
        """IWI format 4 is luminance (L8 on the console), 5 is alpha (A8L8 with white luminance)."""
        import struct

        def iwi(fmt):
            pixels = bytes(range(64))  # 8x8, one byte per texel, no mip maps
            return b"IWi\x06" + bytes([fmt, 0x02]) + struct.pack("<3H", 8, 8, 1) + bytes(16) + pixels

        luminance = images.parse_iwi("gray", iwi(4))
        self.assertEqual(luminance.format, "L8")
        self.assertEqual(images.build_console_texture(luminance, compress=True).format.name, "L8")
        alpha = images.parse_iwi("mask", iwi(5))
        self.assertEqual(alpha.format, "A8")
        texture = images.build_console_texture(alpha, compress=True)
        self.assertEqual(texture.format.name, "A8L8")


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

    def test_xma1_repack(self):
        """Loaded sounds are XMA1 with the frames back to back: the padding ending every 64 KiB
        block of xma2encode is dropped, the frames keep their bits (all but the last one, set when
        another frame starts in its packet), every packet header gives its first frame."""
        lengths = [3000 + 97 * i % 2000 for i in range(40)]
        lengths[7] = 30000
        data, _ = xma2_packets(lengths)
        packed = audio.xma1_repack(data)
        frames = audio.xma_frames(packed)
        self.assertEqual([length for _, length in frames], lengths)
        payload = (audio.XMA_PACKET_SIZE - 4) * 8

        def rel(bit):
            return (bit // (audio.XMA_PACKET_SIZE * 8)) * payload + bit % (audio.XMA_PACKET_SIZE * 8) - 32

        self.assertEqual([rel(bit) for bit, _ in frames], [sum(lengths[:i]) for i in range(len(lengths))])
        self.assertEqual(len(packed) // audio.XMA_PACKET_SIZE, -(-sum(lengths) // payload))
        headers = [int.from_bytes(packed[o : o + 4], "big") for o in range(0, len(packed), audio.XMA_PACKET_SIZE)]
        self.assertEqual([h >> 28 for h in headers], [k & 0xF for k in range(len(headers))])
        self.assertTrue(all((h >> 26) & 3 == 2 and h & 0x7FF == 0 for h in headers))

    def test_loaded_sound_loop_region(self):
        """The loop region is laid out as in CoD Xenon's loaded sounds: from 3 subframes into the
        first frame to the subframe of decoded sample length + 383."""
        lengths = [3000 + 97 * i % 2000 for i in range(40)]
        data, _ = xma2_packets(lengths)
        expected = audio.xma_frames(audio.xma1_repack(data))  # the frames of the loaded sound
        for valid, frame, subframe in ((40 * 512 - 384, 39, 3), (40 * 512, 39, 3), (1000, 2, 2), (20 * 512 - 380, 20, 0)):
            stream = audio.XmaStream(48000, 1, 40 * 512, data, valid)
            sound = audio.loaded_sound(stream, 1234)
            self.assertEqual(sound.format[0], expected[0][0])
            self.assertEqual(sound.format[1], expected[frame][0])
            self.assertEqual(sound.format[2], (subframe << 24) | (3 << 16))
            self.assertEqual((sound.format[33], sound.format[34]), (1234, len(sound.seek_table) + 2))
        # decoded samples before every packet (frames starting in the packets before it)
        packet_starts = [bit // (audio.XMA_PACKET_SIZE * 8) for bit, _ in expected]
        self.assertEqual(sound.seek_table, [512 * sum(p < k for p in packet_starts) for k in range(len(sound.data) // audio.XMA_PACKET_SIZE)])

    def test_ffmpeg_message_is_one_line(self):
        report = "[wmav2 @ 0x1] next_block_len_bits 4 out of range\n[dec] Error submitting packet\n\n[dec] Task finished\n"
        self.assertEqual(audio.ffmpeg_message(report), "[wmav2 @ 0x1] next_block_len_bits 4 out of range (and 2 more messages)")

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
        # repacking the frames changes nothing to the audio
        as_is = audio.decode_loaded_sound(audio.LoadedXma(stream.rate, 1, audio.xma2_to_xma1(stream.data), [], []))
        self.assertTrue(np.array_equal(decoded.samples, as_is.samples))
        self.assertLess(len(sound.data), len(stream.data))
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
        from t4ff.zone import BLOCK_PHYSICAL, Reader, Writer

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

        # with console fastfiles given, the technique set stays a reference to the game's own (as in
        # CoD Xenon's): a copy in a zone unloaded once the map runs would leave the menus without it
        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec_load.ff"))
        options = ConvertOptions(console_zones=[sample("x360", "nazi_zombie_aztec.ff")], reference_techsets=True, log=lambda msg: None)
        with contextlib.redirect_stdout(io.StringIO()):
            load = ZoneConverter(Reader(pc(), data).load(), pc(), x360(), options).convert()
        again = Reader(x360(), Writer(x360()).write(load)).load()
        self.assertEqual([a.name for a in again.assets if a.type == "techset"], [",2d"])
        self.assertEqual(again.block_sizes[BLOCK_PHYSICAL], 0)  # no shaders

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

    def test_loaded_sounds_encoded_in_parallel(self):
        """The loaded sounds of a zone are encoded on several threads before the conversion; the
        conversion then takes them from the cache."""
        import threading
        from unittest import mock

        from t4ff.assets import _loaded_sound_data, console_sound_name, encode_loaded_sounds
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, asset_name

        with open(sample("x360", "sounds", "para_egg.xma"), "rb") as f:
            xma = audio.loaded_sound(audio.read_sdns(f.read()), 1000)
        threads, calls = set(), []

        def fake_encode(wav, encoder, max_rate=0, mono=False):
            threads.add(threading.get_ident())
            calls.append(len(wav))
            if len(calls) == 3:
                raise audio.AudioError("broken sound")
            return xma

        class Encoder:
            available = True

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        zone = Reader(pc(), data).load()
        options = ConvertOptions(xma_encoder=Encoder(), jobs=4, log=lambda msg: None)
        conv = ZoneConverter(zone, pc(), x360(), options)
        with mock.patch.object(audio, "encode_loaded_sound", fake_encode):
            encode_loaded_sounds(conv)
            sounds = [n for n in zone.extra_root.walk() if (n.extra.get("origin") or ("",))[0] == "asset" and n.type.name == "LoadedSound" and not asset_name(pc(), n).startswith(",")]
            self.assertEqual(len(calls), len(options.sound_cache))
            self.assertGreater(len(threads), 1)
            self.assertEqual(sum(isinstance(v, audio.AudioError) for v in options.sound_cache.values()), 1)
            def key(node):
                return (console_sound_name(asset_name(pc(), node)), len(_loaded_sound_data(node).data))

            encoded = next(n for n in sounds if _loaded_sound_data(n) is not None and not isinstance(options.sound_cache[key(n)], Exception))
            before = len(calls)
            conv.convert_asset_node("loaded_sound", encoded)
            self.assertEqual(len(calls), before)  # taken from the cache
            self.assertEqual(conv.stats.sound_bytes, len(xma.data))

    def test_shared_string_edit(self):
        """The PC linker stores equal strings once: all the empty strings of Aztec are the stream
        directory of one sound alias. Renaming that directory (a stream of the map is served from
        its sounds folder) must leave the others alone (kar98k's alternate weapon became 'sounds')."""
        from t4ff.assets import apply_string_edits
        from t4ff.commands import find_field
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, Writer, asset_name

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        conv = ZoneConverter(Reader(pc(), data).load(), pc(), x360(), ConvertOptions(log=lambda msg: None))
        zone = conv.convert()
        alt = find_field(x360().record("WeaponDef"), "szAltWeaponName").offset

        def kar98k(z):
            return next(n for n in z.extra_root.walk() if (n.extra.get("origin") or ("",))[0] == "asset" and n.type.name == "WeaponDef" and asset_name(x360(), n) == "kar98k")

        empty = kar98k(zone).relocs[alt].node
        users = sum(1 for n in zone.extra_root.walk() for q in n.relocs.values() if q.kind == "ref" and q.node is empty)
        self.assertGreater(users, 1000)
        owner, offset = next((n, off) for n in zone.extra_root.walk() for off, q in n.relocs.items() if q.kind == "follow" and q.node is empty)
        self.assertEqual(owner.type.name, "SoundFile")
        conv._string_edits = [(owner, offset, "sounds")]
        apply_string_edits(conv, zone.extra_root)

        out = Writer(x360()).write(zone)
        again = Reader(x360(), out).load()
        self.assertEqual(Writer(x360()).write(again), out)
        target = kar98k(again).relocs[alt].target()
        self.assertEqual(bytes(target.data), b"\0")
        dirs = [bytes(q.target().data) for n in again.extra_root.walk() if n.type.name == "SoundFile" for off, q in n.relocs.items() if off == offset and q.kind != "null"]
        self.assertEqual(dirs.count(b"sounds\0"), 1)
        still_empty = sum(1 for n in again.extra_root.walk() for q in n.relocs.values() if q.kind == "ref" and q.node is not None and bytes(q.node.data) == b"\0")
        self.assertGreaterEqual(still_empty, users - 1)

    def test_loaded_sound_limit(self):
        """Identical loaded sounds are shared, then the longest become streamed sounds (files in
        the map's sounds folder) until the zone is within the limit; the zone still loads."""
        import struct
        import zlib
        from unittest import mock

        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.soundbudget import limit_loaded_sounds
        from t4ff.zone import Reader, Writer

        with open(sample("x360", "sounds", "para_egg.xma"), "rb") as f:
            packets = audio.read_sdns(f.read()).data[: 2 * audio.XMA_PACKET_SIZE]

        def fake_encode(wav, encoder, max_rate=0, mono=False):
            # the same sound for the same PC data, a length growing with it
            stream = audio.XmaStream(48000, 1, 1024, packets, 1 + len(wav) % 1000)
            return audio.loaded_sound(stream, zlib.crc32(wav))

        class Encoder:
            available = True

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        options = ConvertOptions(xma_encoder=Encoder(), log=lambda msg: None)
        with mock.patch.object(audio, "encode_loaded_sound", fake_encode):
            zone = ZoneConverter(Reader(pc(), data).load(), pc(), x360(), options).convert()
        streams = {key[0].lower(): xma.stream for key, xma in options.sound_cache.items()}
        with tempfile.TemporaryDirectory() as tmp:
            stats = limit_loaded_sounds(x360(), zone, 1500, streams, tmp, log=lambda msg: None)
            self.assertEqual((stats["shared"], stats["count"]), (18, 1500))
            out = Writer(x360()).write(zone)
            again = Reader(x360(), out).load()
            self.assertEqual(Writer(x360()).write(again), out)
            loaded = [n for n in again.extra_root.walk() if (n.extra.get("origin") or ("",))[0] == "asset" and n.type.name == "LoadedSound"]
            self.assertEqual(len(loaded), 1500)
            custom = [n for n in again.extra_root.walk() if n.type.name == "SoundFile" and n.data[0] == 2 and struct.unpack_from(">I", n.data, 4)[0] == 0]
            self.assertGreaterEqual(len(custom), stats["streamed"])
            files = set()
            for n in custom:
                directory, name = (bytes(c.data).rstrip(b"\0").decode() for c in n.children)
                self.assertTrue(directory.startswith("sounds\\"))
                files.add(os.path.join(tmp, *directory.split("\\"), name + ".xma"))
            self.assertEqual(len(files), stats["streamed"])
            self.assertTrue(all(os.path.exists(f) for f in files))
            with open(next(iter(files)), "rb") as f:
                self.assertEqual(audio.read_sdns(f.read()).data, packets)
            # the aliases say their sound is streamed too (flags bits 13-14 = SoundFile.type):
            # an alias still flagged loaded reads the stream name as a sound pointer and crashes
            from t4ff.commands import find_field

            rec = x360().record("snd_alias_t")
            flags_off = find_field(rec, "flags").offset
            file_off = find_field(rec, "soundFile").offset
            types = collections.Counter()
            for node in again.extra_root.walk():
                if node.type.name == "snd_alias_t":
                    for i in range(node.count):
                        ptr = node.relocs.get(i * rec.size + file_off)
                        target = ptr.target() if ptr is not None and ptr.kind != "null" else None
                        if target is not None:
                            flags = struct.unpack_from(">I", node.data, i * rec.size + flags_off)[0]
                            types[(target.data[0], (flags >> 13) & 3)] += 1
            self.assertEqual({k for k in types if k[0] != k[1]}, set())
            self.assertGreaterEqual(types[(2, 2)], stats["streamed"])

    def test_xwma_loaded_sounds_decode(self):
        """PC loaded sounds in xWMA whose header bit rate is not the real one decode completely
        (32 kHz ones also need 3 block sizes); FFmpeg alone rejects them."""
        import struct

        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc
        from t4ff.zone import Reader, asset_name

        if audio.ffmpeg_exe() is None:
            self.skipTest("FFmpeg not available")
        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        zone = Reader(pc(), data).load()
        wanted = {"sfx/weapon/mg/30cal/foley/gr_30cal.wav": 32000, "sfx/destruction/explosion_debris/water/water_00.wav": 22050, "sfx/levels/zombie/tele/beam/beam_fx.wav": 44100}
        found = {}
        for node in zone.extra_root.walk():
            if (node.extra.get("origin") or ("",))[0] == "asset" and node.type.name == "LoadedSound" and asset_name(pc(), node) in wanted:
                wav = bytes(next(c for c in node.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("snd_asset", "data")).data)
                found[asset_name(pc(), node)] = wav
        self.assertEqual(set(found), set(wanted))
        for name, wav in found.items():
            self.assertTrue(audio.is_xwma(wav))
            chunks = dict(audio.riff_chunks(wav))
            expected = struct.unpack_from("<I", chunks[b"dpds"], len(chunks[b"dpds"]) - 4)[0] // 2
            pcm = audio.read_wav(wav)
            self.assertEqual(pcm.rate, wanted[name])
            self.assertLessEqual(abs(pcm.frames - expected), audio.XWMA_LENGTH_TOLERANCE, name)
            self.assertGreater(int(np.abs(pcm.samples.astype(np.int32)).max()), 1000, name)

    def test_convert_map(self):
        """The whole usermap (map + patch + mod, merged) converts to a zone the console loader reads,
        with the scripts the PC game runs: the map's loose scripts win over its fastfiles, and scripts
        it uses from the game's own zones are added (from the console fastfiles), as in CoD Xenon's."""
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.images import IwdLibrary
        from t4ff.merge import merge_zones, prune_references
        from t4ff.platforms import pc, x360
        from t4ff.scripts import missing_scripts_zone, override_scripts
        from t4ff.zone import Reader, Writer, asset_name

        library = sample("x360", "nazi_zombie_aztec.ff")
        iwd = sample("pc", "nazi_zombie_aztec.iwd")
        options = ConvertOptions(iwd_paths=[iwd], console_zones=[library], log=lambda msg: None)

        def scripts(zone):
            found = {}
            for node in zone.extra_root.walk():
                if (node.extra.get("origin") or ("",))[0] == "asset" and node.type.name == "RawFile":
                    name = asset_name(zone_platform[id(zone)], node)
                    buffer = next((c for c in node.children if (c.extra.get("origin") or ("", "", ""))[-1] == "buffer"), None)
                    found.setdefault(name, bytes(buffer.data).rstrip(b"\0") if buffer is not None else None)
            return found

        zone_platform = {}
        pc_zones, expected = [], {}
        for name in ("nazi_zombie_aztec.ff", "nazi_zombie_aztec_patch.ff", "mod.ff"):
            _, _, data = read_fastfile(sample("pc", name))
            zone = Reader(pc(), data).load()
            zone_platform[id(zone)] = pc()
            expected.update({k: v for k, v in scripts(zone).items() if not k.startswith(",")})  # the last zone's version
            pc_zones.append(zone)
        map_files = IwdLibrary([iwd])
        self.assertEqual(override_scripts(pc(), pc_zones, map_files, log=lambda msg: None), 3)
        for name in ("maps/_zombiemode.gsc", "maps/_zombiemode_spawner.gsc", "maps/_loadout.gsc"):
            expected[name] = map_files.read(name).rstrip(b"\0")
        convs = [ZoneConverter(zone, pc(), x360(), options) for zone in pc_zones]
        merged = merge_zones(x360(), [c.convert() for c in convs], log=lambda msg: None)
        extra = missing_scripts_zone(x360(), merged, [map_files], convs[0].console_library, log=lambda msg: None)
        self.assertEqual([a.name for a in extra.assets], ["maps/_zombiemode_weapons_sumpf.gsc"])
        merged = merge_zones(x360(), [merged, extra], log=lambda msg: None)
        prune_references(x360(), merged, log=lambda msg: None)
        out = Writer(x360()).write(merged)
        converted = Reader(x360(), out).load()
        zone_platform[id(converted)] = x360()
        self.assertEqual(len(converted.assets), len(merged.assets))
        self.assertEqual(Writer(x360()).write(converted), out)

        ours = scripts(converted)
        # every script is there in full (merging twice keeps them), with the version PC runs
        self.assertEqual({n[1:] for n in ours if n.startswith(",")} - set(ours), set())
        for name, data in expected.items():
            self.assertEqual(ours[name], data, name)
        _, _, data = read_fastfile(library)
        xenon = Reader(x360(), data).load()
        zone_platform[id(xenon)] = x360()
        theirs = {k: v for k, v in scripts(xenon).items() if not k.startswith(",")}
        self.assertEqual(set(theirs), {k for k in ours if not k.startswith(",")})
        # CoD Xenon edited one script by hand (split screen fog)
        self.assertEqual([n for n in theirs if theirs[n] != ours[n]], ["maps/createart/nazi_zombie_aztec_art.gsc"])


if __name__ == "__main__":
    unittest.main()
