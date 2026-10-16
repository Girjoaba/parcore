from coyote_test import fpga_stream
from os.path import dirname, realpath, join
from random import randint, Random

from libstf_utils.configured_test_case import ConfiguredTestCase
from parcore_utils.fpga_configuration import ColumnChunkDecoderConfig, Compression

def read_data(filename: str) -> bytearray:
    dir = dirname(realpath(__file__))
    with open(join(dir, 'data', filename), 'rb') as f:
        data = bytearray(f.read())

    return data

def make_overlapping_copies(seed: int, num_elements: int) -> tuple[bytearray, bytearray]:
    """
    Returns a raw Snappy stream and its decompressed bytes. The stream alternates short literals with
    self-overlapping copies (offset < length) whose offset is not a power of two, as Snappy emits for
    short repeating patterns (e.g., 3-bit packed dictionary indices).
    """
    rng = Random(seed)
    compressed, decompressed = bytearray(), bytearray()

    def literal(data: bytes):
        compressed.append((len(data) - 1) << 2)
        compressed.extend(data)
        decompressed.extend(data)

    def copy(offset: int, length: int):
        if 4 <= length <= 11 and offset < 2048:
            compressed.extend([((offset >> 8) << 5) | ((length - 4) << 2) | 1, offset & 0xff])
        else:
            compressed.append(((length - 1) << 2) | 2)
            compressed.extend(offset.to_bytes(2, 'little'))
        # Byte by byte, so that an offset smaller than the length repeats the last offset bytes
        for _ in range(length):
            decompressed.append(decompressed[-offset])

    literal(bytes(rng.randrange(256) for _ in range(32)))
    for _ in range(num_elements):
        literal(bytes(rng.randrange(256) for _ in range(rng.randint(1, 8))))
        offset = rng.choice([o for o in range(3, 32) if o & (o - 1) != 0])
        copy(offset, rng.randint(offset + 1, min(64, 2 * offset + 8)))
        if rng.random() < 0.25:
            copy(rng.randint(32, min(len(decompressed), 2000)), rng.randint(4, 32))

    # Preamble: decompressed length as varint
    preamble, n = bytearray(), len(decompressed)
    while n >= 0x80:
        preamble.append((n & 0x7f) | 0x80)
        n >>= 7
    preamble.append(n)

    return preamble + compressed, decompressed

class DecompressorTestCase(ConfiguredTestCase):
    alternative_vfpga_top_file = "vfpga_tops/decompressor_test.sv"
    debug_mode = True
    # verbose_logging = True

    def configure(self):
        # The decompressor test top only consumes the compression field of the column chunk config.
        config = self.config.get_config(ColumnChunkDecoderConfig)
        for compression, _, _ in self.pages:
            config.enqueue_column_chunk(0, compression, 0, fpga_stream.StreamType.UNSIGNED_INT_8)
        for _, input, _ in self.pages:
            self.set_stream_input(0, input)
        for _, _, output in self.pages:
            self.set_expected_output(0, output)

    def run_pages(self, pages):
        """Streams in (compression, input, expected output) pages and asserts the outputs."""
        self.pages = pages

        # Act
        self.simulate_fpga()

        # Assert
        self.assert_simulation_output()

    def test_one_page_decompress(self):
        compressed_data = read_data('data_rg0_col0_chunk_compressed.bin')
        decompressed_data = read_data('data_rg0_col0_chunk_decompressed.bin')

        self.run_pages([(Compression.SNAPPY, compressed_data, decompressed_data)])

    def test_overlapping_copies_with_backpressure(self):
        # Regression test: the quad Snappy core cuts a self-overlapping copy with a non-power-of-two
        # offset to one period and doubles the offset for the rest. While the output was stalled,
        # it doubled the offset of the head copy without emitting the period, so the copy read from
        # twice the offset (seen as wrong l_linenumber values in TPC-H lineitem).
        self.set_system_verilog_defines({"DECOMPRESSOR_OUT_BACKPRESSURE": "1"})
        compressed_data, decompressed_data = make_overlapping_copies(0, 1000)

        self.run_pages([(Compression.SNAPPY, compressed_data, decompressed_data)])

    def test_two_different_compression_decompress(self):
        n = 1024
        data = [randint(-n, n) for _ in range(n)]
        data_type = fpga_stream.StreamType.SIGNED_INT_32
        raw_data = fpga_stream.Stream(data_type, data)

        compressed_data = read_data('data_rg0_col0_chunk_compressed.bin')
        decompressed_data = read_data('data_rg0_col0_chunk_decompressed.bin')

        self.run_pages([
            (Compression.SNAPPY, compressed_data, decompressed_data),
            (Compression.RAW, raw_data, raw_data),
        ])

    def test_multiple_compression_decompress(self):
        reps = 2
        n = 1024
        data = [randint(-n, n) for _ in range(n)]
        data_type = fpga_stream.StreamType.SIGNED_INT_32
        raw_data = fpga_stream.Stream(data_type, data)

        compressed_data = read_data('data_rg0_col0_chunk_compressed.bin')
        decompressed_data = read_data('data_rg0_col0_chunk_decompressed.bin')

        self.run_pages([
            (Compression.SNAPPY, compressed_data, decompressed_data),
            (Compression.RAW, raw_data, raw_data),
            (Compression.SNAPPY, compressed_data, decompressed_data),
        ] * reps)
