#!/usr/bin/env bash
# Builds duckmidi's OoT demo songs with a zeldaret/oot checkout's own audio tools, without writing
# into the checkout. Run in the Linux environment that builds the decomp (for example WSL):
#
#   tools/validate-oot-export.sh <oot checkout> <folder with .seq/.xml> [version]
#
# Create the folder's files with `oot_export_test <folder>`. The checkout must have been built
# once for the version (default ntsc-1.0). Each soundfont XML is compiled by sfc into a C file and
# header; each sequence is preprocessed with aseq.h, assembled with the MIPS assembler, then
# disassembled again with the decomp's disassembler. Intermediate files go to <folder>/oot-build.
set -euo pipefail

oot=$(cd "$1" && pwd)
dir=$(cd "$2" && pwd)
version=${3:-ntsc-1.0}
build="$oot/build/$version"
work="$dir/oot-build"
prefix=""
mkdir -p "$work"

for candidate in mips-linux-gnu- mips64-elf- mips64-linux-gnu- mips64-ultra-elf- mips64-; do
    if command -v "${candidate}as" >/dev/null; then
        prefix=$candidate
        break
    fi
done

if [[ -z $prefix ]]; then
    echo "No MIPS binutils found." >&2
    exit 1
fi

for xml in "$dir"/Soundfont_*.xml; do
    [[ -e $xml ]] || continue
    name=$(basename "${xml%.xml}")
    sed "s|\$(BUILD_DIR)|$build|g" "$xml" >"$work/$name.xml"
    # The built sample bank names its samples relative to the checkout root.
    (cd "$oot" && tools/audio/sfc "$work/$name.xml" "$work/$name.c" "$work/$name.h" "$work/$name.name")
    echo "sfc: $name.xml compiled; $(grep -c '^#define SF' "$work/$name.h") names in $name.h"
done

for seq in "$dir"/*.seq; do
    name=$(basename "${seq%.seq}")
    out="$work/$name"
    gcc -E -x assembler-with-cpp -fno-dollars-in-identifiers -D_LANGUAGE_ASEQ \
        -DMML_VERSION=MML_VERSION_OOT -DOOT_VERSION=NTSC_1_0 -DOOT_REVISION=0 \
        -DOOT_REGION=REGION_US -DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 \
        -I "$oot/include" -I "$oot/include/audio" -I "$oot/include/tables/sfx" \
        -I "$work" -I "$build/assets/audio/soundfonts" "$seq" -o "$out.s"
    "${prefix}as" -march=vr4300 -32 -no-pad-sections -I"$oot/include" -I"$oot/include/audio" \
        "$out.s" -o "$out.o"
    "${prefix}objcopy" -O binary -j .data "$out.o" "$out.aseq"
    fonts=$("${prefix}objdump" -s -j .note.fonts "$out.o" | awk '/^ [0-9a-f]+ /{print $2}')
    echo "as: $name.seq assembled to $(stat -c %s "$out.aseq") bytes; .note.fonts bytes: $fonts"

    if command -v python3 >/dev/null; then
        (cd "$oot/tools/audio" && PYTHONDONTWRITEBYTECODE=1 python3 -B -m extraction.disassemble_sequence \
            "$out.aseq" "$out.dis.seq")
        echo "disassembled: $name.dis.seq, $(grep -c 'note' "$out.dis.seq") note commands"
    fi
done
