`timescale 1ns / 1ps

`include "axi_macros.svh"
`include "lynx_macros.svh"

import lynxTypes::*;
import parcore::*;

module ExpandRLE #(
    parameter type data_t,
    parameter NUM_ELEMENTS
) (
    input logic clk,
    input logic rst_n,

    tagged_i.s in,  // #(data_t, rle_count_t)
    ndata_i.m out   // #(data_t, NUM_ELEMENTS)
);

tagged_i #(data_t, rle_count_t) in_inner(clk, rst_n);

TaggedSkidBuffer #(data_t, rle_count_t) inst_in_skid_buffer (
    .clk(clk),
    .rst_n(rst_n),

    .in(in),
    .out(in_inner)
);

ExpandRLEInternal #(data_t, NUM_ELEMENTS) inst_expand_rle_internal (
    .clk(clk),
    .rst_n(rst_n),

    .in(in_inner),
    .out(out)
);

endmodule

module ExpandRLEInternal #(
    parameter type data_t,
    parameter NUM_ELEMENTS
) (
    input logic clk,
    input logic rst_n,

    tagged_i.s in,  // #(data_t, rle_count_t)
    ndata_i.m out   // #(data_t, NUM_ELEMENTS)
);

// Extracting data from the tagged_i interface
data_t in_data;
rle_count_t in_meta;

assign in_data = in.data;
assign in_meta = in.tag;

// State machine state and logic
typedef enum logic {
    ST_IDLE,
    ST_CONF
} state_t;
data_t element;
rle_count_t count;
state_t state;

task reset();
    state <= ST_IDLE;
    element <= 'x;
    count <= '0;
endtask

always_ff @(posedge clk) begin
    if (rst_n == 1'b0) begin
        reset();
    end else begin
        case (state)
            ST_IDLE: begin
                if (in.valid) begin
                    element <= in_data;
                    count <= in_meta;
                    state <= ST_CONF;
                end
            end

            ST_CONF: begin
                if (out.valid && out.ready) begin
                  if (out.last) begin
                      // If there's no more input to take, go back to idle,
                      // otherwise remain on CONF but update the configuration
                      if (in.valid) begin
                          element <= in_data;
                          count <= in_meta;
                      end else begin
                          reset();
                      end
                  end else begin
                      // If ~out.last, then count > NUM_ELEMENTS
                      count <= count - NUM_ELEMENTS;
                  end
                end
            end
        endcase
    end
end

assign in.ready = state == ST_IDLE || (state == ST_CONF && out.ready && out.valid && out.last);

// Driving output based on the current intrenal state
assign out.valid = count > 0;
assign out.last = count <= NUM_ELEMENTS;
generate
for (genvar i = 0; i < NUM_ELEMENTS; i++) begin
    assign out.data[i] = element;
    assign out.keep[i] = i < count;
end
endgenerate

endmodule
