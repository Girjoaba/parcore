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

def make_straddling_literals(seed: int, size: int) -> tuple[bytearray, bytearray]:
    """
    Returns a raw Snappy stream and its decompressed bytes. The stream repeats a long literal whose
    3-byte header starts at byte 30 of a 64-byte input beat, so it straddles two 32-byte lines, and
    whose data starts at a 32-byte aligned output position. A long literal directly precedes it, as
    in incompressible pages (e.g., random INT64 values), which Snappy writes as 64 KiB literals.
    Short literals and copies in between restore the alignment.
    """
    rng = Random(seed)
    compressed, decompressed = bytearray(), bytearray()

    preamble, n = bytearray(), size
    while n >= 0x80:
        preamble.append((n & 0x7f) | 0x80)
        n >>= 7
    preamble.append(n)

    def position() -> int:
        return len(preamble) + len(compressed)

    def literal(data: bytes):
        if len(data) <= 60:
            compressed.append((len(data) - 1) << 2)
        else:
            compressed.append(61 << 2)
            compressed.extend((len(data) - 1).to_bytes(2, 'little'))
        compressed.extend(data)
        decompressed.extend(data)

    def copy(offset: int, length: int):
        compressed.append(((length - 1) << 2) | 2)
        compressed.extend(offset.to_bytes(2, 'little'))
        for _ in range(length):
            decompressed.append(decompressed[-offset])

    literal(rng.randbytes(64))
    while True:
        # A copy and a short literal of m bytes put the next header at byte 27 of a beat and the
        # output at a 32-byte boundary
        while not 1 <= (27 - position() - 4) % 64 <= 60:
            literal(rng.randbytes(1))
        m = (27 - position() - 4) % 64
        copy(rng.randint(64, min(len(decompressed), 2000)), (-(len(decompressed) + m)) % 32 or 32)
        literal(rng.randbytes(m))

        # Then a literal of a multiple of 64 bytes, which puts the next header at byte 30
        first, second = rng.randint(2, 16) * 64, rng.randint(2, 16) * 32
        if len(decompressed) + first + second > size:
            break
        literal(rng.randbytes(first))
        literal(rng.randbytes(second))

    literal(rng.randbytes(size - len(decompressed)))

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

    def test_straddling_literal_headers_with_input_gaps(self):
        # Regression test: the quad Snappy core checked that the literals of the elements in its
        # group window were in the literal buffer, but used that check one cycle later, when an
        # element that just arrived could already be in the window. If the input then stalled, the
        # first line of a literal whose header straddles two lines read the third line before it
        # was written, which corrupted its last byte (seen as one wrong byte at offset 0x9001F of
        # PLAIN INT64 pages in ClickBench hits with multiple threads).
        self.set_system_verilog_defines({"DECOMPRESSOR_IN_STARVATION": "1"})
        compressed_data, decompressed_data = make_straddling_literals(0, 1 << 16)

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
