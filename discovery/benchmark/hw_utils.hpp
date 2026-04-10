#ifndef HW_UTILS_HPP
#define HW_UTILS_HPP

#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

namespace HardwareUtils {
    inline std::uint64_t rdtsc_lfence() {
#if defined(__aarch64__)
        std::uint64_t tsc_val;
        asm volatile("isb\n\t"
                     "mrs %0, pmccntr_el0"
                     : "=r" (tsc_val));
        return tsc_val;
#else
        std::uint64_t lo, hi;
        asm volatile("lfence\n\t"
                     "rdtsc"
                     : "=a"(lo), "=d"(hi));
        return (hi << 32) | lo;
#endif
    }

    inline std::uint64_t rdtscp_lfence() {
#if defined(__aarch64__)
        std::uint64_t val;
        asm volatile(
            "dsb sy\n\t"
            "isb\n\t"
            "mrs %0, pmccntr_el0\n\t"
            "isb"
            : "=r"(val)
            :
            : "memory"
        );
        return val;
#else
        std::uint64_t lo, hi;
        std::uint32_t aux;
        asm volatile("rdtscp"
                     : "=a"(lo), "=d"(hi), "=c"(aux));
        asm volatile("lfence" ::: "memory");
        return (hi << 32) | lo;
#endif
    }

    inline void clflush_addr(volatile void *addr) {
#if defined(__aarch64__)
        asm volatile(
            "dc civac, %0\n\t"
            "dsb sy\n\t"
            :: "r"(addr)
            : "memory"
        );
#else
        asm volatile("clflush (%0)" :: "r"(addr) : "memory");
#endif
    }

    inline void mfence_inst() {
#if defined(__aarch64__)
        asm volatile("dmb ish" ::: "memory");
#else
        asm volatile("mfence" ::: "memory");
#endif
    }

    inline void lfence_inst() {
#if defined(__aarch64__)
        asm volatile("dsb ld\n\t" "isb" ::: "memory");
#else
        asm volatile("lfence" ::: "memory");
#endif
    }

    inline int pin_to_core(int core_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return sched_setaffinity(0, sizeof(cpuset), &cpuset);
    }

    inline int compute_channel(uint64_t phys, int channel_bit) {
        return (phys >> channel_bit) & 1;
    }
    
    inline uint64_t virt_to_phys(uint64_t vaddr) {
        int fd = open("/proc/self/pagemap", O_RDONLY);
        if (fd < 0) return 0;
        uint64_t entry;
        off_t offset = (vaddr / 4096) * 8;
        if (pread(fd, &entry, 8, offset) != 8) { close(fd); return 0; }
        close(fd);
        if (!(entry & (1ULL << 63))) return 0;
        uint64_t pfn = entry & ((1ULL << 55) - 1);
        return (pfn * 4096) | (vaddr & 0xFFF);
    }
    
    inline double calibrate_tsc_ghz() {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t tsc0 = HardwareUtils::rdtsc_lfence();
    
        struct timespec req = { 0, 100000000 }; //100ms
        nanosleep(&req, nullptr);
    
        uint64_t tsc1 = HardwareUtils::rdtscp_lfence();
        clock_gettime(CLOCK_MONOTONIC, &t1);
    
        double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
        return static_cast<double>(tsc1 - tsc0) / elapsed_ns;
    }

} // namespace HardwareUtils

#endif // HW_UTILS_HPP
