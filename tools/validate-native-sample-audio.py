"""Decode real-plugin validation tones with each checkout's native tools (run in WSL)."""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def chunks(data):
    at = 12
    while at + 8 <= len(data):
        tag, size = struct.unpack_from(">4sI", data, at)
        yield tag, data[at + 8:at + 8 + size]
        at += 8 + size + size % 2


def inspect(stage, oot, report):
    stage = stage.resolve(strict=True)
    allowed = (ROOT / "build/development/native-custom-export").resolve()
    assert stage.is_relative_to(allowed) and stage != allowed
    build = stage / "build" / ("ntsc-1.0" if oot else "us_n64" if (stage / "build/us_n64").exists() else "us")
    folder = build / ("assets/audio/samples/SampleBank_0" if oot else "sound/samples/instruments")
    encoded = next(folder.glob("Validation_tone_1_*.aifc"))
    output = report / (stage.name + "-decoded.aiff")
    decoder = stage / ("tools/audio/sampleconv/sampleconv" if oot else "tools/aifc_decode")
    command = [str(decoder)] + (["pcm16"] if oot else []) + [str(encoded), str(output)]
    subprocess.run(command, check=True, capture_output=True)
    metadata = {}
    for tag, body in chunks(encoded.read_bytes()):
        if tag != b"APPL":
            continue
        for name in (b"VADPCMCODES", b"VADPCMLOOPS"):
            index = body.find(name)
            if index < 0:
                continue
            at = index + len(name)
            if name.endswith(b"CODES"):
                metadata["book_version"], metadata["book_order"], metadata["predictors"] = struct.unpack_from(">HHH", body, at)
            else:
                version, count, start, end, repeats = struct.unpack_from(">HHIII", body, at)
                state = struct.unpack_from(">16h", body, at + 16)
                assert version == 1 and count == 1 and start == 16 and end >= 464
                assert (end - start) % 448 == 0 and repeats == 0xFFFFFFFF and any(state)
                metadata.update(loop_start=start, loop_end=end, source_period=448, loop_state=list(state))
    assert metadata["book_version"] == 1 and metadata["book_order"] == 2
    assert 1 <= metadata["predictors"] <= 16 and "loop_end" in metadata
    pcm, rate = None, None
    for tag, body in chunks(output.read_bytes()):
        if tag == b"COMM":
            channels, frames, bits = struct.unpack_from(">HIH", body)
            assert channels == 1 and frames >= 496 and bits == 16
            exponent, mantissa = struct.unpack_from(">HQ", body, 8)
            rate = math.ldexp(mantissa / 2**63, exponent - 16383)
        elif tag == b"SSND":
            offset = struct.unpack_from(">I", body)[0]
            raw = body[8 + offset:]
            pcm = struct.unpack(">" + "h" * (len(raw) // 2), raw)
    assert rate == 32000 and pcm and len(pcm) >= 496
    crossings = []
    for i in range(33, 432):
        if pcm[i - 1] <= 0 < pcm[i]:
            crossings.append(i - 1 - pcm[i - 1] / (pcm[i] - pcm[i - 1]))
    assert len(crossings) >= 5
    frequency = rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])
    expected = 32000 * .1 / (2 * math.pi)
    assert abs(frequency / expected - 1) < .01
    rms = math.sqrt(sum((n / 32768)**2 for n in pcm[32:432]) / 400)
    assert .14 < rms < .18
    metadata.update(encoded=str(encoded.relative_to(ROOT)), decoded=str(output.relative_to(ROOT)),
                    rate=rate, frequency_hz=frequency, expected_hz=expected, rms=rms)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    report = args.report.resolve()
    assert report.is_relative_to((ROOT / "agents/reports").resolve())
    report.mkdir(parents=True, exist_ok=True)
    results = {}
    for name, oot in (("sm64-stage", False), ("hacker-stage", False), ("oot-audio-stage", True)):
        results[name] = inspect(ROOT / "build/development/native-custom-export" / name, oot, report)
        print(name, results[name]["frequency_hz"], "Hz; native book/loop state valid", flush=True)
    (report / "measurements.json").write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
