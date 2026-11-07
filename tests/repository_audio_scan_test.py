#!/usr/bin/env python3
"""Generated, disposable native-layout tests; no user checkout or media inputs."""

import hashlib
import importlib.util
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("repository_audio_scan", ROOT / "tools/repository_audio_scan.py")
discovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(discovery)


def put(root, name, data):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)
    return path


def snapshot(root):
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def tone_wav():
    pcm = struct.pack("<32h", *([0, 12000, 0, -12000] * 8))
    fmt = struct.pack("<HHIIHH", 1, 1, 32000, 64000, 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(pcm)) + pcm
    return b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks


def tone_aiff():
    pcm = struct.pack(">32h", *([0, 12000, 0, -12000] * 8))
    comm = struct.pack(">HIHHQ", 1, 32, 16, 16397, 32000 << 49)
    chunks = b"COMM" + struct.pack(">I", len(comm)) + comm
    chunks += b"SSND" + struct.pack(">I", len(pcm) + 8) + bytes(8) + pcm
    return b"FORM" + struct.pack(">I", len(chunks) + 4) + b"AIFF" + chunks


def sentinels(root):
    for name in ("levels/sentinel.bin", "build/us/sound/generated.aifc", ".git/private",
                 "assets/graphics/texture.bin", "preferences.ini", "project.duck"):
        put(root, name, b"unrelated\0sentinel\xff")


def sm64_fixture(root, hacker=False):
    put(root, "tools/assemble_sound.py", "# generated layout fixture\n")
    if hacker:
        put(root, "include/config/config_audio.h", "#define EXPAND_AUDIO_HEAP\n")
    bank = {"sample_bank": "instruments", "envelopes": {}, "instruments": {
        "piano": {"sound_lo": "shared", "sound": {"sample": "shared", "tuning": 0.75},
                  "sound_hi": "tail"},
        "percussion": [{"sound": "shared", "pan": 32}]}, "instrument_list": ["piano"]}
    put(root, "sound/sound_banks/00.json", json.dumps(bank))
    put(root, "sound/sound_banks/01_custom.json", json.dumps({
        **bank, "instruments": {"other": {"sound": "shared"}}}))
    put(root, "sound/sequences.json", json.dumps({"song_a": ["00"], "song_b": ["01_custom"]}))
    for name in ("shared", "tail", "custom_untracked"):
        put(root, f"sound/samples/instruments/{name}.aiff", tone_aiff())
    put(root, "sound/samples/instruments/shared.aiff.bkp", b"unrecorded original")
    put(root, "sound/sequences/us/song_a.m64", b"\xff\x00")
    put(root, "sound/sequences/us/song_b.m64", b"\xff\x01")
    put(root, "src/audio/data.c", "/* generated pool fixture */\n")
    sentinels(root)


def oot_fixture(root):
    put(root, "tools/audio/soundfont_compiler.c", "/* generated layout marker */\n")
    base = "extracted/ntsc-1.0/assets/audio/"
    put(root, base + "samples/SampleBank_0/shared.wav", tone_wav())
    put(root, base + "samples/SampleBank_0/tail.wav", tone_wav())
    put(root, base + "samplebanks/SampleBank_0.xml", """<SampleBank Name="SampleBank_0" Index="0">
        <Pointer Index="1"/>
        <Sample Name="SHARED" Path="$(BUILD_DIR)/assets/audio/samples/SampleBank_0/shared.aifc"/>
        <Sample Name="TAIL" Path="$(BUILD_DIR)/assets/audio/samples/SampleBank_0/tail.aifc"/>
    </SampleBank>""")
    font = """<Soundfont Name="Font" Index="{index}" SampleBank="$(BUILD_DIR)/assets/audio/samplebanks/SampleBank_0.xml">
        <Samples><Sample Name="SHARED" BaseNote="C4"/><Sample Name="TAIL"/></Samples>
        <Instruments><Instrument Name="piano" ProgramNumber="0" Sample="SHARED" SampleHi="TAIL" BaseNote="C5"/></Instruments>
        <Drums><Drum Note="C4" Sample="SHARED"/></Drums>
        <Effects><Effect Name="effect" Sample="SHARED"/></Effects>
    </Soundfont>"""
    put(root, base + "soundfonts/Soundfont_0.xml", font.format(index=0))
    put(root, "assets/audio/soundfonts/custom.xml", font.format(index=38))
    put(root, "assets/audio/samples/SampleBank_0/custom_untracked.wav", tone_wav())
    put(root, "assets/audio/sequences/song_a.seq", '#include "Soundfont_0.h"\n.sequence song_a\n')
    for name in ("include/tables/sequence_table.h", "spec/spec",
                 "src/audio/game/session_config.c", "src/buffers/audio_heap.c", "include/buffers.h"):
        put(root, name, "/* generated fixture */\n")
    sentinels(root)


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        # All evidence/media stays under agents/reports; executables are not generated here.
        reports = ROOT / "agents/reports/repository-discovery"
        reports.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="fixture-", dir=reports)
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def scan_unchanged(self):
        before = snapshot(self.root)
        result = discovery.scan(self.root)
        self.assertEqual(before, snapshot(self.root))
        self.assertFalse(result["capabilities"]["writes_enabled"])
        paths = {item["path"] for item in result["files"]}
        for sentinel in ("levels/sentinel.bin", "build/us/sound/generated.aifc", ".git/private",
                         "assets/graphics/texture.bin", "preferences.ini", "project.duck"):
            self.assertNotIn(sentinel, paths)
        return result

    def test_sm64_shared_regions_drums_and_current_custom_inputs(self):
        sm64_fixture(self.root)
        result = self.scan_unchanged()
        self.assertEqual(result["target"], "sm64")
        edges = [edge for edge in result["dependencies"] if edge["provider"].endswith("shared.aiff")]
        self.assertEqual(len(edges), 4)
        self.assertEqual({edge["representation"] for edge in edges}, {"string", "object"})
        self.assertTrue(any(item["path"].endswith("custom_untracked.aiff") for item in result["files"]))
        self.assertTrue(any(item["kind"] == "preexisting_backup" for item in result["files"]))

    def test_hacker_is_separate_target(self):
        sm64_fixture(self.root, hacker=True)
        self.assertEqual(self.scan_unchanged()["target"], "hackersm64")

    def test_preexisting_orphan_backup_is_preserved_in_inventory(self):
        sm64_fixture(self.root)
        put(self.root, "sound/samples/instruments/removed.aiff.bkp", b"owned by user")
        put(self.root, "sound/sequences/us/removed.m64.bkp", b"previous song")
        result = self.scan_unchanged()
        paths = {item["path"] for item in result["files"] if item["kind"] == "preexisting_backup"}
        self.assertIn("sound/samples/instruments/removed.aiff.bkp", paths)
        self.assertIn("sound/sequences/us/removed.m64.bkp", paths)
        self.assertFalse((self.root / "sound/samples/instruments/removed.aiff").exists())

    def test_version_condition_selects_bank_and_region_and_skips_inactive_instrument(self):
        sm64_fixture(self.root)
        data = {"sample_bank": {"ifdef": ["VERSION_SH"], "then": "absent_bank",
                                "else": "instruments"}, "instruments": {
            "active": {"sound": {"ifdef": ["VERSION_US"], "then": "shared", "else": "absent"}},
            "inactive": {"ifdef": ["VERSION_SH"], "sound": "absent"}}}
        put(self.root, "sound/sound_banks/00.json", json.dumps(data))
        result = self.scan_unchanged()
        edges = [edge for edge in result["dependencies"] if edge["consumer"].endswith("00.json")]
        edges = [edge for edge in edges if edge["role"] == "sample"]
        self.assertEqual(len(edges), 1)
        self.assertTrue(edges[0]["selector"].endswith("/sound/then"))

    def test_unknown_build_condition_rejected(self):
        sm64_fixture(self.root)
        put(self.root, "sound/sound_banks/00.json", json.dumps({"sample_bank": "instruments",
            "instruments": {"active": {"ifdef": ["CUSTOM_BUILD"], "sound": "shared"}}}))
        with self.assertRaisesRegex(discovery.DiscoveryError, "unsupported native condition"):
            discovery.scan(self.root)

    def test_oot_cart_aliases_instruments_drums_sfx_and_defaults(self):
        oot_fixture(self.root)
        result = self.scan_unchanged()
        self.assertEqual(result["target"], "oot")
        self.assertEqual(result["samplebank_aliases"][0]["index"], 1)
        edges = [edge for edge in result["dependencies"] if edge["provider"].endswith("shared.wav")]
        self.assertEqual(len(edges), 9)
        self.assertTrue(any(edge.get("attributes", {}).get("BaseNote") == "C5" for edge in edges))
        self.assertTrue(any(edge["consumer"].endswith("song_a.seq") and
                            edge["provider"].endswith("Soundfont_0.xml") and
                            edge["role"] == "soundfont" for edge in result["dependencies"]))

    def test_oot_wav_precedence_preserves_shadowed_input(self):
        oot_fixture(self.root)
        put(self.root, "assets/audio/samples/SampleBank_0/shared.wav", tone_wav() + b"authored")
        result = self.scan_unchanged()
        self.assertEqual(len(result["shadowed_inputs"]), 1)
        for edge in result["dependencies"]:
            if edge["provider"].endswith("shared.wav"):
                self.assertEqual(edge["provider"], "assets/audio/samples/SampleBank_0/shared.wav")

    def test_oot_samples_inherit_dd_bank_and_preserve_region_tuning(self):
        oot_fixture(self.root)
        put(self.root, "assets/audio/samples/SampleBank_2/dd.wav", tone_wav())
        put(self.root, "assets/audio/samplebanks/SampleBank_2.xml", """<SampleBank Index="2">
            <Sample Name="DD" Path="$(BUILD_DIR)/assets/audio/samples/SampleBank_2/dd.aifc"/>
        </SampleBank>""")
        put(self.root, "assets/audio/soundfonts/custom.xml", """<Soundfont Index="38"
            SampleBank="$(BUILD_DIR)/assets/audio/samplebanks/SampleBank_0.xml"
            SampleBankDD="$(BUILD_DIR)/assets/audio/samplebanks/SampleBank_2.xml">
            <Samples IsDD="true"><Sample Name="DD" SampleRate="22050" FineTune="-7"/></Samples>
            <Instruments><Instrument Sample="DD" BaseNote="D5"/></Instruments>
        </Soundfont>""")
        result = self.scan_unchanged()
        edges = [edge for edge in result["dependencies"] if edge["provider"].endswith("dd.wav")]
        self.assertEqual(len(edges), 3)
        region = next(edge for edge in edges if edge.get("attributes", {}).get("BaseNote") == "D5")
        self.assertEqual(region["attributes"]["FineTune"], "-7")
        self.assertEqual(region["attributes"]["BaseNote"], "D5")
        self.assertEqual(region["attributes"]["IsDD"], "true")

    def test_oot_duplicate_xml_overlay_rejected(self):
        oot_fixture(self.root)
        old = self.root / "extracted/ntsc-1.0/assets/audio/samplebanks/SampleBank_0.xml"
        put(self.root, "assets/audio/samplebanks/SampleBank_0.xml", old.read_bytes())
        with self.assertRaisesRegex(discovery.DiscoveryError, "Duplicate native XML"):
            discovery.scan(self.root)

    def test_oot_duplicate_physical_index_rejected(self):
        oot_fixture(self.root)
        old = self.root / "extracted/ntsc-1.0/assets/audio/samplebanks/SampleBank_0.xml"
        put(self.root, "assets/audio/samplebanks/different.xml", old.read_bytes())
        with self.assertRaisesRegex(discovery.DiscoveryError, "Duplicate native SampleBank index"):
            discovery.scan(self.root)

    def test_oot_pointer_collision_rejected(self):
        oot_fixture(self.root)
        put(self.root, "assets/audio/samplebanks/alias.xml", '<SampleBank Index="1"/>')
        with self.assertRaisesRegex(discovery.DiscoveryError, "Duplicate (native SampleBank|samplebank pointer) index"):
            discovery.scan(self.root)

    def test_oot_missing_wav_reports_definition(self):
        oot_fixture(self.root)
        (self.root / "extracted/ntsc-1.0/assets/audio/samples/SampleBank_0/shared.wav").unlink()
        with self.assertRaisesRegex(discovery.DiscoveryError, "SampleBank_0.xml: no source WAV"):
            discovery.scan(self.root)

    def test_coopdx_only_managed_mod_namespace(self):
        put(self.root, "src/pc/lua/utils/smlua_audio_utils.c", "/* fixture */\n")
        put(self.root, "mods/duckmidi_song/main.lua", "-- user-edited current script\n")
        put(self.root, "mods/duckmidi_song/sound/song.m64", b"\xff")
        put(self.root, "mods/other/main.lua", "-- unrelated\n")
        put(self.root, "mods/duckmidi_song/texture.png", b"unrelated")
        sentinels(self.root)
        result = self.scan_unchanged()
        self.assertFalse(result["capabilities"]["custom_layout"])
        self.assertEqual({item["path"] for item in result["files"]}, {
            "mods/duckmidi_song/main.lua", "mods/duckmidi_song/sound/song.m64"})

    def test_ambiguous_target_markers_rejected(self):
        sm64_fixture(self.root, hacker=True)
        put(self.root, "tools/audio/soundfont_compiler.c", "/* fixture */")
        with self.assertRaisesRegex(discovery.DiscoveryError, "Ambiguous repository"):
            discovery.scan(self.root)

    def test_duplicate_json_rejected(self):
        sm64_fixture(self.root)
        put(self.root, "sound/sequences.json", '{"song": [], "song": []}')
        with self.assertRaisesRegex(discovery.DiscoveryError, "Duplicate JSON member"):
            discovery.scan(self.root)

    def test_traversal_sample_rejected(self):
        sm64_fixture(self.root)
        put(self.root, "sound/sound_banks/00.json", json.dumps({
            "sample_bank": "../outside", "instruments": {}}))
        with self.assertRaises(discovery.DiscoveryError):
            discovery.scan(self.root)

    def test_missing_reference_reports_path(self):
        sm64_fixture(self.root)
        (self.root / "sound/samples/instruments/shared.aiff").unlink()
        with self.assertRaisesRegex(discovery.DiscoveryError, "00.json: unresolved sample"):
            discovery.scan(self.root)

    def test_hardlink_alias_rejected(self):
        sm64_fixture(self.root)
        source = self.root / "sound/samples/instruments/shared.aiff"
        target = source.with_name("alias.aiff")
        try:
            os.link(source, target)
        except OSError as error:
            self.skipTest(f"Hardlink creation unavailable: {error}")
        with self.assertRaisesRegex(discovery.DiscoveryError, "Aliased audio files"):
            discovery.scan(self.root)

    def test_symlink_escape_rejected(self):
        sm64_fixture(self.root)
        source = self.root / "sound/samples/instruments/shared.aiff"
        try:
            source.with_name("escape.aiff").symlink_to(self.root / "levels/sentinel.bin")
        except OSError as error:
            self.skipTest(f"Symlink creation unavailable: {error}")
        with self.assertRaisesRegex(discovery.DiscoveryError, "Symlink"):
            discovery.scan(self.root)

    def test_xml_dtd_rejected(self):
        oot_fixture(self.root)
        put(self.root, "assets/audio/soundfonts/custom.xml", '<!DOCTYPE Soundfont [<!ENTITY a "x">]><Soundfont/>')
        with self.assertRaisesRegex(discovery.DiscoveryError, "DTD/entity"):
            discovery.scan(self.root)

    def test_utf16_xml_dtd_rejected(self):
        oot_fixture(self.root)
        put(self.root, "assets/audio/soundfonts/custom.xml",
            '<!DOCTYPE Soundfont [<!ENTITY a "x">]><Soundfont/>'.encode("utf-16"))
        with self.assertRaisesRegex(discovery.DiscoveryError, "DTD/entity"):
            discovery.scan(self.root)

    def test_utf8_and_equivalent_root_paths(self):
        nested = self.root / "música 日本語"
        sm64_fixture(nested)
        result = discovery.scan(nested)
        alias = discovery.scan(nested / ".." / nested.name)
        self.assertEqual(result, alias)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
