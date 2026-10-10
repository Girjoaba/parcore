`timescale 1ns / 1ps

import libstf::*;
import parcore::*;

`include "libstf_macros.svh"
`include "config_macros.svh"

/**
 * Currently only used for testing: configures a standalone RunDecoder in unit tests.
 * Inside the HybridPageDecoder, the RunDecoder is configured internally.
 */
module RunDecoderConfig (
    input logic clk,
    input logic rst_n,

    write_config_i.s write_config,
    read_config_i.s  read_config,

    ready_valid_i.m out // #(run_decoder_config_t)
);

localparam MAX_NUM_ENQUEUED_CONFIGS = 8;

`RESET_RESYNC // Reset pipelining

// -- Read -----------------------------------------------------------------------------------------
data64_t values[2];
assign values[0] = RUN_DECODER_CONFIG_ID;
assign values[1] = MAX_NUM_ENQUEUED_CONFIGS;

ConfigReadRegisterFile #(
    .NUM_REGS(2)
) inst_read_regs (
    .clk(clk),
    .rst_n(reset_synced),

    .in(read_config),
    .values(values)
);

// -- Write ----------------------------------------------------------------------------------------
ready_valid_i #(bit_width_t) bit_width(clk, reset_synced);
ConfigWriteFIFO #(0, MAX_NUM_ENQUEUED_CONFIGS, bit_width_t) inst_bit_width (clk, reset_synced, write_config, bit_width);

ready_valid_i #(offset_t) offset(clk, reset_synced);
ConfigWriteFIFO #(1, MAX_NUM_ENQUEUED_CONFIGS, offset_t) inst_offset (clk, reset_synced, write_config, offset);

ready_valid_i #(data32_t) num_values(clk, reset_synced);
ConfigWriteFIFO #(2, MAX_NUM_ENQUEUED_CONFIGS, data32_t) inst_num_values (clk, reset_synced, write_config, num_values);

run_decoder_config_t data;
assign data.bit_width  = bit_width.data;
assign data.offset     = offset.data;
assign data.num_values = num_values.data;

assign out.data  = data;
assign out.valid = bit_width.valid && offset.valid && num_values.valid;

assign bit_width.ready  = offset.valid && num_values.valid && out.ready;
assign offset.ready     = bit_width.valid && num_values.valid && out.ready;
assign num_values.ready = bit_width.valid && offset.valid && out.ready;

endmodule
