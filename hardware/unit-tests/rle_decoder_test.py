from dataclasses import dataclass
from coyote_test import fpga_test_case, fpga_stream

@dataclass
class _TestCase:
    element: int
    count: int

    @property
    def input(self) -> bytearray:
        # One 64-byte databeat per run: value (4 bytes) followed by the run length
        # (vfpga_tops/rle_decoder_test.sv).
        beat = self.element.to_bytes(4, 'little') + self.count.to_bytes(4, 'little')
        return bytearray(beat.ljust(64, b'\x00'))

    @property
    def output(self) -> fpga_stream.Stream:
        return fpga_stream.Stream(fpga_stream.StreamType.SIGNED_INT_32, [self.element] * self.count)

_test_cases = (
    _TestCase(element=1024, count=64),
    _TestCase(element=1337, count=7),
    _TestCase(element=11, count=54),
    _TestCase(element=98412, count=127),
)

class RLEDecoderTestCase(fpga_test_case.FPGATestCase):
    alternative_vfpga_top_file = "vfpga_tops/rle_decoder_test.sv"
    debug_mode = True
    # verbose_logging = True

    def run_cases(self, test_cases):
        for test_case in test_cases:
            self.set_stream_input(0, test_case.input)
            self.set_expected_output(0, test_case.output)

        # Act
        self.simulate_fpga()

        # Assert
        self.assert_simulation_output()

    def test_one_rle_decoding(self):
        self.run_cases(_test_cases[:1])

    def test_two_rle_decoding(self):
        self.run_cases(_test_cases[:2])

    def test_many_rle_decoding(self):
        self.run_cases([_test_cases[i % len(_test_cases)] for i in range(24)])
