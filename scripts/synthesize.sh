#!/bin/bash

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_dir="$(dirname "$script_dir")"

usage() {
    cat <<'EOF'
Usage: synthesize.sh <example>

Configure and launch a Vivado bitstream build for one of the ParCore examples
(see examples/) in a fresh hardware/build-NN directory (run detached in a 
tmux session).

Examples:
  decompressor         Build the bitstream for examples/01_decompressor.
  run-decoder          Build the bitstream for examples/02_run_decoder.

Options:
  -h, --help           Show this help message and exit.

Usage examples:
  synthesize.sh decompressor
  synthesize.sh run-decoder
EOF
}

example=""
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        decompressor) example=decompressor; top=decompressor_test ;;
        run-decoder|run_decoder) example=run-decoder; top=run_decoder_test ;;
        *) echo "Unknown argument: $1" >&2; echo "" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

if [ -z "$example" ]; then
    echo "No example given." >&2
    echo "" >&2
    usage >&2
    exit 1
fi

pushd "$repo_dir/hardware"

# Finds the build directory with the highest number and starts the synthesis in a new directory with that number + 1
n=0
for d in build-[0-9][0-9]; do
    [ -d "$d" ] || continue
    num="${d#build-}"
    [ "$((10#$num))" -gt "$n" ] && n=$((10#$num))
done
build_dir="$PWD/build-$(printf '%02d' $((n + 1)))"
echo "Building $example bitstream in hardware/$(basename "$build_dir")..."

mkdir "$build_dir"
# The examples reuse the vFPGA tops of the unit tests.
cmake -S . -B "$build_dir" \
    -DVFPGA_TOP="$repo_dir/hardware/unit-tests/vfpga_tops/$top.sv" || exit 1

session="parcore-$(basename "$build_dir")"
log="$build_dir/bitgen.log"
# tmux runs the command with its default shell, so the command is run through bash explicitly.
build_cmd="cmake --build $(printf %q "$build_dir") --target project --target bitgen > $(printf %q "$log") 2>&1"
tmux new-session -d -s "$session" "bash -c $(printf %q "$build_cmd")"
echo "Synthesis running in tmux session $session (log: $log)"
