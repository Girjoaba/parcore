`timescale 1ns / 1ps

import parcore::*;

import libstf::data32_t;

/* -- Tie-off unused interfaces and signals ----------------------------- */
always_comb axi_ctrl.tie_off_s();
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

// Every host databeat carries one RLE run: the value in bits [31:0] and the
// run length in the following $bits(rle_count_t) bits.
AXI4S #(.AXI4S_DATA_BITS(512)) host_in (.aclk(clk), .aresetn(rst_n));
assign axis_host_recv[0].tready = host_in.tready;
assign host_in.tdata = axis_host_recv[0].tdata;
assign host_in.tkeep = axis_host_recv[0].tkeep;
assign host_in.tlast = axis_host_recv[0].tlast;
assign host_in.tvalid = axis_host_recv[0].tvalid;

tagged_i #(data32_t, rle_count_t) in(clk, rst_n);
assign host_in.tready = in.ready;
assign in.valid = host_in.tvalid;
assign in.data  = host_in.tdata[$bits(data32_t) - 1:0];
assign in.tag   = host_in.tdata[$bits(data32_t) +: $bits(rle_count_t)];
assign in.keep  = 1'b1;
assign in.last  = 1'b1;

/* -- OUTPUT ------------------------------------------------------------ */

AXI4S #(.AXI4S_DATA_BITS(512)) host_out (.aclk(clk), .aresetn(rst_n));
assign host_out.tready = axis_host_send[0].tready;
assign axis_host_send[0].tdata = host_out.tdata;
assign axis_host_send[0].tkeep = host_out.tkeep;
assign axis_host_send[0].tlast = host_out.tlast;
assign axis_host_send[0].tvalid = host_out.tvalid;

ndata_i #(data32_t, 16) out(clk, rst_n);
NDataToAXI #(data32_t, 16) inst_ndata_to_axi (
    .clk(clk),
    .rst_n(rst_n),

    .in(out),
    .out(host_out)
);

/* -- DESIGN WIRING ----------------------------------------------------- */

ExpandRLE #(data32_t, 16) inst_expand_rle (
    .clk(clk),
    .rst_n(rst_n),

    .in(in),
    .out(out)
);
