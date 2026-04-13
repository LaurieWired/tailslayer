#include <tailslayer/hedged_reader.hpp>
#include <iostream>
#include <cstring>

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -v, --verbose    Enable verbose logging\n"
              << "  -s, --size N    Hugepage size in MB (default: 1024, use 0 for regular memory)\n"
              << "  -h, --help      Show this help\n";
}

/*
Example user functions that could be passed to tailslayer
*/

// Example with arguments
[[gnu::always_inline]] inline std::size_t dummy_read_signal2(int arg1, int arg2) {
    std::cout << "Hi with args: " << arg1 << " " << arg2 << "\n";
    return 0; // Index to read
}

template <typename T>
[[gnu::always_inline]] inline void dummy_final_work2(T val, int arg2) {
    std::cout << "Hi with args: " << val << " " << arg2 << "\n";
}

// Example with no arguments
[[gnu::always_inline]] inline std::size_t dummy_read_signal() {
    // UPDATE HERE - signal
    // This is the signal that the worker will wait for
    // Once this loop completes, the read will be triggered
    
#if defined(__x86_64__) || defined(__i386__) // Avoid compilation errors for testers on non-x86 platforms. Read happens immediately though :)
    uint64_t starting_time_dumb = tailslayer::detail::rdtsc_lfence();
    tailslayer::detail::rdtsc_lfence();
    uint64_t num_cycles{0};
    do {
        num_cycles = tailslayer::detail::rdtsc_lfence() - starting_time_dumb;
    } while (num_cycles < 2000000000);
#endif // x86

    // UPDATE HERE - index
    std::size_t index_to_read = 1; // Desired index to read (example reading the second value 0x44)
    return index_to_read;
}

template <typename T>
[[gnu::always_inline]] inline void dummy_final_work(T val) {
    // UPDATE HERE - final work
    // This is the function that will be executed with the value as soon as the signal function finishes
    asm volatile("" :: "r"(val)); // Dummy using value
    std::cout << "Val: " << val << "\n";
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    int hugepage_mb = 1024;  // Default 1GB
    bool use_hugepage = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (i + 1 < argc) {
                hugepage_mb = std::atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    use_hugepage = hugepage_mb > 0;

    using target_size_t = uint8_t;

    if (verbose) {
        std::cout << "=== Config: verbose=" << verbose << ", hugepage=" << hugepage_mb << "MB ===\n";
        std::cout << "=== Main thread pinning to core " << tailslayer::CORE_MAIN << " ===\n";
    }
    tailslayer::pin_to_core(tailslayer::CORE_MAIN);
    tailslayer::verbose = verbose;

    if (verbose) std::cout << "=== Starting tailslayer demo ===\n";

    if (use_hugepage) {
        tailslayer::set_hugepage_size(hugepage_mb);
        if (verbose) std::cout << "=== Using " << hugepage_mb << "MB hugepages ===\n";
    } else {
        tailslayer::set_use_hugepage(false);
    }

    // Example with arguments
    // tailslayer::HedgedReader<target_size_t, dummy_read_signal2, dummy_final_work2<target_size_t>, tailslayer::ArgList<1, 2>, tailslayer::ArgList<2>> reader_args{};
    // reader_args.insert(0x43);
    // reader_args.insert(0x44);
    // reader_args.start_workers();

    // Example with no arguments
    if (verbose) std::cout << "=== Creating HedgedReader with type uint8_t ===\n";
    tailslayer::HedgedReader<target_size_t, dummy_read_signal, dummy_final_work<target_size_t>> reader{};

    if (verbose) std::cout << "=== Inserting values ===\n";
    if (verbose) std::cout << "--- Inserting 0x43 (ASCII 'C') ---\n";
    reader.insert(0x43);
    if (verbose) std::cout << "--- Inserting 0x44 (ASCII 'D') ---\n";
    reader.insert(0x44);

    if (verbose) std::cout << "=== Starting worker threads ===\n";
    reader.start_workers();

    if (verbose) std::cout << "=== Main thread: start_workers() returned ===\n";
    if (verbose) std::cout << "=== Wait for workers to complete ===\n";

    return 0;
}