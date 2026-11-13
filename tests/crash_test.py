"""Run fatal handlers in child processes with isolated generated settings."""
import pathlib
import subprocess
import sys
import tempfile

helper = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=True)
version = subprocess.check_output([str(helper), "--version"], text=True).strip()
with tempfile.TemporaryDirectory(prefix="crash-", dir=root) as temporary:
    for mode in ("fault", "abort"):
        settings = pathlib.Path(temporary) / mode
        logs = settings / "logs"
        logs.mkdir(parents=True)
        for i in range(12):
            (logs / f"crash-2000-01-{i + 1:02d}_00-00-00.txt").write_text("old")
        result = subprocess.run([str(helper), str(settings), mode], timeout=20)
        assert result.returncode not in (0, 2, 3, 4), result.returncode
        reports = [p for p in logs.glob("crash-*.txt") if not p.name.startswith("crash-2000")]
        assert len(reports) == 1, reports
        report = reports[0].read_text(errors="replace")
        assert f"duckmidi {version}" in report, report
        assert "Stack trace:\n  #0 " in report, report
        assert "Application base: 0x" in report, report
        assert "breadcrumb 39" in report and "breadcrumb 00" not in report, report
        assert len(list(logs.glob("crash-2000*.txt"))) == 10
        print(f"PASS {mode}: version, stack, breadcrumbs and startup pruning")
