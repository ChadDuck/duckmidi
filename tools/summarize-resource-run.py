"""Summarize per-process sampler CSV; preserve raw measurements in the report folder."""
import argparse
import csv
from datetime import datetime, timedelta
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument('report', type=Path)
args = parser.parse_args()
rows = list(csv.DictReader((args.report / 'resources.csv').open(encoding='utf-8-sig')))
groups = {}
for row in rows:
    phase = row['Phase']
    if phase in ('settling', 'large-open-dialog', 'transition', 'large-editing', 'large-editing-actions'):
        continue
    if phase == 'large-loading':
        start = datetime.fromisoformat((args.report / 'load-start.txt').read_text(encoding='utf-8-sig').strip())
        end = datetime.fromisoformat((args.report / 'load-observed.txt').read_text(encoding='utf-8-sig').strip())
        if not start <= datetime.fromisoformat(row['Utc']) <= end:
            continue
    if phase == 'large-export-wav' and (args.report / 'export-times.json').exists():
        timing = json.loads((args.report / 'export-times.json').read_text(encoding='utf-8-sig'))
        start = datetime.fromisoformat(timing['CreatedUtc']) + timedelta(seconds=2)
        end = datetime.fromisoformat(timing['FinishedUtc'])
        if not start <= datetime.fromisoformat(row['Utc']) <= end:
            continue
    groups.setdefault(phase, []).append(row)
summary = []
for phase, samples in groups.items():
    stable = phase in ('sample-active', 'sample-inactive', 'sample-minimized',
                       'large-idle-active', 'large-inactive', 'large-minimized', 'large-playback')
    # Window actions precede the next label by a UI observation; remove boundary samples.
    if stable and len(samples) >= 10:
        samples = samples[2:-3]
    def values(field):
        return [float(row[field]) for row in samples if row[field] != '']
    def mean(field):
        data = values(field)
        return round(statistics.mean(data), 3) if data else None
    def peak(field):
        data = values(field)
        return round(max(data), 3) if data else None
    summary.append(dict(phase=phase, samples=len(samples),
                        sampled_seconds=round(sum(values('IntervalSeconds')), 2),
                        ram_mean_mib=mean('WorkingSetMiB'), ram_peak_mib=peak('WorkingSetMiB'),
                        private_mean_mib=mean('PrivateMiB'), private_peak_mib=peak('PrivateMiB'),
                        cpu_mean_percent=mean('CpuPercent'), cpu_peak_percent=peak('CpuPercent'),
                        gpu_mean_percent=mean('GpuPercent'), gpu_peak_percent=peak('GpuPercent')))
(args.report / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
with (args.report / 'summary.csv').open('w', newline='', encoding='utf-8') as stream:
    writer = csv.DictWriter(stream, fieldnames=summary[0].keys())
    writer.writeheader(); writer.writerows(summary)
print(json.dumps(summary, indent=2))
