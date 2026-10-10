`timescale 1ns / 1ps

`include "lynx_macros.svh"
`include "libstf_macros.svh"

import libstf::data8_t;

import parcore::*;
import libstf::*;

/* -- Tie-off unused interfaces and signals ----------------------------- */
always_comb notify.tie_off_m();
always_comb sq_rd.tie_off_m();
always_comb sq_wr.tie_off_m();
always_comb cq_rd.tie_off_s();
always_comb cq_wr.tie_off_s();

for (genvar I = 1; I < N_STRM_AXI; I++) begin
    always_comb axis_host_recv[I].tie_off_s();
    always_comb axis_host_send[I].tie_off_m();
end

/* -- Fix clock and reset names ----------------------------------------- */
logic clk;
logic rst_n;

assign clk   = aclk;
assign rst_n = aresetn;

/* -- INPUT ------------------------------------------------------------- */

AXI4S axi_host_recv_0 (.aclk(clk), .aresetn(rst_n));
`AXIS_ASSIGN(axis_host_recv[0], axi_host_recv_0)

ndata_i #(data8_t, 64) in(clk, rst_n);
AXIToNData #(data8_t, 64) inst_axi_to_ndata (
    .clk(clk),
    .rst_n(rst_n),

    .in(axi_host_recv_0),
    .out(in)
);

/* -- OUTPUT ------------------------------------------------------------ */

AXI4S axi_host_send_0 (.aclk(clk), .aresetn(rst_n));
`AXIS_ASSIGN(axi_host_send_0, axis_host_send[0])

ndata_i #(data8_t, 64) out(clk, rst_n);
NDataToAXI #(data8_t, 64) inst_ndata_to_axi (
    .clk(clk),
    .rst_n(rst_n),

    .in(out),
    .out(axi_host_send_0)
);


/* -- CONFIG ------------------------------------------------------------ */
write_config_i write_configs[1](.*);
read_config_i  read_configs [1](.*);
GlobalConfig #(
    .SYSTEM_ID(PARCORE_SYSTEM_ID),
    .NUM_CONFIGS(1),
    .ADDR_SPACE_SIZES({COLUMN_CHUNK_DECODER_READ_REGS(1)})
) inst_config (
    .clk(clk),
    .rst_n(rst_n),

    .axi_ctrl(axi_ctrl),

    .write_configs(write_configs),
    .read_configs(read_configs)
);

// No decoder is instantiated in this test, so the profiling counters are tied off.
decoder_profile_i profile[1]();
assign profile[0].counters = '0;

ready_valid_i #(column_chunk_conf_t) chunk_conf[1](.*);
ColumnChunkDecoderConfig #(
    .NUM_DECODERS(1)
) inst_column_chunk_decoder_config (
    .clk(clk),
    .rst_n(rst_n),

    .write_config(write_configs[0]),
    .read_config(read_configs[0]),

    .profile(profile),

    .out(chunk_conf)
);

// The Decompressor only consumes the compression field of the chunk config.
ready_valid_i #(compression_t) conf(clk, rst_n);
assign conf.data           = chunk_conf[0].data.compression;
assign conf.valid          = chunk_conf[0].valid;
assign chunk_conf[0].ready = conf.ready;

/* -- DESIGN WIRING ----------------------------------------------------- */

Decompressor #(64) inst_decompressor (
    .clk(aclk),
    .rst_n(aresetn),

    .in(in),
    .conf(conf),

    .out(out)
);
