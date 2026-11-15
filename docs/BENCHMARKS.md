# Benchmarks

Results in this file were measured, on the machine described with each of them. The method is specified in [SPEC.md](SPEC.md), section 13.

All results so far were taken under WSL2, a virtual machine. They are indicative: use them to compare two versions of this code on the same machine, not as absolute figures.

## Engine benchmark

Measures one call to `MatchingEngine::process()`, on one thread, with no networking and no queues.

```sh
cmake --workflow --preset release
./build/release/bench/exchange-bench engine
```

`exchange-bench engine --help` lists the workload and measurement options. `--json FILE` also writes the results as JSON.

### Baseline

This is the first implementation: standard containers, no optimization work. Later changes are compared against it.

| | |
|---|---|
| Date | 2026-11-15 |
| Processor | Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz, 8 logical CPUs |
| System | Linux 6.18.58.1-microsoft-standard-WSL2 (WSL2) |
| Compiler | GCC 15.2.0 |
| Build | Release, `-O3 -DNDEBUG` |
| Workload | seed 42, 10,000 initial orders, 1,000,000 measured commands |
| Mix | 50.0 % passive orders, 10.0 % aggressive orders, 40.0 % cancels |
| Work done | 197,919 trades, 2,648,700 events |
| Clock | TSC, smallest measurable interval 11.5 ns, 15.3 ns per reading |
| Runs | 1 warm-up, 5 measured |

Latency of one command in nanoseconds, and throughput in commands per second:

| Run | min | p50 | p95 | p99 | p99.9 | max | mean | throughput |
|---|---|---|---|---|---|---|---|---|
| 1 | 57 | 404 | 730 | 1,088 | 2,284 | 180,131 | 444 | 2,279,585 |
| 2 | 63 | 428 | 753 | 1,129 | 2,453 | 193,114 | 463 | 2,293,515 |
| 3 | 61 | 441 | 905 | 1,406 | 3,219 | 331,294 | 503 | 2,299,406 |
| 4 | 61 | 460 | 1,034 | 1,628 | 4,001 | 9,869,679 | 588 | 1,962,173 |
| 5 | 62 | 399 | 723 | 1,092 | 2,405 | 203,470 | 441 | 2,312,609 |
| **median** | **61** | **428** | **753** | **1,129** | **2,453** | **203,470** | **463** | **2,293,515** |

The raw output is in [benchmarks/engine-baseline.json](benchmarks/engine-baseline.json).

How to read it:

- Each latency includes one clock reading, about 15 ns here.
- The mean is higher than the median because a few commands are much slower than the others: an aggressive order that trades with several resting orders, or a command interrupted by the operating system.
- The maximum varies by a factor of 50 between runs. It reflects interruptions of the virtual machine, not the engine.
- Run 4 is visibly slower than the others. This kind of variation between runs is normal on a laptop under WSL2, which is why the summary is a median over runs.

## Queue benchmark

Sends commands from one thread to another, through the lock-free `SpscQueue` and through the same ring buffer protected by a mutex.

```sh
./build/release/bench/exchange-bench queue
```

### Baseline

| | |
|---|---|
| Date | 2026-11-15 |
| Processor | Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz, 8 logical CPUs |
| System | Linux 6.18.58.1-microsoft-standard-WSL2 (WSL2) |
| Compiler | GCC 15.2.0 |
| Build | Release, `-O3 -DNDEBUG` |
| Messages | 10,000,000 per run, 48 bytes each |
| Queue capacity | 4,096 |

Throughput in messages per second:

| Run | `SpscQueue` | Mutex queue |
|---|---|---|
| 1 | 27,332,344 | 6,997,030 |
| 2 | 26,278,046 | 6,323,154 |
| 3 | 27,244,474 | 6,164,789 |
| 4 | 25,693,882 | 6,610,145 |
| 5 | 52,527,739 | 7,188,455 |
| **median** | **27,244,474** | **6,610,145** |

The lock-free queue transferred 4.1 times as many messages as the mutex queue. The ratio itself varies between sessions on this machine, from about 3 to 6; what is stable is that the lock-free queue is several times faster. The raw output is in [benchmarks/queue-baseline.json](benchmarks/queue-baseline.json).

How to read it:

- Both threads spin, so this is the most favorable situation for a lock-free queue and a hard one for a mutex, which the two threads fight over constantly.
- Either queue is faster than the matching engine, which handles about 2 million commands per second in the engine benchmark. In this system the queue is not the bottleneck; the reason to prefer the lock-free queue is that a thread can never be made to wait by the other one holding a lock.

## End-to-end benchmark

Measures a request from the moment a client gives it to the kernel until the client has read the server's answer, over TCP on the loopback interface. The server runs in the benchmark's own process and is measured in its three configurations.

```sh
./build/release/bench/exchange-bench e2e
```

`--connections`, `--requests` and `--rate` choose the load; `exchange-bench --help` lists every option.

### Baseline

| | |
|---|---|
| Date | 2026-11-15 |
| Processor | Intel(R) Core(TM) i7-8550U CPU @ 1.80GHz, 8 logical CPUs |
| System | Linux 6.18.58.1-microsoft-standard-WSL2 (WSL2) |
| Compiler | GCC 15.2.0 |
| Build | Release, `-O3 -DNDEBUG` |
| Workload | 10,000 initial orders, 10 % aggressive orders, 40 % cancels, the rest passive orders |
| Runs | 3 per server, each against a new server; the tables show the median of each statistic |

Latencies are in microseconds. Client threads spin on non-blocking sockets, so the time to wake a sleeping client is not included.

**Closed loop, 1 connection, 100,000 requests.** The connection sends its next request when the previous one is answered.

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 13.3 | 64.2 | 109.7 | 145.7 | 281.9 | 4,946.8 | 70.0 | 14,163 |
| pipeline (backoff) | 29.0 | 96.0 | 170.9 | 295.9 | 649.8 | 15,513.8 | 107.3 | 9,248 |
| pipeline (spin) | 25.3 | 92.2 | 163.5 | 235.1 | 532.5 | 14,201.9 | 103.9 | 9,556 |

**Closed loop, 4 connections, 50,000 requests each.**

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 13.1 | 47.6 | 114.4 | 206.1 | 1,064.8 | 10,781.1 | 63.3 | 61,937 |
| pipeline (backoff) | 18.1 | 61.1 | 125.4 | 405.1 | 1,929.4 | 26,650.1 | 79.1 | 50,049 |
| pipeline (spin) | 18.8 | 61.0 | 120.4 | 299.4 | 1,585.2 | 8,746.4 | 73.0 | 54,124 |

**Open loop, 1 connection, 20,000 requests per second.** Requests are sent on a fixed schedule and latency is counted from the scheduled time.

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 13.3 | 62.8 | 139.6 | 1,697.9 | 10,223.0 | 14,407.7 | 123.5 | 19,998 |
| pipeline (backoff) | 15.7 | 70.8 | 169.5 | 522.7 | 5,846.8 | 8,871.4 | 106.9 | 19,997 |
| pipeline (spin) | 15.5 | 74.2 | 288.3 | 14,476.8 | 32,629.4 | 37,629.4 | 498.2 | 19,998 |

**Open loop, 1 connection, 1,000 requests per second.**

| Server | min | p50 | p95 | p99 | p99.9 | max | mean | requests/s |
|---|---|---|---|---|---|---|---|---|
| inline | 41.6 | 108.3 | 135.0 | 170.9 | 281.4 | 537.9 | 110.4 | 1,000 |
| pipeline (backoff) | 84.0 | 266.8 | 384.5 | 467.1 | 10,611.0 | 15,165.6 | 300.6 | 1,000 |
| pipeline (spin) | 65.0 | 134.5 | 233.6 | 315.9 | 747.6 | 1,751.8 | 149.0 | 1,000 |

The raw outputs are in [benchmarks/](benchmarks/) (`e2e-*.json`).

### What these results say

- **The network path dominates.** The engine handles a command in about 0.4 µs (engine benchmark above); a request over loopback TCP takes about 64 µs at the median. More than 99 % of that time is spent in the kernel's TCP stack and in waking threads, not in matching.
- **One thread is faster than two here.** With one connection the single-threaded server answers in 64 µs at the median, the two-thread server in 92 to 96 µs. The two-thread server pays for two extra hand-offs per request: the network thread hands the command to the engine thread, then goes back to sleep and has to be woken up for the answer. With four connections the single-threaded server still handles more requests per second (61,937 against 54,124).
- **Why keep two threads, then.** The engine is so much faster than the network path that a second thread has nothing to relieve on this workload. Separating the threads would pay off if matching were expensive compared with I/O, or if the network thread were saturated. The two-thread mode is kept as the default because it is the architecture this project sets out to build and study; the measurements say plainly that it is not the fastest on this machine.
- **An idle engine thread that naps costs latency.** At 1,000 requests per second the engine thread has time to fall asleep between requests: the median goes from 134 µs when it spins to 267 µs when it naps. At 20,000 requests per second it never sleeps and the two settings are equivalent.
- **Closed-loop and open-loop numbers are not comparable.** In a closed loop a slow answer delays the next request instead of being counted against it, so the percentiles look better than what a client sending at a steady rate would see.
- **The high percentiles are noise on this machine.** Above p99 the values change by a factor of ten from one session to the next and do not rank the three servers consistently. They reflect the virtual machine and the laptop, not the servers.
