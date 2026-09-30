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
import struct
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

    def test_map_stream_hash(self):
        """The streams a map serves from its sounds folder get the hash of their path, as the game's
        do."""
        from t4ff.assets import map_stream_hash, stream_name_hash

        songs = {map_stream_hash("sounds\\music_box", name) for name in ("bart2", "ned", "simpsons1")}
        self.assertEqual(len(songs), 3)
        self.assertNotIn(0, songs)
        self.assertEqual(map_stream_hash("sounds\\music_box", "bart2"), stream_name_hash("sounds\\music_box\\bart2"))
        self.assertEqual(map_stream_hash("Sounds\\Music_Box", "BART2"), map_stream_hash("sounds\\music_box", "bart2"))
        self.assertEqual(map_stream_hash("", "a"), stream_name_hash("a"))

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
            env = {"HOME": home, "USERPROFILE": home, "XMA2ENCODE": "", "PATH": tmp}
            # no developer kit on this computer, even where one is installed (their variables and
            # the Program Files folders searched on Windows)
            env.update(dict.fromkeys(("XEDK", "DurangoXDK", "GXDKLatest", "GameDKLatest", "GameDKXboxLatest", "GameDK"), ""))
            with mock.patch.dict(os.environ, env), mock.patch.object(deps, "BIN_DIR", bin_dir), mock.patch.object(deps, "TOOL_DIR", tmp), mock.patch.object(deps, "_program_files", lambda: []):
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

    def test_anim_tree_parsing(self):
        from t4ff.named import anim_tree_animations, anim_trees_used

        tree = b"""attack_player : nonloopsync
{
\tgerman_shepherd_attack_player
}
german_shepherd_idle // a comment
german_shepherd_look_2 : additive
{
\tgerman_shepherd_look_down
}
/* german_shepherd_commented_out */
knob
{
\tinner : nonloopsync
\t{
\t\tLeaf_A
\t}
\tleaf_b
}
"""
        self.assertEqual(anim_tree_animations(tree), ["german_shepherd_attack_player", "german_shepherd_idle", "german_shepherd_look_down", "leaf_a", "leaf_b"])
        scripts = [("maps/_zombiemode_dogs.gsc", b'#using_animtree( "dog" );\n// #using_animtree("commented");'), ("x.gsc", b'#using_animtree("zombie_cymbal_monkey");')]
        self.assertEqual(anim_trees_used(scripts), ["animtrees/dog.atr", "animtrees/zombie_cymbal_monkey.atr"])

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


    def test_use_key_hints(self):
        """Hints naming the PC's use key (F) show the console's use button (&&1), in scripts that
        set hint strings only."""
        from unittest import mock

        from t4ff.commands import find_field
        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.scripts import make_rawfile, rawfile_text, use_key_hints
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        music = make_rawfile(p, template, "maps/tom_player_unl.gsc",
                             b'ths_music( "Press F To Play A Song" );\nhint(h) { self setHintString(h); }\n// "Press Fire"\n')
        other = make_rawfile(p, template, "maps/other.gsc", b'iprintln("Press F to pay respects");\n')
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: [("maps/tom_player_unl.gsc", music), ("maps/other.gsc", other)]):
            self.assertEqual(use_key_hints(p, None, log=lambda msg: None), ["maps/tom_player_unl.gsc"])
        text = b'ths_music( "Press &&1 To Play A Song" );\nhint(h) { self setHintString(h); }\n// "Press Fire"\n'
        self.assertEqual(rawfile_text(music), text)
        self.assertEqual(p.u32.unpack_from(music.data, find_field(p.record("RawFile"), "len").offset)[0], len(text))
        self.assertEqual(rawfile_text(other), b'iprintln("Press F to pay respects");\n')

    def test_modder_help_without_developer(self):
        """The modding kits' modderHelp says a missing entity is missing also without developer, so
        the setups stop there (The Simpsons' zipline setup looped forever); once, CRLF kept."""
        from unittest import mock

        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.scripts import fix_modder_help, make_rawfile, rawfile_text
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        kit = (b'modderHelp( Entity, Msg )\r\n{\r\n\t// Developer Needs To Be Set To 1\r\n\tif( getDvarInt( "developer" ) >= 1 )\r\n'
               b'\t{\r\n\t\treturn true;\r\n\t}\r\n\treturn false;\r\n}\r\n\r\ninit()\r\n{\r\n\tif( modderHelp( trig, "Missing." ) )\r\n\t{\r\n\t\treturn;\r\n\t}\r\n}\r\n')
        util = make_rawfile(p, template, "maps/dlc2_util.gsc", kit)
        other = make_rawfile(p, template, "maps/zipline.gsc", b'init()\n{\n\tif( modderHelp( trig, "Missing." ) )\n\t\treturn;\n}\n')
        files = [("maps/dlc2_util.gsc", util), ("maps/zipline.gsc", other)]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            self.assertEqual(fix_modder_help(p, None, log=lambda msg: None), ["maps/dlc2_util.gsc"])
            self.assertEqual(fix_modder_help(p, None, log=lambda msg: None), [])  # once
        text = rawfile_text(util)
        self.assertTrue(text.startswith(b'modderHelp( Entity, Msg )\r\n{\r\n\t// t4ff: a missing entity stops the setup without developer too\r\n'
                                        b'\tif( !isDefined( Entity ) && isDefined( Msg ) && getDvarInt( "developer" ) < 1 )\r\n\t\treturn true;\r\n'
                                        b'\r\n\t// Developer Needs To Be Set To 1\r\n'))
        self.assertEqual(text.count(b"modderHelp"), kit.count(b"modderHelp"))  # the calls are left alone
        self.assertEqual(rawfile_text(other), b'init()\n{\n\tif( modderHelp( trig, "Missing." ) )\n\t\treturn;\n}\n')

    def _rawfile_template(self):
        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        return p, template

    def test_precache_before_waits(self):
        """Dead Sand's level script calls the zombie mode's main(), which waits for the players, then
        its own setup, which precaches its rocket barrage: the console refuses precaches after a wait,
        so the setup's precaches are made at the start of the level script's main() too, and so are
        those of the zone's models scripts set by name and nothing precaches (its rockets')."""
        from unittest import mock

        from t4ff.scripts import make_rawfile, precache_before_waits, rawfile_text

        p, template = self._rawfile_template()
        level = (b"#include maps\\_dead_sand_utility;\r\nmain()\r\n{\r\n\tmaps\\dead_sand_zombiemode::main();\r\n"
                 b"\tdead_sand_init();\r\n\tlevel thread maps\\_other::setup( 1 );\r\n}\r\n")
        utility = (b'dead_sand_init()\r\n{\r\n\t// PrecacheItem( "commented_out" );\r\n\tPrecacheItem( "rocket_barrage" );\r\n'
                   b'\tif( x ) { y(); }\r\n}\r\nother()\r\n{\r\n\tPrecacheItem( "not_called" );\r\n}\r\n')
        other = b'setup( n )\n{\n\tprecacheShader( "hud_icon" );\n\tprecacheString( &"MAP_HINT" );\n\tPrecacheModel( var );\n}\n'
        zombiemode = b'main()\n{\n\tPrecacheItem( "colt" );\n\tflag_wait( "all_players_connected" );\n}\n'
        # models set by name: the zone's that nothing precaches are precached, the others are not
        rockets = (b'fire()\n{\n\trocket SetModel( "katyusha_rocket" );\n\tbox SetModel( "zombie_box" );\n'
                   b'\tother SetModel( "not_in_zone" );\n\tSetModel( "katyusha_rocket" );\n}\ninit()\n{\n\tPrecacheModel( "zombie_box" );\n}\n')
        files = [(n, make_rawfile(p, template, n, t)) for n, t in (("maps/dead_sand.gsc", level), ("maps/_dead_sand_utility.gsc", utility),
                                                                  ("maps/_other.gsc", other), ("maps/dead_sand_zombiemode.gsc", zombiemode),
                                                                  ("maps/_rockets.gsc", rockets))]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files), \
                mock.patch("t4ff.scripts._model_names", lambda p, z: ["Katyusha_Rocket", "zombie_box"]):
            added = precache_before_waits(p, None, "maps/dead_sand.gsc", log=lambda msg: None)
            self.assertEqual(added, ['PrecacheItem( "rocket_barrage" );', 'precacheShader( "hud_icon" );', 'precacheString( &"MAP_HINT" );',
                                     'PrecacheModel( "katyusha_rocket" );'])
            self.assertEqual(precache_before_waits(p, None, "maps/dead_sand.gsc", log=lambda msg: None), [])  # once
        self.assertTrue(rawfile_text(files[0][1]).startswith(
            b"#include maps\\_dead_sand_utility;\r\nmain()\r\n{\r\n\t// t4ff: the precaches of the setup below, before the level script's first wait\r\n"
            b'\tPrecacheItem( "rocket_barrage" );\r\n\tprecacheShader( "hud_icon" );\r\n\tprecacheString( &"MAP_HINT" );\r\n'
            b'\tPrecacheModel( "katyusha_rocket" );\r\n\r\n'
            b"\tmaps\\dead_sand_zombiemode::main();\r\n"))
        # the setup's own calls, which would stop it after the wait, are comments
        self.assertIn(b'\t// PrecacheItem( "commented_out" );\r\n\t/* t4ff: made first in the level script: PrecacheItem( "rocket_barrage" ); */\r\n',
                      rawfile_text(files[1][1]))
        self.assertIn(b'PrecacheItem( "not_called" );', rawfile_text(files[1][1]))
        self.assertIn(b'\t/* t4ff: made first in the level script: precacheShader( "hud_icon" ); */\n\t/* t4ff: made first in the level script: '
                      b'precacheString( &"MAP_HINT" ); */\n\tPrecacheModel( var );\n', rawfile_text(files[2][1]))
        self.assertIn(b'\tPrecacheItem( "colt" );\n', rawfile_text(files[3][1]))

    def test_speed_up_zombies_only(self):
        """speed_up_zombies() gave every axis AI the zombies' sprint (Dead Sand's SS soldiers ran as
        zombies): it hurries zombies only."""
        from unittest import mock

        from t4ff.scripts import make_rawfile, rawfile_text, speed_up_zombies_only

        p, template = self._rawfile_template()
        text = (b'speed_up_zombies()\r\n{\t\r\n\tzombie_stragglers = GetAiArray( "axis" );\r\n\t\r\n\tfor (i=0; i<zombie_stragglers.size; i++)\r\n'
                b'\t{\r\n\t\tzombie_stragglers[i].zombie_move_speed = "sprint";\r\n\t}\r\n}\r\n')
        node = make_rawfile(p, template, "maps/_zombiemode_spawner.gsc", text)
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: [("maps/_zombiemode_spawner.gsc", node)]):
            self.assertEqual(speed_up_zombies_only(p, None, log=lambda msg: None), ["maps/_zombiemode_spawner.gsc"])
            self.assertEqual(speed_up_zombies_only(p, None, log=lambda msg: None), [])
        self.assertIn(b"\t{\r\n\t\t// t4ff: only zombies\r\n\t\tif( !IsDefined( zombie_stragglers[i].is_zombie ) || !zombie_stragglers[i].is_zombie )\r\n"
                      b"\t\t\tcontinue;\r\n\r\n\t\tzombie_stragglers[i].zombie_move_speed", rawfile_text(node))

    def test_usermap_scripts(self):
        """The mod's versions of the game's scripts that keep_mod_scripts could not rename (PhilMod's
        _load, its client scripts) go to the map's scripts folder, which CoD Xe loads in place of the
        game's; the same as the game's (whitespace aside), renamed ones and the map's own stay out, and
        what an earlier conversion wrote there goes."""
        import os
        import tempfile
        from unittest import mock

        from t4ff.scripts import make_rawfile, usermap_scripts, write_usermap_scripts

        p, template = self._rawfile_template()
        mine = {"maps/_load.gsc": b"main()\n{\n\tphil();\n}\n", "maps/_same.gsc": b"f() { }\n", "clientscripts/_load.csc": b"main() { phil(); }\n",
                "maps/_zombiemode_zone_manager.gsc": b"g() { phil(); }\n", "maps/mymap.gsc": b"main() { }\n"}
        game = {"maps/_load.gsc": b"main()\n{\n}\n", "maps/_same.gsc": b"f()\n{\n}\n", "clientscripts/_load.csc": b"main() { }\n",
                "maps/_zombiemode_zone_manager.gsc": b"g() { }\n"}
        files = [(n, make_rawfile(p, template, n, t)) for n, t in mine.items()]

        class Library:
            def find_in_game_zones(self, asset_type, name):
                return (None, make_rawfile(p, template, name, game[name])) if name in game else None

        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            scripts = usermap_scripts(p, None, set(mine), Library(), {"maps/_zombiemode_zone_manager.gsc": "maps/_zombiemode_zone_manager_mod.gsc"})
        self.assertEqual(scripts, {"maps/_load.gsc": mine["maps/_load.gsc"], "clientscripts/_load.csc": mine["clientscripts/_load.csc"]})
        with tempfile.TemporaryDirectory() as out:
            os.makedirs(os.path.join(out, "scripts", "maps"))
            with open(os.path.join(out, "scripts", "maps", "_stale.gsc"), "wb") as f:
                f.write(b"old")
            self.assertEqual(write_usermap_scripts(scripts, out, log=lambda msg: None), ["clientscripts/_load.csc", "maps/_load.gsc"])
            with open(os.path.join(out, "scripts", "maps", "_load.gsc"), "rb") as f:
                self.assertEqual(f.read(), mine["maps/_load.gsc"])
            self.assertFalse(os.path.exists(os.path.join(out, "scripts", "maps", "_stale.gsc")))
            write_usermap_scripts({}, out, log=lambda msg: None)
            self.assertFalse(os.path.exists(os.path.join(out, "scripts")))

    def test_options_asked_in_game(self):
        """The map's options are asked as the level starts: the level script precaches their menus
        and the restart menu first thing, starts the thread asking them, and gets its functions; a
        second run changes nothing."""
        from unittest import mock

        from t4ff.scripts import make_rawfile, options_script, rawfile_text

        p, template = self._rawfile_template()
        level = b"#include maps\\_utility;\r\nmain()\r\n{\r\n\tmaps\\_zombiemode::main();\r\n}\r\nloadout()\r\n{\r\n\tself OpenMenu(\"loadout\");\r\n}\r\n"
        briefing = b'connect()\n{\n\tself openMenu( "briefing" );\n\tself closeMenu( "briefing" );\n}\n'
        files = [("maps/mymap.gsc", make_rawfile(p, template, "maps/mymap.gsc", level)),
                 ("maps/_callbackglobal.gsc", make_rawfile(p, template, "maps/_callbackglobal.gsc", briefing))]
        options = [{"dvar": "philmod_gamemode", "label": "Difficulty", "default": "2", "choices": [("0", "Easy"), ("2", "Default")]}]
        menus = ["t4ff_option0", "t4ff_options_restart"]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            self.assertTrue(options_script(p, None, "maps/mymap.gsc", options, menus, log=lambda msg: None))
            self.assertFalse(options_script(p, None, "maps/mymap.gsc", options, menus, log=lambda msg: None))
            self.assertFalse(options_script(p, None, "maps/mymap.gsc", options, menus[:1], log=lambda msg: None))
        text = rawfile_text(files[0][1])
        self.assertTrue(text.startswith(b"#include maps\\_utility;\r\nmain()\r\n{\r\n\t// t4ff: the map's options: asked as the level starts (t4ff_options)\r\n"
                                        b'\tPrecacheMenu( "t4ff_option0" );\r\n\tPrecacheMenu( "t4ff_options_restart" );\r\n'
                                        b'\tlevel.t4ff_asking = GetDvar( "t4ff_options_restart" ) != "1";\r\n'
                                        b"\tlevel thread t4ff_options();\r\n\r\n\tmaps\\_zombiemode::main();\r\n}\r\n"))
        # the map's menus wait for the options: they open through t4ff_open_menu
        self.assertIn(b'\tself t4ff_open_menu("loadout");\r\n', text)
        # other scripts keep theirs (a call into the level script is one more script reference)
        self.assertEqual(rawfile_text(files[1][1]), briefing)
        self.assertIn(b"\tplayer OpenMenu( menu_name );\r\n", text)  # the options' own menus open at once
        self.assertIn(b'\tif( t4ff_ask( player, "t4ff_option0", "philmod_gamemode" ) )\r\n\t\tchanged = true;\r\n', text)
        self.assertIn(b'\t\tplayer OpenMenu( "t4ff_options_restart" );\r\n', text)
        self.assertNotIn(b"\r\r", text)

    def test_menu_dvar_defaults(self):
        """PhilMod's main menu sets its difficulty (philmod_gamemode 2) before the map loads; the
        console shows the game's menus, so the level script sets it when nothing did. Dvars set to
        several values, set by expressions, the game's own and those no script reads are left out."""
        from unittest import mock

        from t4ff.layout import TypeRef
        from t4ff.scripts import make_rawfile, menu_dvar_defaults, menu_dvar_values, rawfile_text
        from t4ff.zone import BLOCK_VIRTUAL, Node, Zone

        p, template = self._rawfile_template()
        root = Node(TypeRef("record", "MenuList"), 1, BLOCK_VIRTUAL)
        for text in ('"setdvar" "philmod_gamemode" 2 ; "close" "self"',
                     '"setdvar" "philmod_gamemode" "dvarString" ( "philsv_gamemode" ) ;',
                     '"setdvar" "credits_frommenu" 1 ; "setdvar" "cg_blood" 0 ;',
                     '"setdvar" "credits_frommenu" 0 ; "setdvar" "unread_option" 1 ; "setdvar" "phil_hud" "on"'):
            node = Node(TypeRef("scalar", "char", 1, 1), len(text) + 1, BLOCK_VIRTUAL)
            node.string = True
            node.data = bytearray(text.encode("latin-1") + b"\0")
            root.children.append(node)
        zone = Zone(p.name, [], [], [], 0, 0, None, None)
        zone.extra_root = root
        values = menu_dvar_values(zone)
        self.assertEqual(values["philmod_gamemode"], {"2"})
        self.assertEqual(values["credits_frommenu"], {"0", "1"})

        level = b'main()\r\n{\r\n\tlevel.philMod.gameMode = GetDvarInt( "philmod_gamemode" );\r\n}\r\n'
        other = (b'f()\n{\n\tif( GetDvar( "credits_frommenu" ) == "1" ) x();\n\tb = GetDvarInt("cg_blood");\n'
                 b'\th = GetDvar( "phil_hud" );\n}\n')
        files = [(n, make_rawfile(p, template, n, t)) for n, t in (("maps/mymap.gsc", level), ("maps/_phil_hud.gsc", other))]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            self.assertEqual(menu_dvar_defaults(p, None, values, "maps/mymap.gsc", log=lambda msg: None), {"phil_hud": "on", "philmod_gamemode": "2"})
            self.assertEqual(menu_dvar_defaults(p, None, values, "maps/mymap.gsc", log=lambda msg: None), {})  # once
        self.assertTrue(rawfile_text(files[0][1]).startswith(
            b"main()\r\n{\r\n\t// t4ff: the options the map's own menus set on PC, which the console does not show\r\n"
            b'\tif( GetDvar( "phil_hud" ) == "" )\r\n\t\tSetDvar( "phil_hud", "on" );\r\n'
            b'\tif( GetDvar( "philmod_gamemode" ) == "" )\r\n\t\tSetDvar( "philmod_gamemode", "2" );\r\n\r\n'
            b"\tlevel.philMod.gameMode"))

    def test_zombie_idles_for_zombies(self):
        """The zombie mode replaced the stand and crouch idles of every AI with the zombies' (Dead
        Sand's soldiers stood with their arms out): on maps with soldiers, the zombie idles get poses
        of their own that only zombies' stop script plays. Maps with zombies only keep theirs."""
        from unittest import mock

        from t4ff.scripts import make_rawfile, rawfile_text, zombie_idles_for_zombies

        p, template = self._rawfile_template()
        text = (b'#include maps\\_utility;\r\n#using_animtree( "generic_human" );\r\nmain()\r\n{\r\n\tinit_animscripts();\r\n}\r\n'
                b'// anim.idleAnimArray["stand"][0][0] = %ai_zombie_idle_v1_delta;\r\n'
                b'init_animscripts()\r\n{\r\n\tanimscripts\\init::firstInit();\r\n\r\n'
                b'\tanim.idleAnimArray\t\t["stand"] = [];\r\n\tanim.idleAnimWeights\t["stand"] = [];\r\n'
                b'\tanim.idleAnimArray\t\t["stand"][0][0] \t= %ai_zombie_idle_v1_delta;\r\n\tanim.idleAnimWeights\t["stand"][0][0] \t= 10;\r\n'
                b'\tanim.idleAnimArray\t\t["crouch"][0][0] \t= %ai_zombie_idle_crawl_delta;\r\n}\r\n'
                b'other()\r\n{\r\n\tx = anim.idleAnimArray["stand"];\r\n}\r\n')
        node = make_rawfile(p, template, "maps/dead_sand_zombiemode.gsc", text)
        files = [("maps/dead_sand_zombiemode.gsc", node)]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            with mock.patch("t4ff.scripts._actor_classnames", lambda p, z: {"actor_axis_zombie_ger_ber_sshonor", "actor_zombie_dog"}):
                self.assertEqual(zombie_idles_for_zombies(p, None, log=lambda msg: None), [])
            with mock.patch("t4ff.scripts._actor_classnames", lambda p, z: {"actor_axis_zombie_ger_ber_sshonor", "actor_ally_rus_commissar_ppsh"}):
                self.assertEqual(zombie_idles_for_zombies(p, None, log=lambda msg: None), ["maps/dead_sand_zombiemode.gsc"])
                self.assertEqual(zombie_idles_for_zombies(p, None, log=lambda msg: None), [])  # once
        new = rawfile_text(node)
        self.assertIn(b'init_animscripts()\r\n{\r\n\tlevel thread t4ff_zombie_idles();\r\n\tanimscripts\\init::firstInit();\r\n\r\n'
                      b'\tanim.idleAnimArray\t\t["zombie_stand"] = [];\r\n\tanim.idleAnimWeights\t["zombie_stand"] = [];\r\n'
                      b'\tanim.idleAnimArray\t\t["zombie_stand"][0][0] \t= %ai_zombie_idle_v1_delta;\r\n\tanim.idleAnimWeights\t["zombie_stand"][0][0] \t= 10;\r\n'
                      b'\tanim.idleAnimArray\t\t["zombie_crouch"][0][0] \t= %ai_zombie_idle_crawl_delta;\r\n}\r\n', new)
        # the comment above it and the other functions stay
        self.assertIn(b'// anim.idleAnimArray["stand"][0][0]', new)
        self.assertIn(b'\tx = anim.idleAnimArray["stand"];\r\n}\r\n\r\n// t4ff: ', new)
        self.assertIn(b'ai[i].exception[ "stop_immediate" ] = ::t4ff_zombie_stop;\r\n', new)
        self.assertIn(b'\t\tsets = anim.idleAnimArray[ "zombie_" + pose ];\r\n', new)
        self.assertNotIn(b"\r\r", new)
        self.assertNotIn(b"\n\n\n", new.replace(b"\r", b""))

    def test_splitscreen_fog(self):
        """In splitscreen the game's _load.gsc gives a map without splitscreen fog yellow fog: the level
        script says the fog is set, and the map's own SetVolFog calls set the game's splitscreen fog
        there. A player's fog, the game's scripts, scripts with a splitscreen fog of their own and
        comments stay; a second run changes nothing."""
        from unittest import mock

        from t4ff.scripts import make_rawfile, rawfile_text, splitscreen_fog

        p, template = self._rawfile_template()
        level = b"main()\r\n{\r\n\tmaps\\createart\\mymap_art::main();\r\n}\r\n"
        art = (b"main()\r\n{\r\n\tlevel thread fog_settings();\r\n}\r\nfog_settings()\r\n{\r\n\tstart_dist = 440;\r\n"
               b"\t// SetVolFog( 0, 1, 2, 3, 0.5, 0.5, 0.5, 0 );\r\n"
               b"\tSetVolFog( start_dist, 3200, 225, 64, 0.533, 0.717, 1, 0 );\r\n"
               b"\tif( level.dark ) setVolFog(0, 500, 225, 64, 0, 0, 0, 2);\r\n\telse SetVolFog(0, 900, 225, 64, 0, 0, 0, 2);\r\n"
               b"\tplayers[i] SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n\tplayer SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n"
               b"\tget_players()[0]SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n\tget_player() SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n}")
        treyarch = (b"fog_settings()\n{\n\tif( IsSplitScreen() )\n\t\tmaps\\_utility::set_splitscreen_fog( 1, 2, 3, 4, 5, 6, 7, 0, 4000 );\n"
                    b"\telse\n\t\tSetVolFog( 1, 2, 3, 4, 5, 6, 7, 0 );\n}\n")
        game = b"fog()\n{\n\tSetVolFog( 1, 2, 3, 4, 5, 6, 7, 0.4 );\n}\n"
        files = [(n, make_rawfile(p, template, n, t)) for n, t in (("maps/mymap.gsc", level), ("maps/createart/mymap_art.gsc", art),
                                                                    ("maps/createart/other_art.gsc", treyarch), ("maps/_load.gsc", game))]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            self.assertEqual(splitscreen_fog(p, None, "maps/mymap.gsc", lambda name: name == "maps/_load.gsc", log=lambda msg: None),
                             ["maps/createart/mymap_art.gsc", "maps/mymap.gsc"])
            self.assertEqual(splitscreen_fog(p, None, "maps/mymap.gsc", lambda name: name == "maps/_load.gsc", log=lambda msg: None), [])  # once
        self.assertEqual(rawfile_text(files[0][1]), b"main()\r\n{\r\n\t// t4ff: in splitscreen, the map's fog is its own: no placeholder fog of "
                                                    b"maps/_load.gsc\r\n\tlevel.splitscreen_fog = true;\r\n\r\n\tmaps\\createart\\mymap_art::main();\r\n}\r\n")
        new = rawfile_text(files[1][1])
        self.assertIn(b"\t// SetVolFog( 0, 1, 2, 3, 0.5, 0.5, 0.5, 0 );\r\n\tt4ff_vol_fog( start_dist, 3200, 225, 64, 0.533, 0.717, 1, 0 );\r\n"
                      b"\tif( level.dark ) t4ff_vol_fog(0, 500, 225, 64, 0, 0, 0, 2);\r\n\telse t4ff_vol_fog(0, 900, 225, 64, 0, 0, 0, 2);\r\n"
                      b"\tplayers[i] SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n\tplayer SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n"
                      b"\tget_players()[0]SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n\tget_player() SetVolFog( 75, 200, 225, 64, 0, 0, 0, 0 );\r\n}\r\n\r\n"
                      b"// t4ff: the map's fog", new)
        self.assertIn(b"\t\tSetVolFog( start_dist, halfway_dist, halfway_height, base_height, red, green, blue, trans_time );\r\n", new)
        self.assertIn(b"\tmaps\\_utility::set_splitscreen_fog( start_dist, halfway_dist, halfway_height, base_height, red, green, blue, trans_time, cull_dist );\r\n", new)
        self.assertNotIn(b"\r\r", new)
        self.assertNotIn(b"\n\n\n", new.replace(b"\r", b""))
        self.assertEqual(rawfile_text(files[2][1]), treyarch)
        self.assertEqual(rawfile_text(files[3][1]), game)

    def test_valid_cursor_hints(self):
        """The console's SetCursorHint crashes the game on a hint type it has not (it lists the valid
        ones past the end of their table): Dead Sand's "HINT_NONE" becomes "HINT_NOICON"; valid ones,
        in any case, stay."""
        from unittest import mock

        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.scripts import make_rawfile, rawfile_text, valid_cursor_hints
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        battery = make_rawfile(p, template, "maps/_dead_sand_utility.gsc",
                               b'think()\r\n{\r\n\tself setcursorhint( "HINT_NONE" );\r\n\tself SetCursorHint("hint_activate");\r\n}\r\n')
        other = make_rawfile(p, template, "maps/other.gsc", b'f()\n{\n\tself setCursorHint( "HINT_NOICON" );\n}\n')
        files = [("maps/_dead_sand_utility.gsc", battery), ("maps/other.gsc", other)]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            self.assertEqual(valid_cursor_hints(p, None, log=lambda msg: None), ["maps/_dead_sand_utility.gsc"])
            self.assertEqual(valid_cursor_hints(p, None, log=lambda msg: None), [])
        self.assertEqual(rawfile_text(battery), b'think()\r\n{\r\n\tself setcursorhint( "HINT_NOICON" );\r\n\tself SetCursorHint("hint_activate");\r\n}\r\n')
        self.assertEqual(rawfile_text(other), b'f()\n{\n\tself setCursorHint( "HINT_NOICON" );\n}\n')

    def test_mod_scripts_over_the_games(self):
        """The console runs the game's own copy of a script over the map's, the PC the mod's: the
        mod's zone manager (the DLC3 kit's) gets a name of its own and the map calls it by that
        name, not Der Riese's of the console's patch.ff. A script the game's own scripts call too
        (maps/_load.gsc calls maps/_laststand.gsc) and a script the engine runs stay the game's."""
        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.scripts import _rawfiles, keep_mod_scripts, make_rawfile, normalize, rawfile_text, zone_of_assets
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Reader, Writer

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        game = {
            "maps/_zombiemode_zone_manager.gsc": b'manage_zones(z)\n{\n\tlevel.zones["receiver_zone"].is_active = true;\n}\n',
            "maps/_load.gsc": b"main()\n{\n\tmaps\\_laststand::init();\n}\n",
            "maps/_laststand.gsc": b"init()\n{\n}\n",
            "animscripts/death.gsc": b"main()\n{\n}\n",
            "maps/_mgturret.gsc": b"init()\n{\n}\n",
        }

        class Library:
            def find_in_game_zones(self, rec_name, name):
                return (None, make_rawfile(p, template, name, game[name])) if name in game else None

        level = (b"#include maps\\_zombiemode_zone_manager;\nmain()\n{\n\tmaps\\_load::main();\n\tmaps\\_mgturret::init();\n"
                 b'\tlevel thread maps\\_zombiemode_zone_manager::manage_zones("start_zone");\n'
                 b'\tlevel thread Maps/_Zombiemode_Zone_Manager ::manage_zones("start_zone");\n'
                 b"\tlevel thread maps\\_zombiemode_zone_manager_other::f();\n\tanimscripts\\death::main();\n}\n")
        scripts = {
            "maps/testmap.gsc": level,
            "maps/_zombiemode_zone_manager.gsc": b"manage_zones(z)\n{\n\tlevel.zones[z].is_active = true;\n}\n",
            "maps/_laststand.gsc": b"init()\n{\n\tlevel.mod = 1;\n}\n",
            "animscripts/death.gsc": b"main()\n{\n\tmod();\n}\n",
            "maps/_mgturret.gsc": b"init()\r\n{\r\n}\r\n",  # the same apart from white space
        }
        zone = zone_of_assets(p, [("rawfile", name, make_rawfile(p, template, name, text)) for name, text in scripts.items()])
        logs = []
        renamed = keep_mod_scripts(p, zone, set(scripts) - {"maps/testmap.gsc"}, Library(), "maps/testmap.gsc", log=logs.append)
        self.assertEqual(renamed, {"maps/_zombiemode_zone_manager.gsc": "maps/_zombiemode_zone_manager_mod.gsc"})
        texts = {normalize(name): rawfile_text(node) for name, node in _rawfiles(p, Reader(p, Writer(p).write(zone)).load())}
        self.assertEqual(sorted(texts), sorted(set(scripts) - {"maps/_zombiemode_zone_manager.gsc"} | {"maps/_zombiemode_zone_manager_mod.gsc"}))
        self.assertEqual(texts["maps/_zombiemode_zone_manager_mod.gsc"], scripts["maps/_zombiemode_zone_manager.gsc"])
        self.assertEqual(texts["maps/testmap.gsc"], level.replace(b"maps\\_zombiemode_zone_manager;", b"maps\\_zombiemode_zone_manager_mod;")
                         .replace(b"maps\\_zombiemode_zone_manager::", b"maps\\_zombiemode_zone_manager_mod::")
                         .replace(b"Maps/_Zombiemode_Zone_Manager ::", b"maps\\_zombiemode_zone_manager_mod ::"))
        self.assertEqual(texts["maps/_laststand.gsc"], scripts["maps/_laststand.gsc"])
        self.assertEqual(len(logs), 3, logs)
        self.assertTrue(any("maps/_laststand.gsc" in line and "maps/_load.gsc" in line for line in logs), logs)
        self.assertTrue(any("animscripts/death.gsc" in line and "engine" in line for line in logs), logs)
        # nothing else to do the second time
        self.assertEqual(keep_mod_scripts(p, zone, set(scripts), Library(), "maps/testmap.gsc", log=lambda msg: None), {})

    def test_script_structs_spawn_as_script_origins(self):
        """The console cannot spawn script_struct entities (The Simpsons' rocket barrage links one to
        each rocket): they are spawned as script_origin; structs made otherwise are left alone."""
        from unittest import mock

        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.scripts import make_rawfile, rawfile_text, spawn_script_origins
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        rocket = make_rawfile(p, template, "maps/artillery.gsc", b'fire_rocket()\r\n{\r\n\tsound_struct = spawn("script_struct", self.origin);\r\n\ts = Spawn( "script_struct",o );\r\n\tsound_struct linkTo (self);\r\n}\r\n')
        other = make_rawfile(p, template, "maps/_utility.gsc", b'f()\n{\n\tstruct = spawnStruct();\n\tstructs = getstructarray("script_struct", "classname");\n}\n')
        files = [("maps/artillery.gsc", rocket), ("maps/_utility.gsc", other)]
        with mock.patch("t4ff.scripts._rawfiles", lambda p, z: files):
            self.assertEqual(spawn_script_origins(p, None, log=lambda msg: None), ["maps/artillery.gsc"])
            self.assertEqual(spawn_script_origins(p, None, log=lambda msg: None), [])
        self.assertEqual(rawfile_text(rocket), b'fire_rocket()\r\n{\r\n\tsound_struct = spawn("script_origin", self.origin);\r\n\ts = Spawn( "script_origin",o );\r\n\tsound_struct linkTo (self);\r\n}\r\n')
        self.assertIn(b'getstructarray("script_struct", "classname")', rawfile_text(other))


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

            # maps t4ff converted, in the same usermaps folder, are left out (an earlier conversion
            # of the map would come first and hand its old copies back), and so is the output folder
            from t4ff.library import T4FF_MARKER

            simpsons = os.path.join(t4, "usermaps", "simpsons")
            os.makedirs(simpsons)
            for n in ("simpsons.ff", "simpsons_load.ff", T4FF_MARKER):
                open(os.path.join(simpsons, n), "wb").close()
            skipped = []
            self.assertEqual(short(library_files([t4], "simpsons", skipped=skipped)), everything)
            self.assertEqual(short(skipped), ["usermaps/simpsons/simpsons.ff", "usermaps/simpsons/simpsons_load.ff"])
            os.remove(os.path.join(simpsons, T4FF_MARKER))  # converted before the marker: the output folder
            self.assertEqual(short(library_files([t4], "simpsons", [simpsons + os.sep])), everything)
            self.assertEqual(short(library_files([t4], "simpsons"))[0], "usermaps/simpsons/simpsons.ff")  # what the marker prevents


class TechsetTests(unittest.TestCase):
    # the sun lit pass of mc_l_sm_b0c0d0n0s0 in CoD Xenon's maps (type, dest, value), and the
    # counts of its sections: the dynamic shadow texture (code sampler 0x12) is a stable argument
    XENON_LIT_SUN = [
        (3, 0x4, 0x6B0004), (3, 0x8, 0x3B0001),
        (3, 0x0, 0x7B0004), (3, 0x11, 0x350001), (3, 0x12, 0x360001),
        (2, 0x7, 0x34ECCCB3), (2, 0x6, 0x59D30D0F), (2, 0x0, 0xA0AB1041), (2, 0x8, 0xEB529B4D), (3, 0x15, 0x2A0001),
        (3, 0x3E, 0x5E0001), (4, 0x4, 0x3), (4, 0x5, 0x12), (5, 0x0, 0x2B0001), (5, 0x5, 0x270001), (5, 0x11, 0x240001),
        (5, 0x12, 0x250001), (5, 0x13, 0x260001), (6, 0x7, 0x8D36A09), (6, 0x6, 0x3D9994DC),
    ]  # fmt: skip

    def test_dynamic_shadow_texture_per_object(self):
        """The game sets the dynamic shadow texture before the per object arguments of the models
        and surfaces it draws lit, and nowhere else: its own sun lit passes read it per object
        (the same pass of mc_l_sm_b0c0d0n0s0_sco on the disc: 2, 4 and 14 arguments, the texture
        last of the per object ones). Stable, it is read before any object set it (The Simpsons'
        "Tried to use '(null)' when it isn't valid" at a window)."""
        from t4ff.platforms import x360
        from t4ff.techsets import sort_sections

        p = x360()
        args = [struct.pack(">HHI", *a) for a in self.XENON_LIT_SUN]
        new_args, counts = sort_sections(p, args, (2, 3, 15))
        self.assertEqual(counts, (2, 4, 14))
        game = self.XENON_LIT_SUN[:5] + [(4, 0x5, 0x12)] + self.XENON_LIT_SUN[5:12] + self.XENON_LIT_SUN[13:]
        self.assertEqual([struct.unpack(">HHI", a) for a in new_args], game)
        # as the game has it: nothing to move
        self.assertIsNone(sort_sections(p, new_args, counts))

    def test_sections_of_copied_technique_sets(self):
        """Every pass of CoD Xenon's Aztec reads its code arguments where the game's technique sets do."""
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import x360
        from t4ff.techsets import fix_argument_sections
        from t4ff.zone import Reader, Writer

        p = x360()
        _, _, data = read_fastfile(sample("x360", "nazi_zombie_aztec.ff"))
        zone = Reader(p, data).load()
        self.assertEqual(fix_argument_sections(p, zone, log=lambda msg: None), 8)
        self.assertEqual(fix_argument_sections(p, zone, log=lambda msg: None), 0)
        Reader(p, Writer(p).write(zone)).load()


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
            self.assertEqual(map_title(IwdLibrary([tmp]), "simpsons"), "Simpsons")  # the file name again: written like the others
            self.assertEqual(map_title(IwdLibrary([tmp]), "other_map"), "Other Map")
            # a localization key (Dead Sand's): the text of the map's localized string
            with open(os.path.join(tmp, "mod.arena"), "w") as f:
                f.write('{\n\tmap "nazi_zombie_dead_sand"\n\tlongname "MOD_LEVEL_ZOMBIE_DEAD_SAND"\n}\n')
            self.assertEqual(map_title(IwdLibrary([tmp]), "nazi_zombie_dead_sand"), "Nazi Zombie Dead Sand")
            self.assertEqual(map_title(IwdLibrary([tmp]), "nazi_zombie_dead_sand", {"MOD_LEVEL_ZOMBIE_DEAD_SAND": "^3Dead Sand"}), "Dead Sand")

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

    def test_script_menu_on_a_controller(self):
        """A PC script menu (a music box) answers the number keys and Escape: their actions also go
        to the controller's buttons and its labels name them. A menu without number keys stays."""
        import struct

        from t4ff.commands import find_field
        from t4ff.layout import TypeRef
        from t4ff.menu import gamepad_script_menus
        from t4ff.platforms import x360
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Zone, ZoneAsset

        p = x360()
        mrec, irec, krec = p.record("menuDef_t"), p.record("itemDef_s"), p.record("ItemKeyHandler")
        window_name = find_field(mrec, "window").offset + find_field(p.record("windowDef_t"), "name").offset

        def record(name):
            node = Node(TypeRef("record", name, p.record(name).size), 1, BLOCK_VIRTUAL)
            node.data = bytearray(p.record(name).size)
            return node

        def string(text):
            node = Node(TypeRef("scalar", "char", 1, 1), len(text) + 1, BLOCK_VIRTUAL)
            node.string = True
            node.data = bytearray(text.encode("latin-1") + b"\0")
            return node

        def point(owner, off, target, kind="follow"):
            ptr = Ptr(kind, target)
            ptr.owner, ptr.offset = owner, off
            owner.relocs[off] = ptr
            if kind == "follow":
                owner.children.append(target)
                struct.pack_into(">I", owner.data, off, 0xFFFFFFFF)

        def menu(name, keys, escape, texts):
            m = record("menuDef_t")
            point(m, window_name, string(name))
            if escape:
                point(m, find_field(mrec, "onESC").offset, string(escape))
            owner, off = m, find_field(mrec, "onKey").offset
            for key, action in keys:
                handler = record("ItemKeyHandler")
                struct.pack_into(">i", handler.data, find_field(krec, "key").offset, key)
                point(handler, find_field(krec, "action").offset, string(action))
                point(owner, off, handler)
                owner, off = handler, find_field(krec, "next").offset
            items = Node(TypeRef("pointer", "itemDef_s", 4, 4), len(texts), BLOCK_VIRTUAL)
            items.data = bytearray(4 * len(texts))
            for i, text in enumerate(texts):
                item = record("itemDef_s")
                point(item, find_field(irec, "text").offset, string(text))
                point(items, 4 * i, item)
            point(m, find_field(mrec, "items").offset, items)
            struct.pack_into(">i", m.data, find_field(mrec, "itemCount").offset, len(texts))
            return m

        music = menu("music_box", [(ord("1"), "respond 1"), (ord("2"), "respond 2")], "respond 9",
                     ["^11: ^4First song", "^12: ^4Second song", "Press ESC to close menu", "Made by 1 person"])
        other = menu("other", [(27, "close")], "close", ["Press ESC"])
        lists = Node(TypeRef("record", "MenuList"), 1, BLOCK_VIRTUAL)
        lists.children = [music, other]
        ptr = Ptr("follow", lists)
        zone = Zone(p.name, [], [ZoneAsset("menulist", ptr, "ui/scriptmenus/music.menu")], [], 0, 0, None, None)
        zone.extra_root = lists
        messages = []
        self.assertEqual(gamepad_script_menus(p, zone, log=messages.append), ["music_box"])
        self.assertEqual(messages, ["menu music_box: controller buttons for its keys: A (1), X (2), B (Escape)"])

        def handlers(m):
            out, ptr = [], m.relocs.get(find_field(mrec, "onKey").offset)
            while ptr is not None and ptr.kind != "null":
                h = ptr.target()
                action = h.relocs[find_field(krec, "action").offset].target()
                out.append((struct.unpack_from(">i", h.data, 0)[0], bytes(action.data).rstrip(b"\0").decode()))
                ptr = h.relocs.get(find_field(krec, "next").offset)
            return out

        def texts(m):
            items = m.relocs[find_field(mrec, "items").offset].target()
            return [bytes(items.relocs[4 * i].target().relocs[find_field(irec, "text").offset].target().data).rstrip(b"\0").decode()
                    for i in range(items.count)]

        self.assertEqual(handlers(music), [(49, "respond 1"), (50, "respond 2"), (1, "respond 1"), (3, "respond 2"), (2, "respond 9")])
        self.assertEqual(texts(music), ["^1A: ^4First song", "^1X: ^4Second song", "Press B to close menu", "Made by 1 person"])
        self.assertEqual(handlers(other), [(27, "close")])
        self.assertEqual(texts(other), ["Press ESC"])

    def test_inert_menu_items_are_decorations(self):
        """Mini-Labor's weapon choice puts a background under each button, its ACCEPT's at the same
        place and before it: the console's focus stopped on them and ACCEPT could not be pressed.
        Items that do nothing become decorations; the buttons, items already decorations and menus
        with nothing to press stay as they are."""
        import struct

        from t4ff.commands import find_field
        from t4ff.layout import TypeRef
        from t4ff.menu import WINDOW_DECORATION, decorate_inert_items
        from t4ff.platforms import x360
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Zone

        p = x360()
        mrec, irec, wrec = p.record("menuDef_t"), p.record("itemDef_s"), p.record("windowDef_t")
        window_name = find_field(mrec, "window").offset + find_field(wrec, "name").offset
        flags_off = find_field(irec, "window").offset + find_field(wrec, "staticFlags").offset

        def record(name):
            node = Node(TypeRef("record", name, p.record(name).size), 1, BLOCK_VIRTUAL)
            node.data = bytearray(p.record(name).size)
            return node

        def string(text):
            node = Node(TypeRef("scalar", "char", 1, 1), len(text) + 1, BLOCK_VIRTUAL)
            node.string = True
            node.data = bytearray(text.encode("latin-1") + b"\0")
            return node

        def point(owner, off, target):
            owner.relocs[off] = Ptr("follow", target)
            owner.children.append(target)
            struct.pack_into(">I", owner.data, off, 0xFFFFFFFF)

        def menu(name, specs):  # (type, action, static flags)
            m = record("menuDef_t")
            point(m, window_name, string(name))
            items = Node(TypeRef("pointer", "itemDef_s", 4, 4), len(specs), BLOCK_VIRTUAL)
            items.data = bytearray(4 * len(specs))
            for i, (item_type, action, flags) in enumerate(specs):
                item = record("itemDef_s")
                struct.pack_into(">i", item.data, find_field(irec, "type").offset, item_type)
                struct.pack_into(">I", item.data, flags_off, flags)
                if action:
                    point(item, find_field(irec, "action").offset, string(action))
                point(items, 4 * i, item)
            point(m, find_field(mrec, "items").offset, items)
            struct.pack_into(">i", m.data, find_field(mrec, "itemCount").offset, len(specs))
            return m

        def flags(m):
            items = m.relocs[find_field(mrec, "items").offset].target()
            return [struct.unpack_from(">I", items.relocs[4 * i].target().data, flags_off)[0] for i in range(items.count)]

        loadout = menu("loadout", [(0, None, 0), (1, '"scriptMenuResponse" "accept"', 0), (0, None, 0),
                                   (1, '"scriptMenuResponse" 1', 0), (0, None, WINDOW_DECORATION | 0x800000), (8, None, 0)])
        hud = menu("hud", [(0, None, 0), (1, None, 0)])
        root = Node(TypeRef("record", "MenuList"), 1, BLOCK_VIRTUAL)
        root.children = [loadout, hud]
        zone = Zone(p.name, [], [], [], 0, 0, None, None)
        zone.extra_root = root
        messages = []
        self.assertEqual(decorate_inert_items(p, zone, log=messages.append), {"loadout": 2})
        self.assertEqual(flags(loadout), [WINDOW_DECORATION, 0, WINDOW_DECORATION, 0, WINDOW_DECORATION | 0x800000, 0])
        self.assertEqual(flags(hud), [0, 0])  # nothing to press: left alone
        self.assertEqual(len(messages), 1)
        self.assertEqual(decorate_inert_items(p, zone, log=lambda msg: None), {})  # once

    def test_menu_grid_on_a_controller(self):
        """Mini-Labor's weapon choice: buttons in a grid under an ACCEPT. Each button gets D-pad and
        stick handlers giving the focus to the nearest button that way, unnamed buttons a name for
        them; the choices (they set a local variable and answer the scripts) follow the focus, and A
        on one confirms it. Menus the scripts do not name are left alone."""
        import struct

        from t4ff.commands import find_field
        from t4ff.layout import TypeRef
        from t4ff.menu import controller_navigation
        from t4ff.platforms import x360
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Zone

        p = x360()
        mrec, irec, wrec, krec = p.record("menuDef_t"), p.record("itemDef_s"), p.record("windowDef_t"), p.record("ItemKeyHandler")
        window = find_field(irec, "window").offset
        name_off, rect_off = window + find_field(wrec, "name").offset, window + find_field(wrec, "rect").offset
        action_off, focus_off, key_off = (find_field(irec, f).offset for f in ("action", "onFocus", "onKey"))

        def record(name):
            node = Node(TypeRef("record", name, p.record(name).size), 1, BLOCK_VIRTUAL)
            node.data = bytearray(p.record(name).size)
            return node

        def string(text):
            node = Node(TypeRef("scalar", "char", 1, 1), len(text) + 1, BLOCK_VIRTUAL)
            node.string = True
            node.data = bytearray(text.encode("latin-1") + b"\0")
            return node

        def point(owner, off, target):
            owner.relocs[off] = Ptr("follow", target)
            owner.children.append(target)
            struct.pack_into(">I", owner.data, off, 0xFFFFFFFF)

        def text(node, off):
            ptr = node.relocs.get(off)
            return bytes(ptr.target().data).rstrip(b"\0").decode() if ptr is not None and ptr.kind != "null" else None

        def menu(name, specs):  # (x, y, action)
            m = record("menuDef_t")
            point(m, find_field(mrec, "window").offset + find_field(wrec, "name").offset, string(name))
            items = Node(TypeRef("pointer", "itemDef_s", 4, 4), len(specs), BLOCK_VIRTUAL)
            items.data = bytearray(4 * len(specs))
            for i, (x, y, action) in enumerate(specs):
                item = record("itemDef_s")
                struct.pack_into(">i", item.data, find_field(irec, "type").offset, 1)
                struct.pack_into(">4f", item.data, rect_off, x, y, 100, 50)
                point(item, action_off, string(action))
                point(items, 4 * i, item)
            point(m, find_field(mrec, "items").offset, items)
            struct.pack_into(">i", m.data, find_field(mrec, "itemCount").offset, len(specs))
            return m, [items.relocs[4 * i].target() for i in range(len(specs))]

        accept = '"scriptMenuResponse" "accept" ; "setLocalVarBool" "done" 1 ; '
        choice = '"scriptMenuResponse" {0} ; "setLocalVarInt" "choice" {0} ; '
        loadout, items = menu("loadout", [(300, 0, accept), (0, 100, choice.format(1)), (200, 100, choice.format(2)),
                                          (0, 200, choice.format(3)), (200, 200, choice.format(4))])
        other, other_items = menu("other", [(0, 0, '"close" "self"'), (200, 0, '"close" "self"')])
        root = Node(TypeRef("record", "MenuList"), 1, BLOCK_VIRTUAL)
        root.children = [loadout, other]
        zone = Zone(p.name, [], [], [], 0, 0, None, None)
        zone.extra_root = root
        self.assertEqual(controller_navigation(p, zone, log=lambda msg: None, script_menus={"loadout"}), {"loadout": 5})
        names = [text(item, name_off) for item in items]
        self.assertEqual(names, [f"t4ff_focus_{i}" for i in range(5)])

        def moves(item):
            out, ptr = {}, item.relocs.get(key_off)
            while ptr is not None and ptr.kind != "null":
                h = ptr.target()
                out[struct.unpack_from(">i", h.data, 0)[0]] = text(h, find_field(krec, "action").offset)
                ptr = h.relocs.get(find_field(krec, "next").offset)
            return out

        def target(item, key):
            return moves(item)[key].split('"')[3]

        up, down, left, right = 20, 21, 22, 23
        first = items[1]
        self.assertEqual((target(first, up), target(first, down), target(first, left), target(first, right)),
                         ("t4ff_focus_0", "t4ff_focus_3", "t4ff_focus_1", "t4ff_focus_2"))
        self.assertEqual(target(items[4], up), "t4ff_focus_2")
        self.assertEqual(target(items[0], down), "t4ff_focus_2")  # ACCEPT: to the button below it
        self.assertEqual(sorted(moves(first)), [20, 21, 22, 23, 28, 29, 30, 31])  # the stick too
        # the choices follow the focus, A confirms
        self.assertEqual(text(first, focus_off), choice.format(1))
        self.assertEqual(text(first, action_off), accept)
        self.assertEqual(text(items[0], action_off), accept)
        self.assertIsNone(text(items[0], focus_off))
        # a menu the scripts do not name
        self.assertIsNone(other_items[0].relocs.get(key_off))

    def test_mod_options_for_the_custom_maps_menu(self):
        """PhilMod's main menu chooses its difficulty (a multiple choice item bound to philmod_gamemode,
        which the level script reads; the menu sets 2 on open): options.txt lists it for CoD Xe's
        Custom Maps menu, with the label of its row in another menu ("Difficulty:"). Choices bound to
        dvars no script reads are left out."""
        import os
        import struct
        import tempfile

        from t4ff.commands import find_field
        from t4ff.layout import TypeRef
        from t4ff.menu import menu_options, write_options
        from t4ff.platforms import pc
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr, Zone

        p = pc()
        mrec, irec, wrec, mdef = p.record("menuDef_t"), p.record("itemDef_s"), p.record("windowDef_t"), p.record("multiDef_s")
        rect_off = find_field(irec, "window").offset + find_field(wrec, "rect").offset

        def record(name):
            node = Node(TypeRef("record", name, p.record(name).size), 1, BLOCK_VIRTUAL)
            node.data = bytearray(p.record(name).size)
            return node

        def string(text):
            node = Node(TypeRef("scalar", "char", 1, 1), len(text) + 1, BLOCK_VIRTUAL)
            node.string = True
            node.data = bytearray(text.encode("latin-1") + b"\0")
            return node

        def point(owner, off, target):
            owner.relocs[off] = Ptr("follow", target)
            owner.children.append(target)

        def item(item_type, y, text=None, dvar=None, choices=()):
            it = record("itemDef_s")
            struct.pack_into("<i", it.data, find_field(irec, "type").offset, item_type)
            struct.pack_into("<4f", it.data, rect_off, 30, y, 200, 22)
            if text:
                point(it, find_field(irec, "text").offset, string(text))
            if dvar:
                point(it, find_field(irec, "dvar").offset, string(dvar))
            if choices:
                multi = record("multiDef_s")
                for k, (name, value) in enumerate(choices):
                    point(multi, find_field(mdef, "dvarList").offset + 4 * k, string(name))
                    struct.pack_into("<f", multi.data, find_field(mdef, "dvarValue").offset + 4 * k, value)
                struct.pack_into("<i", multi.data, find_field(mdef, "count").offset, len(choices))
                point(it, find_field(irec, "typeData").offset, multi)
            return it

        def menu(items):
            m = record("menuDef_t")
            array = Node(TypeRef("pointer", "itemDef_s", 4, 4), len(items), BLOCK_VIRTUAL)
            array.data = bytearray(4 * len(items))
            for i, it in enumerate(items):
                point(array, 4 * i, it)
            point(m, find_field(mrec, "items").offset, array)
            struct.pack_into("<i", m.data, find_field(mrec, "itemCount").offset, len(items))
            return m

        difficulty = [("Easy", 0), ("Medium", 1), ("Default", 2), ("Insane", 3), ("Overkill", 4)]
        main = menu([item(12, 138, dvar="philmod_gamemode", choices=difficulty), item(12, 170, dvar="unread_option", choices=[("A", 0), ("B", 1)])])
        lobby = menu([item(0, 300, text="Difficulty:"), item(12, 300, dvar="philmod_gamemode", choices=difficulty), item(0, 340, text="Other:")])
        root = Node(TypeRef("record", "MenuList"), 1, BLOCK_VIRTUAL)
        root.children = [main, lobby]
        zone = Zone(p.name, [], [], [], 0, 0, None, None)
        zone.extra_root = root
        options = menu_options(p, [zone], {"philmod_gamemode"}, {"philmod_gamemode": {"2"}})
        self.assertEqual(options, [{"dvar": "philmod_gamemode", "label": "Difficulty", "default": "2",
                                    "choices": [("0", "Easy"), ("1", "Medium"), ("2", "Default"), ("3", "Insane"), ("4", "Overkill")]}])
        with tempfile.TemporaryDirectory() as out:
            path = write_options(options, out, log=lambda msg: None)
            with open(path, encoding="latin-1") as f:
                self.assertEqual(f.read(), "option philmod_gamemode 2 Difficulty\nchoice 0 Easy\nchoice 1 Medium\nchoice 2 Default\n"
                                           "choice 3 Insane\nchoice 4 Overkill\n")
            self.assertIsNone(write_options([], out, log=lambda msg: None))  # none: an earlier one goes
            self.assertFalse(os.path.exists(path))

    def test_dynamic_map_list(self):
        """The Nazi Zombies map list of CoD Xenon's patch_ui.ff: the stock rows stay, the 13 rows of
        their maps become one Custom Maps row opening the Custom Maps menu: 13 rows showing dvars
        (CoD Xe fills them from the usermaps folder), with scroll catchers, a counter, the preview of
        the focused map and LB / RB paging."""
        import contextlib
        import io
        import shutil
        import struct

        from t4ff.__main__ import main
        from t4ff.fastfile import read_fastfile
        from t4ff.menu import COUNTER_DX, MENU_LIST, PREVIEW_SLOT, USERMAPS_MENU, USERMAPS_TITLE, MenuEditor
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
            # a menu that is not CoD Xenon's (made already, or another CoD Xe menu) is refused, and
            # not kept as the original
            other = os.path.join(tmp, "other")
            os.makedirs(os.path.join(other, "zone"))
            shutil.copy(os.path.join(tmp, "zone", "patch_ui.ff"), os.path.join(other, "zone"))
            for orig in (False, True):
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(main(["--no-install", "menu", other]), 1)
                self.assertIn("already has a Custom Maps menu", out.getvalue())
                self.assertEqual(os.path.exists(os.path.join(other, "zone", "patch_ui.ff.orig")), orig)
                self.assertEqual("and delete" in out.getvalue(), orig)
                if not orig:  # then as the kept original
                    os.replace(os.path.join(other, "zone", "patch_ui.ff"), os.path.join(other, "zone", "patch_ui.ff.orig"))
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
            levels = MenuEditor(p, zone)
            actions = [levels.string(item, "action") or "" for item in levels.items]
            self.assertEqual(sorted(a.split("devmap ")[1].split('"')[0] for a in actions if "devmap" in a), sorted(["nazi_zombie_asylum", "nazi_zombie_factory", "nazi_zombie_prototype", "nazi_zombie_sumpf"]))
            custom = [item for item in levels.items if levels.string(item, "text") == USERMAPS_TITLE]
            self.assertEqual(len(custom), 1)
            self.assertIn(f'"open" "{USERMAPS_MENU}"', levels.string(custom[0], "action"))
            self.assertAlmostEqual(levels.rect(custom[0])[1], 134)
            self.assertNotRegex(levels.string(custom[0], "onFocus"), r'"show"\s+"image_')  # no map picture
            self.assertFalse(any((levels.string(item, "window.name") or "").startswith("codxe_map") for item in levels.items))
            # the Custom Maps menu, loaded by the game with the menus of ui/patch_menus.txt
            names = [a.name for a in zone.assets]
            self.assertEqual(names.index(USERMAPS_MENU), names.index("levels_unlock") + 1)
            listed = next(a for a in zone.assets if a.name == MENU_LIST).ptr.node
            menus = listed.relocs[8].node
            self.assertEqual(struct.unpack_from(">i", listed.data, 4)[0], menus.count)
            self.assertIs(menus.relocs[4 * (menus.count - 1)].target(), next(a for a in zone.assets if a.name == USERMAPS_MENU).ptr.node)
            editor = MenuEditor(p, zone, USERMAPS_MENU)
            self.assertEqual([editor.string(item, "text") for item in editor.items].count(USERMAPS_TITLE), 1)
            self.assertFalse(any("devmap" in (editor.string(item, "action") or "") for item in editor.items))
            actions = [editor.string(item, "action") or "" for item in editor.items]
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
                self.assertAlmostEqual(editor.rect(item)[1], 54 + 20 * k)
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
            self.assertIn(f'"close" "{USERMAPS_MENU}" ; \0'.encode(), handlers[2])  # Back
            self.assertIn(b'"close" "levels_unlock" ; \0', handlers[2])


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

    def test_cube_maps(self):
        """Cube IWIs (skies) store the six faces of every level, smallest level first; the console
        stores the base level of every face, then every mip level of every face, each 4 KiB aligned."""
        import struct

        rng = np.random.default_rng(3)
        w = 128
        sizes = [images.level_size("DXT1", w >> i, w >> i) * 6 for i in range(images.mip_count(w, w))]
        levels = [rng.integers(0, 256, size, dtype=np.uint8).tobytes() for size in sizes]
        data = b"IWi\x06" + bytes([0x0B, images.IWI_FLAG_CUBEMAP]) + struct.pack("<3H", w, w, 1) + bytes(16) + b"".join(reversed(levels))
        cube = images.parse_iwi("sky_ft", data)
        self.assertEqual((cube.faces, cube.levels), (6, levels))
        self.assertEqual(cube.face(2).levels[1], levels[1][len(levels[1]) // 6 * 2 : len(levels[1]) // 6 * 3])

        tex = images.build_console_texture(cube)
        self.assertEqual((tex.faces, tex.format.name, tex.levels), (6, "DXT1", 6))  # down to 4x4
        fetch = struct.unpack_from("<6I", tex.header, 28)
        self.assertEqual((fetch[5] >> 9) & 3, xenos.GPUDIMENSION_CUBEMAP)
        self.assertEqual(fetch[5] >> 12, tex.base_size >> 12)  # the mips follow the six base levels
        self.assertEqual(tex.base_size, 6 * xenos.level_layout(w, w, 0, tex.format)[4])
        self.assertEqual(len(tex.pixels), images.console_texture_size(cube))
        self.assertEqual(xenos.untile_mip_chain(tex.pixels, w, w, tex.format, tex.levels, 6), levels[: tex.levels])
        # small ones (reflection probes) stay uncompressed, as the game's own
        probe = images.ImageData("*reflection_probe0", "A8R8G8B8", 64, 64, [bytes(64 * 64 * 4 * 6)], faces=6)
        self.assertEqual(images.build_console_texture(probe).format.name, "A8R8G8B8")

    def test_normal_maps_become_dxn(self):
        """PC normal maps keep x in alpha and y in green (DXT5: a grey colour block); the console's
        are DXN, x then y, which its shaders read."""
        from t4ff import dxt

        w, h = 64, 32
        yy, xx = np.mgrid[0:h, 0:w]
        x = (xx * 4 % 256).astype(np.uint8)
        y = (255 - yy * 8 % 256).astype(np.uint8)
        pc = np.stack([y, y, y, x], axis=2)  # grey y, x in alpha
        levels, rgba = [], pc
        while True:
            levels.append(dxt.encode(rgba, "DXT5"))
            if min(rgba.shape[:2]) <= 4:
                break
            rgba = dxt.downscale(rgba)
        image = images.ImageData("brick_n", "DXT5", w, h, levels)
        dxn = images.normal_map_to_dxn(image)
        self.assertEqual((dxn.format, len(dxn.levels)), ("DXN", len(levels)))
        self.assertEqual([len(level) for level in dxn.levels], [len(level) for level in levels])
        decoded = dxt.decode(dxn.levels[0], w, h, "DXN").astype(int)
        source = dxt.decode(levels[0], w, h, "DXT5").astype(int)
        self.assertTrue(np.array_equal(decoded[:, :, 0], source[:, :, 3]))  # the alpha blocks are kept
        self.assertLessEqual(np.abs(decoded[:, :, 1] - source[:, :, 1]).max(), 12)
        self.assertLessEqual(np.abs(decoded[:, :, 0] - x.astype(int)).max(), 12)

        # uncompressed (BGRA): the same channels
        bgra = pc[:, :, [2, 1, 0, 3]].tobytes()
        flat = images.normal_map_to_dxn(images.ImageData("flat_n", "A8R8G8B8", w, h, [bgra]))
        decoded = dxt.decode(flat.levels[0], w, h, "DXN").astype(int)
        self.assertLessEqual(np.abs(decoded[:, :, 0] - x.astype(int)).max(), 12)
        self.assertLessEqual(np.abs(decoded[:, :, 1] - y.astype(int)).max(), 12)

        texture = images.build_console_texture(image, normal_map=True)
        self.assertEqual(texture.format.name, "DXN")
        self.assertEqual(images.console_texture_size(image, normal_map=True), images.console_texture_size(dxn))
        self.assertEqual(images.build_console_texture(image).format.name, "DXT5")  # not a normal map
        # the texture budget can scale down a DXN texture without mip levels
        single = images.reduce_image(images.ImageData("n", "DXN", w, h, dxn.levels[:1]), 1)
        self.assertEqual((single.width, single.height, len(single.levels[0])), (32, 16, images.level_size("DXN", 32, 16)))


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

    def test_swizzles_as_the_games_textures(self):
        """Fetch constant swizzles as the SDK's D3DFORMATs hold them in bits 18-29 (and CoD Xenon's
        textures have them): 8_8_8_8 reads PC BGRA bytes, L8 has an opaque alpha."""
        import struct

        for name, word3, d3d in [("DXT1", 0xD10, 0x1A200152), ("A8R8G8B8", 0xC14, 0x18280186), ("L8", 0x1400, 0x28000102), ("A8L8", 0x400, 0x0800014A)]:
            fmt = xenos.FORMATS[name]
            self.assertEqual(struct.unpack_from("<I", xenos.texture_header(64, 64, fmt, 1), 28 + 12)[0], word3, name)
            self.assertEqual((fmt.d3d, fmt.d3d >> xenos.D3DFMT_SWIZZLE_SHIFT), (d3d, word3 >> 1), name)
        self.assertEqual(xenos.format_of_d3d(0x2800014A).name, "A8L8")  # as older t4ff versions wrote it


def xma2_packets(lengths, block_packets=2, seed=1):
    """XMA2 packets holding frames of the given bit lengths laid out as xma2encode does: frames do
    not cross block boundaries (the rest of a block is padded with ones), the last bit of a frame is
    0 when no other frame starts in its packet, packet headers give the frame count and the offset
    of the first frame starting in the packet."""
    rng = np.random.default_rng(seed)
    payload_bits = (audio.XMA_PACKET_SIZE - 4) * 8
    block_bits = block_packets * payload_bits
    starts, pos, per_packet = [], 0, collections.Counter()
    for length in lengths:
        if per_packet[pos // payload_bits] >= 63:  # the frame count of a packet header has 6 bits
            pos += payload_bits - pos % payload_bits
        if pos % block_bits + length > block_bits:
            pos += block_bits - pos % block_bits
        starts.append(pos)
        per_packet[pos // payload_bits] += 1
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
        data, _ = xma2_packets([1600 + 37 * (i % 11) for i in range(40)], block_packets=32)
        stream = audio.XmaStream(44100, 1, 40 * 512, data, 20000)
        out = audio.write_sdns(stream)
        back = audio.read_sdns(out)
        self.assertEqual((back.rate, back.channels, back.samples), (44100, 1, 20000))
        self.assertEqual(back.data, audio.xma2_reblock(data)[0])

    def test_streams_in_the_games_layout(self):
        """Streams as the game's own: XMA2 in 4 KiB blocks (2 packets), the header listing the
        decoded samples at the end of every block. xma2encode's 64 KiB blocks without a table (CoD
        Xenon's maps, earlier conversions) play a split second: the frames are repacked, their bits
        unchanged."""
        lengths = [1500 + (i * 7919) % 1500 for i in range(300)] + [200] * 150  # silence: tiny frames
        data, frames = xma2_packets(lengths, block_packets=32)
        out, block_frames = audio.xma2_reblock(data)
        self.assertEqual(sum(block_frames), len(lengths))
        self.assertEqual(len(block_frames), -(-len(out) // (2 * audio.XMA_PACKET_SIZE)))
        new_frames = audio.xma_frames(out)
        self.assertEqual([length for _, length in new_frames], lengths)

        def frame_bits(buf, bit, length):
            source = np.unpackbits(np.frombuffer(buf, dtype=np.uint8))
            chunks, at = [], bit
            while length:
                take = min(length, audio.XMA_PACKET_SIZE * 8 - at % (audio.XMA_PACKET_SIZE * 8))
                chunks.append(source[at : at + take])
                length -= take
                at += take + 32
            return np.concatenate(chunks)[:-1]  # the last bit follows the packets

        for (a, la), (b, lb) in zip(frames, new_frames):
            self.assertTrue(np.array_equal(frame_bits(data, a, la), frame_bits(out, b, lb)))
        packet_bits = audio.XMA_PACKET_SIZE * 8
        per_packet = collections.Counter(bit // packet_bits for bit, _ in new_frames)
        self.assertLessEqual(max(per_packet.values()), 63)
        for bit, length in new_frames:  # no frame crosses a block
            start = bit // packet_bits // 2
            self.assertEqual(start, (bit + length + 32 - 1) // packet_bits // 2)
        for k in range(len(out) // audio.XMA_PACKET_SIZE):
            header = struct.unpack_from(">I", out, k * audio.XMA_PACKET_SIZE)[0]
            self.assertEqual((header >> 26, (header >> 8) & 7, header & 0xFF), (per_packet.get(k, 0), 1, 0))
            if k % 2 == 0:
                self.assertEqual((header >> 11) & 0x7FFF, 0)  # every block starts with a frame

        stream = audio.XmaStream(48000, 2, len(lengths) * 512, data, len(lengths) * 512 - 300)
        sdns = audio.write_sdns(stream)
        table = struct.unpack_from(f">{2 + len(block_frames)}I", sdns, 0x18)
        self.assertEqual(table[:2], (0, 0))
        self.assertEqual(list(table[2:]), [512 * sum(block_frames[: i + 1]) for i in range(len(block_frames))])
        self.assertEqual(struct.unpack_from(">I", sdns, 0x10)[0], len(lengths) * 512 - 300)

        # files of earlier conversions are rewritten, once
        old = audio.SDNS_MAGIC + struct.pack(">5I", 0, 48000, 2, len(lengths) * 512, len(data)) + bytes(0x1000 - 24) + data
        self.assertEqual(audio.upgrade_sdns(old)[0x1000:], out)
        self.assertIsNone(audio.upgrade_sdns(audio.upgrade_sdns(old)))
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "a", "sounds"))
            with open(os.path.join(tmp, "a", "sounds", "song.xma"), "wb") as f:
                f.write(old)
            self.assertEqual(audio.upgrade_stream_files(tmp), {"files": 1, "upgraded": 1, "failed": 0})
            self.assertEqual(audio.upgrade_stream_files(tmp), {"files": 1, "upgraded": 0, "failed": 0})

    def test_stock_streams_keep_their_layout(self):
        """The game's own stream files (its disc's .xma) come out of the writer byte for byte."""
        folder = sample("x360", "stock_sounds")
        if not os.path.isdir(folder):
            self.skipTest("no stock stream samples")
        names = sorted(n for n in os.listdir(folder) if n.endswith(".xma"))
        for name in names:
            with open(os.path.join(folder, name), "rb") as f:
                self.assertIsNone(audio.upgrade_sdns(f.read()), name)

    def test_streamed_sound_target(self):
        self.assertEqual(audio.streamed_sound_target("sound/eggs/para_egg.wav"), "sounds/eggs/para_egg.xma")

    def test_encoder_pipeline_matches_cod_xenon(self):
        """An encoder producing CoD Xenon's XMA packets yields CoD Xenon's SDNS file in the game's layout."""
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
            if sys.platform.startswith("win"):
                # Windows starts no script by its #! line: a batch file runs it with this Python
                launcher = fake + ".cmd"
                with open(launcher, "w") as f:
                    f.write(f'@"{sys.executable}" "{fake}" %*\n')
                fake = launcher
            encoder = audio.XmaEncoder(fake)
            pcm = audio.Pcm(44100, np.zeros((1000, 1), dtype=np.int16))
            # CoD Xenon's file, in the game's layout (its 64 KiB blocks stop after a split second)
            out = audio.write_sdns(encoder.encode(pcm))
            upgraded = audio.upgrade_sdns(reference)
            self.assertEqual(out[0x14:], upgraded[0x14:])
            self.assertEqual(struct.unpack_from(">I", out, 0x10)[0], 1000)

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

    def test_stock_streams_shipped(self):
        """Streamed sounds of the game's own that the map's aliases use (Der Riese's voices) are
        encoded from the PC game's files next to the map's own; those already there are kept and
        the others are reported."""
        import types

        from t4ff.assets import ship_stock_streams, streamed_sound_files
        from t4ff.commands import find_field
        from t4ff.images import IwdLibrary
        from t4ff.layout import TypeRef
        from t4ff.platforms import pc
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = pc()
        u = find_field(p.record("SoundFile"), "u").offset
        fn = u + find_field(p.record("StreamedSound"), "filename").offset
        sfn = p.record("StreamFileName")
        dir_off, name_off = fn + find_field(sfn, "dir").offset, fn + find_field(sfn, "name").offset

        def string(text):
            node = Node(TypeRef("scalar", "char", 1, 1), len(text) + 1, BLOCK_VIRTUAL)
            node.string = True
            node.data = bytearray(text.encode("latin-1") + b"\0")
            return node

        def sound_file(kind, directory, name):
            node = Node(TypeRef("record", "SoundFile", p.record("SoundFile").size), 1, BLOCK_VIRTUAL)
            node.data = bytearray(p.record("SoundFile").size)
            node.data[0] = kind
            node.extra["origin"] = ("member", "snd_alias_t", "soundFile")
            for off, text in ((dir_off, directory), (name_off, name)):
                node.relocs[off] = Ptr("follow", string(text))
                node.children.append(node.relocs[off].node)
            return node

        root = Node(TypeRef("record", "root"), 1, BLOCK_VIRTUAL)
        root.children = [
            sound_file(2, "voiceovers\\zombie\\dlc3\\plr0", "power_out_01.wav"),
            sound_file(2, "Voiceovers\\Zombie\\dlc3\\plr0", "POWER_OUT_01.wav"),  # the same file
            sound_file(2, "music_box", "bart2.wav"),
            sound_file(2, "sfx\\levels\\zombie\\chalk", "round_over.wav"),
            sound_file(1, "", "loaded.wav"),
        ]
        zone = types.SimpleNamespace(extra_root=root)
        self.assertEqual(streamed_sound_files(p, [zone]), ["music_box\\bart2.wav", "sfx\\levels\\zombie\\chalk\\round_over.wav",
                                                           "voiceovers\\zombie\\dlc3\\plr0\\power_out_01.wav"])

        class Encoder:
            available = True
            calls = 0

            def encode(self, pcm):
                Encoder.calls += 1
                return audio.XmaStream(pcm.rate, pcm.channels, pcm.frames, xma2_packets([2000] * 4)[0])

        with tempfile.TemporaryDirectory() as tmp:
            game, out = os.path.join(tmp, "main"), os.path.join(tmp, "out")
            wav = audio.write_wav(audio.Pcm(22050, np.zeros((100, 1), dtype=np.int16)))
            os.makedirs(os.path.join(game, "sound", "voiceovers", "zombie", "dlc3", "plr0"))
            with open(os.path.join(game, "sound", "voiceovers", "zombie", "dlc3", "plr0", "power_out_01.wav"), "wb") as f:
                f.write(wav)
            os.makedirs(os.path.join(out, "sounds", "music_box"))
            open(os.path.join(out, "sounds", "music_box", "bart2.xma"), "wb").close()  # the map's own
            messages = []
            stats = ship_stock_streams(p, [zone], IwdLibrary([game]), out, Encoder(), log=messages.append)
            self.assertEqual(stats["converted"], 1)
            shipped = os.path.join(out, "sounds", "voiceovers", "zombie", "dlc3", "plr0", "power_out_01.xma")
            with open(shipped, "rb") as f:
                self.assertEqual(audio.read_sdns(f.read()).rate, 22050)
            self.assertEqual(len(messages), 2)
            self.assertIn("1 of the game's own the map uses taken from the PC game's files", messages[0])
            self.assertIn("1 the map uses are not in its files nor in the PC game's files given", messages[1])
            self.assertIn("sfx/levels/zombie/chalk/round_over.wav", messages[1])
            # converting again keeps what is there
            ship_stock_streams(p, [zone], IwdLibrary([game]), out, Encoder(), log=lambda msg: None)
            self.assertEqual(Encoder.calls, 1)


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

    def test_reflection_probes_as_the_game(self):
        """The map's reflection probes (cube maps the PC zone holds, face after face) convert to the
        console's: the same image, load def, texture header and pixels as CoD Xenon's Aztec."""
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, Writer, asset_name

        def images_of(zone):
            return {asset_name(x360(), n): n for n in zone.extra_root.walk() if n.type.name == "GfxImage" and (n.extra.get("origin") or ("",))[0] == "asset"}

        def parts(image):
            load_def = next(c for c in image.walk() if c.type.name == "GfxImageLoadDef")
            header = next(c for c in image.walk() if c.type.name == "D3DBaseTexture360")
            pixels = next(c for c in image.walk() if c.extra.get("delayed"))
            # the image without its pointers: the load def is loaded here (retail inserts it)
            return bytes(image.data[:4] + image.data[8:24] + image.data[28:36]), bytes(load_def.data[:12]), bytes(header.data), bytes(pixels.data)

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        zone = ZoneConverter(Reader(pc(), data).load(), pc(), x360(), ConvertOptions(log=lambda msg: None)).convert()
        converted = images_of(Reader(x360(), Writer(x360()).write(zone)).load())
        _, _, data = read_fastfile(sample("x360", "nazi_zombie_aztec.ff"))
        retail = images_of(Reader(x360(), data).load())
        probes = [n for n in retail if n.startswith("*reflection_probe")]
        self.assertTrue(probes)
        for name in probes:
            self.assertEqual(parts(converted[name]), parts(retail[name]), name)

    def test_pointer_into_a_string_of_a_referenced_asset(self):
        """The PC linker stores equal strings once: napalmbloblight's display name points into
        napalmblob's. With napalmblob replaced by a reference (as assets the console has, or a cube
        map, are), napalmbloblight gets a copy of the string, loaded where its pointer is. The models
        and materials napalmblob loaded, that later weapons alias, load at the first of them."""
        from t4ff.convert import ConvertOptions, ZoneConverter
        from t4ff.fastfile import read_fastfile
        from t4ff.platforms import pc, x360
        from t4ff.zone import Reader, Writer, asset_name

        _, _, data = read_fastfile(sample("pc", "nazi_zombie_aztec.ff"))
        conv = ZoneConverter(Reader(pc(), data).load(), pc(), x360(), ConvertOptions(log=lambda msg: None))
        weapon_hook = conv.hooks.get("weapon")

        def hook(c, asset_type, node, name):
            if name == "napalmblob":
                return c.reference_asset(asset_type, node, name)
            return weapon_hook(c, asset_type, node, name) if weapon_hook else None

        conv.hooks["weapon"] = hook
        zone = conv.convert()
        weapons = {asset_name(x360(), n): n for n in zone.extra_root.walk() if n.type.name == "WeaponDef" and (n.extra.get("origin") or ("",))[0] == "asset"}
        light = weapons["napalmbloblight"]
        copies = [p.node for p in light.relocs.values() if p.kind == "follow" and p.node.string and bytes(p.node.data) == b"WEAPON_FIREBLOB\0"]
        self.assertEqual(len(copies), 1)
        # every string of it loads in the order of its pointers
        loaded = [light.relocs[off].node for off in sorted(light.relocs) if light.relocs[off].kind in ("follow", "insert")]
        self.assertEqual([c for c in light.children if c in loaded], loaded)
        inserted = {light.relocs[off].node.type.name for off in light.relocs if light.relocs[off].kind == "insert"}
        self.assertEqual(inserted, {"XModel", "Material"})
        # nothing points to data the zone does not load
        live = {id(n) for n in zone.extra_root.walk()}
        for n in zone.extra_root.walk():
            for p in n.relocs.values():
                if p.kind == "alias":
                    self.assertIn(id(p.slot.owner), live)
                elif p.kind in ("follow", "insert", "ref"):
                    self.assertIn(id(p.node), live)
        out = Writer(x360()).write(zone)
        self.assertEqual(Writer(x360()).write(Reader(x360(), out).load()), out)

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
            from t4ff.assets import map_stream_hash

            custom, hashes = [], {}
            for n in again.extra_root.walk():
                if n.type.name == "SoundFile" and n.data[0] == 2 and len(n.children) == 2:
                    directory, name = (bytes(c.data).rstrip(b"\0").decode() for c in n.children)
                    if directory.startswith("sounds\\"):
                        custom.append((directory, name))
                        # a hash of its own, as the game's streams
                        self.assertEqual(struct.unpack_from(">I", n.data, 4)[0], map_stream_hash(directory, name))
                        hashes.setdefault(map_stream_hash(directory, name), set()).add((directory, name))
            self.assertGreaterEqual(len(custom), stats["streamed"])
            self.assertTrue(all(len(v) == 1 for v in hashes.values()))
            files = {os.path.join(tmp, *directory.split("\\"), name + ".xma") for directory, name in custom}
            self.assertEqual(len(files), stats["streamed"])
            self.assertTrue(all(os.path.exists(f) for f in files))
            with open(next(iter(files)), "rb") as f:
                self.assertEqual(audio.read_sdns(f.read()).data, audio.xma2_reblock(packets)[0])
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
            # looping sounds stay loaded: a looping stream would hold a stream channel all along
            self.assertGreater(stats["looping"], 0)
            for node in again.extra_root.walk():
                if node.type.name == "snd_alias_t":
                    for i in range(node.count):
                        ptr = node.relocs.get(i * rec.size + file_off)
                        target = ptr.target() if ptr is not None and ptr.kind != "null" else None
                        if target is not None and struct.unpack_from(">I", node.data, i * rec.size + flags_off)[0] & 1:
                            self.assertFalse(target.data[0] == 2 and struct.unpack_from(">I", target.data, 4)[0] == 0)

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

    def test_anim_tree_animations(self):
        """The dogs' animations are named only by the dog anim tree (the PC game's own zones have
        them): they are added from the console fastfiles, those the map has are kept."""
        from t4ff.fastfile import read_fastfile
        from t4ff.library import ConsoleLibrary
        from t4ff.named import named_assets_zone
        from t4ff.platforms import x360
        from t4ff.scripts import _rawfiles, make_rawfile, zone_of_assets
        from t4ff.zone import Reader, asset_name

        tranzit = sample("v020", "_codxe", "t4", "usermaps", "zm_tranzit", "zm_tranzit.ff")
        library = ConsoleLibrary(x360(), [tranzit], log=lambda msg: None)
        _, _, data = read_fastfile(tranzit)
        template = next(node for name, node in _rawfiles(x360(), Reader(x360(), data).load()) if not name.startswith(","))
        tree = b"""attack_player_late : nonloopsync
{
\tgerman_shepherd_attack_player_late
}
german_shepherd_run // the run cycle
german_shepherd_traverse_up_40
not_an_animation_anywhere
"""
        zone = zone_of_assets(x360(), [("rawfile", "animtrees/dog.atr", make_rawfile(x360(), template, "animtrees/dog.atr", tree))])
        logs = []
        extra = named_assets_zone(x360(), zone, [], library, log=logs.append)
        added = sorted(asset_name(x360(), n) for n in extra.extra_root.walk() if n.type.name == "XAnimParts" and (n.extra.get("origin") or ("",))[0] == "asset")
        self.assertEqual(added, ["german_shepherd_attack_player_late", "german_shepherd_run", "german_shepherd_traverse_up_40"])
        self.assertTrue(any("of animtrees/dog.atr" in line for line in logs), logs)
        self.assertTrue(any("not_an_animation_anywhere" in line for line in logs), logs)

    def test_zombie_engine_scripts(self):
        """A zombie map made with the first mod tools (Dead Sand) has neither the client scripts the
        game loads by name for zombie maps: they are added, from the console fastfiles, even though no
        script names them; a map that has them keeps its own."""
        from t4ff.fastfile import read_fastfile
        from t4ff.library import ConsoleLibrary
        from t4ff.platforms import x360
        from t4ff.scripts import ZOMBIE_ENGINE_SCRIPTS, _rawfiles, make_rawfile, missing_scripts_zone, rawfile_text, zone_of_assets
        from t4ff.zone import Reader

        library = ConsoleLibrary(x360(), [sample("x360", "nazi_zombie_aztec.ff")], log=lambda msg: None)
        _, _, data = read_fastfile(sample("x360", "nazi_zombie_aztec.ff"))
        template = next(node for name, node in _rawfiles(x360(), Reader(x360(), data).load()) if not name.startswith(","))

        def zone(*names):
            return zone_of_assets(x360(), [("rawfile", n, make_rawfile(x360(), template, n, b"main() { maps\\_zombiemode_utility::init(); }")) for n in names])

        logs = []
        extra = missing_scripts_zone(x360(), zone("maps/dead_sand_zombiemode.gsc", "maps/_zombiemode_utility.gsc"), [], library, log=logs.append)
        added = {name: node for name, node in _rawfiles(x360(), extra)}
        self.assertTrue(set(ZOMBIE_ENGINE_SCRIPTS) <= set(added), logs)
        self.assertIn(b"sound_notify", rawfile_text(added["clientscripts/_callbacks.csc"]))
        self.assertTrue(any("the game loads it for zombie maps" in line for line in logs))
        # the map's own are kept; a map that is no zombie map gets none
        extra = missing_scripts_zone(x360(), zone("maps/_zombiemode_utility.gsc", *ZOMBIE_ENGINE_SCRIPTS), [], library, log=lambda msg: None)
        kept = {name for name, _ in _rawfiles(x360(), extra)} if extra is not None else set()
        self.assertFalse(set(ZOMBIE_ENGINE_SCRIPTS) & kept)
        self.assertIsNone(missing_scripts_zone(x360(), zone_of_assets(x360(), [("rawfile", "maps/mak.gsc", make_rawfile(x360(), template, "maps/mak.gsc", b"main() {}"))]), [], library, log=lambda msg: None))

    def test_level_script_from_the_maps_files(self):
        """PhilMod's maps keep every script in an .iwd: the level script and client script, which the
        engine loads by name and no script names, come from the map's files, and so does all they
        use; a script the game's zones have too comes from the map's files when they have it (as on
        PC), and is reported for keep_mod_scripts."""
        from t4ff.layout import TypeRef
        from t4ff.platforms import x360
        from t4ff.scripts import _rawfiles, make_rawfile, missing_scripts_zone, zone_of_assets
        from t4ff.zone import BLOCK_VIRTUAL, Node, Ptr

        p = x360()
        template = Node(TypeRef("record", "RawFile", p.record("RawFile").size), 1, BLOCK_VIRTUAL)
        template.data = bytearray(p.record("RawFile").size)
        buffer = Node(TypeRef("scalar", "char", 1, 1), 1, BLOCK_VIRTUAL)
        buffer.extra["origin"] = ("member", "RawFile", "buffer")
        buffer.segments = [(buffer.type, 1, 1, False)]
        template.relocs[8] = Ptr("follow", buffer)
        template.children = [buffer]
        files = {
            "maps/mymap.gsc": b"main()\n{\n\tmaps\\_load::main();\n\tmaps\\_phil_mod::init();\n\tmaps\\_other::go();\n}\n",
            "clientscripts/mymap.csc": b"main()\n{\n}\n",
            "maps/_load.gsc": b"main()\n{\n\tmaps\\_phil_extra::go();\n}\n",
            "maps/_phil_mod.gsc": b"init()\n{\n}\n",
            "maps/_phil_extra.gsc": b"go()\n{\n}\n",
        }

        class MapFiles:
            def read(self, name):
                return files.get(name)

        class Library:  # the game's zones have maps/_load.gsc and maps/_other.gsc
            def in_game_zones(self, asset_type, name):
                return name in ("maps/_load.gsc", "maps/_other.gsc")

            def find(self, asset_type, name):
                return None

        zone = zone_of_assets(p, [("rawfile", "maps/unrelated.gsc", make_rawfile(p, template, "maps/unrelated.gsc", b"main() {}"))])
        from_map, logs = set(), []
        extra = missing_scripts_zone(p, zone, [MapFiles()], Library(), log=logs.append,
                                     roots=("maps/mymap.gsc", "clientscripts/mymap.csc"), from_map=from_map)
        added = sorted(name for name, _ in _rawfiles(p, extra))
        self.assertEqual(added, ["clientscripts/mymap.csc", "maps/_load.gsc", "maps/_phil_extra.gsc", "maps/_phil_mod.gsc", "maps/mymap.gsc"])
        self.assertEqual(from_map, {"maps/_load.gsc"})
        self.assertTrue(any("maps/mymap.gsc (the game loads it by name" in line for line in logs), logs)
        # without the roots nothing names them, as before
        self.assertIsNone(missing_scripts_zone(p, zone, [MapFiles()], Library(), log=lambda msg: None))

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
