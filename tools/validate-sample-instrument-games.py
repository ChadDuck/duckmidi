#!/usr/bin/env python3
"""Build original loop fixtures in disposable stages using the local game toolchains.

Run in the game build environment with already built US SM64 and OoT NTSC 1.0
checkouts. The optional work directory must be a new directory. Copy only the
console output to the validation report if retaining a ROM is unnecessary.
"""

import argparse
import json
import math
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from xml.etree import ElementTree


RATE = 32000
FRAMES = 4096
START = 1024
END = 3072


def run(cwd, *args, capture=False):
    print("+", *args, flush=True)
    return subprocess.run(
        [str(arg) for arg in args], cwd=cwd, check=True,
        stdout=subprocess.PIPE if capture else None,
    ).stdout


def chunk(tag, data, endian):
    return tag + struct.pack(endian + "I", len(data)) + data + bytes(len(data) % 2)


def container(path, kind, chunks, endian):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(chunk(kind[0], kind[1] + b"".join(
        chunk(tag, data, endian) for tag, data in chunks
    ), endian))


def wave_samples(endian):
    # 32 complete periods in the loop. The tail doubles the frequency.
    return struct.pack(endian + str(FRAMES) + "h", *(
        round(16000 * math.sin(math.tau * frame / (64 if frame < END else 32)))
        for frame in range(FRAMES)
    ))


def write_aiff(path):
    exponent = RATE.bit_length() - 1
    rate80 = struct.pack(">HQ", 16383 + exponent, RATE << (63 - exponent))
    markers = struct.pack(">H", 2)
    markers += struct.pack(">HI", 1, START) + b"\1s"
    markers += struct.pack(">HI", 2, END) + b"\1e"
    instrument = struct.pack(">6bh6H", 71, 0, 0, 127, 1, 127, 0, 1, 1, 2, 0, 0, 0)
    container(path, (b"FORM", b"AIFF"), (
        (b"COMM", struct.pack(">HIH", 1, FRAMES, 16) + rate80),
        (b"MARK", markers),
        (b"INST", instrument),
        (b"SSND", bytes(8) + wave_samples(">")),
    ), ">")


def write_wav(path):
    # The OoT converter copies smpl endpoints verbatim into the VADPCM loop.
    sampler = struct.pack("<9I", 0, 0, 10**9 // RATE, 71, 0, 0, 0, 1, 0)
    sampler += struct.pack("<6I", 0, 0, START, END, 0, 0)
    container(path, (b"RIFF", b"WAVE"), (
        (b"fmt ", struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16)),
        (b"smpl", sampler),
        (b"data", wave_samples("<")),
    ), "<")


def check_loop(path):
    data = path.read_bytes()
    marker = b"stoc\x0bVADPCMLOOPS"
    index = data.index(marker) + len(marker)
    version, count, start, end, repeats = struct.unpack_from(">HHIII", data, index)
    assert (version, count, start, end, repeats) == (1, 1, START, END, 0xFFFFFFFF), (
        version, count, start, end, repeats
    )
    print(f"{path.name}: VADPCM loop [{start}, {end}) with infinite repeats, {len(data)} bytes")


def sm64_sequence(work, checkout, path, prefix, vanilla_slot):
    asm = work / "mixed.s"
    asm.write_text(f"""
#include "{checkout}/include/seq_macros.inc"
.data
sequence_start:
    seq_settempo 120
    seq_initchannels 1
    seq_startchannel 0, channel
    seq_delay 144
    seq_end
channel:
    chan_largenoteson
    chan_setbank 0
    chan_setinstr {vanilla_slot}
    chan_setlayer 0, note
    chan_delay 48
    chan_setbank 1
    chan_setinstr 0
    chan_setlayer 0, note
    chan_delay 48
    chan_setbank 0
    chan_setinstr {vanilla_slot}
    chan_setlayer 0, note
    chan_delay 48
    chan_end
note:
    layer_note0 50, 44, 100, 64
    layer_end
""")
    run(work, "gcc", "-E", "-P", "-x", "assembler-with-cpp", "-DVERSION_US", asm,
        "-o", "mixed.pp.s")
    run(work, prefix + "as", "-march=vr4300", "-32", "-no-pad-sections", "mixed.pp.s",
        "-o", "mixed.o")
    run(work, prefix + "objcopy", "-j", ".data", "-O", "binary", "mixed.o", path)
    assert b"\xc6\x00" in path.read_bytes() and b"\xc6\x01" in path.read_bytes()


def offset_table(path):
    data = path.read_bytes()
    _, count = struct.unpack_from(">HH", data)
    return [struct.unpack_from(">II", data, 4 + i * 8) for i in range(count)]


def sm64_stage(checkout, work, prefix):
    work.mkdir()
    (work / "tools").mkdir()
    for name in ("aiff_extract_codebook", "tabledesign", "vadpcm_enc"):
        shutil.copy2(checkout / "tools" / name, work / "tools" / name)
    banks = work / "banks"
    banks.mkdir()
    for path in sorted((checkout / "sound/sound_banks").glob("*.json")):
        shutil.copy2(path, banks)
    sample_banks = work / "sample_banks"
    shutil.copytree(checkout / "build/us/sound/samples", sample_banks)
    assembler = checkout / "tools/assemble_sound.py"

    def assemble_sound(label):
        run(work, "python3", "-B", assembler, sample_banks, banks, f"{label}.ctl",
            f"{label}.ctl_header", f"{label}.tbl", f"{label}.tbl_header", "-DVERSION_US",
            "--endian", "big", "--bitwidth", "32")
        return offset_table(work / f"{label}.ctl"), offset_table(work / f"{label}.tbl")

    old_ctl, old_tbl = assemble_sound("baseline")
    vanilla = "03"
    bank_names = sorted(path.stem for path in banks.glob("*.json"))
    assert vanilla in bank_names
    vanilla_index = bank_names.index(vanilla)

    name = f"{int(bank_names[-1].split('_')[0], 16) + 1:02x}_custom_sample_spike"
    assert name > bank_names[-1]
    aiff = work / "tone.aiff"
    write_aiff(aiff)
    codebook = run(work, "./tools/aiff_extract_codebook", aiff, capture=True)
    assert codebook and codebook == run(work, "./tools/tabledesign", "-s", "1", aiff, capture=True)
    (work / "tone.table").write_bytes(codebook)
    sample = sample_banks / "custom_spike/tone.aifc"
    sample.parent.mkdir()
    run(work, "./tools/vadpcm_enc", "-c", "tone.table", aiff, sample)
    check_loop(sample)
    bank = {
        "sample_bank": "custom_spike",
        "envelopes": {"env": [[2, 32700], "hang"]},
        "instruments": {
            "tone": {"release_rate": 32, "envelope": "env",
                     "sound": {"sample": "tone", "tuning": 0.296533}}
        },
        "instrument_list": ["tone"],
    }
    (banks / f"{name}.json").write_text(json.dumps(bank))
    new_ctl, new_tbl = assemble_sound("mixed")
    assert len(new_ctl) == len(old_ctl) + 1 and len(new_tbl) == len(old_tbl) + 1
    assert [size for _, size in new_ctl[:-1]] == [size for _, size in old_ctl]
    assert [size for _, size in new_tbl[:-1]] == [size for _, size in old_tbl]
    assert all(offset < new_tbl[-1][0] for offset, _ in new_tbl[:-1])
    seq = work / "00_custom_sample_spike.m64"
    sm64_sequence(work, checkout, seq, prefix, 0)
    sequence_json = work / "sequences.json"
    sequence_json.write_text(json.dumps({seq.stem: [vanilla, name]}))
    run(work, "python3", "-B", assembler, "--sequences", "sequences.bin",
        "sequences_header", "bank_sets", banks, sequence_json, seq, "-DVERSION_US",
        "--endian", "big", "--bitwidth", "32")
    sets = (work / "bank_sets").read_bytes()
    offset, = struct.unpack_from(">H", sets)
    assert sets[offset:offset + 3] == bytes((2, len(old_ctl), vanilla_index))
    print(f"SM64: {len(old_ctl)} bank indices preserved, sequence uses slots 0 and 1 "
          f"for banks {vanilla_index:#x} and {len(old_ctl):#x}; "
          f"{seq.stat().st_size} sequence bytes, {new_ctl[-1][1]} new control bytes, "
          f"{new_tbl[-1][1]} new table bytes")


def append_spec(path, segment, object_path):
    source = path.read_text()
    start = source.index(f'name "{segment}"')
    end = source.index("endseg", start)
    source = source[:end] + f'    include "$(BUILD_DIR)/{object_path}"\n' + source[end:]
    path.write_text(source)


def oot_stage(checkout, work, prefix, title_audition):
    # Copy the initialized build so the real Makefile can regenerate tables,
    # generate a new linker script and link a ROM. Keep the source checkout read-only.
    work.mkdir()
    run(work, "cp", "-a", "--reflink=auto", *(
        path for path in checkout.iterdir() if path.name != ".git"
    ), work)
    version = "ntsc-1.0"
    build = work / "build" / version
    rel_build = Path("build") / version
    banks = build / "assets/audio/samplebanks"
    fonts = build / "assets/audio/soundfonts"
    bank_indices = {
        int(element.attrib["Index"])
        for path in banks.glob("*.xml")
        for element in ElementTree.parse(path).getroot().iter()
        if element.tag in {"SampleBank", "Pointer"}
    }
    font_indices = {int(ElementTree.parse(path).getroot().attrib["Index"])
                    for path in fonts.glob("*.xml")}
    bank_id = max(bank_indices) + 1
    font_id = max(font_indices) + 1
    samplebank = f"SampleBank_{bank_id}"
    soundfont = f"Soundfont_{font_id}"
    sample = Path(f"assets/audio/samples/{samplebank}/spike.wav")
    bank = Path(f"assets/audio/samplebanks/{samplebank}.xml")
    font = Path(f"assets/audio/soundfonts/{soundfont}.xml")
    sequence = Path("assets/audio/sequences/sample_spike.seq")
    write_wav(work / sample)
    (work / bank).parent.mkdir(parents=True, exist_ok=True)
    (work / bank).write_text(f"""<SampleBank Name="{samplebank}" Index="{bank_id}"
    Medium="MEDIUM_CART" CachePolicy="CACHE_LOAD_EITHER_NOSYNC">
    <Sample Name="CUSTOM_SPIKE" Path="$(BUILD_DIR)/{sample.with_suffix('.aifc')}"/>
</SampleBank>
""")
    (work / font).parent.mkdir(parents=True, exist_ok=True)
    (work / font).write_text(f"""<Soundfont Name="{soundfont}" Index="{font_id}"
    Medium="MEDIUM_CART" CachePolicy="CACHE_LOAD_TEMPORARY"
    SampleBank="$(BUILD_DIR)/assets/audio/samplebanks/SampleBank_0.xml"
    SampleBankDD="$(BUILD_DIR)/{bank}">
    <Envelopes>
        <Envelope Name="Env" Release="100">
            <Point Delay="2" Arg="32700"/>
            <Hang/>
        </Envelope>
    </Envelopes>
    <Samples>
        <Sample Name="SAMPLE_0_0"/>
        <Sample Name="CUSTOM_SPIKE" IsDD="true"/>
    </Samples>
    <Instruments>
        <Instrument ProgramNumber="0" Name="VANILLA_SPIKE" Envelope="Env" Sample="SAMPLE_0_0"/>
        <Instrument ProgramNumber="1" Name="CUSTOM_INSTR" Envelope="Env" Sample="CUSTOM_SPIKE"/>
    </Instruments>
</Soundfont>
""")
    (work / sequence).write_text(f"""#include "aseq.h"
#include "{soundfont}.h"
.startseq Sequence_sample_spike
.sequence Seq
    initchan 1
    tempo 120
    vol 100
    ldchan 0, Chan
    delay 144
    end
.channel Chan
    noshort
    instr 0
    vol 127
    ldlayer 0, Layer
    delay 48
    instr 1
    ldlayer 0, Layer
    delay 48
    instr 0
    ldlayer 0, Layer
    delay 48
    end
.layer Layer
    notedvg PITCH_A4, 44, 100, 64
    end
.endseq Sequence_sample_spike
""")
    table = work / "include/tables/sequence_table.h"
    if title_audition:
        text, count = re.subn(r"(?m)^(DEFINE_SEQUENCE\s*\(\s*)\w+(,\s*NA_BGM_TITLE\s*,)",
                              r"\g<1>Sequence_sample_spike\2", table.read_text())
        assert count == 1
        table.write_text(text)
    else:
        with table.open("a") as stream:
            stream.write("\nDEFINE_SEQUENCE(Sequence_sample_spike, NA_BGM_SAMPLE_SPIKE, "
                         "MEDIUM_CART, CACHE_LOAD_TEMPORARY, 0)\n")
    for segment, asset in (("Audiotable", bank), ("Audiobank", font), ("Audioseq", sequence)):
        append_spec(work / "spec/spec", segment, asset.with_suffix(".o"))
    make = ["make", "-j4", f"VERSION={version}", f"MIPS_BINUTILS_PREFIX={prefix}",
            "COMPARE=0", "NON_MATCHING=1"]
    rom = rel_build / f"oot-{version}.z64"
    # Build just the audio objects first, so an unrelated ROM build failure
    # still leaves the generated audio, metadata, and linker inputs for inspection.
    run(work, *make, *(rel_build / asset.with_suffix(".o") for asset in (bank, font, sequence)),
        rel_build / "assets/audio/samplebank_table.h", rel_build / "assets/audio/soundfont_table.h")
    check_loop(build / sample.with_suffix(".aifc"))
    for asset in (bank, font, sequence):
        section = ".data" if asset == sequence else ".rodata"
        filename = asset.with_suffix(".bin").name
        run(work, prefix + "objcopy", "-j", section, "-O", "binary",
            rel_build / asset.with_suffix(".o"), filename)
        print(f"OoT {filename}: {(work / filename).stat().st_size} bytes in {section}")
    assert re.search(rf"\b{samplebank}\b", (banks.parent / "samplebank_table.h").read_text())
    font_line = next(line for line in (banks.parent / "soundfont_table.h").read_text().splitlines()
                     if line.startswith(f"DEFINE_SOUNDFONT({soundfont},"))
    assert f"0, {bank_id}," in font_line
    print(f"OoT font {font_id}, primary samplebank 0, secondary samplebank {bank_id}: {font_line}")
    run(work, *make, rom)
    symbols = run(work, prefix + "nm", "-n", rom.with_suffix(".elf"), capture=True).decode()
    for symbol in (f"{samplebank}_Start", f"{soundfont}_Start", "Sequence_sample_spike_Start"):
        assert re.search(rf"^\S+\s+\S\s+{symbol}$", symbols, re.MULTILINE), symbol
    print(f"Linked OoT ROM: {work / rom}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("sm64", "oot"))
    parser.add_argument("checkout", type=Path)
    parser.add_argument("stage", type=Path, nargs="?")
    parser.add_argument("--mips-prefix", default="mips64-elf-")
    parser.add_argument("--oot-title-audition", action="store_true",
                        help="Play the fixture at boot in the disposable OoT ROM")
    args = parser.parse_args()
    checkout = args.checkout.resolve(strict=True)
    stage = args.stage.resolve() if args.stage else Path(tempfile.mkdtemp(prefix="dm-samples-")) / args.target

    if stage.exists() or checkout in stage.parents:
        parser.error("stage must be a new directory outside the game checkout")

    if args.target == "sm64":
        if args.oot_title_audition:
            parser.error("--oot-title-audition requires the oot target")
        sm64_stage(checkout, stage, args.mips_prefix)
    else:
        oot_stage(checkout, stage, args.mips_prefix, args.oot_title_audition)


if __name__ == "__main__":
    main()
