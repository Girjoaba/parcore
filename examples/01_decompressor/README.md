# Decompressor Benchmark

Throughput benchmark for the Snappy `Decompressor` of the ParCore hardware library against the
`decompressor_test` hardware design
([hardware/unit-tests/vfpga_tops/decompressor_test.sv](../../hardware/unit-tests/vfpga_tops/decompressor_test.sv)).

Synthetic raw Snappy pages are generated on the host with a controllable mix of literal and copy
elements, streamed through the vFPGA and timed. The literal-to-copy mix is controlled through
`--copy-fractions`: the fraction of decompressed bytes that are produced by copy elements
(0 = literals only, close to 1 = almost only copies). One CSV row is printed per copy fraction.

## Synthesizing the hardware

```bash
scripts/synthesize.sh decompressor
```

## Building the software

```bash
cmake -S . -B build && cmake --build build --target decompressor
```

For simulations, add `-DEN_SIMULATION=ON` to the first cmake call.

## Running the example

```bash
build/decompressor [OPTIONS]
```

| Option | Description |
| --- | --- |
| `-c, --copy-fractions LIST` | Comma-separated fractions of decompressed bytes produced by copy elements, one benchmark row each (default: 0,0.25,0.5,0.75,0.95) |
| `-s, --size BYTES` | Decompressed page size (default: 8MiB) |
| `-l, --literal-length N` | Bytes per literal element, 1-65536 (default: 32) |
| `-k, --copy-length N` | Bytes per copy element, 4-64 (default: 32) |
| `-r, --reps N` | Repetitions per benchmark row (default: 10) |
| `-d, --depth N` | Pages kept in flight, at most the config FIFO capacity of the design (64) (default: 4) |
| `-S, --seed N` | Seed for the input data generator (default: 42) |
| `-v, --verify` | Check the decompressed output against the expected data once per row (and per RAW row with `--raw`) |
| `-R, --raw` | Also benchmark the RAW (bypass) path with the uncompressed data |

For simulations, add `COYOTE_SIM_DIR="/<path-to-this-repo>/hardware/build-sim"` before executing
the example software. It is recommended to use a small page (`-s 4096`) and a single repetition
per row (`-r 1 -d 1`).
