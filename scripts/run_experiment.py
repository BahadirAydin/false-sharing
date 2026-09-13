#!/usr/bin/env python3
"""Build, test, verify CPU topology, and preserve a reproducible experiment run."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def command(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, text=True, **kwargs)


def topology():
    result = []
    for cpu in sorted(os.sched_getaffinity(0)):
        base = Path(f"/sys/devices/system/cpu/cpu{cpu}")
        result.append({
            "cpu": cpu,
            "package": int(read(base / "topology/physical_package_id")),
            "core": int(read(base / "topology/core_id")),
            "siblings": read(base / "topology/thread_siblings_list"),
            "l1_data_line_bytes": int(read(base / "cache/index0/coherency_line_size")),
            "l1_type": read(base / "cache/index0/type"),
        })
    return result


def state(cpus):
    result = {}
    for cpu in cpus:
        base = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq")
        result[str(cpu)] = {name: read(base / name) for name in (
            "scaling_driver", "scaling_governor", "scaling_cur_freq", "scaling_min_freq",
            "scaling_max_freq", "cpuinfo_max_freq")}
    result["intel_pstate_no_turbo"] = read("/sys/devices/system/cpu/intel_pstate/no_turbo")
    result["cpufreq_boost"] = read("/sys/devices/system/cpu/cpufreq/boost")
    result["load_average"] = os.getloadavg()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpu-a", type=int)
    parser.add_argument("--cpu-b", type=int)
    parser.add_argument("--cpu-smt", type=int)
    parser.add_argument("--items", type=int, default=2_000_000)
    parser.add_argument("--rounds", type=int, default=11)
    parser.add_argument("--seed", type=int, default=20260913)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--line-size", type=int, default=64)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("The experiment runner requires Linux affinity and sysfs topology")
    if not 1 <= args.items <= 1_000_000_000 or not 1 <= args.rounds <= 1000:
        parser.error("Invalid items or rounds")
    if not 0 <= args.seed <= 0xFFFFFFFF:
        parser.error("Seed must fit in uint32")
    if args.line_size < 32 or args.line_size & (args.line_size - 1):
        parser.error("Line size must be a power of two, at least 32")
    topo = topology()
    by_cpu = {entry["cpu"]: entry for entry in topo}
    identity = lambda entry: (entry["package"], entry["core"])
    # Prefer not to use logical CPU 0 when enough other physical cores exist.
    candidates = sorted(topo, key=lambda e: (e["cpu"] == 0, e["cpu"]))
    a = args.cpu_a if args.cpu_a is not None else candidates[0]["cpu"]
    if a not in by_cpu:
        parser.error("CPU A is not allowed by the current affinity mask")
    choices = [e for e in candidates if e["package"] == by_cpu[a]["package"]
               and identity(e) != identity(by_cpu[a])]
    if args.cpu_b is None and not choices:
        parser.error("Two allowed physical cores in one package are required")
    b = args.cpu_b if args.cpu_b is not None else choices[0]["cpu"]
    if b not in by_cpu or identity(by_cpu[a]) == identity(by_cpu[b]):
        parser.error("CPU A and B must be on different allowed physical cores")
    if by_cpu[a]["package"] != by_cpu[b]["package"]:
        parser.error("Use one CPU package for this experiment")
    siblings = [e["cpu"] for e in topo if e["cpu"] != a and identity(e) == identity(by_cpu[a])]
    smt = args.cpu_smt if args.cpu_smt is not None else (siblings[0] if siblings else None)
    if smt is not None and smt not in siblings:
        parser.error("CPU SMT must be an allowed hardware-thread sibling of CPU A")
    selected = [a, b] + ([smt] if smt is not None else [])
    for cpu in selected:
        if by_cpu[cpu]["l1_type"] not in ("Data", "Unified"):
            parser.error("sysfs index0 is not a data/unified cache; inspect topology manually")
        if by_cpu[cpu]["l1_data_line_bytes"] != args.line_size:
            parser.error("Configured separation differs from detected L1 data cache-line size")
    compiler = shutil.which(args.compiler)
    if not compiler:
        parser.error(f"Compiler not found: {args.compiler}")
    build = args.build_dir.resolve()
    command(["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_CXX_COMPILER={compiler}", "-DCPP_WORK_SANITIZER=none",
             f"-DCPP_WORK_CACHE_LINE_SIZE={args.line_size}"])
    command(["cmake", "--build", build, "-j", "2"])
    tests = command(["ctest", "--test-dir", build, "--output-on-failure"], capture_output=True)
    print(tests.stdout, end="")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = args.output.resolve() if args.output else ROOT / "results" / stamp
    output.mkdir(parents=True, exist_ok=False)  # Never overwrite an earlier run.
    (output / "tests.txt").write_text(tests.stdout + tests.stderr)
    binary = build / "false_sharing_bench"
    bench_args = [binary, "--cpu-a", a, "--cpu-b", b, "--items", args.items,
                  "--rounds", args.rounds, "--seed", args.seed]
    if smt is not None:
        bench_args += ["--cpu-smt", smt]
    files = [ROOT / "CMakeLists.txt"]
    for folder in ("include", "benchmarks", "tests", "scripts"):
        files += [p for p in (ROOT / folder).rglob("*") if p.is_file()
                  and "__pycache__" not in p.parts]
    source_hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in sorted(files)}
    with tarfile.open(output / "source.tar.gz", "w:gz") as archive:
        for source in sorted(files):
            archive.add(source, arcname=str(source.relative_to(ROOT)))
    git = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                         text=True, capture_output=True)
    cpu_model = next((line.split(":", 1)[1].strip()
                      for line in Path("/proc/cpuinfo").read_text().splitlines()
                      if line.startswith("model name")), platform.machine())
    metadata = {
        "status": "running", "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "cpu_model": cpu_model, "platform": platform.platform(), "cpu_topology": topo,
        "selected_cpus": {"producer_or_counter_a": a, "consumer_or_counter_b": b, "smt": smt},
        "line_size": args.line_size, "items": args.items, "rounds": args.rounds,
        "seed": args.seed, "warmup_items_per_case": min(args.items, 100_000),
        "compiler_version": command([compiler, "--version"], capture_output=True).stdout,
        "compile_commands": json.loads((build / "compile_commands.json").read_text()),
        "command": [str(v) for v in bench_args], "before": state(selected),
        "git_head": git.stdout.strip() if git.returncode == 0 else None,
        "source_sha256": source_hashes,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "notes": "Queue ns/op is amortized wall time, not per-message latency. "
                 "Every queue payload is checked in order inside the timed loop. "
                 "CPU frequency samples are snapshots, not a stable-frequency guarantee.",
    }
    meta_path = output / "environment.json"
    meta_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"CPUs: A={a}, B={b}, SMT={smt}; output: {output}", flush=True)
    try:
        with (output / "samples.csv").open("w") as samples, (output / "benchmark.log").open("w") as log:
            command(bench_args, stdout=samples, stderr=log, timeout=300)
        metadata["status"] = "complete"
    except Exception as error:
        metadata["status"] = "failed"
        metadata["error"] = str(error)
        raise
    finally:
        metadata["after"] = state(selected)
        metadata["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        meta_path.write_text(json.dumps(metadata, indent=2) + "\n")
    command([sys.executable, ROOT / "scripts/summarize.py", output])
    print((output / "summary.md").read_text())


if __name__ == "__main__":
    main()
