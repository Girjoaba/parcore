`timescale 1ns / 1ps

import libstf::*;
import parcore::*;

`include "libstf_macros.svh"
`include "config_macros.svh"

/**
 * Currently only used for testing: configures a standalone HybridPageDecoder in unit tests.
 * Inside the ColumnChunkDecoder, the HybridPageDecoder is configured internally.
 */
module HybridPageDecoderConfig (
    input logic clk,
    input logic rst_n,

    write_config_i.s write_config,
    read_config_i.s  read_config,

    ready_valid_i.m out // #(hybrid_page_conf_t)
);


`RESET_RESYNC // Reset pipelining

// -- Read -----------------------------------------------------------------------------------------
data64_t values[1];
assign values[0] = HYBRID_PAGE_DECODER_CONFIG_ID;

ConfigReadRegisterFile #(
    .NUM_REGS(1)
) inst_read_regs (
    .clk(clk),
    .rst_n(reset_synced),

    .in(read_config),
    .values(values)
);

// -- Write ----------------------------------------------------------------------------------------
ConfigWriteFIFO #(0, 8, hybrid_page_conf_t) inst_page_conf (clk, reset_synced, write_config, out);

endmodule
