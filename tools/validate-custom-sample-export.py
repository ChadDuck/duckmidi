"""Exercise the real game-plugin entry points against explicitly disposable native stages."""

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[1]


def disposable(path):
    resolved = path.resolve(strict=True)
    allowed = (ROOT / "build/development/native-custom-export").resolve()
    if not resolved.is_relative_to(allowed) or resolved == allowed:
        raise ValueError(f"Native validation requires a disposable stage under {allowed}: {path}")
    return resolved


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def snapshot(checkout, oot):
    folders = ["assets/audio", "extracted/ntsc-1.0/assets/audio"] if oot else ["sound"]
    fixed = (["Makefile", "spec/spec", "include/tables/sequence_table.h",
              "src/audio/game/session_config.c", "src/buffers/audio_heap.c", "include/buffers.h"]
             if oot else ["Makefile", "src/audio/data.c", "src/audio/data.h", "src/buffers/buffers.c",
                          "include/config/config_audio.h", "tools/assemble_sound.py"])
    files, directories = {}, []
    for folder in folders:
        start = checkout / folder
        if not start.exists():
            continue
        directories.append(folder)
        for path in start.rglob("*"):
            relative = path.relative_to(checkout).as_posix()
            if path.is_dir():
                directories.append(relative)
            elif path.is_file():
                files[relative] = digest(path)
    for name in fixed:
        path = checkout / name
        if path.is_file():
            files[name] = digest(path)
    return {"files": dict(sorted(files.items())), "directories": sorted(directories)}


def native_build(checkout, oot, report, label):
    """Build only disposable stages, using their ordinary incremental Make rules."""
    checkout = disposable(checkout)
    relative = checkout.relative_to(ROOT).as_posix()
    linux_root = "/mnt/" + ROOT.drive[0].lower() + ROOT.as_posix()[2:]
    targets = []
    if oot:
        for folder in ("samplebanks", "soundfonts"):
            names = set()
            for base in ("assets/audio", "extracted/ntsc-1.0/assets/audio"):
                names.update(p.stem for p in (checkout / base / folder).glob("*.xml"))
            targets.extend(f"build/ntsc-1.0/assets/audio/{folder}/{name}.o"
                           for name in sorted(names))
        for name in ("duckmidi_song", "duckmidi_song_b"):
            if (checkout / f"assets/audio/sequences/{name}.seq").exists():
                targets.append(f"build/ntsc-1.0/assets/audio/sequences/{name}.o")
        targets.extend(["build/ntsc-1.0/assets/audio/samplebank_table.h",
                        "build/ntsc-1.0/assets/audio/soundfont_table.h"])
    else:
        targets.append("COMPARE=0")
    command = ["wsl.exe", "-d", "archlinux", "--cd", f"{linux_root}/{relative}",
               "--", "make", "-j4", *targets]
    with (report / f"native-{checkout.name}-{label}.log").open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise AssertionError(f"Native build failed: {checkout.name} {label}")
    if not oot:
        roms = [p for folder in ("us", "us_n64")
                for p in (checkout / "build" / folder).glob("*.z64")]
        assert len(roms) == 1
        return digest(roms[0])
    # The generated table must describe current XMLs, including after managed fonts disappear.
    table = (checkout / "build/ntsc-1.0/assets/audio/soundfont_table.h").read_text()
    for name in ("duckmidi_song", "duckmidi_song_b"):
        assert (name in table) == (checkout / f"assets/audio/soundfonts/Soundfont_{name}.xml").exists()
    return hashlib.sha256(table.encode()).hexdigest()


def validate(executable, checkout, plugins, oot, report, build=False, other_bank=False):
    checkout = disposable(checkout)
    plugins.mkdir(parents=True, exist_ok=True)
    log = []

    def run(mode, target=None):
        args = [str(executable.resolve(strict=True)), str(plugins), str(checkout), mode]
        if target:
            args.append(target)
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
        log.append({"mode": mode, "status": result.returncode,
                    "stdout": result.stdout, "stderr": result.stderr})
        report.write_text(json.dumps(log, indent=2), encoding="utf-8")
        if result.returncode:
            raise AssertionError(f"{mode}: {result.stdout}\n{result.stderr}")

    baseline = snapshot(checkout, oot)
    native_baseline = native_build(checkout, oot, report.parent, "baseline") if build else None
    run("inspect")
    assert snapshot(checkout, oot) == baseline
    database = plugins / "history/repositories.sqlite3"
    before_db = digest(database)
    run("package-replace", "auto-piano")
    assert snapshot(checkout, oot) == baseline, "Package generation changed native inputs"
    assert digest(database) == before_db, "Package generation changed the current ledger"
    run("replace-missing-region", "auto-piano")
    assert snapshot(checkout, oot) == baseline, "Rejected region assignment changed inputs"
    run("replace-layered", "auto-piano")
    if build:
        native_build(checkout, oot, report.parent, "replace")
    replaced = snapshot(checkout, oot)
    backups = [name for name in replaced["files"] if name.endswith(".wav.bkp" if oot else ".aiff.bkp")]
    assert len(backups) == 1
    source = backups[0][:-4]
    assert replaced["files"][backups[0]] == baseline["files"][source]
    run("song-b-stock")
    run("song-b-stock-update")
    song_b = ("assets/audio/sequences/duckmidi_song_b.seq" if oot
              else "sound/sequences/us/04_level_inside_castle.m64")
    latest_b = digest(checkout / song_b)
    run("revert-sample")
    if build:
        native_build(checkout, oot, report.parent, "selective-revert")
    reverted = snapshot(checkout, oot)
    assert reverted["files"][source] == baseline["files"][source]
    assert backups[0] not in reverted["files"]
    assert digest(checkout / song_b) == latest_b, "Sample revert overwrote the latest B song"
    for _ in range(3):
        run("song-b-stock-update")
    connection = sqlite3.connect(f"file:{database.as_posix()}?mode=ro", uri=True)
    assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    assert not connection.execute("PRAGMA foreign_key_check").fetchall()
    scope = connection.execute("SELECT id FROM scopes WHERE root=?",
                               (str(checkout).replace("\\", "/"),)).fetchone()
    if not scope:
        scopes = connection.execute("SELECT id,root FROM scopes").fetchall()
        scope = next((row for row in scopes if Path(row[1]).samefile(checkout)), None)
    assert scope and connection.execute("SELECT count(*) FROM modifications WHERE scope=?",
                                       (scope[0],)).fetchone()[0] == 2
    connection.close()
    run("revert-all")
    assert snapshot(checkout, oot) == baseline, "Revert all did not restore initialized inputs"
    run("add")
    run("song-b")
    if build:
        native_build(checkout, oot, report.parent, "add")
    added = snapshot(checkout, oot)
    extension = ".wav" if oot else ".aiff"
    assets = [name for name in added["files"] if "Validation_tone_" in name and name.endswith(extension)]
    assert len(assets) == 2
    if not oot:
        for folder, extension in (("sound/sound_banks/", ".json"),
                                  ("sound/samples/instruments/", ".aiff")):
            before = sorted(p for p in baseline["files"] if p.startswith(folder) and p.endswith(extension))
            after = sorted(p for p in added["files"] if p.startswith(folder) and p.endswith(extension))
            assert after[:len(before)] == before, f"Add shifted native IDs in {folder}"
    run("revert-song-a")
    after_a = snapshot(checkout, oot)
    assert all(after_a["files"].get(name) == added["files"][name] for name in assets)
    run("revert-all")
    assert snapshot(checkout, oot) == baseline
    if other_bank:
        run("replace", "/SampleBank_2/" if oot else "/sfx_terrain/")
        if build:
            native_build(checkout, oot, report.parent, "other-bank-replace")
        run("revert-all")
        assert snapshot(checkout, oot) == baseline
    # Preset pan, reverb and vibrato commands plus baked filter and chorus through the real tools.
    for mode, target in (("add-effects", None), ("replace-effects", "auto-piano")):
        run(mode, target)
        if build:
            native_build(checkout, oot, report.parent, mode)
        run("revert-all")
        assert snapshot(checkout, oot) == baseline, f"Revert all after {mode} left changes"
    native_final = native_build(checkout, oot, report.parent, "revert-all") if build else None
    assert native_final == native_baseline, "Incremental Revert all did not rebuild baseline audio"
    return {"checkout": str(checkout), "baseline_files": len(baseline["files"]),
            "commands": len(log), "replacement": source, "shared_added_assets": assets,
            "native_baseline": native_baseline, "native_reverted": native_final}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--m64-test", type=Path, required=True)
    parser.add_argument("--oot-test", type=Path, required=True)
    parser.add_argument("--hacker-stage", type=Path, required=True)
    parser.add_argument("--sm64-stage", type=Path)
    parser.add_argument("--oot-stage", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--native-builds", action="store_true")
    parser.add_argument("--other-bank", action="store_true")
    parser.add_argument("--only", choices=("sm64", "hackersm64", "oot"))
    parser.add_argument("--plugins", type=Path, help="Reuse an isolated validation ledger under agents/reports")
    args = parser.parse_args()
    report = args.report.resolve()
    if not report.is_relative_to((ROOT / "agents/reports").resolve()):
        raise ValueError("Validation reports must stay under agents/reports")
    report.mkdir(parents=True, exist_ok=True)
    identity = uuid.uuid4().hex[:10]
    results = {}
    plugins = args.plugins.resolve() if args.plugins else report / f"plugins-shared-{identity}"
    if not plugins.is_relative_to((ROOT / "agents/reports").resolve()):
        raise ValueError("The validation ledger must stay under agents/reports")
    stages = [
            ("hackersm64", args.m64_test, args.hacker_stage, False),
            ("oot", args.oot_test, args.oot_stage, True)]
    if args.sm64_stage:
        stages.insert(0, ("sm64", args.m64_test, args.sm64_stage, False))
    expected_scopes = len(stages) if args.plugins else 1 if args.only else len(stages)
    for target, executable, checkout, oot in stages:
        if args.only and args.only != target:
            continue
        results[target] = validate(executable, checkout, plugins, oot,
                                   report / f"{target}-{identity}.json",
                                   args.native_builds, args.other_bank)
        print(f"PASS {target}: {results[target]}", flush=True)
        (report / f"results-{identity}.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    connection = sqlite3.connect(f"file:{(plugins / 'history/repositories.sqlite3').as_posix()}?mode=ro", uri=True)
    assert connection.execute("SELECT count(*) FROM scopes").fetchone()[0] == expected_scopes
    assert connection.execute("SELECT count(*) FROM modifications").fetchone()[0] == 0
    assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    connection.close()
    (report / f"results-{identity}.json").write_text(json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
