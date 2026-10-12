# RunDecoder Benchmark

Throughput benchmark for the `RunDecoder` of the ParCore hardware library against the
`run_decoder_test` hardware design
([hardware/unit-tests/vfpga_tops/run_decoder_test.sv](../../hardware/unit-tests/vfpga_tops/run_decoder_test.sv)).

Synthetic Parquet RLE/bit-packed hybrid pages are generated on the host, streamed through the
vFPGA and timed. For RLE the interesting knob is the run length (values per RLE run,
`--run-lengths`); for BPE it is the bit width of the packed values (`--bit-widths`). Both are
swept as a cross product per selected encoding and one CSV row is printed per configuration.

## Synthesizing the hardware

```bash
scripts/synthesize.sh run-decoder
```

## Building the software

```bash
cmake -S . -B build && cmake --build build --target run_decoder
```

For simulations, add `-DEN_SIMULATION=ON` to the first cmake call.

## Running the example

```bash
build/run_decoder [OPTIONS]
```

| Option | Description |
| --- | --- |
| `-e, --encoding ENC` | Encoding to benchmark: `rle`, `bpe` or `both` (default: both) |
| `-b, --bit-widths LIST` | Comma-separated bit widths of the encoded values, 1-19 (default: 1,2,4,8,16,19) |
| `-l, --run-lengths LIST` | Comma-separated values per run; for BPE rounded up to whole groups of 8, duplicates after rounding are skipped (default: 1,4,16,64,256,1024) |
| `-n, --num-values N` | Decoded values per page (default: 4Mi) |
| `-r, --reps N` | Repetitions per benchmark row (default: 10) |
| `-d, --depth N` | Pages kept in flight, at most the config FIFO capacity of the design (8) (default: 4) |
| `-s, --seed N` | Seed for the input data generator (default: 42) |
| `-v, --verify` | Check the decoded output against the expected values once per row |

For simulations, add `COYOTE_SIM_DIR="/<path-to-this-repo>/hardware/build-sim"` before executing
the example software. It is recommended to use a small page (`-n 1024`) and a single repetition
per row (`-r 1 -d 1`).
