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

/* -- USER LOGIC -------------------------------------------------------- */

/* -- INPUT ------------------------------------------------------------- */

AXI4S #(.AXI4S_DATA_BITS(512)) host_in (.aclk(clk), .aresetn(rst_n));
assign axis_host_recv[0].tready = host_in.tready;
assign host_in.tdata = axis_host_recv[0].tdata;
assign host_in.tkeep = axis_host_recv[0].tkeep;
assign host_in.tlast = axis_host_recv[0].tlast;
assign host_in.tvalid = axis_host_recv[0].tvalid;

localparam int BITS = $bits(data32_t) * 16;

tagged_i #(logic [BITS - 1:0], bpe_config_t) in(clk, rst_n);
assign host_in.tready = in.ready;
assign in.valid = host_in.tvalid;
assign in.data  = host_in.tdata;

bpe_count_t count = 55;
bpe_config_t in_tag;
assign in_tag.mask = ((1 << 7) - 1);
assign in_tag.bit_width = 7;
assign in_tag.count = count;
assign in.tag = in_tag;
assign in.last = count <= 16;

/* -- OUTPUT ------------------------------------------------------------ */

AXI4S #(.AXI4S_DATA_BITS(512)) host_out (.aclk(clk), .aresetn(rst_n));
assign host_out.tready = axis_host_send[0].tready;
assign axis_host_send[0].tdata = host_out.tdata;
assign axis_host_send[0].tkeep = host_out.tkeep;
assign axis_host_send[0].tlast = host_out.tlast;
assign axis_host_send[0].tvalid = host_out.tvalid;

ndata_i #(logic[31:0], 16) out(clk, rst_n);
NDataToAXI #(logic[31:0], 16) inst_ndata_to_axi (
    .clk(clk),
    .rst_n(rst_n),

    .in(out),
    .out(host_out)
);

/* -- DESIGN WIRING ----------------------------------------------------- */

always_ff @(posedge clk) begin
    if(rst_n == 1'b1) begin 
        if (in.valid && in.ready) begin
            if (count > 16) begin
                count <= count - 16;
            end else begin
                count <= 55;
            end
        end
    end
end

ExpandBPE #(data32_t, 16) inst_expand_bpe (
    .clk(clk),
    .rst_n(rst_n),

    .in(in),
    .out(out)
);
