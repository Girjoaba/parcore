[![SystemVerilog](https://img.shields.io/badge/SystemVerilog-IEEE%201800-blue.svg)](https://github.com/celeris-labs/parcore)
[![GitHub last commit](https://img.shields.io/github/last-commit/celeris-labs/parcore)](https://github.com/celeris-labs/parcore/commits/main)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

# ParCore - Hardware Parquet Decoder

ParCore is a hardware decoder for Parquet column chunks of specific compression/encoding
combinations (Snappy or no compression, plain and dictionary/hybrid encoding, data page v1). The
main components are: the ParCore hardware design for [Coyote](https://github.com/fpgasystems/Coyote)
vFPGAs, a software library to configure and drive it, and examples that benchmark its components.
ParCore is the Parquet decoder of [Oasis](https://github.com/celeris-labs/oasis), which also
schedules column chunks over the decoders and integrates them into DuckDB.

ParCore only supports flat columns, i.e., columns without repetition levels
(`max_repetition_level == 0`). Columns that are, or are nested in, repeated fields (lists, maps) are
not supported, because the hardware does not strip the repetition levels at the start of their data
pages. Definition levels are supported: they are stripped for OPTIONAL columns and absent for
REQUIRED columns.

ParCore depends on [libSTF](https://github.com/fpgasystems/libstf), which in turn contains Coyote,
and on a [fork](https://github.com/JonasDann/vhsnunzip) of
[VHSNUnzip](https://github.com/abs-tudelft/vhsnunzip). The dependencies are loaded by either cloning
this repo with submodules directly:

```bash
git clone --recurse-submodules git@github.com:celeris-labs/parcore.git
```

Or initializing the submodules as a step after cloning:

```bash
git submodule update --init --recursive
```

## Hardware
The functionality of the hardware components can be verified with unit tests that are built on top
of the Coyote unit test framework. We also describe how to synthesize the hardware.

### Unit tests
To run the unit tests, a Vivado simulation project needs to be set up:

```bash
./scripts/setup_simulation.sh
```

After this script is finished, VSCode shows the unit tests as a test flask on the left side of the
UI. Run unit tests by clicking on the corresponding run button. The simulation project needs to be
regenerated whenever new files are added (also true for the dependencies). The tests need `pyarrow`.

### Synthesis
For synthesis, execute the following command with the example to build the bitstream for (only
tested for the build servers in the [HACC](https://github.com/fpgasystems/hacc), i.e.,
`hacc-build-**`):

```bash
./scripts/synthesize.sh <decompressor|run-decoder|column-chunk-decoder>
```

The script spins off the synthesis in the background in a new tmux session so the user can
disconnect from the server without the synthesis stopping. You can check the progress in
`hardware/build-**/bitgen.log`. It is expected that the synthesis takes multiple hours to finish
sometimes not printing anything new to the log for a while. The finished bitstream will be available
in `hardware/build-**/bitstreams/cyt_top.bit`.

### Programming the FPGA (in the HACC)
To program an FPGA, book one of the `hacc-u55c-**` or `hacc-gpu-u55c-01` servers and clone the
Coyote repo. Inside the Coyote repo, go to the driver folder and execute `make -j`. Then, execute
this command in the Coyote repo root:

```bash
./util/program_hacc_local.sh <bitstream> driver/build/coyote_driver.ko
```

The easiest way to share bitstreams across servers is to copy them to `/scratch/<nethz-user>/...`
which is available as a network mounted file system across servers.

## Software
The software consists of the ParCore software library and examples.

### ParCore library
The ParCore software library contains the host-side counterparts of the hardware: the configuration
of the `ColumnChunkDecoder`s (`configuration.hpp`), a `ColumnChunkDecoder` class that streams a
column chunk to a decoder and returns its output (`column_chunk_decoder.hpp`), and the column chunk
metadata it needs (`metadata/metadata.hpp`). Scheduling column chunks over decoders and reading
Parquet files is left to the application, e.g., Oasis.

The library has dependencies on the Coyote and libSTF software libraries. It uses installed Coyote
and libSTF packages if CMake finds them and the submodules otherwise. libSTF also has a dependency on
jemalloc that can be installed with `./libstf/scripts/install_jemalloc.sh`. The ParCore software
library can be built as follows:

```bash
mkdir software/build
cmake -S software -B software/build
cmake --build software/build -j
```

On the HACC cluster, your home directory is a network mounted file system available across all
servers. So, if you want to install the library to e.g., `$HOME/opt`, you need to append
`-DCMAKE_INSTALL_PREFIX=$HOME/opt` to the first `cmake` command and execute
`cmake --install software/build` after the build.

## Publications

If you use ParCore, please cite:

```
@article{journals/corr/abs-2608-02268,
  author       = {Jonas Dann and
                  Luca Tagliavini and
                  Gustavo Alonso},
  title        = {Oasis: Hiding the Cost of Querying Parquet Files in the Datapath},
  journal      = {CoRR},
  volume       = {abs/2608.02268},
  year         = {2026},
  doi          = {10.48550/ARXIV.2608.02268}
}
```

## License
The ParCore code is licensed under the terms in
[LICENSE.md](https://github.com/celeris-labs/parcore/blob/main/LICENSE.md), which corresponds to the
MIT Licence. Any contributions to ParCore will be accepted under the terms of the same license.
