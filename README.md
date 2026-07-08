## Build and Benchmark

Requires CMake and a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

Run benchmark examples:

```bash
# Thin/liquid book workload
./build/bench_exchange_replay 1000000 1000 ./thin_journal 5 50 30

# Deeper resting book workload
./build/bench_exchange_replay 1000000 1000 ./deep_journal 20 18 2
```

Benchmark arguments:

```text
./build/bench_exchange_replay <events> <symbols> <journal_path> <half_spread> <jitter> <cancel_pct>
```

Where:

```text
events        Number of generated client events.
symbols       Number of symbols/order books.
journal_path  Output path for the command journal.
half_spread   Initial bid/ask distance around the midpoint.
jitter        Generated price variation around the midpoint.
cancel_pct    Approximate percentage of cancel attempts.
```

For example:

```bash
./build/bench_exchange_replay 1000000 1000 ./thin_journal 5 50 30
```

runs 1,000,000 generated events across 1,000 symbols, writes the replay journal to `./thin_journal`, uses a half spread of 5 ticks, generates prices with +-50 ticks of jitter, and attempts cancels around 30% of the time.
