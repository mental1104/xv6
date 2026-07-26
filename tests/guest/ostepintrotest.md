# ostepintrotest

Issue #133 的 guest-first 概念实验，统一入口：

```text
/usr/bin/xv6test --run lab3-ostep-intro
```

它验证三条最小闭环：

- 两个 CPU-bound 进程都获得运行机会；`CPUS=1` 时还必须共享唯一 CPU lane 并各自被重新调度；
- `fork` 后父子进程在相同虚拟地址观察到彼此隔离的私有值；
- `write(fd, buffer, n)` 严格服从显式字节数，`sizeof(payload)` 会把结尾 NUL 一并写入。

该实验不要求多核进程落在同一 CPU，不依赖严格交替顺序，也不证明掉电持久化或生产系统调度策略。
