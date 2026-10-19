#!/usr/bin/env python3
"""Validate preserved pairs and render their complete component result tables."""
import json
from pathlib import Path
import statistics


ROOT = Path(__file__).resolve().parent


def read(component, compiler):
    data = json.loads((ROOT / f"{component}-{compiler}-timed.json").read_text())
    groups = 20 if component == "external" else 16
    assert data["rounds"] == 7 and not data["clock_suppressed"]
    assert data["finished_utc"] and len(data["summary"]) == groups
    assert len(data["samples"]) == groups * 14
    keys = ["native", "icache", "scenario"] if component == "external" else ["writer", "placement", "icache"]
    for summary in data["summary"]:
        samples = [s for s in data["samples"] if all(s[k] == summary[k] for k in keys)]
        assert len(samples) == 14
        for round_ in range(7):
            pair = sorted([s for s in samples if s["round"] == round_], key=lambda s: s["order"])
            assert [s["variant"] for s in pair] == (["before", "after"] if round_ % 2 == 0 else ["after", "before"])
            assert all(not s["profile_enabled"] and s["elapsed_ns"] > 0 for s in pair)
        for variant in ["before", "after"]:
            expected = statistics.median(s["ns_per_call"] for s in samples if s["variant"] == variant)
            assert expected == summary[variant + "_ns"]
    return data


def main():
    lines = ["# External code-write and HLE output measurements", "",
        "The complete integrated Round 5 source was compared with the preceding public review.",
        "This is a component study on one shared x86-64 host, not a game/FPS or physical handheld benchmark.", "",
        "## Source and method", "",
        "- Before: clean local `85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f`, tree-identical to public `2bace9afd9c3d1e5e7cac9d23379e2f60d167fec`.",
        "- After: clean local source/test freeze `865f88e2d095f71ac80935bd775a38f3b8985246`. Later commits change documentation only.",
        "- AMD EPYC 9V74, Linux 6.18.55, CPU affinity 0. GCC 13.3 and Clang 18.1.3; identical `-O3 -DNDEBUG -std=c2x -fno-lto` driver flags. CMake core libraries require C23 and disable IPO.",
        "- Profiling is disabled in every timed binary. Separate profiling-enabled binaries pass 40 external and 32 HLE verification rows per compiler, with clocks suppressed; native rows require all retired instructions to be native.",
        "- Every individual condition has seven before/after pairs, ordered AB, BA, AB, BA, AB, BA, AB. No outlier was removed. The two studies contain 560 external and 448 HLE timed samples: **1,008 total**.",
        "- All builds, sanitizer tests and functional/WASM executions completed before the sequential measurement window. Agents continued lightweight documentation work. Other shared-host activity, frequency scaling and temperature were not controlled.",
        "- Each sample excludes setup, 64 warmups and verification. The driver verifies registers, PC, CPSR, retired work and every byte of 8 MiB RAM outside its clock. The compiler barrier materializes repeated host stores; it is not a CPU memory fence.",
        "- The tables show medians of seven samples in nanoseconds per call, their percentage change, the median of seven paired percentage changes, and the number of improving pairs. Negative change means less time.", "",
        "The build manifests contain every compiler command, source and driver hash, matching core-library compilation, and executable hash. The eight `*-build.log` files match the manifest checksums. Timing JSON files retain every sample and pair; `measurement-window.json` records all four sequential commands. Its original context wording overstated the pause as all agent execution; documentation work continued. The context was corrected without changing commands, times or samples, and the exact original manifest and runner are retained as `measurement-window-original.json` and `run-measurements-original.py`. `driver-selfcheck-*` compares a baseline executable with itself and is only a harness self-check, not an optimization result.", "",
        "These are **whole-source-snapshot comparisons**. Mapping correctness, rare-region tracking, direct-span bounds and compiler layout changes coexist with the external-watch refinement and string batching. No isolated percentage is assigned exclusively to one edited C function.", "",
        "## External-write workload", "",
        "Each timed iteration stores one data word, optionally notifies an external physical span, restores r0 and PC, and executes `MOV r0,#0x11; BX r12`. The native-disabled case uses the cached executor. MMU is disabled; CP15.I=0 and I=1 are separate cases. Every sample runs 250,000 iterations and retires exactly 500,000 guest instructions. The unit includes the store, notification, two public register updates and dispatch; it is not isolated notification latency.", "",
        "| Scenario | Meaning |", "| --- | --- |",
        "| 0 | No-notification control; the data store still executes. |",
        "| 1 | Four-byte data notification immediately after the two watched instructions, on the same page. |",
        "| 2 | A 128-byte non-code span farther away on the same page; only its first word is written in this workload. |",
        "| 3 | Four-byte data notification on another page. |",
        "| 4 | Notification overlaps an unchanged instruction word, while the materialized data store is at code+8. This positive-hit control must still renew the epoch. |", "",
        "Same-page data notifications avoid false-positive epoch changes after refinement. All 112 pairs in scenarios 1/2 improve; their median component time falls by about 27–34% for cached and 49–54% for native execution. Other-page notifications add about 1.1–2.2 ns, and actual watched-word notifications add about 2.3–4.7 ns. These costs are retained, not called noise. The old path remains correct in these particular data cases but performs unnecessary translation revalidation; this study does not imply compilation on every epoch change.", ""]
    for compiler in ["gcc", "clang"]:
        data = read("external", compiler)
        lines += [f"### External: {compiler}", "",
            "| Executor | I | Scenario | Before ns | After ns | Change | Paired median | Improving pairs |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for r in data["summary"]:
            lines.append(f"| {'Native' if r['native'] else 'Cached'} | {r['icache']} | {r['scenario']} | {r['before_ns']:.3f} | {r['after_ns']:.3f} | {r['change_percent']:+.2f}% | {statistics.median(r['paired_change_percent']):+.2f}% | {r['improved_pairs']}/7 |")
        lines.append("")
    lines += ["## HLE output workload", "",
        "A real guest MCR and two-instruction cached block establish the I-bit and code watches before timing. **No guest instruction executes in the timed loop**. The measured gateway writes a word, a byte, a 32-byte C string, or a 256-byte C string. String sizes include the terminator. Scalar cases execute 2,000,000 outputs; string cases 500,000. Data either occupies a separate page or starts 128 bytes after the translated code. All data outputs preserve the epoch in the coverage run.", "",
        "The earlier scalar gateways omitted required code-change notification. Restoring that correctness costs about 2.2–3.7 ns per tested output, corresponding to 71–136% for gateways that previously took only 2.5–3.1 ns. These relative regressions are real component costs, not a whole-emulator slowdown estimate. The string gateway replaces repeated one-byte mapping/writes with the existing bounded bulk copy and one notification; tested 32-byte strings take about 82–84% less time and 256-byte strings about 97–98% less time. All 112 string pairs improve. Existing 512-byte cstring bounds and partial scalar semantics remain covered by separate functional regressions.", ""]
    for compiler in ["gcc", "clang"]:
        data = read("hle", compiler)
        lines += [f"### HLE: {compiler}", "",
            "| Writer | Placement | I | Before ns | After ns | Added ns | Change | Paired median | Improving pairs |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for r in data["summary"]:
            writer = ["word", "byte", "cstring 32", "cstring 256"][r["writer"]]
            place = "same code page" if r["placement"] else "different page"
            lines.append(f"| {writer} | {place} | {r['icache']} | {r['before_ns']:.3f} | {r['after_ns']:.3f} | {r['added_ns']:+.3f} | {r['change_percent']:+.2f}% | {statistics.median(r['paired_change_percent']):+.2f}% | {r['improved_pairs']}/7 |")
        lines.append("")
    lines += ["## Interpretation and remaining limits", "",
        "The external-watch optimization exchanges additional notification filtering for fewer false-positive translation invalidations. Its value depends on the proportion of writes to data sharing a translated code page. Correct notification remains necessary even if scalar-only workloads are slower. No game instruction histogram or device profile was available to weight these cases into an application-wide percentage. The no-notification controls move between -3.34% and +3.28%, demonstrating snapshot/layout and host effects even outside the edited notification path.", "",
        "This hot MMU-off study does not measure raw ARM dispatch, cold collection/emission, arena allocation, MMU aliases, AArch64 host costs, iOS JIT permission handling, or frontend audio/video scheduling. Functional regressions cover several of those semantics separately. The JIT predicate candidates have their own controlled studies and remain rejected; their target gains must not be added to these adopted results.", "",
        "## Reproduction", "",
        "Use the final checkout's drivers for both builds. Create a before worktree at the public prior-review commit, then build matching source snapshots using `scripts/build_core_write_benchmarks.py` (see `--help` and the recorded manifests). Run the two `--verify` drivers first with profile1 binaries. With other local work stopped, run:", "",
        "```sh",
        "python3 docs/benchmarks/1.1.0-round5/writes/run-measurements.py \\",
        "  /path/to/final-checkout /path/to/before-build /path/to/after-build \\",
        "  /path/to/new-results",
        "```", "",
        "The recorded source, executable and driver hashes make this evidence reproducible without treating local audit commit IDs as public Git objects. `render-summary.py` validates all 1,008 retained sample/group/order records and regenerates these tables; it does not rerun or adjust a measurement.", ""]
    (ROOT / "README.md").write_text("\n".join(lines))
    print("Validated 1,008 samples and rendered every external/HLE result group.")


if __name__ == "__main__":
    main()
