# Event-driven concurrency teaching regression

## Scope

This experiment establishes one narrow, observable event-loop path in xv6:

```text
wait for readable pipe sources
→ receive a readiness bitmap
→ read one complete event
→ restore source-owned state
→ run a short handler
→ update state and wait again
```

It does **not** implement POSIX `select(2)` / `poll(2)` or Linux `epoll(7)`. The teaching syscall `pollread()` accepts only readable pipe descriptors, reports buffered data and writer-close EOF as readable, and has no timeout, write-readiness, signal-mask, device, file, or socket support.

## Focused guest commands

```text
/usr/bin/xv6test --run core-eventloop
/usr/bin/eventloop
/usr/bin/eventloop --slow
```

The automated host entry is:

```bash
python3 tests/run.py --suite usertests-core --cpus 3
```

A single-CPU run may be used as an additional observation, but it does not replace the multi-CPU result:

```bash
python3 tests/run.py --suite usertests-core --cpus 1
```

## Behavioral contracts

`tests/guest/eventlooptest.c` verifies:

- an empty nonblocking snapshot returns no ready slots;
- two independent pipe sources produce the expected two-bit readiness bitmap;
- consuming an event clears data readiness, while later events remain observable;
- closing the last writer makes the read end EOF-ready, and `read()` then returns zero;
- a blocking wait wakes for both data and EOF without losing the transition between scan and sleep;
- write-only pipes, invalid descriptors, invalid counts/modes, and an unmapped user array are rejected;
- each event source owns an independent expected sequence and handled count;
- the final state is exactly `A=2`, `B=1`, `total=3`;
- `eventloop --slow` delays source B by at least ten ticks while source A's handler sleeps, demonstrating that one blocking handler stalls unrelated ready work in the same event loop.

Stable public oracles include:

```text
EVENTLOOP snapshot ready=0
ORACLE state-complete PASS a=2 b=1 total=3
ORACLE slow-handler PASS source=B lag=<n>
EVENTLOOP done mode=slow
eventlooptest: OK
XV6TEST done status=0
```

The exact dispatch order and measured lag are intentionally not golden values beyond the stated invariants.

## Kernel ownership and lock boundary

- `struct pipe` owns buffered byte positions and writer lifetime under `pipe.lock`.
- `pollstate.lock` protects the global teaching wait channel and the scan-to-sleep boundary.
- `pollreadfiles()` acquires `pollstate.lock` before scanning pipe state.
- `pipewrite()` and writer `pipeclose()` commit state under `pipe.lock`, release it, then call `pollnotify()`.

This fixed lock order avoids a `pollstate.lock ↔ pipe.lock` inversion and prevents a readiness transition from being lost between the final scan and `sleep()`.

## Teaching boundary

The single global wait channel deliberately favors a small, inspectable mechanism over scalability. It can wake unrelated waiters and therefore models neither per-object wait queues nor production `epoll` registration. Interrupts and xv6 `sleep/wakeup` remain lower-level mechanisms; the user program's explicit per-source state and dispatch loop are what form the event-driven concurrency model.
