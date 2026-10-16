#!/usr/bin/env bash
# Runs the fuzzers of a BOWL_FUZZ build, each for a while, one after another:
#
#     tests/fuzz/run.sh build/fuzz            # every fuzzer, 60 seconds each
#     tests/fuzz/run.sh build/fuzz 600 font   # just the font fuzzer, 10 minutes
#
# Each fuzzer starts from its seeds and keeps what it finds in <build>/fuzz/corpus/<name>, so later runs pick up where
# earlier ones stopped. An input that crashes, leaks, hangs or trips a sanitizer lands in <build>/fuzz/crashes; replay
# it with `<build>/fuzz/fuzz_<name> <file>`. Exits non-zero if any fuzzer found something.
set -u

build=${1:?usage: run.sh <build dir> [seconds] [fuzzer...]}
seconds=${2:-60}
shift $(($# < 2 ? $# : 2))
dir="$build/fuzz"
if [ $# -gt 0 ]; then
    names=("$@")
else
    names=()
    for exe in "$dir"/fuzz_*; do names+=("$(basename "$exe" | sed 's/^fuzz_//')"); done
fi

mkdir -p "$dir/crashes"
failed=()
for name in "${names[@]}"; do
    mkdir -p "$dir/corpus/$name"
    echo "== $name: $seconds s"
    # -timeout: an input that takes over 10 s is a hang. -rss_limit_mb: one that needs over 2 GB is a memory bomb.
    if ! "$dir/fuzz_$name" "$dir/corpus/$name" "$dir/seeds/$name" -max_total_time="$seconds" -timeout=10 \
        -rss_limit_mb=2048 -print_final_stats=1 -artifact_prefix="$dir/crashes/$name-" 2>"$dir/$name.log"; then
        failed+=("$name")
        tail -n 40 "$dir/$name.log"
    fi
    grep -E "^stat::(number_of_executed_units|new_units_added)" "$dir/$name.log"
done

if [ ${#failed[@]} -gt 0 ]; then
    echo "Found something: ${failed[*]} (inputs in $dir/crashes, logs in $dir/<name>.log)"
    exit 1
fi
echo "Nothing found."
