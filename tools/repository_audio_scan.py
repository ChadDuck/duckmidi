#!/usr/bin/env python3
"""Read-only native audio discovery probe for exporter validation.

This is a validation tool, not an application entry point. It never initializes
history, invokes Git, reads backups as originals, or writes into a checkout.
"""

import argparse
import hashlib
import json
import os
import re
import stat
from pathlib import Path, PurePosixPath
from xml.etree import ElementTree as ET


MAX_FILES = 20000
MAX_BYTES = 1024 * 1024 * 1024
MAX_DEFINITION = 8 * 1024 * 1024
VERSIONS = {
    "sm64": {"us", "jp", "eu", "sh", "cn"},
    "hackersm64": {"us", "jp", "eu", "sh", "cn"},
}


class DiscoveryError(ValueError):
    pass


class AudioXmlBuilder(ET.TreeBuilder):
    def doctype(self, name, pubid, system):
        raise DiscoveryError("Unsupported XML DTD/entity declarations")


def relative(value):
    """Accept only canonical, unambiguous repository-relative input paths."""
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        raise DiscoveryError(f"Invalid relative audio path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise DiscoveryError(f"Unsafe relative audio path: {value!r}")
    if any(":" in part for part in path.parts):
        raise DiscoveryError(f"Unsupported audio path: {value!r}")
    return path


def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise DiscoveryError(f"Duplicate JSON member: {key}")
        result[key] = value
    return result


class Inventory:
    def __init__(self, root):
        self.root = Path(root).resolve(strict=True)
        if not self.root.is_dir() or self.root == Path(self.root.anchor):
            raise DiscoveryError("Select a checkout directory, not a filesystem root")
        self.files = {}
        self.identities = {}
        self.edges = []
        self.aliases = []
        self.total_bytes = 0

    def path(self, name, missing=False):
        parts = relative(name).parts
        current = self.root
        for part in parts:
            current = current / part
            try:
                info = current.lstat()
            except FileNotFoundError:
                if missing:
                    return None
                raise DiscoveryError(f"Missing audio input: {name}") from None
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_reparse_tag", 0):
                raise DiscoveryError(f"Symlink/reparse audio path: {name}")
            if current != self.root / Path(*parts) and not stat.S_ISDIR(info.st_mode):
                raise DiscoveryError(f"Audio path parent is not a directory: {name}")
        return current

    def exists(self, name):
        return self.path(name, missing=True) is not None

    def children(self, name):
        folder = self.path(name, missing=True)
        if folder is None:
            return []
        if not folder.is_dir():
            raise DiscoveryError(f"Expected an audio directory: {name}")
        # Enumerate only explicitly scoped directories; never use a checkout-wide glob.
        return sorted(folder.iterdir(), key=lambda entry: entry.name)

    def collect(self, folder, suffixes, kind, recursive=False, depth=0):
        if depth > 32:
            raise DiscoveryError(f"Audio directory nesting limit exceeded: {folder}")
        for entry in self.children(folder):
            name = entry.relative_to(self.root).as_posix()
            info = entry.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_reparse_tag", 0):
                raise DiscoveryError(f"Symlink/reparse audio path: {name}")
            if stat.S_ISDIR(info.st_mode):
                if recursive:
                    self.collect(name, suffixes, kind, recursive=True, depth=depth + 1)
            elif any(entry.name.endswith(suffix) for suffix in suffixes):
                self.add(name, kind)

    def add(self, name, kind):
        if name in self.files:
            return
        path = self.path(name)
        info = path.stat()
        if not stat.S_ISREG(info.st_mode):
            raise DiscoveryError(f"Audio input is not a regular file: {name}")
        if len(self.files) >= MAX_FILES or info.st_size > MAX_BYTES - self.total_bytes:
            raise DiscoveryError(f"Audio inventory limit exceeded at {name}")
        identity = (info.st_dev, info.st_ino)
        if identity in self.identities:
            raise DiscoveryError(f"Aliased audio files: {self.identities[identity]} and {name}")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (opened.st_dev, opened.st_ino) != identity:
                raise DiscoveryError(f"Audio file changed while opening: {name}")
            count = 0
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                count += len(block)
                if count > info.st_size:
                    raise DiscoveryError(f"Audio file changed while reading: {name}")
                digest.update(block)
            after = os.fstat(stream.fileno())
        if count != info.st_size or after.st_mtime_ns != info.st_mtime_ns:
            raise DiscoveryError(f"Audio file changed while reading: {name}")
        self.identities[identity] = name
        self.total_bytes += count
        self.files[name] = {"path": name, "kind": kind, "size": count,
                            "sha256": digest.hexdigest()}

    def optional(self, name, kind):
        if self.exists(name):
            self.add(name, kind)

    def definition(self, name):
        if self.files[name]["size"] > MAX_DEFINITION:
            raise DiscoveryError(f"Definition exceeds size limit: {name}")
        data = self.path(name).read_bytes()
        if hashlib.sha256(data).hexdigest() != self.files[name]["sha256"]:
            raise DiscoveryError(f"Definition changed during discovery: {name}")
        return data

    def json(self, name):
        try:
            return json.loads(self.definition(name), object_pairs_hook=no_duplicates,
                              parse_constant=lambda value: self.invalid_number(value))
        except (ValueError, UnicodeError, RecursionError) as error:
            raise DiscoveryError(f"{name}: {error}") from error

    @staticmethod
    def invalid_number(value):
        raise DiscoveryError(f"Non-finite JSON number: {value}")

    def xml(self, name):
        data = self.definition(name)
        if re.search(br"<!\s*(DOCTYPE|ENTITY)", data, re.IGNORECASE):
            raise DiscoveryError(f"Unsupported XML DTD/entity declarations: {name}")
        try:
            return ET.fromstring(data, parser=ET.XMLParser(target=AudioXmlBuilder()))
        except ET.ParseError as error:
            raise DiscoveryError(f"{name}: {error}") from error

    def edge(self, consumer, provider, role, selector, **values):
        if provider not in self.files:
            raise DiscoveryError(f"{consumer}: unresolved {role} {provider} ({selector})")
        if len(self.edges) >= MAX_FILES * 32:
            raise DiscoveryError("Audio dependency limit exceeded")
        self.edges.append({"consumer": consumer, "provider": provider, "role": role,
                           "selector": selector, **values})

    def backups(self):
        # Existing backups are inventory only, never a source of original audio.
        for name in list(self.files):
            if not name.endswith(".bkp"):
                self.optional(name + ".bkp", "preexisting_backup")


def component(value, context):
    if not isinstance(value, str) or "/" in value or len(relative(value).parts) != 1:
        raise DiscoveryError(f"{context}: expected a single native asset name")
    return value


def detect(inv):
    markers = {
        "oot": "tools/audio/soundfont_compiler.c",
        "sm64coopdx": "src/pc/lua/utils/smlua_audio_utils.c",
        "hackersm64": "include/config/config_audio.h",
    }
    found = [target for target, marker in markers.items() if inv.exists(marker)]
    if len(found) > 1:
        raise DiscoveryError(f"Ambiguous repository target markers: {', '.join(found)}")
    if found:
        return found[0]
    if inv.exists("tools/assemble_sound.py") and inv.exists("sound/sequences.json"):
        return "sm64"
    raise DiscoveryError("Unrecognized audio checkout layout")


def sm64(inv, version):
    if version not in VERSIONS["sm64"]:
        raise DiscoveryError(f"Unsupported SM64 build version: {version}")
    inv.add("sound/sequences.json", "sequence_registration")
    inv.collect("sound/sound_banks", (".json",), "sound_bank")
    inv.collect("sound/sound_banks", (".json.bkp",), "preexisting_backup")
    for folder in inv.children("sound/samples"):
        name = folder.relative_to(inv.root).as_posix()
        if folder.is_symlink():
            raise DiscoveryError(f"Symlink audio path: {name}")
        if folder.is_dir():
            inv.collect(name, (".aiff",), "sample")
            inv.collect(name, (".aiff.bkp",), "preexisting_backup")
    inv.collect("sound/sequences", (".m64", ".s"), "sequence")
    inv.collect(f"sound/sequences/{version}", (".m64", ".s"), "sequence")
    inv.collect("sound/sequences", (".m64.bkp", ".s.bkp"), "preexisting_backup")
    inv.collect(f"sound/sequences/{version}", (".m64.bkp", ".s.bkp"), "preexisting_backup")
    for name in ("src/audio/data.c", "src/audio/data.h", "src/buffers/buffers.c",
                 "include/config/config_audio.h"):
        inv.optional(name, "memory_configuration")
    banks = {PurePosixPath(name).stem: name for name, item in inv.files.items()
             if item["kind"] == "sound_bank"}
    if not banks or not any(item["kind"] == "sample" for item in inv.files.values()):
        raise DiscoveryError("Extract audio inputs before using this checkout")

    def sound(consumer, bank, value, selector, depth=0):
        if depth > 32:
            raise DiscoveryError(f"{consumer}: sound-reference nesting limit exceeded")
        if isinstance(value, dict) and "ifdef" in value and "then" in value and "else" in value:
            branch = "then" if condition(value["ifdef"], consumer) else "else"
            sound(consumer, bank, value[branch], f"{selector}/{branch}", depth + 1)
            return
        if value is None:
            return
        if isinstance(value, str):
            sample, tuning = value, None
        elif isinstance(value, dict) and isinstance(value.get("sample"), str):
            sample, tuning = value["sample"], value.get("tuning")
            if tuning is not None and (not isinstance(tuning, (int, float)) or tuning <= 0):
                raise DiscoveryError(f"{consumer}: invalid tuning at {selector}")
        else:
            raise DiscoveryError(f"{consumer}: unsupported sound reference at {selector}")
        component(sample, consumer)
        inv.edge(consumer, f"sound/samples/{bank}/{sample}.aiff", "sample", selector,
                 tuning=tuning, representation="string" if isinstance(value, str) else "object")

    def instrument(consumer, bank, value, selector, depth=0):
        if depth > 32:
            raise DiscoveryError(f"{consumer}: instrument nesting limit exceeded")
        if value is None:
            return
        if isinstance(value, list):
            for index, item in enumerate(value):
                instrument(consumer, bank, item, f"{selector}/{index}", depth + 1)
        elif isinstance(value, dict):
            if "ifdef" in value and not condition(value["ifdef"], consumer):
                return
            for key in ("sound_lo", "sound", "sound_hi"):
                if key in value:
                    sound(consumer, bank, value[key], f"{selector}/{key}")
        else:
            raise DiscoveryError(f"{consumer}: unsupported instrument at {selector}")

    def condition(value, consumer):
        if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
            raise DiscoveryError(f"{consumer}: invalid native ifdef")
        # Unknown compiler definitions need an explicit build configuration,
        # rather than silently selecting the wrong branch.
        known = {f"VERSION_{name.upper()}" for name in VERSIONS["sm64"]}
        if any(item not in known for item in value):
            raise DiscoveryError(f"{consumer}: unsupported native condition {value}")
        return f"VERSION_{version.upper()}" in value

    for name in sorted(banks.values()):
        data = inv.json(name)
        if not isinstance(data, dict):
            raise DiscoveryError(f"{name}: expected bank object")
        bank = data.get("sample_bank")
        while isinstance(bank, dict) and all(key in bank for key in ("ifdef", "then", "else")):
            bank = bank["then" if condition(bank["ifdef"], name) else "else"]
        bank = component(bank, name)
        instruments = data.get("instruments")
        if not isinstance(instruments, dict):
            raise DiscoveryError(f"{name}: missing native instruments")
        for key, value in instruments.items():
            escaped = key.replace("~", "~0").replace("/", "~1")
            instrument(name, bank, value, f"/instruments/{escaped}")
    registration = inv.json("sound/sequences.json")
    if not isinstance(registration, dict):
        raise DiscoveryError("sound/sequences.json: expected sequence object")
    for name, value in registration.items():
        if name == "comment":
            continue
        if isinstance(value, dict):
            conditions = value.get("ifdef")
            if not condition(conditions, "sound/sequences.json"):
                continue
            value = value.get("banks")
        if value is None:
            continue
        if not isinstance(value, list):
            raise DiscoveryError(f"sound/sequences.json: invalid banks for {name}")
        for index, bank in enumerate(value):
            component(bank, "sound/sequences.json")
            if bank not in banks:
                raise DiscoveryError(f"sound/sequences.json: missing bank {bank} for {name}")
            inv.edge("sound/sequences.json", banks[bank], "bank", f"/{name}/{index}")
            sources = [path for path, item in inv.files.items()
                       if item["kind"] == "sequence" and PurePosixPath(path).stem == name]
            if len(sources) != 1:
                raise DiscoveryError(f"sound/sequences.json: expected one sequence source for {name}, found {sources}")
            inv.edge(sources[0], banks[bank], "bank", f"/registration/{name}/{index}")


def native_path(value, folder, suffix, context):
    prefix = f"$(BUILD_DIR)/assets/audio/{folder}/"
    if not isinstance(value, str) or not value.startswith(prefix) or not value.endswith(suffix):
        raise DiscoveryError(f"{context}: unsupported native path {value!r}")
    return f"assets/audio/{folder}/" + str(relative(value[len(prefix):]))


def oot(inv, version):
    if not re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,47}", version):
        raise DiscoveryError(f"Invalid OoT version: {version}")
    extracted = f"extracted/{version}/"
    # These definitions are enumerated together, including duplicates. WAV and
    # sequence pattern rules prefer authored inputs; retain shadowed inputs too.
    effective = {}
    shadowed = []
    for folder, suffix, kind in (("samples", ".wav", "sample"),
                                 ("samplebanks", ".xml", "sample_bank"),
                                 ("soundfonts", ".xml", "soundfont"),
                                 ("sequences", ".seq", "sequence")):
        for prefix in (extracted, ""):
            base = f"{prefix}assets/audio/{folder}"
            inv.collect(base, (suffix,), kind, recursive=True)
            inv.collect(base, (suffix + ".bkp",), "preexisting_backup", recursive=True)
            for name in sorted(inv.files):
                if not name.startswith(base + "/") or inv.files[name]["kind"] != kind:
                    continue
                key = name[len(prefix):]
                if key in effective:
                    if kind in ("sample_bank", "soundfont"):
                        raise DiscoveryError(f"Duplicate native XML input: {effective[key]} and {name}")
                    shadowed.append({"effective": name, "shadowed": effective[key]})
                effective[key] = name
    for name in ("include/tables/sequence_table.h", "spec/spec",
                 "src/audio/game/session_config.c", "src/buffers/audio_heap.c",
                 "include/buffers.h"):
        inv.add(name, "registration" if name in ("spec/spec", "include/tables/sequence_table.h")
                else "memory_configuration")
    banks, indices, fonts, font_indices = {}, {}, {}, {}

    def index(value, context):
        try:
            number = int(value, 0) if value.startswith("0x") else int(value, 10)
        except (TypeError, ValueError, AttributeError):
            raise DiscoveryError(f"{context}: invalid native index {value!r}") from None
        if not 0 <= number <= 255:
            raise DiscoveryError(f"{context}: native index out of range: {number}")
        return number

    for logical, physical in sorted(effective.items()):
        if inv.files[physical]["kind"] not in ("sample_bank", "soundfont"):
            continue
        tree = inv.xml(physical)
        expected = "SampleBank" if inv.files[physical]["kind"] == "sample_bank" else "Soundfont"
        if tree.tag != expected:
            raise DiscoveryError(f"{physical}: expected {expected} root")
        number = index(tree.get("Index"), physical)
        table = indices if expected == "SampleBank" else font_indices
        if number in table:
            raise DiscoveryError(f"Duplicate native {expected} index {number}: {table[number]} and {physical}")
        table[number] = physical
        if expected == "SampleBank":
            samples = {}
            for child in tree:
                if child.tag == "Pointer":
                    alias = index(child.get("Index"), physical)
                    if alias in indices:
                        raise DiscoveryError(f"Duplicate samplebank pointer index {alias}: {physical}")
                    indices[alias] = physical
                    inv.aliases.append({"index": alias, "definition_index": number, "path": physical})
                elif child.tag == "Sample":
                    name = child.get("Name")
                    if not name or name in samples:
                        raise DiscoveryError(f"{physical}: missing/duplicate sample name {name!r}")
                    native = native_path(child.get("Path"), "samples", ".aifc", physical)
                    logical_wav = native[:-5] + ".wav"
                    if logical_wav not in effective:
                        raise DiscoveryError(f"{physical}: no source WAV for {native}")
                    samples[name] = effective[logical_wav]
                    inv.edge(physical, samples[name], "sample", f"Sample[@Name={name!r}]")
                else:
                    raise DiscoveryError(f"{physical}: unsupported samplebank element {child.tag}")
            banks[logical] = (physical, samples)
        else:
            fonts[logical] = (physical, tree)
    if not banks or not any(item["kind"] == "sample" for item in inv.files.values()):
        raise DiscoveryError("Extract audio inputs for the selected OoT build version")
    for physical, tree in fonts.values():
        selected = {}
        for attr, dd in (("SampleBank", False), ("SampleBankDD", True)):
            value = tree.get(attr)
            if value is None:
                continue
            native = native_path(value, "samplebanks", ".xml", physical)
            if native not in banks:
                raise DiscoveryError(f"{physical}: unresolved {attr} {native}")
            selected[dd] = banks[native]
            inv.edge(physical, banks[native][0], "sample_bank", f"@{attr}")
        if False not in selected:
            raise DiscoveryError(f"{physical}: missing cart SampleBank")
        # Defaults are local to each soundfont. References carry any per-consumer
        # attributes intact, including IsDD and tuning overrides.
        defaults = {}
        for sample in tree.findall("Samples/Sample"):
            name = sample.get("Name")
            if not name or name in defaults:
                raise DiscoveryError(f"{physical}: missing/duplicate Samples name {name!r}")
            defaults[name] = {**tree.find("Samples").attrib, **sample.attrib}

        def visit(element, selector, depth=0):
            if depth > 32:
                raise DiscoveryError(f"{physical}: soundfont nesting limit exceeded")
            for attr, value in element.attrib.items():
                if attr not in ("Sample", "SampleLo", "SampleHi"):
                    continue
                if value not in defaults:
                    raise DiscoveryError(f"{physical}: {value} has no Samples entry at {selector}")
                # IsDD belongs to Samples/Sample, not to individual regions.
                config = {**defaults[value], **element.attrib}
                config["IsDD"] = defaults[value].get("IsDD", "false")
                dd_value = config.get("IsDD", "false")
                if dd_value not in ("true", "false"):
                    raise DiscoveryError(f"{physical}: invalid IsDD at {selector}")
                bank = selected.get(dd_value == "true")
                if bank is None or value not in bank[1]:
                    raise DiscoveryError(f"{physical}: unresolved {attr} {value} at {selector}")
                inv.edge(physical, bank[1][value], "sample", selector + "/@" + attr,
                         sample_name=value, attributes=config)
            for position, child in enumerate(element):
                visit(child, f"{selector}/{child.tag}[{position}]", depth + 1)

        visit(tree, "/Soundfont")
        for name, config in defaults.items():
            dd = config.get("IsDD", "false")
            bank = selected.get(dd == "true")
            if dd not in ("true", "false") or bank is None or name not in bank[1]:
                raise DiscoveryError(f"{physical}: unresolved Samples default {name}")
            inv.edge(physical, bank[1][name], "sample_default", f"/Samples/Sample[@Name={name!r}]",
                     sample_name=name, attributes=config)
    for logical, physical in sorted(effective.items()):
        if inv.files[physical]["kind"] != "sequence":
            continue
        try:
            source = inv.definition(physical).decode("utf-8")
        except UnicodeError as error:
            raise DiscoveryError(f"{physical}: invalid sequence source encoding") from error
        # Native .seq sources include the generated font header to select banks.
        # Do not inspect/decompile sequence bytecode or unrelated C includes.
        for header in re.findall(r'^\s*#\s*include\s+"(Soundfont_[^"/]+)\.h"', source, re.MULTILINE):
            font = f"assets/audio/soundfonts/{header}.xml"
            if font not in fonts:
                raise DiscoveryError(f"{physical}: unresolved soundfont header {header}.h")
            inv.edge(physical, fonts[font][0], "soundfont", f'#include "{header}.h"')
    return shadowed


def coopdx(inv):
    for folder in inv.children("mods"):
        if not folder.name.startswith("duckmidi_"):
            continue
        component(folder.name, "mods")
        base = f"mods/{folder.name}"
        inv.optional(base + "/main.lua", "mod_script")
        inv.optional(base + "/main.lua.bkp", "preexisting_backup")
        inv.collect(base + "/sound", (".m64",), "sequence")
        inv.collect(base + "/sound", (".m64.bkp",), "preexisting_backup")


def scan(root, version=None):
    inv = Inventory(root)
    target = detect(inv)
    if version is None:
        version = "ntsc-1.0" if target == "oot" else "us"
    shadowed = []
    if target in ("sm64", "hackersm64"):
        sm64(inv, version)
    elif target == "oot":
        shadowed = oot(inv, version)
    else:
        version = "mod"
        coopdx(inv)
    inv.backups()
    return {"probe_schema": 1, "target": target, "root": str(inv.root), "version": version,
            "capabilities": {"sequence": True, "custom_layout": target != "sm64coopdx",
                             "writes_enabled": False},
            "files": sorted(inv.files.values(), key=lambda item: item["path"]),
            "dependencies": sorted(inv.edges, key=lambda e: (e["consumer"], e["selector"], e["provider"])),
            "samplebank_aliases": inv.aliases, "shadowed_inputs": shadowed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkout", type=Path)
    parser.add_argument("--version")
    args = parser.parse_args()
    try:
        result = scan(args.checkout, args.version)
    except (DiscoveryError, OSError) as error:
        parser.exit(1, f"Audio discovery failed: {error}\n")
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
