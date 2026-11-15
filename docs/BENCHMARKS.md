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
