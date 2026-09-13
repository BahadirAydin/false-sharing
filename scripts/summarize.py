#!/usr/bin/env python3
"""Generate descriptive statistics and an SVG from saved experiment samples."""
import csv
import html
import json
from pathlib import Path
import statistics
import sys


def main():
    directory = Path(sys.argv[1])
    meta = json.loads((directory / "environment.json").read_text())
    if meta["status"] != "complete":
        raise ValueError("Refusing to summarize an incomplete or failed run")
    groups = {}
    with (directory / "samples.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            key = (row["group"], row["variant"], row["placement"])
            groups.setdefault(key, []).append(row)
    expected_cases = 9 if meta["selected_cpus"]["smt"] is not None else 8
    if len(groups) != expected_cases:
        raise ValueError("Missing benchmark cases")
    stats = []
    for key, rows in sorted(groups.items()):
        if sorted(int(r["round"]) for r in rows) != list(range(meta["rounds"])):
            raise ValueError(f"Missing or duplicate rounds: {key}")
        values = [float(row["ns_per_op"]) for row in rows]
        stats.append((key, min(values), statistics.median(values), max(values)))
    lines = ["# Experiment results", "", f"Machine: {meta['cpu_model']}. "
             f"{meta['rounds']} measured rounds per case; {meta['items']:,} operations per round.", "",
             "Counters: elapsed wall time until all writers finish / increments per thread. "
             "Queue: elapsed wall time until the consumer finishes / delivered items. "
             "Thread creation and joins are outside the measured interval. "
             "Barrier release and worker scheduling are included.", "",
             "| Group | Variant | Placement | Min ns/op | Median ns/op | Max ns/op |",
             "| --- | --- | --- | ---: | ---: | ---: |"]
    for (group, variant, placement), low, median, high in stats:
        lines.append(f"| {group} | {variant} | {placement} | {low:.3f} | {median:.3f} | {high:.3f} |")
    values = {key: median for key, _, median, _ in stats}
    counter_ratio = (values[("counters", "shared_line", "different_cores")] /
                     values[("counters", "separate_lines", "different_cores")])
    queue_ratio = (values[("queue", "shared_uncached", "different_cores")] /
                   values[("queue", "separated_cached", "different_cores")])
    lines += ["", f"The shared-line counters took {counter_ratio:.2f} times the separated-line "
              "median time per increment on different cores.", "",
              f"The separated, cached queue achieved {queue_ratio:.2f} times the sustained "
              "throughput of the shared, uncached queue, using the ratio of median ns/item.", "",
              "The queue includes per-item FIFO validation and full/empty retry counters. "
              "It uses uint64 payloads stored in optional slots (see benchmark.log for slot size). "
              "These are measurements of this implementation and workload. They are not message "
              "latency percentiles, isolated coherence costs, or guarantees for another machine.", "",
              "Raw data: [samples.csv](samples.csv). Setup and source hashes: "
              "[environment.json](environment.json). [Chart](comparison.svg).", ""]
    (directory / "summary.md").write_text("\n".join(lines))
    # Plain SVG keeps plotting reproducible using Python's standard library alone.
    height = 130 + 46 * len(stats)
    width, left, plot_width = 1100, 390, 550
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
           '<title id="title">False sharing: measured time per operation</title>',
           '<desc id="desc">Dots show median, whiskers show minimum to maximum across rounds. '
           'Counters and queues use separate horizontal scales.</desc>',
           '<rect width="100%" height="100%" fill="#fff"/>',
           '<g font-family="sans-serif" font-size="14" fill="#17212b">',
           '<text x="25" y="30" font-size="21">False sharing: time per operation</text>',
           '<text x="25" y="55">Median with min–max range. Lower is faster. Separate scale per group.</text>']
    y = 75
    for group in ("counters", "queue"):
        entries = [s for s in stats if s[0][0] == group]
        maximum = max(s[3] for s in entries) * 1.1
        y += 25
        svg.append(f'<text x="25" y="{y}" font-weight="bold">{group.upper()} '
                   f'(0–{maximum:.1f} ns/op)</text>')
        for (_, name, placement), low, median, high in entries:
            y += 40
            x = lambda value: left + plot_width * value / maximum
            label = html.escape(f"{name} / {placement}")
            color = "#20618b" if group == "counters" else "#087a60"
            svg += [f'<text x="25" y="{y+5}" font-size="12">{label}</text>',
                    f'<line x1="{left}" y1="{y}" x2="{left+plot_width}" y2="{y}" stroke="#e4e9ee"/>',
                    f'<line x1="{x(low):.2f}" y1="{y}" x2="{x(high):.2f}" y2="{y}" stroke="{color}" stroke-width="3"/>',
                    f'<circle cx="{x(median):.2f}" cy="{y}" r="5" fill="{color}"/>',
                    f'<text x="970" y="{y+5}">{median:.3f} ns</text>']
    svg += ['</g>', '</svg>']
    (directory / "comparison.svg").write_text("\n".join(svg) + "\n")


if __name__ == "__main__":
    main()
