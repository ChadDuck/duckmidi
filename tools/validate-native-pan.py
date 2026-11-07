"""Check Replace drum pan and Revert on fresh copies of the generated repository fixtures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def snapshot(path):
    return {
        item.relative_to(path).as_posix(): hashlib.sha256(item.read_bytes()).hexdigest()
        for item in sorted(path.rglob("*"))
        if item.is_file()
    }


def fixture(group, suffix):
    directory = ROOT / "agents/reports" / group
    candidates = [p for p in directory.iterdir() if p.is_dir() and p.name.endswith(suffix)]
    if not candidates:
        raise ValueError(f"Run the repository-install CTest fixture first: {group} {suffix}")
    source = max(candidates, key=lambda p: p.stat().st_mtime_ns).resolve()
    if not source.is_relative_to(directory.resolve()) or (source / ".git").exists():
        raise ValueError(f"Expected an isolated generated fixture: {source}")
    return source


def seed(checkout, oot):
    if oot:
        path = checkout / "assets/audio/soundfonts/Soundfont_0.xml"
        text = path.read_text(encoding="utf-8")
        marker = "</Soundfont>"
        addition = (
            '<Drums><Drum Name="fixture_drum" Note="C2" Pan="17" Envelope="Env" '
            'Sample="SAMPLE_0_0"/></Drums>'
        )
    else:
        path = checkout / "sound/sound_banks/11.json"
        text = path.read_text(encoding="utf-8")
        marker = '"instruments":{'
        addition = (
            marker + '"percussion":[{"release_rate":128,"pan":17,"envelope":"env",'
            '"sound":"44_grand_piano_F#5"}],'
        )
    if text.count(marker) != 1:
        raise ValueError(f"Unexpected generated bank structure: {path}")
    text = text.replace(marker, addition + marker if oot else addition, 1)
    path.write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--bin", type=Path, default=ROOT / "build/development/cmake/bin")
    args = parser.parse_args()
    report = args.report.resolve()
    if not report.is_relative_to((ROOT / "agents/reports").resolve()):
        raise ValueError("Validation output must stay under agents/reports")
    generated = report / "generated"
    generated.mkdir(parents=True, exist_ok=False)
    results = {}
    for name, group, suffix in (
        ("sm64", "m64-install", "-sm64"),
        ("hacker", "m64-install", "-hacker"),
        ("oot", "oot-install", "-song"),
    ):
        source = fixture(group, suffix)
        checkout = generated / name
        shutil.copytree(source, checkout)
        seed(checkout, name == "oot")
        before = snapshot(checkout)
        plugins = generated / f"{name}-plugins"
        plugins.mkdir()
        binary = "custom_oot_export_test" if name == "oot" else "custom_m64_export_test"
        executable = args.bin.resolve() / (binary + (".exe" if os.name == "nt" else ""))
        for mode in ("centre-replace", "pan-replace", "revert-all"):
            with (report / f"{name}-{mode}.log").open("w", encoding="utf-8") as log:
                subprocess.run(
                    [str(executable), str(plugins), str(checkout), mode],
                    cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True,
                )
        after = snapshot(checkout)
        changed = sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))
        results[name] = {
            "source": source.as_posix(), "files": len(before),
            "reverted": not changed, "changed": changed,
        }
    (report / "pan-revert.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    if not all(result["reverted"] for result in results.values()):
        raise RuntimeError("Revert did not restore every generated fixture file")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
