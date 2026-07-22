# xv6 regression suites

`tests/run.py` drives the integrated `main` kernel through QEMU and runs the host-side pthread exercises. It is a repository regression runner, not a copy of the course scoring scripts.

The runner now uses a guest-first boundary for the migrated tests:

```text
make test
→ unit-test the Python runner
→ Python starts an isolated QEMU snapshot
→ Python asks xv6test to run a guest group
→ xv6test forks one process per registered test
→ the existing xv6 test program owns assertions and exit status
→ xv6test emits the stable XV6TEST result protocol
→ Python handles timeout, logs, and suite aggregation
```

`tests/test_runner.py` unit-tests the host runner without booting QEMU. It checks suite composition, output matching, failure detection, counted expectations, the guest protocol, and helper behavior.

## Commands

```bash
# Default developer entry: runner unit tests, then the PR-level QEMU regression.
make test

# Validate only the Python runner; does not build or boot xv6.
make test-unit
make test-grader

# Build xv6 and run the PR-level QEMU integration suite.
make test-integration
make test-labs

# Other host scopes.
make test-usertests
make test-full
make test-suite SUITE=lab-vm
python3 tests/run.py --list
```

Inside an xv6 shell, the guest registry supports:

```text
xv6test --list
xv6test --all
xv6test --group lab3
xv6test --group lab5
xv6test --group lab6
xv6test --run lab3-copyin
```

A successful guest invocation always ends with:

```text
XV6TEST summary selected=<n> passed=<n> failed=0
XV6TEST done status=0
```

An unknown or empty selection is a failure rather than a no-op success. Each selected test runs in its own child process, and `wait()` propagates the test program's exit status into the group summary.

Each atomic QEMU suite starts from `fs.img` with QEMU `-snapshot`, so disk writes are discarded when that suite finishes. `lab9-bigfile`, `lab9-symlink`, and `lab10-mmap` intentionally use separate snapshots because the large-file test substantially changes the guest file system.

Raw output is stored below `test-results/<suite>/`.

## Test boundaries

The layers verify different things:

| Layer | Starts QEMU | What it validates |
|---|---:|---|
| `make test-unit` | No | Host suite graph, matching rules, guest protocol, failure rules, and helpers |
| `xv6test` | Already in guest | Test registration, per-test process isolation, exit-status propagation, stable summary protocol |
| Host tests in `tests/run.py` | No | `ph` and `barrier` pthread behavior |
| `make test-integration` | Yes | User programs, syscalls, traps, VM, locks, file system, and mmap behavior |
| `make test-usertests` | Yes | Complete xv6 cross-subsystem regression |

Most kernel behavior cannot be meaningfully proven by ordinary host unit tests because it depends on RISC-V traps, page tables, interrupts, multiple CPUs, and QEMU devices. Pure helpers can be extracted and unit-tested with stubs, but the final evidence must still include QEMU integration tests.

## Coverage map

| Lab | Stable regression coverage | Current owner |
|---|---|---|
| Lab1 util | `sleep`, `pingpong`, `primes`, `find`, `xargs` | Host runner output rules |
| Lab2 syscall | `trace`, `sysinfotest` | Host runner output rules |
| Lab3 page tables | `copyin`, `copyout`, `copyinstr1`, `sbrkmuch` | `xv6test --group lab3` |
| Lab4 traps | `bttest`, `alarmtest` | Host runner output rules |
| Lab5 lazy allocation | `lazytests` | `xv6test --group lab5` |
| Lab6 COW | `cowtest` | `xv6test --group lab6` |
| Lab7 threads | `uthread`, host `ph`, host `barrier` | Mixed guest/host legacy rules |
| Lab8 locks | allocator and buffer-cache behavior under `usertests`; no shared-runner performance threshold | Host runner output rules |
| Lab9 file system | `bigfile`, `symlinktest` | Host runner output rules |
| Lab10 mmap | `mmaptest` | Host runner output rules |

The first migration intentionally covers programs whose exit status is or can be made a reliable test result. Lab1 command-output checks and Lab4 diagnostic-output checks remain host-owned until a guest output-capture abstraction is justified.

The Lab3 grader checks tied to fixed page-table addresses and answer files are intentionally excluded. Lab8 contention counters and the historical `ph` 1.25x speedup threshold are also excluded from blocking regression because they require test-only instrumentation or are unstable on shared runners. The integrated regression instead verifies observable behavior and keeps performance experiments separate.

## Suite policy

- `pr`: Lab1-10 behavior plus focused cross-lab `usertests`.
- `usertests-full`: the complete xv6 regression, intended for `main` and manual runs.
- `full`: Lab1-10 behavior plus complete `usertests`.
- `CPUS=3` is the default. `CPUS=1` is useful as supplemental diagnosis, but it does not prove multi-core correctness.
