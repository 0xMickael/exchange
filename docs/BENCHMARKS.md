# Benchmarks

Every number in this file was measured. The method is specified in [SPEC.md](SPEC.md), section 13.

## Machine and method

| | |
|---|---|
| Processor | Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz, 4 cores, 8 logical CPUs (a laptop, on mains power) |
| System | Linux 6.18.58.1-microsoft-standard-WSL2 (WSL2, a virtual machine) |
| Compiler | GCC 15.2.0, Release build, `-O3 -DNDEBUG` |
| Clock | Processor timestamp counter (TSC), smallest measurable interval about 10 ns |
| Date | 2026-11-16, except the queue benchmark (2026-11-15) |

Two things follow from this machine:

- **The figures are indicative.** A laptop under a virtual machine is not a controlled environment. Use these numbers to compare two versions of this code, not as absolute figures.
- **Results move by 20 to 30 % from one session to the next.** A change is therefore never judged against a number from another day. The two versions are run alternately in the same session, several times, and the noise is measured first by comparing a version with itself (SPEC section 13.7). `scripts/ab_compare.py` does this.

Measured noise on the day of these results, same build against itself, 5 alternating rounds: at most 2 % on any engine statistic; on the end-to-end benchmark, at most 4 % on the median, 2 % on p99 and 3 % on throughput.

## Results at a glance

| Measurement | First implementation | Now |
|---|---|---|
| Engine, median time of one command | 323 ns | 159 ns |
| Engine, commands per second | 2.96 million | 6.71 million |
| Request over TCP, one client, two-thread server, median | 70.4 µs | 60.5 µs |
| Same, requests per second | 12,302 | 15,187 |
| Request over TCP, four clients, two-thread server, requests per second | 69,083 | 79,732 |
| Request over TCP, one client, network thread never sleeping, median | not available | 12.3 µs |

Both columns were measured in the same session with the corrected benchmark (see "The benchmark was measuring itself" below): 5 alternating rounds; each round is 5 runs of 1,000,000 commands for the engine, 3 runs of 30,000 requests per connection for the server.

## What the measurements changed

This section follows the optimization work in the order it happened. Two tools from outside the project were used to look for causes: `perf`, which samples where a running program spends its time, and `valgrind` (its `cachegrind` tool), which counts instructions and cache misses in a simulated processor. Neither was used to measure a gain: every "before" and "after" below comes from `exchange-bench`, in alternating rounds as described at the top. The raw outputs of the comparisons that have a file are in [benchmarks/comparisons/](benchmarks/comparisons/); the full results of the final code are at the end of this document.

| Change | Statistic | Before | After | Rounds | Kept |
|---|---|---|---|---|---|
| Benchmark: coordinating thread sleeps | one client, inline, median | 64.6 µs | 54.1 µs | 5 | yes |
| Network thread never sleeps (option) | one client, inline, median | 52.2 µs | 10.4 µs | 5 | as an option |
| Network thread stays awake while the engine owes answers | one client, pipeline, median | 74.2 µs | 58.4 µs | 5 | yes |
| Engine thread no longer signals the network thread | four clients, pipeline, requests/s | 71,544 | 78,878 | 5 | yes |
| Same change | one client, pipeline, requests/s | 19,972 | 15,238 | 5 | yes, see below |
| Orders of a session chained in the book | engine, median | 334 ns | 173 ns | 5 | yes |
| Duplicate lookups removed | engine, commands per second | 5.70 million | 6.48 million | 5 | yes |
| Orders taken from a standard memory pool | engine, median | 166 ns | 212 ns | 7 | no |

### The benchmark was measuring itself

The first thing `perf` showed was not in the server. The thread of the benchmark that waits for the clients to finish was doing so in a loop, and took more than 40 % of the samples: one of the four cores was busy doing nothing, next to the threads being measured. Making that thread sleep lowered every latency by about 15 % (one client, inline: 64.6 to 54.1 µs at the median) with no change to the server. All earlier end-to-end figures were inflated by that amount, and everything below uses the corrected benchmark.

### Where a request's time goes

The engine handles a command in about 0.2 µs and a request took more than 50 µs, so the time was clearly not in matching. To find out where it was, an option was added that keeps the server's network thread from ever sleeping: instead of waiting in the kernel for a request, it asks again and again.

With that option the median falls from 52.2 to 10.4 µs for the one-thread server and from 59.4 to 12.3 µs for the two-thread server. About 80 % of a request on this machine is the time the system takes to wake a sleeping thread. The remaining 10 to 12 µs are the real work: system calls on both sides, the kernel's TCP path, decoding, matching and encoding.

At 1,000 requests per second the option brings the one-thread server from 98.0 to 21.5 µs. A thread that has been idle for longer is slower to wake, which is why sleeping costs 77 µs there against 42 µs in a closed loop.

The option is kept, off by default: it uses a whole core all the time (decision D25).

### The second wake-up

In the two-thread server, the network thread handed each command to the engine thread and went back to sleep. The engine thread answered within microseconds and had to wake it up again: two wake-ups per request instead of one.

The network thread now stays awake while the engine thread owes it answers. For one client the median went from 74.2 to 58.4 µs and the throughput from 11,179 to 17,379 requests per second. With four clients the throughput did not move beyond the noise (71,951 to 69,399).

That left the engine thread signalling a thread that was already awake. Removing the signal gave opposite results: 10 % more requests per second with four clients (71,544 to 78,878), and 24 % fewer with one (19,972 to 15,238).

The explanation is that the useless signal had a side effect. It made the network thread go round its loop once more before sleeping, and the benchmark's client sends its next request a few microseconds after it receives an answer: that request arrived during the extra turn and found the thread awake. The gain came from the rhythm of this particular client, not from the server.

The signal was removed (decision D24). The design is simpler, with one mechanism less and no missed-wake-up case to reason about; the four-client result is better; and a gain that only exists for a client that answers within microseconds is not one to keep by accident.

Against the first implementation, the final two-thread server is faster in every scenario measured: the table at the top gives the closed-loop figures, and on a fixed schedule the median went from 62.2 to 53.4 µs at 20,000 requests per second and from 262.3 to 187.0 µs at 1,000. The one-thread server, whose network code did not change, stayed within the noise with one client (53.3 and 52.1 µs).

### The engine: a wrong guess, then the profile

The plan was to make the book more compact in memory, on the assumption that allocating each order separately was the main cost. `perf` said otherwise: allocation was about 6 % of the engine's time, and the largest item, at least a quarter of the samples, was the ordered set in which the engine kept the orders of each session. That set exists to cancel a session's orders when it disconnects, and it was searched and rebalanced for every order added, filled or cancelled.

The set was replaced by a chain through the orders themselves: each order points to the previous and the next order of its session (decision D23). Adding or removing an order now updates its neighbors, with no search and no allocation. The median went from 334 to 173 ns and the throughput from 2.86 to 5.54 million commands per second.

The next profile showed lookups done twice: the level's total quantity was searched for again after each change, and a cancel looked its order up three times. Removing those raised the throughput from 5.70 to 6.48 million commands per second (+14 %) and lowered the median from 172 to 164 ns. The 99th percentile went from 407 to 427 ns, with overlapping ranges.

Over both changes, `cachegrind` counts 31 % fewer instructions and 51 % fewer first-level cache misses (13.6 to 6.7 million) for the same workload of 200,000 commands.

### The memory pool that was slower

The original idea was still tested: orders and index entries were taken from the standard library's pool (`std::pmr::unsynchronized_pool_resource`) instead of being allocated one by one. The engine became slower: 166 to 212 ns at the median, 6.61 to 4.91 million commands per second, over 7 rounds. Two earlier series gave the same direction but are not counted, because the machine was not stable.

The default allocator already keeps recently freed small blocks at hand and returns one in a few instructions. The standard pool adds an indirect call and a search for the right block size to every allocation. The change was not kept (decision D26).

### Not done

- **Waiting briefly before sleeping.** The accidental gain described above could be obtained on purpose: let the network thread keep asking for a short time after its last request before it sleeps. It would help clients that send their requests close together, at the price of some processor time, and would need a duration to choose and justify. It is a natural next experiment.
- **A pool or index written for the book.** Allocation and the hash index are what is left in the engine's profile. The engine is now about 0.3 % of a request's time, so nothing there can be seen from a client.

## What these results say

- **Waking a thread dominates.** On this machine about 80 % of a request is the wake-up of the network thread, about 20 % is the kernel's network path, and matching is about 0.3 %.
- **Two threads cost little once the second wake-up is gone.** With one client the two-thread server answers in 60.5 µs at the median against 52.1 µs for one thread; with four clients it handles 79,732 requests per second against 83,206. Before, the gaps were 70.4 against 53.3 µs and 69,083 against 77,778. Two threads still have nothing to relieve here: the engine is far faster than the network path.
- **An idle engine thread that naps costs latency.** At 1,000 requests per second the median is 187 µs when it naps and 105 µs when it spins. At 20,000 requests per second it never has time to sleep and the two are equivalent.
- **Closed-loop and open-loop numbers are not comparable.** In a closed loop a slow answer delays the next request instead of being counted against it, so the percentiles look better than what a client sending at a steady rate would see.
- **The high percentiles are noise on this machine.** Above p99 the values change by a factor of ten from one run to the next and do not rank the servers consistently. They reflect the virtual machine and the laptop, not the servers.

## Full results

One full run of each benchmark with the final code, for reference.

### Engine

Measures one call to `MatchingEngine::process()`, on one thread, with no networking and no queues.

```sh
cmake --workflow --preset release
./build/release/bench/exchange-bench engine
```

Seed 42, 10,000 initial orders, 1,000,000 measured commands (50 % passive orders, 10 % aggressive orders, 40 % cancels), producing 197,919 trades and 2,648,700 events; 1 warm-up run, then 5 measured runs. Latency in nanoseconds, throughput in commands per second:

| | min | p50 | p95 | p99 | p99.9 | max | mean | throughput |
|---|---|---|---|---|---|---|---|---|
| median of 5 runs | 58 | 179 | 326 | 596 | 1,867 | 210,348 | 201 | 5,787,309 |

This run was taken last, after 35 minutes of benchmarks, and is 10 to 15 % slower than the comparison rounds quoted above. Raw output: [benchmarks/engine.json](benchmarks/engine.json); the first implementation's is [benchmarks/engine-baseline.json](benchmarks/engine-baseline.json).

- Each latency includes one clock reading, about 13 ns.
- The mean is above the median because a few commands are much slower: an aggressive order that trades with several resting orders, or a command interrupted by the operating system.
- The maximum reflects interruptions of the virtual machine, not the engine.

### Queue

Sends commands from one thread to another, through the lock-free `SpscQueue` and through the same ring buffer protected by a mutex. 10,000,000 messages of 48 bytes per run, queue capacity 4,096. The queue has not changed since this measurement.

```sh
./build/release/bench/exchange-bench queue
```

| | `SpscQueue` | Mutex queue |
|---|---|---|
| messages per second, median of 5 runs | 27,244,474 | 6,610,145 |

The lock-free queue transferred 4.1 times as many messages per second. The ratio varies between sessions, from about 3 to 6; what is stable is that the lock-free queue is several times faster. Raw output: [benchmarks/queue-baseline.json](benchmarks/queue-baseline.json).

- Both threads spin, which is the most favorable situation for a lock-free queue and a hard one for a mutex.
- Either queue is faster than the matching engine. The queue is not the bottleneck; the reason to prefer the lock-free one is that a thread can never be made to wait by the other one holding a lock.

### End-to-end

Measures a request from the moment a client gives it to the kernel until the client has read the server's answer, over TCP on the loopback interface. The server runs in the benchmark's own process.

```sh
./build/release/bench/exchange-bench e2e
```

`--connections`, `--requests`, `--rate` and `--network-wait` choose the load and the server; `exchange-bench --help` lists every option.

The three servers are: `inline` (one thread does everything), `pipeline (backoff)` (the engine has its own thread, which naps when idle; this is the default server) and `pipeline (spin)` (the engine thread never sleeps). One full run of each scenario follows: 10,000 initial orders, 3 runs per server, median of each statistic. Latencies in microseconds.

**Closed loop, 1 connection, 100,000 requests.** The connection sends its next request when the previous one is answered.

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 12.1 | 56.5 | 81.3 | 118.7 | 207.9 | 9,540.6 | 59.5 | 16,677 |
| pipeline (backoff) | 15.0 | 61.2 | 88.8 | 124.8 | 279.4 | 2,491.7 | 65.1 | 15,233 |
| pipeline (spin) | 15.7 | 59.6 | 85.7 | 122.9 | 196.3 | 941.0 | 64.1 | 15,483 |

**Closed loop, 4 connections, 50,000 requests each.**

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 11.4 | 38.9 | 84.6 | 117.2 | 259.2 | 2,612.2 | 44.9 | 86,153 |
| pipeline (backoff) | 13.7 | 40.6 | 85.0 | 120.6 | 446.1 | 13,873.1 | 50.6 | 77,883 |
| pipeline (spin) | 13.2 | 39.7 | 84.7 | 116.3 | 381.6 | 3,042.8 | 48.1 | 81,841 |

**Open loop, 1 connection, 20,000 requests per second, 100,000 requests.** Requests are sent on a fixed schedule and latency is counted from the scheduled time.

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 11.7 | 52.3 | 87.0 | 117.4 | 241.2 | 637.3 | 59.6 | 19,998 |
| pipeline (backoff) | 15.9 | 54.2 | 95.5 | 229.6 | 411.8 | 877.0 | 58.0 | 19,998 |
| pipeline (spin) | 14.8 | 61.1 | 96.5 | 1,577.3 | 22,128.3 | 27,048.5 | 222.5 | 19,998 |

**Open loop, 1 connection, 1,000 requests per second, 5,000 requests.**

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 42.1 | 98.5 | 112.3 | 143.6 | 209.2 | 392.0 | 98.6 | 1,000 |
| pipeline (backoff) | 52.1 | 186.1 | 285.1 | 321.3 | 581.3 | 2,125.8 | 203.2 | 1,000 |
| pipeline (spin) | 47.4 | 105.9 | 155.6 | 479.4 | 8,140.1 | 12,649.2 | 147.4 | 1,000 |

**Closed loop, 1 connection, 100,000 requests, network thread never sleeping** (`--network-wait spin`).

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 9.0 | 10.3 | 20.4 | 32.9 | 89.7 | 352.6 | 12.1 | 80,969 |
| pipeline (backoff) | 11.1 | 13.0 | 24.9 | 51.2 | 224.6 | 1,729.0 | 16.0 | 61,327 |
| pipeline (spin) | 10.8 | 12.3 | 22.8 | 39.1 | 98.2 | 440.0 | 14.2 | 69,018 |

Raw outputs: [benchmarks/](benchmarks/) (`e2e-*.json`).
