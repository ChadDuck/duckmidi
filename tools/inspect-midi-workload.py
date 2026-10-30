"""Read-only workload inventory of the largest Standard MIDI files in a directory."""
import argparse
import json
from pathlib import Path
import struct

def inspect(path):
    data = path.read_bytes()
    if data[:4] != b'MThd':
        raise ValueError('Not a Standard MIDI file')
    size = int.from_bytes(data[4:8], 'big')
    fmt, tracks, ppq = struct.unpack('>HHH', data[8:14])
    if ppq & 0x8000:
        raise ValueError('SMPTE timing')
    offset = 8 + size
    events = notes = controllers = sysex = 0
    channels = set()
    tempos = [(0, 500000)]
    end_tick = 0
    for _ in range(tracks):
        if data[offset:offset+4] != b'MTrk':
            raise ValueError('Missing track')
        end = offset + 8 + int.from_bytes(data[offset+4:offset+8], 'big')
        pos, tick, running = offset + 8, 0, None
        def vlq():
            nonlocal pos
            value = 0
            while True:
                b = data[pos]; pos += 1
                value = (value << 7) | (b & 127)
                if not b & 128:
                    return value
        while pos < end:
            tick += vlq()
            status = data[pos]
            if status & 128:
                pos += 1
                if status < 0xf0:
                    running = status
            else:
                if running is None:
                    raise ValueError('Invalid running status')
                status = running
            events += 1
            if status == 0xff:
                kind = data[pos]; pos += 1
                length = vlq()
                if kind == 0x51 and length == 3:
                    tempos.append((tick, int.from_bytes(data[pos:pos+3], 'big')))
                pos += length
            elif status in (0xf0, 0xf7):
                length = vlq(); sysex += length; pos += length
            elif status < 0xf0:
                channels.add(status & 15)
                kind = status & 0xf0
                length = 1 if kind in (0xc0, 0xd0) else 2
                if kind == 0x90 and data[pos+1]:
                    notes += 1
                if kind in (0xb0, 0xe0, 0xa0, 0xd0):
                    controllers += 1
                pos += length
            else:
                raise ValueError('Unsupported system status')
        end_tick = max(end_tick, tick)
        offset = end
    seconds, previous, tempo = 0.0, 0, 500000
    for tick, value in sorted(tempos):
        seconds += (tick-previous)*tempo/ppq/1e6
        previous, tempo = tick, value
    seconds += (end_tick-previous)*tempo/ppq/1e6
    return dict(path=str(path), bytes=len(data), format=fmt, tracks=tracks, channels=len(channels),
                notes=notes, events=events, controllers=controllers, sysex_bytes=sysex,
                tempo_events=len(tempos)-1, duration_seconds=round(seconds, 3))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('--limit', type=int, default=30)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--candidates', type=Path, help='JSON array of preselected FullName paths')
    args = parser.parse_args()
    if args.candidates:
        files = [Path(row['FullName']) for row in json.loads(args.candidates.read_text(encoding='utf-8-sig'))]
    else:
        files = [p for p in args.directory.rglob('*') if p.is_file() and p.suffix.lower() in ('.mid', '.midi')]
    results = []
    for path in sorted(files, key=lambda p: p.stat().st_size, reverse=True)[:args.limit]:
        try:
            results.append(inspect(path))
        except Exception as error:
            results.append(dict(path=str(path), error=str(error)))
    args.output.write_text(json.dumps(dict(inspected_candidates=len(files), candidates=results), indent=2), encoding='utf-8')
    print(json.dumps(sorted((r for r in results if 'notes' in r), key=lambda r:r['notes'], reverse=True), indent=2))
