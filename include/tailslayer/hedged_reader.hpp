#ifndef TAILSLAYER_HEDGED_READER_HPP
#define TAILSLAYER_HEDGED_READER_HPP

#include <iostream>
#include <array>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <sched.h>
#include <unistd.h>

namespace tailslayer {

inline bool verbose = false;  // Set to true to enable verbose logging

inline void set_verbose(bool v) { verbose = v; }

inline bool use_hugepage = true;
inline int hugepage_size_mb = 1024;  // Default to 1GB

inline void set_use_hugepage(bool b) { use_hugepage = b; }
inline void set_hugepage_size(int mb) { hugepage_size_mb = mb; }
inline std::uint64_t get_superpage_size() { return (std::uint64_t)hugepage_size_mb * 1024 * 1024; }

inline constexpr int DEFAULT_CHANNEL_OFFSET = 256; 
inline constexpr int DEFAULT_CHANNEL_BIT = 8; 
inline constexpr int DEFAULT_NUM_CHANNELS = 2;
inline constexpr std::size_t DEFAULT_NUM_REPLICAS = 2;
inline std::uint64_t SUPERPAGE_SIZE = (1ULL << 21);  // 2MB, updated at runtime 

inline constexpr int CORE_MEAS_A = 11;
inline constexpr int CORE_MEAS_B = 12;
inline constexpr int CORE_MAIN = 14;

namespace detail {

// These functions are really only used as timing examples for dev purposes
// Lets you get a quick idea if you accidentally added a large amount of cycles
#if defined(__x86_64__) || defined(__i386__)
    static inline void clflush_addr(void *addr) {
        asm volatile("clflush (%0)" :: "r"(addr) : "memory");
    }

    static inline void mfence_inst() {
        asm volatile("mfence" ::: "memory");
    }

    static inline std::uint64_t rdtsc_lfence() {
        std::uint64_t lo, hi;
        asm volatile("lfence\n\t"
                    "rdtsc"
                    : "=a"(lo), "=d"(hi));
        return (hi << 32) | lo;
    }

    static inline std::uint64_t rdtscp_lfence() {
        std::uint64_t lo, hi;
        std::uint32_t aux;
        asm volatile("rdtscp"
                    : "=a"(lo), "=d"(hi), "=c"(aux));
        asm volatile("lfence" ::: "memory");
        return (hi << 32) | lo;
    }
#endif // x86
} // namespace detail

static inline int pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

// This lets the caller pass arguments to their worker functions
template <auto... Vals>
struct ArgList {};

/*
Template parameters:
1. The type of the values to insert and read
2. The "timer" function that waits for an independent signal and returns the target index to read
3. The function that gets executed with the value once it's been read
*/
template <typename T, auto wait_work, auto final_work, 
          typename WaitArgs = ArgList<>, typename WorkArgs = ArgList<>, 
          std::size_t N = DEFAULT_NUM_REPLICAS>
class HedgedReader;

template <typename T, auto wait_work, auto final_work, 
          auto... WaitArgs, auto... WorkArgs, std::size_t N>
class HedgedReader<T, wait_work, final_work, ArgList<WaitArgs...>, ArgList<WorkArgs...>, N> {
public:
    HedgedReader(int channel_offset = DEFAULT_CHANNEL_OFFSET, 
                 int channel_bit = DEFAULT_CHANNEL_BIT,
                 std::size_t num_channels = DEFAULT_NUM_CHANNELS) :
                 channel_offset_(channel_offset), channel_bit_(channel_bit), num_channels_(num_channels), logical_index_(0) {

        if (verbose) {
            std::cout << "[HedgedReader] Constructor: channel_offset=" << channel_offset 
                      << ", channel_bit=" << channel_bit 
                      << ", num_channels=" << num_channels
                      << ", N=" << N << "\n";
        }

        assert(channel_offset_ % sizeof(T) == 0 && "Channel offset must be a multiple of sizeof(T)");
        assert(N <= num_channels_ && "Can't have more replicas than memory channels");

        // Precompute all these so they're not getting computed in the hot path
        std::size_t elements_per_chunk = channel_offset_ / sizeof(T);
        chunk_mask_ = elements_per_chunk - 1;
        chunk_shift_ = __builtin_ctzll(elements_per_chunk); // counts trailing zeros to get the shift amount
        stride_in_elements_ = (num_channels_ * channel_offset_) / sizeof(T);
        std::size_t stride_bytes = num_channels_ * channel_offset_;
        std::size_t page_size = use_hugepage ? get_superpage_size() : (256 * 1024);
        std::size_t max_strides = page_size / stride_bytes;
        capacity_ = max_strides * elements_per_chunk;

        if (verbose) {
            std::cout << "[HedgedReader] Computed: elements_per_chunk=" << elements_per_chunk
                      << ", chunk_shift=" << chunk_shift_
                      << ", chunk_mask=0x" << std::hex << chunk_mask_ << std::dec
                      << ", stride_in_elements=" << stride_in_elements_
                      << ", capacity=" << capacity_ << "\n";
        }

        setup_memory();
        setup_replica_cores();
        if (verbose) std::cout << "[HedgedReader] Constructor complete\n";
    }

    std::size_t size() const { return logical_index_; }
    std::size_t capacity() const { return capacity_; }
    
    void insert(T val) {
        if (verbose) {
            std::cout << "[insert] value=" << +val << " (0x" << std::hex << +val << std::dec << ")"
                      << ", logical_index=" << logical_index_ << "\n";
        }
        assert(logical_index_ + 1 < capacity_ && "Tried to insert out of bounds");
        for (std::size_t i = 0; i < N; ++i) {
            // We have to keep accounting for making sure we're on different channels
            //  especially when we exceed the channel offset size
            T* target_addr = get_next_logical_index_address(i, logical_index_);

            if (verbose) {
                std::cout << "[insert]   replica=" << i << " addr=" << (void*)target_addr 
                          << " <- " << +val << "\n";
            }
            *target_addr = val;
        }
        ++logical_index_;
        if (verbose) std::cout << "[insert] done, size now=" << logical_index_ << "\n";
    }

    void start_workers() {
        if (verbose) std::cout << "[start_workers] spawning " << N << " worker threads...\n";
        for (std::size_t i = 0; i < N; ++i) {
            if (verbose) std::cout << "[start_workers] starting worker " << i << " on core " << cores_[i] << "\n";
            workers_[i] = std::thread(&HedgedReader::worker_func, this, i);
        }
        if (verbose) std::cout << "[start_workers] all workers spawned, waiting 10ms for stabilization...\n";
        usleep(10000); // 10ms delay to make sure the workers are started
                        // If you don't do this, it freezes because the workers can't get to their cores
        if (verbose) std::cout << "[start_workers] workers ready\n";
    }

    ~HedgedReader() {
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }

        if (replica_page_) {
            munmap(replica_page_, SUPERPAGE_SIZE);
            replica_page_ = nullptr;
        }
    }

private:
    int channel_bit_;
    int channel_offset_;
    std::size_t num_channels_;
    void* replica_page_ = nullptr;
    std::size_t logical_index_; // Internal counter to figure out next place to insert the next value
    std::size_t capacity_;

    std::size_t chunk_shift_;
    std::size_t chunk_mask_;
    std::size_t stride_in_elements_;

    std::array<T*, N> replicas_{};
    std::array<int, N> cores_{};
    std::array<std::thread, N> workers_{};

    void worker_func(std::size_t worker_idx) {
        if (verbose) std::cout << "[Worker " << worker_idx << "] Pinned to core " << cores_[worker_idx] << "\n";
        pin_to_core(cores_[worker_idx]);
        if (verbose) std::cout << "[Worker " << worker_idx << "] Running wait_work() signal function...\n";

        std::size_t read_index = wait_work(WaitArgs...);
        if (verbose) std::cout << "[Worker " << worker_idx << "] wait_work returned index=" << read_index << "\n";

        // Only for quick sanity check benchmark counting cycles
        // T* flush_addr = get_next_logical_index_address(worker_idx, read_index);
        // detail::clflush_addr(flush_addr);
        // detail::mfence_inst();
        // std::uint64_t t0 = detail::rdtsc_lfence();

        T* target_addr = get_next_logical_index_address(worker_idx, read_index);
        if (verbose) std::cout << "[Worker " << worker_idx << "] Reading from addr=" << (void*)target_addr 
                  << " value=" << +(*target_addr) << "\n";

        // The actual read of the data
        // Passed directly to the inline worker function for processing
        if (verbose) std::cout << "[Worker " << worker_idx << "] Calling final_work()...\n";
        final_work(*target_addr, WorkArgs...);
        if (verbose) std::cout << "[Worker " << worker_idx << "] Done\n";

        // std::uint64_t t1 = detail::rdtscp_lfence();
        // std::cout << "\nRunning time: " << t1 - t0 << " cycles\n";
    }

    [[gnu::always_inline]] inline T* get_next_logical_index_address(std::size_t replica_idx,
                                                                             std::size_t logical_index) const {
        std::size_t chunk_idx = logical_index >> chunk_shift_; 
        std::size_t offset_in_chunk = logical_index & chunk_mask_;
        std::size_t element_offset = (chunk_idx * stride_in_elements_) + offset_in_chunk;
        
        return replicas_[replica_idx] + element_offset;
    }

    void setup_replica_cores() {
        cores_[0] = CORE_MEAS_A;
        if (num_channels_ > 1 && N > 1) {
            cores_[1] = CORE_MEAS_B;
        }
        for (std::size_t i = 2; i < N; ++i) { 
            cores_[i] = CORE_MEAS_B + i - 1; 
        }
        if (verbose) {
            std::cout << "[setup_replica_cores] Assigned cores: ";
            for (std::size_t i = 0; i < N; ++i) {
                std::cout << cores_[i] << " ";
            }
            std::cout << "\n";
        }
    }

    bool setup_memory() {
        std::uint64_t page_size = use_hugepage ? get_superpage_size() : (256 * 1024);
        
        if (use_hugepage) {
            int huge_shift = 21;
            if (hugepage_size_mb == 1024) huge_shift = 30;
            if (hugepage_size_mb == 1) huge_shift = 20;
            if (verbose) std::cout << "[setup_memory] Allocating " << hugepage_size_mb << "MB hugepage (shift=" << huge_shift << ")\n";
            replica_page_ = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (huge_shift << MAP_HUGE_SHIFT), -1, 0);
            
            if (replica_page_ == MAP_FAILED && hugepage_size_mb >= 1024) {
                if (verbose) std::cerr << "[setup_memory] 1GB hugepage failed, falling back to 2MB\n";
                hugepage_size_mb = 2;
                page_size = get_superpage_size();
                huge_shift = 21;
                if (verbose) std::cout << "[setup_memory] Allocating 2MB hugepage (shift=" << huge_shift << ")\n";
                replica_page_ = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (huge_shift << MAP_HUGE_SHIFT), -1, 0);
            }
        } else {
            if (verbose) std::cout << "[setup_memory] Allocating " << (page_size / 1024) << "KB regular memory\n";
            replica_page_ = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }

        if (replica_page_ == MAP_FAILED) {
            perror(use_hugepage ? "mmap hugepage" : "mmap regular memory");
            replica_page_ = nullptr;
            return false;
        }
        if (verbose) std::cout << "[setup_memory] Allocated at addr=" << replica_page_ << "\n";
        
        std::memset(replica_page_, 0x42, page_size);
        if (verbose) std::cout << "[setup_memory] Filled memory with 0x42 pattern\n";

        char* base = static_cast<char*>(replica_page_);
        for (std::size_t i = 0; i < N; ++i) {
            replicas_[i] = reinterpret_cast<T*>(base + (i * channel_offset_));
            if (verbose) std::cout << "[setup_memory] Replica " << i << " base addr=" << (void*)replicas_[i] 
                      << " (offset " << (i * channel_offset_) << " bytes from base)\n";
        }

        return true;
    }

};

} // namespace tailslayer

#endif // TAILSLAYER_HEDGED_READER_HPP