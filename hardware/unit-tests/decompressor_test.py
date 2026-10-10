from coyote_test import fpga_stream
from os.path import dirname, realpath, join
from random import randint

from libstf_utils.configured_test_case import ConfiguredTestCase
from parcore_utils.fpga_configuration import ColumnChunkDecoderConfig, Compression

def read_data(filename: str) -> bytearray:
    dir = dirname(realpath(__file__))
    with open(join(dir, 'data', filename), 'rb') as f:
        data = bytearray(f.read())

    return data

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
