from dataclasses import dataclass
from coyote_test import fpga_stream
from os.path import dirname, realpath, join

from libstf_utils.configured_test_case import ConfiguredTestCase
from parcore_utils.fpga_configuration import RunDecoderConfig


def read_data(filename: str) -> bytearray:
    dir = dirname(realpath(__file__))
    with open(join(dir, 'data', filename), 'rb') as f:
        return bytearray(f.read())


@dataclass
class _RunDecoderConfig:
    """One RunDecoder page configuration (RunDecoderConfig registers).

    The fixtures store a decompressed page as
        def_levels_length (4 bytes LE) | def_levels | bit_width (1 byte) | body
    so the RLE/BPE body starts at byte `offset` (4 + len(def_levels) + 1).
    """
    bit_width: int
    offset: int
    num_values: int


class RunDecoderTestCase(ConfiguredTestCase):
    alternative_vfpga_top_file = "vfpga_tops/run_decoder_test.sv"
    debug_mode = True

    def configure(self):
        self.config.get_config(RunDecoderConfig).enqueue_page(
            self.page_config.bit_width, self.page_config.offset, self.page_config.num_values
        )
        self.set_stream_input(0, self.chunk)
        self.set_expected_output(
            0, fpga_stream.Stream(fpga_stream.StreamType.SIGNED_INT_32, self.output)
        )

    def run_page(self, config: _RunDecoderConfig, chunk: bytearray, output: list[int]):
        """Drive the RunDecoder with a single configured page and assert its output.

        Page configuration is written to the RunDecoderConfig, the page bytes
        are streamed in, and the expected output is asserted.
        """
        self.page_config = config
        self.chunk = chunk
        self.output = output

        self.simulate_fpga()
        self.assert_simulation_output()

    # Each fixture's def_levels length prefix is 3 bytes, so the body starts at
    # offset 8 (4-byte prefix + 3 def_levels + 1 bit_width byte).
    def test_one_rle_page(self):
        self.run_page(
            _RunDecoderConfig(bit_width=4, offset=8, num_values=145),
            read_data('rle_data_rg0_col0_chunk_decompressed.bin'),
            [v for i in range(10, 20) for v in [i - 10] * i],
        )

    def test_one_bpe_page(self):
        self.run_page(
            _RunDecoderConfig(bit_width=4, offset=8, num_values=150),
            read_data('bpe_data_rg0_col0_chunk_decompressed.bin'),
            list(range(10 - 10, 20 - 10)) * 15,
        )

    def test_mixed_page(self):
        self.run_page(
            _RunDecoderConfig(bit_width=8, offset=8, num_values=802),
            read_data('mixed_data_rg0_col0_chunk_decompressed.bin'),
            ([v for i in range(10, 20) for v in [i - 10] * i]
             + list(range(128 - 118, 256 - 118)) * 2) * 2,
        )

    def test_rle_bit_width_19_long_runs(self):
        # RLE runs of 64 values at the maximum supported bit width of 19
        # (ID_BITS): a 2-byte count varint plus 3 value bytes makes 5-byte
        # runs, so 64 runs sweep all run-start alignments of the decoder's
        # 64-byte double buffer.
        values = [(i * 2654435761) % 2**19 for i in range(64)]
        body = bytearray()
        for value in values:
            body += bytes([0x80, 0x01]) + value.to_bytes(3, 'little')  # varint(64 << 1)
        chunk = bytearray((3).to_bytes(4, 'little')) + bytearray(3) + bytes([19]) + body
        self.run_page(
            _RunDecoderConfig(bit_width=19, offset=8, num_values=64 * len(values)),
            chunk,
            [v for value in values for v in [value] * 64],
        )

    def test_bpe_short_runs(self):
        # Back-to-back BPE runs of a single group (8 values) at bit_width=1,
        # i.e. shorter than one output databeat (NUM_ELEMENTS = 16). This is
        # what the run_decoder benchmark generates for --run-lengths <= 8.
        # 2 bytes per run (header varint + 1 packed byte), 64 runs, so the
        # page wraps the decoder's 64-byte double buffer several times.
        # Every run packs a different byte (37 is odd, so i -> 37 * i + 11 is
        # a bijection mod 256), so dropped, duplicated or reordered runs change
        # the output.
        packed_runs = [(37 * i + 11) & 0xFF for i in range(64)]
        values = [(packed >> j) & 1 for packed in packed_runs for j in range(8)]
        body = bytearray()
        for packed in packed_runs:
            body += bytes([(1 << 1) | 1, packed])  # one bit-packed group of 8 values
        chunk = bytearray((3).to_bytes(4, 'little')) + bytearray(3) + bytes([1]) + body
        self.run_page(
            _RunDecoderConfig(bit_width=1, offset=8, num_values=len(values)),
            chunk,
            values,
        )

    def test_back_to_back_bpe(self):
        # Two BPE runs of 256 values (bit_width=8). The first run is long enough
        # to wrap the RunDecoder's 64-byte double-buffer and then finishes into
        # the second run, exercising finish_bpe()'s offset-wrap handling.
        vals1 = [i % 200 for i in range(256)]
        vals2 = [(i * 3) % 200 for i in range(256)]
        self.run_page(
            _RunDecoderConfig(bit_width=8, offset=8, num_values=512),
            read_data('back_to_back_bpe_chunk_decompressed.bin'),
            vals1 + vals2,
        )

    def test_bpe_bit_width_16_two_groups(self):
        # Back-to-back BPE runs of two groups (16 values, one output databeat) at bit_width=16:
        # 1 header byte plus 32 packed bytes per run, so the run starts shift by one byte per run
        # relative to the decoder's 64-byte double buffer.
        values = [(i * 2654435761) % 2**16 for i in range(16 * 64)]
        body = bytearray()
        for run in range(len(values) // 16):
            body.append((2 << 1) | 1)
            for value in values[run * 16:(run + 1) * 16]:
                body += value.to_bytes(2, 'little')
        chunk = bytearray((3).to_bytes(4, 'little')) + bytearray(3) + bytes([16]) + body
        self.run_page(
            _RunDecoderConfig(bit_width=16, offset=8, num_values=len(values)),
            chunk,
            values,
        )
