`timescale 1ns / 1ps

`include "lynx_macros.svh"
`include "libstf_macros.svh"

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

/* -- CONFIG ------------------------------------------------------------ */
write_config_i write_configs[1](.*);
read_config_i  read_configs [1](.*);
GlobalConfig #(
    .SYSTEM_ID(PARCORE_SYSTEM_ID),
    .NUM_CONFIGS(1),
    .ADDR_SPACE_SIZES({RUN_DECODER_CONFIG_REGS})
) inst_config (
    .clk(clk),
    .rst_n(rst_n),

    .axi_ctrl(axi_ctrl),

    .write_configs(write_configs),
    .read_configs(read_configs)
);

ready_valid_i #(run_decoder_config_t) conf(clk, rst_n);
RunDecoderConfig inst_run_decoder_config (
    .clk(clk),
    .rst_n(rst_n),

    .write_config(write_configs[0]),
    .read_config(read_configs[0]),

    .out(conf)
);

ready_valid_i #(run_decoder_config_t) conf_dup[2](clk, rst_n);
ReadyValidDuplicator #(2) inst_conf_duplicate (
    .clk(clk),
    .rst_n(rst_n),
    .in(conf),
    .out(conf_dup)
);

ready_valid_i #(data32_t) page_num_values(clk, rst_n);
assign page_num_values.data  = conf_dup[1].data.num_values;
assign page_num_values.valid = conf_dup[1].valid;
assign conf_dup[1].ready     = page_num_values.ready;

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

ndata_i #(data32_t, 16) out(clk, rst_n);
NDataToAXI #(data32_t, 16) inst_ndata_to_axi (
    .clk(clk),
    .rst_n(rst_n),

    .in(out),
    .out(axi_host_send_0)
);

/* -- DESIGN WIRING ----------------------------------------------------- */

// The RunDecoder asserts `last` at the end of every RLE/BPE run and emits one
// (partially filled) beat per run. Collapse the per-run `last` into a single
// per-page `last` (after num_values elements) and then normalize into full
// databeats before the AXI sink (which requires every non-final beat to be
// completely full).
ndata_i #(data32_t, 16) run_decoder_out(clk, rst_n);
RunDecoder #(data32_t, 16) inst_run_decoder (
    .clk(clk),
    .rst_n(rst_n),

    .in(in),
    .conf(conf_dup[0]),
    .out(run_decoder_out)
);

ndata_i #(data32_t, 16) rewritten_last(clk, rst_n);
DataRewriteLast #(
    .data_t(data32_t),
    .NUM_ELEMENTS(16)
) inst_rewrite_last (
    .clk(clk),
    .rst_n(rst_n),

    .num_elements(page_num_values),

    .in(run_decoder_out),
    .out(rewritten_last)
);

DataNormalizer #(
    .data_t(data32_t),
    .NUM_ELEMENTS(16)
) inst_normalizer (
    .clk(clk),
    .rst_n(rst_n),

    .in(rewritten_last),
    .out(out)
);
