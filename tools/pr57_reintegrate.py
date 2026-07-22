#!/usr/bin/env python3
"""把 PR #57 的调度功能重新应用到最新 main 结构。"""

from pathlib import Path
import subprocess

ORIGINAL_HEAD = "25f606bdd45f89594c2f5800815b5ec3bdc28e5e"


def git_file(ref: str, path: str) -> str:
    """读取指定 Git 引用中的 UTF-8 文件。

    Args:
        ref: Git 引用，例如 origin/main 或固定提交 SHA。
        path: 仓库内相对路径。

    Returns:
        文件的 UTF-8 文本。
    """
    return subprocess.check_output(["git", "show", f"{ref}:{path}"], text=True)


def main_file(path: str) -> str:
    """读取最新 origin/main 中的文件。"""
    return git_file("origin/main", path)


def replace_once(content: str, old: str, new: str, label: str) -> str:
    """精确替换一次文本，避免主线结构变化时静默改错。

    Args:
        content: 待修改的完整文本。
        old: 预期只出现一次的旧文本。
        new: 替换后的文本。
        label: 失败时用于定位的文件或修改点名称。

    Returns:
        完成一次替换后的文本。

    Raises:
        RuntimeError: old 的出现次数不是一次。
    """
    count = content.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, got {count}: {old!r}")
    return content.replace(old, new, 1)


def write(path: str, content: str) -> None:
    """写入 UTF-8 文本，并自动创建父目录。"""
    file_path = Path(path)
    file_path.parent.mkdir(parents=True, exist_ok=True)
    file_path.write_text(content, encoding="utf-8")


def rebuild_makefile() -> None:
    """以主线 Makefile 为骨架接入调度对象、策略参数和测试入口。"""
    content = main_file("Makefile")
    content = replace_once(
        content,
        "  $K/proc.o \\\n",
        "  $K/proc.o \\\n  $K/sched.o \\\n  $K/rbtree.o \\\n",
        "Makefile OBJS",
    )
    policy_block = """SCHED_POLICY ?= rr
SCHED_POLICY_rr := 0
SCHED_POLICY_fifo := 1
SCHED_POLICY_sjf := 2
SCHED_POLICY_stcf := 3
SCHED_POLICY_mlfq := 4
SCHED_POLICY_cfs := 5
SCHED_POLICY_ID := $(SCHED_POLICY_$(SCHED_POLICY))
ifeq ($(strip $(SCHED_POLICY_ID)),)
$(error unsupported SCHED_POLICY='$(SCHED_POLICY)'; use rr, fifo, sjf, stcf, mlfq, or cfs)
endif
CFLAGS += -DSCHED_POLICY=$(SCHED_POLICY_ID)

# 保留 proc.c 的历史入口作为可链接后备，由 sched.c 提供公开调度入口。
$K/proc.o: CFLAGS += -Dprocinit=legacy_procinit -Dscheduler=legacy_scheduler -Dyield=legacy_yield
$K/trap.o: CFLAGS += -Dyield=sched_timer_yield

"""
    anchor = "CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)\n\n"
    content = replace_once(content, anchor, anchor + policy_block, "Makefile policy")
    content = replace_once(
        content,
        "\t$U/_xv6test\n",
        "\t$U/_xv6test\\\n\t$U/_schedtest\n",
        "Makefile UPROGS",
    )
    content = replace_once(
        content,
        "\t\tmkfs/mkfs .gdbinit $U/usys.S $(UPROGS) $(UEXTRA) ph barrier\n",
        "\t\tmkfs/mkfs .gdbinit $U/usys.S $(UPROGS) $(UEXTRA) ph barrier rbtree_test\n",
        "Makefile clean",
    )
    rbtree_target = """test-rbtree:
\tgcc -Wall -Werror -I. -o rbtree_test $H/rbtree_test.c kernel/rbtree.c
\t./rbtree_test

"""
    review_anchor = "# 默认开发入口：先自测 Python runner，再由同一 Python 入口启动 QEMU\n"
    content = replace_once(
        content, review_anchor, rbtree_target + review_anchor, "Makefile test-rbtree"
    )
    content = replace_once(
        content, "test-unit:\n", "test-unit: test-rbtree\n", "Makefile test-unit"
    )
    content = replace_once(
        content,
        ".PHONY: clean qemu qemu-gdb gdb ph barrier test test-unit test-grader \\\n",
        ".PHONY: clean qemu qemu-gdb gdb ph barrier test-rbtree test test-unit test-grader \\\n",
        "Makefile PHONY",
    )
    write("Makefile", content)


def move_scheduler_tests() -> None:
    """按主线目录约定迁移 guest 与 host 测试源文件。"""
    write("tests/guest/schedtest.c", git_file(ORIGINAL_HEAD, "user/schedtest.c"))
    write(
        "tests/host/rbtree_test.c",
        git_file(ORIGINAL_HEAD, "notxv6/rbtree_test.c"),
    )
    Path("user/schedtest.c").unlink(missing_ok=True)
    Path("notxv6/rbtree_test.c").unlink(missing_ok=True)


def rebuild_syscall_registry() -> None:
    """保留主线系统调用，并把调度控制接口分配到 32 至 34。"""
    syscall_h = main_file("kernel/syscall.h")
    if not syscall_h.endswith("\n"):
        syscall_h += "\n"
    syscall_h += (
        "#define SYS_sched_set_hint   32\n"
        "#define SYS_sched_set_weight 33\n"
        "#define SYS_sched_get_stats  34\n"
    )
    write("kernel/syscall.h", syscall_h)

    syscall_c = main_file("kernel/syscall.c")
    syscall_c = replace_once(
        syscall_c,
        "extern uint64 sys_vaquery(void);\n",
        "extern uint64 sys_vaquery(void);\n"
        "extern uint64 sys_sched_set_hint(void);\n"
        "extern uint64 sys_sched_set_weight(void);\n"
        "extern uint64 sys_sched_get_stats(void);\n",
        "kernel/syscall.c externs",
    )
    syscall_c = replace_once(
        syscall_c,
        "[SYS_vaquery] sys_vaquery,\n",
        "[SYS_vaquery] sys_vaquery,\n"
        "[SYS_sched_set_hint]   sys_sched_set_hint,\n"
        "[SYS_sched_set_weight] sys_sched_set_weight,\n"
        "[SYS_sched_get_stats]  sys_sched_get_stats,\n",
        "kernel/syscall.c table",
    )
    syscall_c = replace_once(
        syscall_c, "(1U << num)", "(1ULL << num)", "kernel/syscall.c trace mask"
    )
    write("kernel/syscall.c", syscall_c)

    names = main_file("kernel/syscall_names.h")
    names = replace_once(
        names,
        '[SYS_memsnapshot] = "memsnapshot",\n',
        '[SYS_memsnapshot] = "memsnapshot",\n'
        '[SYS_vaquery]    = "vaquery",\n'
        '[SYS_sched_set_hint]   = "sched_set_hint",\n'
        '[SYS_sched_set_weight] = "sched_set_weight",\n'
        '[SYS_sched_get_stats]  = "sched_get_stats",\n',
        "kernel/syscall_names.h",
    )
    write("kernel/syscall_names.h", names)


def rebuild_proc_header() -> None:
    """保留主线进程结构，只嵌入调度实体并扩展 trace 掩码。"""
    content = '#include "rbtree.h"\n\nstruct proc;\n\n' + main_file("kernel/proc.h")
    sched_entity = """// 调度实体内嵌于 proc，调度器和中断路径无需动态分配队列节点。
struct sched_entity {
  struct proc *prev;
  struct proc *next;
  struct rb_node rb;
  int on_rq;
  int reserved;
  int generation;
  int weight;
  int mlfq_level;
  int force_preempt;
  uint64 mlfq_used;
  uint64 mlfq_epoch;
  uint64 enqueue_seq;
  uint64 runtime_ticks;
  uint64 dispatches;
  uint64 burst_hint;
  uint64 remaining_hint;
  uint64 vruntime;
  uint64 slice_ticks;
};

"""
    content = replace_once(
        content,
        "// Per-process state\nstruct proc {\n",
        sched_entity + "// Per-process state\nstruct proc {\n",
        "kernel/proc.h sched entity",
    )
    content = replace_once(
        content,
        "  int pid;                     // Process ID\n",
        "  int pid;                     // Process ID\n"
        "  struct sched_entity sched;   // Policy-owned scheduling state.\n",
        "kernel/proc.h sched field",
    )
    content = replace_once(
        content, "  int mask;\n", "  uint64 mask;\n", "kernel/proc.h trace mask"
    )
    write("kernel/proc.h", content)


def rebuild_sysproc() -> None:
    """以主线 sysproc.c 为基础追加调度控制接口和 64 位 trace。"""
    content = main_file("kernel/sysproc.c")
    content = replace_once(
        content,
        '#include "sysinfo.h"\n',
        '#include "sysinfo.h"\n#include "sched.h"\n#include "schedstat.h"\n',
        "kernel/sysproc.c includes",
    )
    content = replace_once(
        content,
        "uint64\nsys_trace(void)\n{\n  int mask;\n  if(argint(0, &mask) < 0)\n",
        "uint64\nsys_trace(void)\n{\n  uint64 mask;\n  if(argaddr(0, &mask) < 0)\n",
        "kernel/sysproc.c trace",
    )
    scheduler_syscalls = """/**
 * 设置当前进程供 SJF/STCF 教学策略使用的 CPU burst hint。
 *
 * @return 参数合法时返回 0，否则返回 -1。
 */
uint64
sys_sched_set_hint(void)
{
  int ticks_hint;
  if(argint(0, &ticks_hint) < 0)
    return -1;
  return sched_set_hint(ticks_hint);
}

/**
 * 设置当前进程供 Minimal CFS 使用的整数权重。
 *
 * @return 参数合法时返回 0，否则返回 -1。
 */
uint64
sys_sched_set_weight(void)
{
  int weight;
  if(argint(0, &weight) < 0)
    return -1;
  return sched_set_weight(weight);
}

/**
 * 将当前进程的调度统计复制到用户地址。
 *
 * @return 复制成功时返回 0，地址或状态无效时返回 -1。
 */
uint64
sys_sched_get_stats(void)
{
  uint64 addr;
  struct sched_stats stats;
  struct proc *p = myproc();

  if(argaddr(0, &addr) < 0)
    return -1;
  if(sched_get_stats(&stats) < 0)
    return -1;
  if(copyout(p->pagetable, addr, (char *)&stats, sizeof(stats)) < 0)
    return -1;
  return 0;
}

"""
    content = replace_once(
        content,
        "/**\n * 显式打印当前系统调用路径",
        scheduler_syscalls + "/**\n * 显式打印当前系统调用路径",
        "kernel/sysproc.c scheduler syscalls",
    )
    write("kernel/sysproc.c", content)


def rebuild_user_abi() -> None:
    """保留主线用户 ABI，追加调度接口并扩展 trace 参数。"""
    user_h = main_file("user/user.h")
    user_h = replace_once(
        user_h,
        "struct memviz_va_query;\n",
        "struct memviz_va_query;\nstruct sched_stats;\n",
        "user/user.h forward declaration",
    )
    user_h = replace_once(
        user_h, "int trace(int);\n", "int trace(uint64);\n", "user/user.h trace"
    )
    user_h = replace_once(
        user_h,
        "int vaquery(uint64 va, struct memviz_va_query *query);\n",
        "int vaquery(uint64 va, struct memviz_va_query *query);\n"
        "int sched_set_hint(int ticks);\n"
        "int sched_set_weight(int weight);\n"
        "int sched_get_stats(struct sched_stats *stats);\n",
        "user/user.h scheduler API",
    )
    write("user/user.h", user_h)

    usys = main_file("user/usys.pl")
    usys = replace_once(
        usys,
        'entry("vaquery");\n',
        'entry("vaquery");\n'
        'entry("sched_set_hint");\n'
        'entry("sched_set_weight");\n'
        'entry("sched_get_stats");\n',
        "user/usys.pl",
    )
    write("user/usys.pl", usys)


def widen_trace_parser() -> None:
    """让 trace 名称解析和断言支持 32 号以上系统调用。"""
    header = main_file("user/tracemask.h")
    header = replace_once(
        header,
        "#define XV6_USER_TRACEMASK_H\n",
        '#define XV6_USER_TRACEMASK_H\n\n#include "kernel/types.h"\n',
        "user/tracemask.h include",
    )
    header = replace_once(
        header,
        "int trace_parse_mask(const char *spec, int *mask);",
        "int trace_parse_mask(const char *spec, uint64 *mask);",
        "user/tracemask.h signature",
    )
    write("user/tracemask.h", header)

    parser = main_file("user/tracemask.c")
    parser = replace_once(
        parser,
        "parse_decimal(const char *spec, int *mask)",
        "parse_decimal(const char *spec, uint64 *mask)",
        "user/tracemask.c parse_decimal",
    )
    parser = replace_once(
        parser,
        "trace_parse_mask(const char *spec, int *mask)",
        "trace_parse_mask(const char *spec, uint64 *mask)",
        "user/tracemask.c trace_parse_mask",
    )
    parser = replace_once(
        parser,
        "unsigned int result = 0;",
        "uint64 result = 0;",
        "user/tracemask.c result",
    )
    parser = parser.replace("1U << syscall_number", "1ULL << syscall_number")
    write("user/tracemask.c", parser)

    trace = main_file("user/trace.c")
    trace = replace_once(
        trace,
        "  int mask;           // 传给现有 trace 系统调用的整数位掩码。\n",
        "  uint64 mask;        // 传给 trace 系统调用的 64 位位掩码。\n",
        "user/trace.c mask",
    )
    write("user/trace.c", trace)

    test = main_file("tests/guest/tracemasktest.c")
    test = test.replace("int expected_mask;", "uint64 expected_mask;")
    test = test.replace("1U <<", "1ULL <<")
    test = test.replace("    int mask = -1;", "    uint64 mask = ~(uint64)0;")
    test = test.replace("    int mask = 0;", "    uint64 mask = 0;")
    test = test.replace(
        "mask != (int)(1ULL << syscall_number)",
        "mask != (1ULL << syscall_number)",
    )
    test = test.replace("mask %d", "mask %p")
    write("tests/guest/tracemasktest.c", test)


def update_scheduler_workflow() -> None:
    """让专用 CI 在 Draft 转 Ready 时也会运行。"""
    path = Path(".github/workflows/scheduler-ci.yml")
    content = path.read_text(encoding="utf-8")
    content = replace_once(
        content,
        "types: [opened, synchronize, reopened]",
        "types: [opened, synchronize, reopened, ready_for_review]",
        "scheduler-ci trigger",
    )
    path.write_text(content, encoding="utf-8")


def main() -> None:
    """依次重建主线集成点；任一步失败都终止，不提交半成品。"""
    rebuild_makefile()
    move_scheduler_tests()
    rebuild_syscall_registry()
    rebuild_proc_header()
    rebuild_sysproc()
    rebuild_user_abi()
    widen_trace_parser()
    update_scheduler_workflow()


if __name__ == "__main__":
    main()
