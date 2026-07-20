# xv6 regression suites

`tests/run.py` drives the integrated `main` kernel through QEMU and runs the host-side pthread exercises. It is a repository regression runner, not a copy of the course scoring scripts.

`tests/test_runner.py` unit-tests the grader itself without booting QEMU. It checks suite composition, output matching, failure detection, counted expectations, and helper behavior.

## Commands

```bash
# Default developer entry: grader unit tests, then Lab1-10 QEMU regression.
make test

# Validate only the grader/runner; does not build or boot xv6.
make test-unit
make test-grader

# Build xv6 and run the PR-level QEMU integration suite.
make test-integration
make test-labs

# Other scopes.
make test-usertests
make test-full
make test-suite SUITE=lab-vm
python3 tests/run.py --list
```

`make test` is deliberately layered:

```text
unit-test grader
→ build kernel and fs.img when required
→ start QEMU snapshots
→ execute xv6 commands
→ match success and failure output
→ stop QEMU and retain logs
```

Each atomic QEMU suite starts from `fs.img` with QEMU `-snapshot`, so disk writes are discarded when that suite finishes. `lab9-bigfile`, `lab9-symlink`, and `lab10-mmap` intentionally use separate snapshots because the large-file test substantially changes the guest file system.

Raw output is stored below `test-results/<suite>/`. GitHub Actions uploads this directory when a run fails.

## Test boundaries

The layers verify different things:

| Layer | Starts QEMU | What it validates |
|---|---:|---|
| `make test-unit` | No | The grader's suite graph, matching rules, failure rules, and helpers |
| Host tests in `tests/run.py` | No | `ph` and `barrier` pthread behavior |
| `make test-integration` | Yes | User programs, syscalls, traps, VM, locks, file system, and mmap behavior |
| `make test-usertests` | Yes | Complete xv6 cross-subsystem regression |

Most kernel behavior cannot be meaningfully proven by ordinary host unit tests because it depends on RISC-V traps, page tables, interrupts, multiple CPUs, and QEMU devices. Pure helpers can be extracted and unit-tested with stubs, but the final evidence must still include QEMU integration tests.

## Coverage map

| Lab | Stable regression coverage |
|---|---|
| Lab1 util | `sleep`, `pingpong`, `primes`, `find`, `xargs` |
| Lab2 syscall | `trace`, `sysinfotest` |
| Lab3 page tables | `usertests` copyin/copyout/copyinstr and address-space growth paths |
| Lab4 traps | `bttest`, `alarmtest` |
| Lab5 lazy allocation | `lazytests` |
| Lab6 COW | `cowtest` |
| Lab7 threads | `uthread`, host `ph`, host `barrier` |
| Lab8 locks | allocator and buffer-cache behavior under `usertests`; no shared-runner performance threshold |
| Lab9 file system | `bigfile`, `symlinktest` |
| Lab10 mmap | `mmaptest` |

The Lab3 grader checks tied to fixed page-table addresses and answer files are intentionally excluded. Lab8 contention counters and the historical `ph` 1.25x speedup threshold are also excluded from blocking CI because they require test-only instrumentation or are unstable on shared GitHub runners. The integrated regression instead verifies observable behavior and keeps performance experiments separate.

## Suite policy

- `pr`: Lab1-10 behavior plus focused cross-lab `usertests`.
- `usertests-full`: the complete xv6 regression, intended for `main` and manual runs.
- `full`: Lab1-10 behavior plus complete `usertests`.
- `CPUS=3` is the default. `CPUS=1` is useful as supplemental diagnosis, but it does not prove multi-core correctness.
