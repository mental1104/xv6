#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/fcntl.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/paths.h"

static struct memviz_snapshot snapshot;
static struct memviz_va_query query;
static struct memviz_va_query paging_lazy_query;
static struct memviz_va_query paging_inside_query;
static struct memviz_va_query paging_left_query;
static struct memviz_va_query paging_right_query;
static struct memviz_va_query paging_unmapped_query;
static struct memviz_va_query paging_cleanup_query;

/** 一个地址空间快照中用户页表页的精确槽位统计。 */
struct paging_table_stats {
  uint64 pages;
  uint64 bytes;
  uint64 used_entries;
  uint64 total_entries;
};

/**
 * fail 输出稳定失败原因并终止当前测试进程。
 *
 * @param message 便于 CI 定位的短文本。
 */
static void
fail(char *message)
{
  printf("addresswindowtest: FAIL: %s\n", message);
  exit(1);
}

/**
 * free_pages 返回当前 kalloc 可立即分配页数。
 *
 * @return 所有 CPU freelist 中的空闲物理页总数。
 */
static uint64
free_pages(void)
{
  if(memsnapshot(MEMVIZ_VIEW_PHYS, &snapshot) < 0)
    fail("memsnapshot failed");
  return snapshot.free_pages;
}

/**
 * require_present 断言用户页表已经为指定数值 VA 建立叶子映射。
 *
 * @param va 已被用户态 load/store 触碰的地址。
 */
static void
require_present(uint64 va)
{
  if(vaquery(va, &query) < 0 || !query.present)
    fail("expected user mapping missing");
}

/**
 * collect_user_pagetable_stats 汇总快照中实际分配的用户页表页。
 *
 * @param state 由 MEMVIZ_VIEW_PAGETABLE 取得的只读快照。
 * @param stats 接收页表页数、字节数和有效槽位数。
 *
 * memviz 先递归扫描用户页表，再扫描进程内核页表。这里仅累计 USER 空间，
 * 因而页表空间开销来自真实页表页和真实 PTE 槽位，而不是按理论层数估算。
 */
static void
collect_user_pagetable_stats(struct memviz_snapshot *state,
                             struct paging_table_stats *stats)
{
  memset(stats, 0, sizeof(*stats));
  for(int i = 0; i < (int)state->pagetable_usage_count; i++){
    struct memviz_pt_usage_page *page = &state->pagetable_usage[i];
    if(page->space != MEMVIZ_PTE_SPACE_USER)
      continue;
    stats->pages++;
    stats->used_entries += page->used_entries;
    stats->total_entries += page->total_entries;
  }
  stats->bytes = stats->pages * PGSIZE;
}

/**
 * print_paging_flags 输出地址转换实验关注的叶子 PTE 权限位。
 *
 * @param flags PTE_FLAGS() 返回的低十位标志。
 */
static void
print_paging_flags(uint64 flags)
{
  printf("%c%c%c%c%c%c",
         (flags & PTE_V) ? 'V' : '-',
         (flags & PTE_R) ? 'R' : '-',
         (flags & PTE_W) ? 'W' : '-',
         (flags & PTE_X) ? 'X' : '-',
         (flags & PTE_U) ? 'U' : '-',
         (flags & PTE_COW) ? 'C' : '-');
}

/**
 * print_paging_translation 展示一个具体 VA 的 VPN、页内偏移、PTE、PFN 与字节 PA。
 *
 * @param label 稳定的样本名称。
 * @param va 用户实际访问的字节虚拟地址。
 * @param result vaquery() 返回的页表链路；PA 是页对齐的物理页基址。
 */
static void
print_paging_translation(char *label, uint64 va,
                          struct memviz_va_query *result)
{
  uint64 offset = va & (PGSIZE - 1);
  printf("PAGING address label=%s va=%p vpn=%p offset=%d mapped=%d",
         label, va, va >> PGSHIFT, (int)offset, result->present);
  if(result->present){
    printf(" pte=%p flags=", result->pte);
    print_paging_flags(result->flags);
    printf(" page_pa=%p pfn=%p byte_pa=%p",
           result->pa, result->pa >> PGSHIFT, result->pa + offset);
  }
  printf("\n");
}

/**
 * test_paging_model 将分页基础概念实体化为一次可重复的真实地址转换实验。
 *
 * 实验先把 break 对齐到页边界，再申请 PGSIZE+1 字节。该范围必然跨越两页，
 * 因而能够同时观察页边界、两个独立 PFN 和 PGSIZE-1 字节内部碎片。sbrk 只
 * 扩大逻辑范围，首次写入才触发 lazy allocation；未触页样本与范围外样本分别
 * 证明“无叶子 PTE”既可能表示合法 lazy VA，也可能表示真正未映射 VA。
 *
 * 结束前恢复原 break 并验证两张叶子映射已移除。空的中间页表页允许保留到
 * 进程退出，这是当前 xv6 freewalk 生命周期的教学边界。
 */
static void
test_paging_model(void)
{
  struct paging_table_stats before_stats;
  struct paging_table_stats after_stats;
  uint64 oldbrk = (uint64)sbrk(0);
  uint64 base = PGROUNDUP(oldbrk);
  uint64 padding = base - oldbrk;
  uint64 requested_bytes = PGSIZE + 1;
  uint64 rounded_bytes = PGROUNDUP(requested_bytes);
  uint64 reserve_bytes64 = padding + requested_bytes;
  uint64 free_before;

  if(PGSIZE != 4096 || PGSHIFT != 12 || sizeof(pte_t) != sizeof(uint64))
    fail("unexpected paging geometry");
  if(reserve_bytes64 > 0x7fffffffULL)
    fail("paging reserve exceeds sbrk ABI");

  if(memsnapshot(MEMVIZ_VIEW_PAGETABLE, &snapshot) < 0)
    fail("paging baseline snapshot");
  collect_user_pagetable_stats(&snapshot, &before_stats);
  free_before = snapshot.free_pages;

  int reserve_bytes = (int)reserve_bytes64;
  if((uint64)sbrk(reserve_bytes) != oldbrk)
    fail("paging sbrk reserve");

  if(vaquery(base, &paging_lazy_query) < 0 || paging_lazy_query.present)
    fail("untouched lazy page already mapped");

  uint64 inside_va = base + 0xabc;
  uint64 left_va = base + PGSIZE - 1;
  uint64 right_va = base + PGSIZE;
  uint64 unmapped_va = base + rounded_bytes;
  volatile char *inside = (volatile char *)inside_va;
  volatile char *left = (volatile char *)left_va;
  volatile char *right = (volatile char *)right_va;

  *inside = 0x5a;
  *left = 0x6b;
  *right = 0x7c;

  if(*inside != 0x5a || *left != 0x6b || *right != 0x7c)
    fail("cross-page readback");
  if(vaquery(inside_va, &paging_inside_query) < 0 ||
     vaquery(left_va, &paging_left_query) < 0 ||
     vaquery(right_va, &paging_right_query) < 0 ||
     vaquery(unmapped_va, &paging_unmapped_query) < 0)
    fail("paging vaquery syscall");

  if(!paging_inside_query.present || !paging_left_query.present ||
     !paging_right_query.present)
    fail("touched paging sample missing");
  if(paging_unmapped_query.present)
    fail("outside paging sample mapped");
  if(paging_inside_query.va != PGROUNDDOWN(inside_va) ||
     paging_left_query.va != PGROUNDDOWN(left_va) ||
     paging_right_query.va != PGROUNDDOWN(right_va))
    fail("vaquery page alignment");
  if((paging_inside_query.pa % PGSIZE) != 0 ||
     (paging_left_query.pa % PGSIZE) != 0 ||
     (paging_right_query.pa % PGSIZE) != 0)
    fail("physical frame alignment");
  if(paging_inside_query.pa != paging_left_query.pa)
    fail("same virtual page changed PFN");
  if(paging_left_query.pa == paging_right_query.pa)
    fail("adjacent virtual pages share frame");
  if((left_va >> PGSHIFT) + 1 != (right_va >> PGSHIFT) ||
     (left_va & (PGSIZE - 1)) != PGSIZE - 1 ||
     (right_va & (PGSIZE - 1)) != 0)
    fail("page boundary decomposition");
  if(((paging_inside_query.pa + (inside_va & (PGSIZE - 1))) &
      (PGSIZE - 1)) != (inside_va & (PGSIZE - 1)) ||
     ((paging_left_query.pa + (left_va & (PGSIZE - 1))) &
      (PGSIZE - 1)) != (left_va & (PGSIZE - 1)) ||
     ((paging_right_query.pa + (right_va & (PGSIZE - 1))) &
      (PGSIZE - 1)) != (right_va & (PGSIZE - 1)))
    fail("page offset not preserved");
  if((paging_inside_query.flags & (PTE_V | PTE_R | PTE_W | PTE_U)) !=
     (PTE_V | PTE_R | PTE_W | PTE_U) ||
     (paging_right_query.flags & (PTE_V | PTE_R | PTE_W | PTE_U)) !=
     (PTE_V | PTE_R | PTE_W | PTE_U))
    fail("paging leaf permissions");

  if(memsnapshot(MEMVIZ_VIEW_PAGETABLE, &snapshot) < 0)
    fail("paging materialized snapshot");
  collect_user_pagetable_stats(&snapshot, &after_stats);
  if(after_stats.pages == 0 || after_stats.pages < before_stats.pages)
    fail("user pagetable page accounting");
  if(after_stats.total_entries != after_stats.pages * (PGSIZE / sizeof(pte_t)))
    fail("user pagetable slot capacity");
  if(after_stats.used_entries < before_stats.used_entries + 2)
    fail("two leaf PTEs not reflected in usage");
  if(rounded_bytes != 2 * PGSIZE ||
     rounded_bytes - requested_bytes != PGSIZE - 1)
    fail("internal fragmentation arithmetic");

  printf("PAGING geometry page_size=%d offset_bits=%d pte_bytes=%d pte_slots=%d\n",
         PGSIZE, PGSHIFT, (int)sizeof(pte_t),
         (int)(PGSIZE / sizeof(pte_t)));
  printf("PAGING request old_break=%p base=%p padding=%d requested=%d rounded=%d frames=2 internal_fragment=%d\n",
         oldbrk, base, (int)padding, (int)requested_bytes,
         (int)rounded_bytes, (int)(rounded_bytes - requested_bytes));
  print_paging_translation("lazy-before-touch", base, &paging_lazy_query);
  print_paging_translation("inside", inside_va, &paging_inside_query);
  print_paging_translation("boundary-left", left_va, &paging_left_query);
  print_paging_translation("boundary-right", right_va, &paging_right_query);
  print_paging_translation("outside-extent", unmapped_va,
                           &paging_unmapped_query);
  printf("PAGING cross left_value=%d right_value=%d vpn_step=1 distinct_frames=1\n",
         (int)*left, (int)*right);
  printf("PAGING pagetable before_pages=%d before_bytes=%d before_used=%d before_slots=%d after_pages=%d after_bytes=%d after_used=%d after_slots=%d delta_pages=%d delta_used=%d\n",
         (int)before_stats.pages, (int)before_stats.bytes,
         (int)before_stats.used_entries, (int)before_stats.total_entries,
         (int)after_stats.pages, (int)after_stats.bytes,
         (int)after_stats.used_entries, (int)after_stats.total_entries,
         (int)(after_stats.pages - before_stats.pages),
         (int)(after_stats.used_entries - before_stats.used_entries));
  int allocator_pages_delta = free_before >= snapshot.free_pages ?
                              (int)(free_before - snapshot.free_pages) : 0;
  printf("PAGING frames data_frames=2 data_bytes=%d first_pfn=%p second_pfn=%p allocator_pages_delta=%d\n",
         2 * PGSIZE, paging_left_query.pa >> PGSHIFT,
         paging_right_query.pa >> PGSHIFT, allocator_pages_delta);

  uint64 expected_current_break = base + requested_bytes;
  if((uint64)sbrk(-reserve_bytes) != expected_current_break)
    fail("paging sbrk release");
  if((uint64)sbrk(0) != oldbrk)
    fail("paging break not restored");
  if(vaquery(base, &paging_cleanup_query) < 0 || paging_cleanup_query.present)
    fail("first paging mapping survived cleanup");
  if(vaquery(right_va, &paging_cleanup_query) < 0 || paging_cleanup_query.present)
    fail("second paging mapping survived cleanup");

  printf("PAGING cleanup restored_break=1 mappings_removed=1\n");
  printf("PAGING RESULT PASS\n");
}

/**
 * grow_to 使用多个 int 范围内的 sbrk 调用推进 p->sz。
 *
 * @param target 目标 break，必须小于等于 USERMAX。
 *
 * 单次 sbrk 仍保留 xv6 的 32 位 ABI；循环推进用于验证跨越 2 GiB direct-map
 * 数值时，用户页仍通过 Sv39 高半区 alias 被内核访问。
 */
static void
grow_to(uint64 target)
{
  uint64 current = (uint64)sbrk(0);
  if(target > USERMAX || current > target)
    fail("invalid grow target");

  while(current < target){
    uint64 remaining = target - current;
    int step = remaining > 0x7ffff000ULL ? 0x7ffff000 : (int)remaining;
    if((uint64)sbrk(step) != current)
      fail("sbrk chunk failed");
    current += (uint64)step;
  }
}

/**
 * test_copy_paths 验证 copyin、copyout 和 copyinstr 能访问跨 MMIO 数值的用户 VA。
 */
static void
test_copy_paths(void)
{
  int fds[2];
  char received = 0;
  char *pipe_source = (char *)PLIC;
  *pipe_source = 'Q';

  if(pipe(fds) < 0)
    fail("pipe failed");
  if(write(fds[1], pipe_source, 1) != 1)
    fail("copyin above PLIC failed");
  if(read(fds[0], &received, 1) != 1 || received != 'Q')
    fail("pipe payload mismatch");
  close(fds[0]);
  close(fds[1]);

  char *path = (char *)UART0;
  strcpy(path, "/README");
  int fd = open(path, O_RDONLY);
  if(fd < 0)
    fail("copyinstr above UART failed");

  char *copyout_target = (char *)(PLIC + PGSIZE);
  if(read(fd, copyout_target, 4) != 4)
    fail("copyout above PLIC failed");
  close(fd);
}

/**
 * test_cow 验证高地址用户页在 fork 后仍遵守 COW 隔离。
 */
static void
test_cow(void)
{
  volatile char *value = (volatile char *)CLINT;
  *value = 'A';

  int pid = fork();
  if(pid < 0)
    fail("cow fork failed");
  if(pid == 0){
    *value = 'B';
    if(*value != 'B')
      exit(2);
    exit(0);
  }

  int status = -1;
  wait(&status);
  if(status != 0 || *value != 'A')
    fail("high VA COW isolation");
}

/**
 * test_mmap 验证 mmap 可以放置在 UART 数值地址之后并由缺页路径建立别名。
 */
static void
test_mmap(void)
{
  int fd = open("/README", O_RDONLY);
  if(fd < 0)
    fail("mmap source open failed");

  char *mapped = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if(mapped == (char *)-1)
    fail("mmap above UART failed");
  if((uint64)mapped <= VIRTIO0 || (uint64)mapped >= USERMAX)
    fail("mmap address outside expected range");

  volatile char first = mapped[0];
  (void)first;
  require_present((uint64)mapped);

  if(munmap(mapped, PGSIZE) < 0)
    fail("munmap above UART failed");
  close(fd);
}

/**
 * test_exec 验证 exec 的 path、argv 数组和参数字符串都可以来自高地址用户页。
 */
static void
test_exec(void)
{
  int pid = fork();
  if(pid < 0)
    fail("exec fork failed");
  if(pid == 0){
    char *path = (char *)(UART0 + 128);
    char *argument = (char *)(UART0 + 192);
    char **arguments = (char **)(PLIC + 2 * PGSIZE);

    strcpy(path, XV6_BIN_PATH("echo"));
    strcpy(argument, "alias-exec");
    arguments[0] = path;
    arguments[1] = argument;
    arguments[2] = 0;
    exec(path, arguments);
    exit(3);
  }

  int status = -1;
  wait(&status);
  if(status != 0)
    fail("exec with high VA arguments failed");
}

/**
 * test_direct_map_crossing 验证用户 VA 可以越过 KERNBASE 数值。
 *
 * 该页在用户页表中位于 2 GiB 附近，但进程内核页表的同一低地址仍是物理
 * direct map；copyin/copyout 必须通过高规范半区 alias 才能访问用户物理页。
 */
static void
test_direct_map_crossing(void)
{
  grow_to(KERNBASE + 3 * PGSIZE);

  char *source = (char *)KERNBASE;
  *source = 'D';
  require_present(KERNBASE);

  int fds[2];
  char received = 0;
  if(pipe(fds) < 0)
    fail("direct-map pipe failed");
  if(write(fds[1], source, 1) != 1)
    fail("copyin above KERNBASE failed");
  if(read(fds[0], &received, 1) != 1 || received != 'D')
    fail("direct-map pipe payload mismatch");
  close(fds[0]);
  close(fds[1]);

  int fd = open("/README", O_RDONLY);
  if(fd < 0)
    fail("direct-map copyout source failed");
  char *target = (char *)(KERNBASE + PGSIZE);
  if(read(fd, target, 4) != 4)
    fail("copyout above KERNBASE failed");
  close(fd);
  require_present(KERNBASE + PGSIZE);
}

/**
 * run_worker 在一个短生命周期进程中完成分页与地址窗口功能验证。
 */
static void
run_worker(void)
{
  test_paging_model();

  uint64 oldbrk = (uint64)sbrk(0);
  uint64 target = VIRTIO0 + 3 * PGSIZE;
  if(oldbrk >= target)
    fail("unexpected initial break");
  grow_to(target);

  uint64 boundaries[] = {CLINT, PLIC, UART0, VIRTIO0};
  int boundary_count = sizeof(boundaries) / sizeof(boundaries[0]);
  for(int i = 0; i < boundary_count; i++){
    volatile char *address = (volatile char *)boundaries[i];
    *address = (char)(0x30 + i);
    if(*address != (char)(0x30 + i))
      fail("numeric MMIO user access mismatch");
    require_present(boundaries[i]);
  }

  test_copy_paths();
  test_cow();
  test_mmap();
  test_exec();
  test_direct_map_crossing();

  if(memsnapshot(MEMVIZ_VIEW_KERNEL, &snapshot) < 0)
    fail("kernel snapshot failed");
  if(snapshot.user_limit != TRAPFRAME || snapshot.user_limit != USERMAX ||
     snapshot.user_mirror_start != KUSERBASE ||
     snapshot.user_mirror_end != KUSERADDR(snapshot.process_size) ||
     KUSERBASE != 0xffffffc000000000ULL || KUSEREND != KUSERADDR(USERMAX))
    fail("Sv39 alias layout mismatch");

  if(vaquery(USERMAX, &query) != -1)
    fail("USERMAX boundary accepted");

  printf("addresswindowtest: worker OK\n");
}

/**
 * main 运行分页实验或完整地址窗口回归。
 *
 * @param argc 不带参数时运行完整回归；传入 paging 时只运行分页闭环。
 * @param argv 命令行参数数组。
 * @return 成功路径通过 exit(0) 结束；断言失败由 fail 以非零状态终止。
 */
int
main(int argc, char **argv)
{
  if(argc == 2 && strcmp(argv[1], "paging") == 0){
    test_paging_model();
    printf("addresswindowtest: paging OK\n");
    exit(0);
  }
  if(argc != 1){
    fprintf(2, "usage: addresswindowtest [paging]\n");
    exit(2);
  }

  uint64 before = free_pages();
  int pid = fork();
  if(pid < 0)
    fail("worker fork failed");
  if(pid == 0){
    run_worker();
    exit(0);
  }

  int status = -1;
  wait(&status);
  if(status != 0)
    fail("worker failed");
  if(free_pages() != before)
    fail("worker leaked pages");

  printf("addresswindowtest: OK\n");
  exit(0);
}
