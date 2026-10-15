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
#include <stdexcept>
#include <string>
#include <vector>

#include <coyote/cDefs.hpp>
#include <coyote/cThread.hpp>
#include <libstf/buffer.hpp>
#include <libstf/common.hpp>
#include <libstf/configuration.hpp>
#include <libstf/error_handling.hpp>
#include <libstf/memory_pool.hpp>
#include <libstf/tlb_manager.hpp>
#include <parcore/configuration.hpp>
#include <parcore/metadata/metadata.hpp>


using namespace libstf;

struct SnappyPage {
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> uncompressed;
    size_t               copy_bytes = 0;
};

struct Args {
    std::vector<double> copy_fractions = {0.0, 0.25, 0.5, 0.75, 0.95};
    size_t              size           = 8 << 20;
    size_t              literal_length = 32;
    size_t              copy_length    = 32;
    size_t              reps           = 10;
    size_t              depth          = 4;
    uint32_t            seed           = 42;
    bool                verify         = false;
    bool                raw            = false;
};

void print_usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [OPTIONS]\n"
              << "\n"
              << "Decompressor throughput benchmark (decompressor_test bitstream). Generates\n"
              << "synthetic raw Snappy pages with a controllable literal-to-copy mix, streams\n"
              << "them through the vFPGA and prints one CSV row per copy fraction.\n"
              << "\n"
              << "Options:\n"
              << "  -c, --copy-fractions LIST Comma-separated fractions of decompressed bytes\n"
              << "                            produced by copy elements, one benchmark row each\n"
              << "                            (default: 0,0.25,0.5,0.75,0.95)\n"
              << "  -s, --size BYTES          Decompressed page size (default: 8MiB)\n"
              << "  -l, --literal-length N    Bytes per literal element, 1-65536 (default: 32)\n"
              << "  -k, --copy-length N       Bytes per copy element, 4-64 (default: 32)\n"
              << "  -r, --reps N              Repetitions per benchmark row (default: 10)\n"
              << "  -d, --depth N             Pages kept in flight, at most the config FIFO\n"
              << "                            capacity of the design (64) (default: 4)\n"
              << "  -S, --seed N              Seed for the input data generator (default: 42)\n"
              << "  -v, --verify              Check the decompressed output against the expected\n"
              << "                            data once per row (and per RAW row with --raw)\n"
              << "  -R, --raw                 Also benchmark the RAW (bypass) path with the\n"
              << "                            uncompressed data\n"
              << "  -h, --help                Show this help message\n";
}

std::vector<double> parse_double_list(const std::string &arg) {
    std::vector<double> values;
    std::stringstream   stream(arg);
    std::string         token;
    while (std::getline(stream, token, ',')) {
        values.push_back(std::stod(token));
    }
    return values;
}

Args parse_args(int argc, char *argv[]) {
    Args args;

    static struct option long_options[] = {{"copy-fractions", required_argument, 0, 'c'},
                                           {"size", required_argument, 0, 's'},
                                           {"literal-length", required_argument, 0, 'l'},
                                           {"copy-length", required_argument, 0, 'k'},
                                           {"reps", required_argument, 0, 'r'},
                                           {"depth", required_argument, 0, 'd'},
                                           {"seed", required_argument, 0, 'S'},
                                           {"verify", no_argument, 0, 'v'},
                                           {"raw", no_argument, 0, 'R'},
                                           {"help", no_argument, 0, 'h'},
                                           {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "c:s:l:k:r:d:S:vRh", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'c':
            args.copy_fractions = parse_double_list(optarg);
            break;
        case 's':
            args.size = std::stoul(optarg);
            break;
        case 'l':
            args.literal_length = std::stoul(optarg);
            break;
        case 'k':
            args.copy_length = std::stoul(optarg);
            break;
        case 'r':
            args.reps = std::stoul(optarg);
            break;
        case 'd':
            args.depth = std::stoul(optarg);
            break;
        case 'S':
            args.seed = std::stoul(optarg);
            break;
        case 'v':
            args.verify = true;
            break;
        case 'R':
            args.raw = true;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            exit(1);
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

SnappyPage generate_snappy_page(size_t size, double copy_fraction, size_t literal_length,
                                size_t copy_length, std::mt19937 &rng) {
    if (literal_length < 1 || literal_length > 65536) {
        throw std::runtime_error("literal length must be in [1, 65536]");
    }
    if (copy_length < 4 || copy_length > 64) {
        throw std::runtime_error("copy length must be in [4, 64]");
    }
    if (copy_fraction < 0.0 || copy_fraction >= 1.0) {
        throw std::runtime_error("copy fraction must be in [0, 1)");
    }

    SnappyPage page;
    auto      &u = page.uncompressed;
    auto      &c = page.compressed;
    u.reserve(size);

    append_varint(c, size);

    std::uniform_int_distribution<int> byte_dist(0, 255);
    while (u.size() < size) {
        size_t remaining = size - u.size();
        // Copies need `copy_length` bytes of history and always replicate exactly `copy_length`
        // bytes, so the page starts with and ends on literals.
        bool copy = u.size() >= copy_length && remaining >= copy_length &&
                    page.copy_bytes < copy_fraction * u.size();
        if (copy) {
            // Offsets of at least `copy_length` avoid self-overlapping copies; Snappy limits copy
            // offsets to 64KiB of history.
            size_t                                max_offset = std::min(u.size(), (size_t)65535);
            std::uniform_int_distribution<size_t> offset_dist(copy_length, max_offset);
            size_t                                offset = offset_dist(rng);
            c.push_back(((copy_length - 1) << 2) | 2); // Copy with two-byte offset
            c.push_back(offset & 0xFF);
            c.push_back(offset >> 8);
            for (size_t i = 0; i < copy_length; ++i) {
                u.push_back(u[u.size() - offset]);
            }
            page.copy_bytes += copy_length;
        } else {
            size_t len = std::min(literal_length, remaining);
            if (len <= 60) {
                c.push_back((len - 1) << 2);
            } else if (len <= 256) {
                c.push_back(60 << 2); // One extra length byte
                c.push_back(len - 1);
            } else {
                c.push_back(61 << 2); // Two extra length bytes
                c.push_back((len - 1) & 0xFF);
                c.push_back((len - 1) >> 8);
            }
            for (size_t i = 0; i < len; ++i) {
                uint8_t b = byte_dist(rng);
                c.push_back(b);
                u.push_back(b);
            }
        }
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

void verify_output(const void *out, const std::vector<uint8_t> &expected) {
    auto values = static_cast<const uint8_t *>(out);
    for (size_t i = 0; i < expected.size(); ++i) {
        if (values[i] != expected[i]) {
            throw std::runtime_error("output mismatch at byte " + std::to_string(i) +
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
    auto         config = global_config.get_config<parcore::ColumnChunkDecoderConfig>();
    check_depth(args.depth, config->maximum_num_enqueued_configs());

    // Worst case is literal_length=1 (one tag byte per literal byte), plus the preamble and a
    // partial trailing element.
    Status status;
    auto   in_buffer = make_buffer(pool, 2 * args.size + 16, status);
    check_status(status);
    auto out_buffer = make_buffer(pool, args.size, status);
    check_status(status);
    tlb->ensure_tlb_mapping(in_buffer->ptr, in_buffer->capacity);
    tlb->ensure_tlb_mapping(out_buffer->ptr, out_buffer->capacity);

    std::mt19937 rng(args.seed);

    // The decompressor test design only consumes the compression field of the column chunk
    // config; num_values, the types and is_required are irrelevant.
    auto enqueue = [&](parcore::metadata::Compression compression) {
        config->enqueue_column_chunk(0, compression, args.size / 4, type_t::INT32_T, false,
                                     type_t::INT32_T);
    };

    std::cout << "compression,copy_fraction,actual_copy_fraction,in_bytes,"
                 "out_bytes,compression_ratio,us,in_gibps,out_gibps"
              << std::endl;

    auto report = [&](const std::string &compression, double copy_fraction,
                      double actual_copy_fraction, size_t in_bytes, double us) {
        std::cout << compression << "," << copy_fraction << "," << actual_copy_fraction << ","
                  << in_bytes << "," << args.size << "," << (double)args.size / in_bytes << ","
                  << us << "," << gibps(in_bytes, us) << "," << gibps(args.size, us) << std::endl;
    };

    for (auto copy_fraction : args.copy_fractions) {
        auto page = generate_snappy_page(args.size, copy_fraction, args.literal_length,
                                         args.copy_length, rng);
        std::memcpy(in_buffer->ptr, page.compressed.data(), page.compressed.size());

        if (args.verify) {
            std::memset(out_buffer->ptr, 0, args.size);
            run_transfers(
                cthread, [&] { enqueue(parcore::metadata::Compression::SNAPPY); }, in_buffer->ptr,
                page.compressed.size(), out_buffer->ptr, args.size, 1, 1);
            verify_output(out_buffer->ptr, page.uncompressed);
        }

        auto us = run_transfers(
            cthread, [&] { enqueue(parcore::metadata::Compression::SNAPPY); }, in_buffer->ptr,
            page.compressed.size(), out_buffer->ptr, args.size, args.reps, args.depth);
        report("snappy", copy_fraction, (double)page.copy_bytes / args.size,
               page.compressed.size(), us);

        if (args.raw) {
            std::memcpy(in_buffer->ptr, page.uncompressed.data(), args.size);

            if (args.verify) {
                std::memset(out_buffer->ptr, 0, args.size);
                run_transfers(
                    cthread, [&] { enqueue(parcore::metadata::Compression::RAW); },
                    in_buffer->ptr, args.size, out_buffer->ptr, args.size, 1, 1);
                verify_output(out_buffer->ptr, page.uncompressed);
            }

            auto us = run_transfers(
                cthread, [&] { enqueue(parcore::metadata::Compression::RAW); }, in_buffer->ptr,
                args.size, out_buffer->ptr, args.size, args.reps, args.depth);
            report("raw", copy_fraction, 0.0, args.size, us);
        }
    }

    return EXIT_SUCCESS;
}
