#include <getopt.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <coyote/cDefs.hpp>
#include <coyote/cThread.hpp>
#include <libstf/buffer.hpp>
#include <libstf/common.hpp>
#include <libstf/configuration.hpp>
#include <libstf/error_handling.hpp>
#include <libstf/memory_pool.hpp>
#include <libstf/tlb_manager.hpp>


using namespace libstf;

// Host-side counterpart of hardware/src/hdl/config/run_decoder_config.sv
class RunDecoderConfig : public Config {
  public:
    RunDecoderConfig(std::shared_ptr<coyote::cThread> cthread, uint32_t addr_offset,
                     uint32_t num_regs)
        : Config(cthread, addr_offset, num_regs),
          maximum_num_enqueued_configs_(read_register(1).value()) {
        if (maximum_num_enqueued_configs_ == 0) {
            throw std::runtime_error("RunDecoderConfig reports no config FIFO capacity; the "
                                     "bitstream predates that register and must be rebuilt");
        }
    }

    void enqueue_page(uint32_t bit_width, uint32_t offset, uint32_t num_values) {
        write_register(ConfigRegister(0, bit_width));
        write_register(ConfigRegister(1, offset));
        write_register(ConfigRegister(2, num_values));
    }

    size_t maximum_num_enqueued_configs() const { return maximum_num_enqueued_configs_; }

    static constexpr uint64_t ID = 0xd6736a190f91fa01; // RUN_DECODER_CONFIG_ID

  private:
    size_t maximum_num_enqueued_configs_;
};

struct HybridPage {
    std::vector<uint8_t>  bytes;
    std::vector<uint32_t> expected;
};

// The unit-test fixtures store a decompressed page as
//   def_levels_length (4 bytes LE) | def_levels | bit_width (1 byte) | body
// with 3 def-levels bytes, so the RLE/BPE body starts at byte 8. The RunDecoder only skips these
// bytes (per the offset config register).
constexpr uint32_t PAGE_BODY_OFFSET = 8;

struct Args {
    std::string         encoding    = "both";
    std::vector<size_t> bit_widths  = {1, 2, 4, 8, 16, 19};
    std::vector<size_t> run_lengths = {1, 4, 16, 64, 256, 1024};
    size_t              num_values  = 4 << 20;
    size_t              reps        = 10;
    size_t              depth       = 4;
    uint32_t            seed        = 42;
    bool                verify      = false;
};

void print_usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [OPTIONS]\n"
              << "\n"
              << "RunDecoder throughput benchmark (run_decoder_test bitstream). Generates\n"
              << "synthetic Parquet RLE/bit-packed hybrid pages, streams them through the vFPGA\n"
              << "and prints one CSV row per swept configuration.\n"
              << "\n"
              << "Options:\n"
              << "  -e, --encoding ENC     Encoding to benchmark: rle, bpe or both (default: "
                 "both)\n"
              << "  -b, --bit-widths LIST  Comma-separated bit widths of the encoded values, "
                 "1-19\n"
              << "                         (default: 1,2,4,8,16,19)\n"
              << "  -l, --run-lengths LIST Comma-separated values per run; for BPE rounded up to\n"
              << "                         whole groups of 8, duplicates after rounding are\n"
              << "                         skipped (default: 1,4,16,64,256,1024)\n"
              << "  -n, --num-values N     Decoded values per page (default: 4Mi)\n"
              << "  -r, --reps N           Repetitions per benchmark row (default: 10)\n"
              << "  -d, --depth N          Pages kept in flight, at most the config FIFO capacity\n"
              << "                         of the design (8) (default: 4)\n"
              << "  -s, --seed N           Seed for the input data generator (default: 42)\n"
              << "  -v, --verify           Check the decoded output against the expected values\n"
              << "                         once per row\n"
              << "  -h, --help             Show this help message\n";
}

std::vector<size_t> parse_size_list(const std::string &arg) {
    std::vector<size_t> values;
    std::stringstream   stream(arg);
    std::string         token;
    while (std::getline(stream, token, ',')) {
        values.push_back(std::stoul(token));
    }
    return values;
}

Args parse_args(int argc, char *argv[]) {
    Args args;

    static struct option long_options[] = {{"encoding", required_argument, 0, 'e'},
                                           {"bit-widths", required_argument, 0, 'b'},
                                           {"run-lengths", required_argument, 0, 'l'},
                                           {"num-values", required_argument, 0, 'n'},
                                           {"reps", required_argument, 0, 'r'},
                                           {"depth", required_argument, 0, 'd'},
                                           {"seed", required_argument, 0, 's'},
                                           {"verify", no_argument, 0, 'v'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "e:b:l:n:r:d:s:vh", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'e':
            args.encoding = optarg;
            break;
        case 'b':
            args.bit_widths = parse_size_list(optarg);
            break;
        case 'l':
            args.run_lengths = parse_size_list(optarg);
            break;
        case 'n':
            args.num_values = std::stoul(optarg);
            break;
        case 'r':
            args.reps = std::stoul(optarg);
            break;
        case 'd':
            args.depth = std::stoul(optarg);
            break;
        case 's':
            args.seed = std::stoul(optarg);
            break;
        case 'v':
            args.verify = true;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            exit(1);
        }
    }

    if (args.encoding != "rle" && args.encoding != "bpe" && args.encoding != "both") {
        throw std::runtime_error("encoding must be rle, bpe or both");
    }
    for (auto bit_width : args.bit_widths) {
        if (bit_width < 1 || bit_width > 19) {
            // ID_BITS in hardware/src/hdl/common.sv
            throw std::runtime_error("bit widths must be in [1, 19]");
        }
    }
    for (auto run_length : args.run_lengths) {
        if (run_length < 1) {
            throw std::runtime_error("run lengths must be at least 1");
        }
    }

    return args;
}

void append_varint(std::vector<uint8_t> &out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out.push_back(value);
}

HybridPage start_page(size_t bit_width) {
    HybridPage page;
    page.bytes = {3, 0, 0, 0, 0, 0, 0, (uint8_t)bit_width};
    return page;
}

// LSB-first, as in Parquet's bit-packed runs
void pack_bits(std::vector<uint8_t> &out, const std::vector<uint32_t> &values, size_t bit_width) {
    uint64_t acc  = 0;
    size_t   bits = 0;
    for (auto value : values) {
        acc |= (uint64_t)value << bits;
        bits += bit_width;
        while (bits >= 8) {
            out.push_back(acc & 0xFF);
            acc >>= 8;
            bits -= 8;
        }
    }
    if (bits > 0) {
        out.push_back(acc & 0xFF);
    }
}

std::uniform_int_distribution<uint32_t> value_dist(size_t bit_width) {
    return std::uniform_int_distribution<uint32_t>(0, (1u << bit_width) - 1);
}

HybridPage generate_rle_page(size_t num_values, size_t bit_width, size_t run_length,
                             std::mt19937 &rng) {
    auto   page        = start_page(bit_width);
    auto   dist        = value_dist(bit_width);
    size_t value_bytes = (bit_width + 7) / 8;

    for (size_t i = 0; i < num_values; i += run_length) {
        size_t   run   = std::min(run_length, num_values - i);
        uint32_t value = dist(rng);

        append_varint(page.bytes, run << 1);
        for (size_t b = 0; b < value_bytes; ++b) {
            page.bytes.push_back((value >> (8 * b)) & 0xFF);
        }
        page.expected.insert(page.expected.end(), run, value);
    }

    return page;
}

size_t bpe_run_length(size_t run_length) { return (run_length + 7) / 8 * 8; }

HybridPage generate_bpe_page(size_t num_values, size_t bit_width, size_t run_length,
                             std::mt19937 &rng) {
    auto page = start_page(bit_width);
    auto dist = value_dist(bit_width);

    for (size_t i = 0; i < num_values; i += run_length) {
        size_t run    = std::min(run_length, num_values - i);
        size_t groups = (run + 7) / 8;

        // The final group of the page is padded with zeros; the design cuts the output after
        // num_values values.
        std::vector<uint32_t> values(groups * 8, 0);
        for (size_t j = 0; j < run; ++j) {
            values[j] = dist(rng);
        }

        append_varint(page.bytes, (groups << 1) | 1);
        pack_bits(page.bytes, values, bit_width);
        page.expected.insert(page.expected.end(), values.begin(), values.begin() + run);
    }

    return page;
}

// The config FIFO silently drops writes when it is full.
void check_depth(size_t depth, size_t max_depth) {
    if (depth < 1 || depth > max_depth) {
        throw std::runtime_error("depth must be in [1, " + std::to_string(max_depth) +
                                 "] (the capacity of the design's config FIFO)");
    }
}

// The configs of the first `depth` transfers are enqueued before the timer starts, the others while
// transfers are running. Returns the average microseconds per repetition.
double run_transfers(std::shared_ptr<coyote::cThread> cthread,
                            const std::function<void()> &enqueue_config, void *in, size_t in_size,
                            void *out, size_t out_size, size_t reps, size_t depth) {
    if (in_size > coyote::MAX_TRANSFER_SIZE || out_size > coyote::MAX_TRANSFER_SIZE) {
        throw std::runtime_error("page exceeds the maximum Coyote transfer size");
    }
    if (depth < 1) {
        throw std::runtime_error("depth must be at least 1");
    }

    coyote::localSg src;
    src.addr   = in;
    src.len    = in_size;
    src.stream = coyote::STRM_HOST;
    src.dest   = 0;

    coyote::localSg dst;
    dst.addr   = out;
    dst.len    = out_size;
    dst.stream = coyote::STRM_HOST;
    dst.dest   = 0;

    cthread->clearCompleted();

    size_t configured = 0, issued = 0, completed = 0;
    for (; configured < std::min(reps, depth); ++configured) {
        enqueue_config();
    }

    auto start = std::chrono::high_resolution_clock::now();
    while (completed < reps) {
        for (; issued < configured; ++issued) {
            cthread->invoke(coyote::CoyoteOper::LOCAL_TRANSFER, src, dst);
        }
        completed = cthread->checkCompleted(coyote::CoyoteOper::LOCAL_TRANSFER);
        for (; configured < reps && configured - completed < depth; ++configured) {
            enqueue_config();
        }
    }
    auto end = std::chrono::high_resolution_clock::now();

    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() /
           (double)reps;
}

void verify_output(const void *out, const std::vector<uint32_t> &expected) {
    auto values = static_cast<const uint32_t *>(out);
    for (size_t i = 0; i < expected.size(); ++i) {
        if (values[i] != expected[i]) {
            throw std::runtime_error("output mismatch at value " + std::to_string(i) +
                                     ": expected " + std::to_string(expected[i]) + ", got " +
                                     std::to_string(values[i]));
        }
    }
}

double gibps(size_t bytes, double us) { return (bytes / 1073741824.0) / (us / 1000000.0); }

int main(int argc, char *argv[]) {
    Args args = parse_args(argc, argv);

    auto cthread = std::make_shared<coyote::cThread>(0, getpid(), 0);
#ifdef EN_SIMULATION
    auto pool = std::make_shared<SimpleMemoryPool>();
#else
    auto pool = std::make_shared<HugePageMemoryPool>();
#endif
    auto tlb = std::make_shared<TLBManager>(cthread, pool);

    GlobalConfig global_config(cthread);
    auto         config = global_config.get_config<RunDecoderConfig>();
    check_depth(args.depth, config->maximum_num_enqueued_configs());

    // Worst case input is RLE with run length 1 (one header byte plus, at bit widths up to 19,
    // three value bytes per value), which is bounded by out_size.
    size_t out_size = args.num_values * sizeof(uint32_t);
    Status status;
    auto   in_buffer = make_buffer(pool, out_size + 64, status);
    check_status(status);
    auto out_buffer = make_buffer(pool, out_size, status);
    check_status(status);
    tlb->ensure_tlb_mapping(in_buffer->ptr, in_buffer->capacity);
    tlb->ensure_tlb_mapping(out_buffer->ptr, out_buffer->capacity);

    std::mt19937 rng(args.seed);

    std::cout << "encoding,bit_width,run_length,num_values,in_bytes,out_bytes,"
                 "us,in_gibps,out_gibps,mvalues_per_s"
              << std::endl;

    auto benchmark = [&](const std::string &enc, size_t bit_width, size_t run_length) {
        auto page = enc == "rle"
                        ? generate_rle_page(args.num_values, bit_width, run_length, rng)
                        : generate_bpe_page(args.num_values, bit_width, run_length, rng);
        if (page.bytes.size() > in_buffer->capacity) {
            throw std::runtime_error("generated page exceeds the input buffer");
        }
        std::memcpy(in_buffer->ptr, page.bytes.data(), page.bytes.size());

        auto enqueue = [&] { config->enqueue_page(bit_width, PAGE_BODY_OFFSET, args.num_values); };

        if (args.verify) {
            std::memset(out_buffer->ptr, 0, out_size);
            run_transfers(cthread, enqueue, in_buffer->ptr, page.bytes.size(), out_buffer->ptr,
                          out_size, 1, 1);
            verify_output(out_buffer->ptr, page.expected);
        }

        auto us = run_transfers(cthread, enqueue, in_buffer->ptr, page.bytes.size(),
                                out_buffer->ptr, out_size, args.reps, args.depth);
        std::cout << enc << "," << bit_width << "," << run_length << "," << args.num_values << ","
                  << page.bytes.size() << "," << out_size << "," << us << ","
                  << gibps(page.bytes.size(), us) << "," << gibps(out_size, us) << ","
                  << args.num_values / us << std::endl;
    };

    std::set<std::tuple<std::string, size_t, size_t>> done;
    for (const std::string &enc : {"rle", "bpe"}) {
        if (args.encoding != enc && args.encoding != "both") {
            continue;
        }
        for (auto bit_width : args.bit_widths) {
            for (auto run_length : args.run_lengths) {
                // Report the run length that is actually generated and skip configurations that
                // collapse onto an already benchmarked one.
                if (enc == "bpe") {
                    run_length = bpe_run_length(run_length);
                }
                if (done.emplace(enc, bit_width, run_length).second) {
                    benchmark(enc, bit_width, run_length);
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
