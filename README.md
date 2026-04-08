[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://www.apache.org/licenses/LICENSE-2.0)
[![GitHub stars](https://img.shields.io/github/stars/LaurieWired/tailslayer)](https://github.com/LaurieWired/tailslayer/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/LaurieWired/tailslayer)](https://github.com/LaurieWired/tailslayer/network/members)
[![GitHub contributors](https://img.shields.io/github/contributors/LaurieWired/tailslayer)](https://github.com/LaurieWired/tailslayer/graphs/contributors)
[![Follow @lauriewired](https://img.shields.io/twitter/follow/lauriewired?style=social)](https://twitter.com/lauriewired)

<img width="2490" height="1148" alt="tailslayer3" src="https://github.com/user-attachments/assets/35d6cd98-ab9d-4ea6-8804-dff3a1b8698b" />

# Tailslayer

Tailslayer is a C++ library that reduces tail latency in RAM reads caused by DRAM refresh stalls. 

It replicates data across multiple, independent DRAM channels with uncorrelated refresh schedules, using (undocumented!) channel scrambling offsets that works on AMD, Intel, and Graviton. Once the request comes in, Tailslayer issues hedged reads across all replicas, allowing the work to be performed on whichever result responds first.

For now, Tailslayer is x86 only and relies on Linux-specific facilities such as CPU affinity control and hugepages. The technique itself does not inherently require hugepages, but it simplifies the memory placement logic for now. 

Tailslayer is designed to cut tail latency, not maximize throughput. Replication adds DRAM traffic and reduces effective memory capacity in proportion to the replica count, so there is an explicit memory tradeoff. Also note that coordination that forces workers to wait for each other will reintroduce tail coupling and defeat the purpose of tailslayer.

<img width="4986" height="2796" alt="cross_platform_nway" src="https://github.com/user-attachments/assets/4b4a5614-00e4-4845-8a4b-f4adecef5b4d" />

## Usage

The library code is available in [hedged_reader.cpp](https://github.com/LaurieWired/tailslayer/blob/main/include/tailslayer/hedged_reader.hpp) and the example using the library can be found in [tailslayer_example.cpp](https://github.com/LaurieWired/tailslayer/blob/main/tailslayer_example.cpp). To use it, copy `include/tailslayer` into your project and `#include <tailslayer/hedged_reader.hpp>`. The library currently works with two channels (updates to come!), but full N-way usage is available in the [benchmark](https://github.com/LaurieWired/tailslayer/tree/main/discovery/benchmark).

You provide the value type and two functions as template parameters:

1. **Signal function**: Add the loop that waits for the external signal. This determines when to read. Return the desired index to read, and the read immediately fires.
2. **Final work function**: This receives the value immediately after it is read. Add the desired value processing code here.

```cpp
#include <tailslayer/hedged_reader.hpp>
#include <system_error>

[[gnu::always_inline]] inline std::size_t my_signal() {
    // Wait for your event, then return the index to read
    return index_to_read;
}

template <typename T>
[[gnu::always_inline]] inline void my_work(T val) {
    // Use the value
}

int main() {
    using T = uint8_t;
    tailslayer::pin_to_core(tailslayer::CORE_MAIN);

    try {
        tailslayer::HedgedReader<T, my_signal, my_work<T>> reader{};
        reader.insert(0x43);
        reader.insert(0x44);
        reader.start_workers();
    } catch (const std::system_error& e) {
        return 1;
    }
}
```

Arguments can be passed to either function via `ArgList`:

```cpp
tailslayer::HedgedReader<T, my_signal, my_work<T>,
    tailslayer::ArgList<1, 2>,   // args to signal function
    tailslayer::ArgList<2>       // args to final work function
> reader{};
```

`HedgedReader` now uses RAII for setup:

1. Construct `HedgedReader` to validate the configuration, precompute the layout, and map the hugepage-backed replica region.
2. Call `insert(...)` to populate replicated values.
3. Call `start_workers()` to launch the hedged readers on their assigned cores.

Construction throws `std::system_error` if `mmap` or `mlock` fails, so executables should catch that and report policy-specific guidance there.

You can also optionally pass in a different channel offset, channel bit, and number of replicas to the constructor. *Note:* Each insert copies the element N times where N is the number of replicas. That means tailslayer is trading memory footprint and some steady-state bandwidth for better tail latency under channel stalls. It does the address calculation work on the backend, allowing tailslayer to act as a hedged vector that uses logical indices. Additionally, each replica is pinned to a separate core, and will spin on that core according to the signal function until the read happens.

## Build the example

```bash
make clean && make DEBUG=1
./tailslayer_example
```

## Benchmarks and spike timing

The `discovery/` directory contains supporting code used to characterize DRAM refresh behavior:

- `discovery/benchmark/`: Channel-hedged read benchmark
- `discovery/trefi_probe.c`: Spike timing probe for measuring the refresh cycle

```bash
cd discovery/benchmark
make
sudo chrt -f 99 ./hedged_read_cpp --all --channel-bit 8
```
