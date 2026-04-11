/*
 * trefi_probe.c — Detect periodic DRAM refresh (tREFI) jitter via clflush timing
 *
 * Probes a single address with clflush+reload, records all spike timestamps.
 *
 * Build: gcc -O2 -o trefi_probe trefi_probe.c -lm
 * Run:   sudo chrt -f 99 taskset -c 3 ./trefi_probe
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <sys/mman.h>
#include <sched.h>
#include <getopt.h>
#include <time.h>

#define HUGEPAGE_2M       (1ULL << 21)
#define CALIB_PROBES      500000
#define MAX_SPIKES        2000000
#define DEFAULT_PROBES    20000000
#define DEFAULT_TREFI_US  7.8

static inline uint64_t rdtsc_lfence(void)
{
#if defined(__aarch64__)
    uint64_t tsc_val;
    asm volatile("isb\n\t"
                 "mrs %0, pmccntr_el0"
                 : "=r" (tsc_val));
    return tsc_val;
#else
    uint64_t lo, hi;
    asm volatile("lfence\n\t"
                 "rdtsc"
                 : "=a"(lo), "=d"(hi));
    return (hi << 32) | lo;
#endif
}

static inline uint64_t rdtscp_lfence(void)
{
#if defined(__aarch64__)
    uint64_t val;
    asm volatile(
        "isb\n\t"
        "mrs %0, pmccntr_el0\n\t"
        "isb"
        : "=r"(val)
        :
        : "memory"
    );
    return val;
#else
    uint64_t lo, hi;
    uint32_t aux;
    asm volatile("rdtscp"
                 : "=a"(lo), "=d"(hi), "=c"(aux));
    asm volatile("lfence" ::: "memory");
    return (hi << 32) | lo;
#endif
}

static inline void clflush_addr(volatile void *addr)
{
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

static inline void mfence_inst(void)
{
#if defined(__aarch64__)
    asm volatile("dmb ish" ::: "memory");
#else
    asm volatile("mfence" ::: "memory");
#endif
}

static inline void lfence_inst(void)
{
#if defined(__aarch64__)
    asm volatile("dsb ld\n\t" "isb" ::: "memory");
#else
    asm volatile("lfence" ::: "memory");
#endif
}

// TSC frequency calibration
static double calibrate_tsc_ghz(void)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t tsc0 = rdtsc_lfence();

    struct timespec req = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&req, NULL);

    uint64_t tsc1 = rdtscp_lfence();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9 +
                        (t1.tv_nsec - t0.tv_nsec);
    return (double)(tsc1 - tsc0) / elapsed_ns;
}

static inline uint64_t timed_probe(volatile char *addr)
{
    clflush_addr(addr);
    mfence_inst();
    lfence_inst();
    uint64_t t0 = rdtsc_lfence();
    *(volatile char *)addr;
    uint64_t t1 = rdtscp_lfence();
    return t1 - t0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

struct spike {
    uint64_t tsc;
    uint64_t latency;
};

int main(int argc, char **argv)
{
    int n_probes = DEFAULT_PROBES;
    uint64_t manual_threshold = 0;
    double trefi_us = DEFAULT_TREFI_US;
    double thresh_mult = 2.0;

    static struct option long_opts[] = {
        {"probes",       required_argument, NULL, 'n'},
        {"threshold",    required_argument, NULL, 'T'},
        {"trefi-us",     required_argument, NULL, 't'},
        {"thresh-mult",  required_argument, NULL, 'm'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:T:t:m:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': n_probes = atoi(optarg); break;
        case 'T': manual_threshold = strtoull(optarg, NULL, 0); break;
        case 't': trefi_us = atof(optarg); break;
        case 'm': thresh_mult = atof(optarg); break;
        case 'h':
        default:
            fprintf(stderr, "Usage: %s [--probes N] [--threshold N] "
                    "[--trefi-us F] [--thresh-mult F]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    double tsc_ghz = calibrate_tsc_ghz();
    fprintf(stderr, "TSC: %.3f GHz\n", tsc_ghz);

    double expected_trefi_cyc = trefi_us * 1000.0 * tsc_ghz;
    fprintf(stderr, "Expected tREFI: %.1f us = %.0f cycles\n",
            trefi_us, expected_trefi_cyc);

    // Map 2MB hugepage
    void *p = mmap(NULL, HUGEPAGE_2M, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
                   (21 << MAP_HUGE_SHIFT), -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap 2MB hugepage");
        fprintf(stderr, "Setup: sudo bash -c 'echo 64 > "
                "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'\n");
        return 1;
    }
    memset(p, 0x42, HUGEPAGE_2M);
    mlock(p, HUGEPAGE_2M);

    volatile char *addr = (volatile char *)p;

    fprintf(stderr, "\n=== CALIBRATING ===\n");
    uint64_t *calib = malloc(CALIB_PROBES * sizeof(uint64_t));
    for (int i = 0; i < 2000; i++)
        timed_probe(addr);
    for (int i = 0; i < CALIB_PROBES; i++)
        calib[i] = timed_probe(addr);

    qsort(calib, CALIB_PROBES, sizeof(uint64_t), cmp_u64);

    uint64_t median = calib[CALIB_PROBES / 2];
    uint64_t p90  = calib[(int)(CALIB_PROBES * 0.90)];
    uint64_t p99  = calib[(int)(CALIB_PROBES * 0.99)];
    uint64_t p999 = calib[(int)(CALIB_PROBES * 0.999)];
    uint64_t p9999 = calib[(int)(CALIB_PROBES * 0.9999)];

    uint64_t threshold;
    if (manual_threshold > 0) {
        threshold = manual_threshold;
    } else {
        threshold = (uint64_t)(thresh_mult * median);
    }

    int n_above = 0;
    for (int i = 0; i < CALIB_PROBES; i++)
        if (calib[i] > threshold) n_above++;

    fprintf(stderr, "  %d probes: median=%lu p90=%lu p99=%lu p99.9=%lu p99.99=%lu\n",
            CALIB_PROBES, median, p90, p99, p999, p9999);
    fprintf(stderr, "  Threshold: %lu (%.1fx median)\n", threshold, thresh_mult);
    fprintf(stderr, "  Calibration spikes: %d (%.3f%%)\n",
            n_above, 100.0 * n_above / CALIB_PROBES);
    free(calib);

    // Allocate spike buffer
    struct spike *spikes = malloc(MAX_SPIKES * sizeof(struct spike));
    if (!spikes) { perror("malloc spikes"); return 1; }
    int n_spikes = 0;

    // Main probe loop
    fprintf(stderr, "\n=== PROBING (%d probes) ===\n", n_probes);
    uint64_t tsc_start = rdtsc_lfence();

    for (int i = 0; i < n_probes; i++) {
        clflush_addr(addr);
        mfence_inst();
        lfence_inst();
        uint64_t t0 = rdtsc_lfence();
        *(volatile char *)addr;
        uint64_t t1 = rdtscp_lfence();
        uint64_t lat = t1 - t0;

        if (lat > threshold && n_spikes < MAX_SPIKES) {
            spikes[n_spikes].tsc = t0;
            spikes[n_spikes].latency = lat;
            n_spikes++;
        }
    }

    uint64_t tsc_end = rdtscp_lfence();
    double elapsed_s = (double)(tsc_end - tsc_start) / (tsc_ghz * 1e9);

    fprintf(stderr, "  Duration: %.2f s\n", elapsed_s);
    fprintf(stderr, "  Spikes: %d (%.4f%%)\n", n_spikes,
            100.0 * n_spikes / n_probes);

    // Output CSV to stdout
    printf("abs_tsc,latency_cyc\n");
    for (int i = 0; i < n_spikes; i++)
        printf("%lu,%lu\n", spikes[i].tsc, spikes[i].latency);

    fprintf(stderr, "\n=== PERIODICITY ANALYSIS ===\n");

    if (n_spikes < 10) {
        fprintf(stderr, "  Too few spikes (%d) for analysis\n", n_spikes);
        fprintf(stderr, "  VERDICT: INSUFFICIENT DATA\n");
        free(spikes);
        munmap(p, HUGEPAGE_2M);
        return 0;
    }

    // Compute inter-spike intervals
    int n_intervals = n_spikes - 1;
    double *intervals = malloc(n_intervals * sizeof(double));
    for (int i = 0; i < n_intervals; i++)
        intervals[i] = (double)(spikes[i + 1].tsc - spikes[i].tsc);

    // Harmonic binning: count intervals near 1T, 2T, 3T
    double T = expected_trefi_cyc;
    int count_1T = 0, count_2T = 0, count_3T = 0, count_other = 0;
    for (int i = 0; i < n_intervals; i++) {
        double iv = intervals[i];
        if (iv >= T * 0.85 && iv <= T * 1.15)       count_1T++;
        else if (iv >= T * 1.85 && iv <= T * 2.15)   count_2T++;
        else if (iv >= T * 2.85 && iv <= T * 3.15)   count_3T++;
        else                                          count_other++;
    }

    double frac_1T = (double)count_1T / n_intervals;
    double frac_2T = (double)count_2T / n_intervals;
    double frac_3T = (double)count_3T / n_intervals;
    double frac_harmonic = (double)(count_1T + count_2T + count_3T) / n_intervals;

    fprintf(stderr, "  Expected tREFI: %.0f cycles (%.1f us)\n", T, trefi_us);
    fprintf(stderr, "  Intervals: %d total\n", n_intervals);
    fprintf(stderr, "  1T (±15%%): %d (%.1f%%)\n", count_1T, frac_1T * 100);
    fprintf(stderr, "  2T (±15%%): %d (%.1f%%)\n", count_2T, frac_2T * 100);
    fprintf(stderr, "  3T (±15%%): %d (%.1f%%)\n", count_3T, frac_3T * 100);
    fprintf(stderr, "  Other:     %d (%.1f%%)\n", count_other,
            100.0 * count_other / n_intervals);
    fprintf(stderr, "  Harmonic total: %.1f%%\n", frac_harmonic * 100);

    // Fine-grained histogram near 1T to find exact peak
    int hist_bins = 200;
    double bin_lo = T * 0.5;
    double bin_hi = T * 1.5;
    double bin_width = (bin_hi - bin_lo) / hist_bins;
    int *hist = calloc(hist_bins, sizeof(int));
    int hist_total = 0;

    for (int i = 0; i < n_intervals; i++) {
        double iv = intervals[i];
        if (iv >= bin_lo && iv < bin_hi) {
            int bin = (int)((iv - bin_lo) / bin_width);
            if (bin >= 0 && bin < hist_bins) {
                hist[bin]++;
                hist_total++;
            }
        }
    }

    int peak_bin = 0, peak_count = 0;
    for (int b = 0; b < hist_bins; b++) {
        if (hist[b] > peak_count) {
            peak_count = hist[b];
            peak_bin = b;
        }
    }
    double peak_cyc = bin_lo + (peak_bin + 0.5) * bin_width;
    double peak_us = peak_cyc / (tsc_ghz * 1000.0);

    fprintf(stderr, "\n  Histogram peak: %.0f cycles (%.2f us), count=%d\n",
            peak_cyc, peak_us, peak_count);
    fprintf(stderr, "  Expected:       %.0f cycles (%.2f us)\n", T, trefi_us);
    fprintf(stderr, "  Deviation:      %.1f%%\n",
            fabs(peak_cyc - T) / T * 100);

    // Spike latency stats
    uint64_t lat_min = UINT64_MAX, lat_max = 0;
    double lat_sum = 0;
    for (int i = 0; i < n_spikes; i++) {
        if (spikes[i].latency < lat_min) lat_min = spikes[i].latency;
        if (spikes[i].latency > lat_max) lat_max = spikes[i].latency;
        lat_sum += spikes[i].latency;
    }
    fprintf(stderr, "\n  Spike latency: min=%lu avg=%.0f max=%lu cycles\n",
            lat_min, lat_sum / n_spikes, lat_max);
    fprintf(stderr, "  Spike latency: min=%.1f avg=%.1f max=%.1f ns\n",
            lat_min / tsc_ghz, (lat_sum / n_spikes) / tsc_ghz,
            lat_max / tsc_ghz);

    fprintf(stderr, "\n");
    if (frac_harmonic > 0.30) {
        fprintf(stderr, "  VERDICT: PERIODIC — %.0f%% of intervals at tREFI harmonics\n",
                frac_harmonic * 100);
        fprintf(stderr, "  tREFI is visible via clflush timing on this DDR4 system\n");
    } else if (frac_harmonic > 0.15) {
        fprintf(stderr, "  VERDICT: WEAK SIGNAL — %.0f%% at harmonics (borderline)\n",
                frac_harmonic * 100);
    } else {
        fprintf(stderr, "  VERDICT: NO PERIODIC SIGNAL — %.0f%% at harmonics\n",
                frac_harmonic * 100);
        fprintf(stderr, "  Spikes are likely controller noise, not refresh\n");
    }

    free(hist);
    free(intervals);
    free(spikes);
    munmap(p, HUGEPAGE_2M);
    return 0;
}
