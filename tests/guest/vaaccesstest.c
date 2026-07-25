#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#include "kernel/memviz.h"
#include "user/user.h"
#include "user/paths.h"
#include "tests/guest/testlib.h"

#define TLB_SWITCH_ROUNDS 64

static char output[4096];

/** 记录 COW 后 child 在同一 VA 上观察到的翻译与权限。 */
struct tlb_switch_report {
  uint64 va;
  uint64 pa;
  uint64 flags;
  int query_ok;
};

/**
 * fail 输出稳定失败原因并以非零状态终止测试。
 *
 * @param message 失败原因，必须是便于定位的短文本。
 */
static void
fail(char *message)
{
  printf("vaaccesstest: FAIL: %s\n", message);
  exit(1);
}

/**
 * require_contains 断言捕获输出中包含指定协议片段。
 *
 * @param text 被检查输出。
 * @param needle 必须出现的连续子串。
 */
static void
require_contains(char *text, char *needle)
{
  if(!xv6_test_contains(text, needle)){
    printf("vaaccesstest: missing output: %s\n", needle);
    fail("missing output");
  }
}

/**
 * run_command 执行用户命令并校验退出状态。
 *
 * @param argv exec 参数数组，argv[0] 是绝对程序路径。
 * @param expected_status 期望 wait status。
 * @return 捕获输出缓冲区；下次调用会覆盖。
 */
static char *
run_command(char **argv, int expected_status)
{
  int status = 0;
  if(xv6_test_run_capture(argv, 0, output, sizeof(output), &status) < 0)
    fail("capture failed");
  if(status != expected_status){
    printf("vaaccesstest: command=%s status=%d expected=%d\n",
           argv[0], status, expected_status);
    printf("%s\n", output);
    fail("unexpected status");
  }
  return output;
}

/**
 * free_pages 读取当前进程可观察的 kalloc 空闲页数量。
 *
 * @return 成功时返回 free_pages；系统调用失败时终止测试。
 */
static uint64
free_pages(void)
{
  static struct memviz_snapshot snapshot;
  if(memsnapshot(MEMVIZ_VIEW_USER, &snapshot) < 0)
    fail("memsnapshot failed");
  return snapshot.free_pages;
}

/** test_mapped_access 验证已映射 ELF 页的读写都走普通命中路径。 */
static void
test_mapped_access(void)
{
  char *read_args[] = {
    XV6_USR_BIN_PATH("varead"), "image+0", "--expect", "ok", "--snapshot", 0,
  };
  char *write_args[] = {
    XV6_USR_BIN_PATH("vawrite"), "image+0", "0x41", "--expect", "ok", 0,
  };

  char *text = run_command(read_args, 0);
  require_contains(text, "VAACCESS result=ok");
  require_contains(text, "VAACCESS BEFORE");
  require_contains(text, "VAACCESS AFTER");

  text = run_command(write_args, 0);
  require_contains(text, "VAACCESS readback=0x41");
  require_contains(text, "VAACCESS result=ok");

  printf("vaaccesstest: mapped access OK\n");
}

/** test_lazy_access 验证 lazy reserve 与首次触页分离，且命令退出后归还页面。 */
static void
test_lazy_access(void)
{
  char *read_args[] = {
    XV6_USR_BIN_PATH("varead"), "--lazy", "0", "--snapshot", 0,
  };
  char *write_args[] = {
    XV6_USR_BIN_PATH("vawrite"), "--lazy", "2", "0x42", "--snapshot", 0,
  };
  uint64 before = free_pages();

  char *text = run_command(read_args, 0);
  require_contains(text, "VAACCESS mode=lazy");
  require_contains(text, "VAACCESS before mapped=0");
  require_contains(text, "VAACCESS value=0x00");
  require_contains(text, "VAACCESS after mapped=1");

  text = run_command(write_args, 0);
  require_contains(text, "VAACCESS reserved_pages=3");
  require_contains(text, "VAACCESS before mapped=0");
  require_contains(text, "VAACCESS readback=0x42");
  require_contains(text, "VAACCESS after mapped=1");

  if(free_pages() != before)
    fail("lazy command leaked pages");

  printf("vaaccesstest: lazy access OK\n");
}

/** test_cow_access 验证 COW 写前共享 PA、写后 child 私有 PA、父子内容隔离。 */
static void
test_cow_access(void)
{
  char *args[] = {
    XV6_USR_BIN_PATH("vawrite"), "--cow", "0x43", "--snapshot", 0,
  };
  uint64 before = free_pages();
  char *text = run_command(args, 0);

  require_contains(text, "VAACCESS mode=cow");
  require_contains(text, "VAACCESS child before_pa=");
  require_contains(text, "VAACCESS readback=0x43");
  require_contains(text, "VAACCESS parent_value=0x31");
  require_contains(text, "VAACCESS result=ok");
  if(free_pages() != before)
    fail("cow command leaked pages");

  printf("vaaccesstest: cow access OK\n");
}

/**
 * test_tlb_switch_isolation 验证同一 VA 在父子地址空间之间反复切换时不会复用旧翻译。
 *
 * child 首次写入会触发 COW，使父子保留相同 VA 但映射到不同 PA，并同时改变写权限。
 * 两个进程随后通过 pipe 交替阻塞和唤醒；在 CPUS=1 下，每轮都要求同一 hart 在两个
 * 地址空间之间切换。断言只观察页表状态与读回值，不把运行时间解释为 TLB 事件。
 */
static void
test_tlb_switch_isolation(void)
{
  int parent_to_child[2];
  int child_to_parent[2];
  int status = 0;
  int ok = 1;
  char token = 'x';
  char *base = sbrk(PGSIZE);
  struct tlb_switch_report report;
  struct memviz_va_query parent_query;

  if(base == (char *)-1)
    fail("tlb page allocation failed");
  volatile unsigned char *page = (volatile unsigned char *)base;
  *page = 0x51;
  memset(&report, 0, sizeof(report));
  memset(&parent_query, 0, sizeof(parent_query));

  if(pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0)
    fail("tlb pipe failed");

  int pid = fork();
  if(pid < 0)
    fail("tlb fork failed");

  if(pid == 0){
    close(parent_to_child[1]);
    close(child_to_parent[0]);

    // 该 store 必须在 COW PTE 更新及 TLB 同步后重试成功。
    *page = 0xa7;
    struct memviz_va_query child_query;
    if(vaquery((uint64)base, &child_query) < 0)
      exit(1);

    report.va = (uint64)base;
    report.pa = child_query.pa;
    report.flags = child_query.flags;
    report.query_ok = child_query.present;
    if(write(child_to_parent[1], &report, sizeof(report)) != sizeof(report))
      exit(1);

    for(int round = 0; round < TLB_SWITCH_ROUNDS; round++){
      if(read(parent_to_child[0], &token, 1) != 1)
        exit(1);
      if(*page != 0xa7)
        exit(1);
      token = (char)(round & 0x7f);
      if(write(child_to_parent[1], &token, 1) != 1)
        exit(1);
    }

    close(parent_to_child[0]);
    close(child_to_parent[1]);
    exit(0);
  }

  close(parent_to_child[0]);
  close(child_to_parent[1]);

  if(read(child_to_parent[0], &report, sizeof(report)) != sizeof(report))
    ok = 0;
  if(ok && vaquery((uint64)base, &parent_query) < 0)
    ok = 0;
  if(ok && (!report.query_ok || !parent_query.present ||
            report.va != (uint64)base || report.pa == parent_query.pa ||
            (report.flags & PTE_W) == 0 || (report.flags & PTE_COW) != 0 ||
            (parent_query.flags & PTE_W) != 0 ||
            (parent_query.flags & PTE_COW) == 0))
    ok = 0;

  for(int round = 0; ok && round < TLB_SWITCH_ROUNDS; round++){
    if(*page != 0x51){
      ok = 0;
      break;
    }
    token = (char)(round & 0x7f);
    if(write(parent_to_child[1], &token, 1) != 1 ||
       read(child_to_parent[0], &token, 1) != 1 ||
       *page != 0x51)
      ok = 0;
  }

  // 先关闭父端 pipe，让任何等待中的 child 能退出，再统一 wait 回收。
  close(parent_to_child[1]);
  close(child_to_parent[0]);
  if(wait(&status) != pid || status != 0)
    ok = 0;
  if(*page != 0x51)
    ok = 0;
  if(sbrk(-PGSIZE) == (char *)-1)
    ok = 0;

  if(!ok)
    fail("tlb context switch isolation");
  printf("vaaccesstest: tlb switch isolation OK rounds=%d\n",
         TLB_SWITCH_ROUNDS);
}

/** test_faults 验证非法访问只杀 worker，supervisor 和后续命令仍可继续。 */
static void
test_faults(void)
{
  char *guard_read[] = {
    XV6_USR_BIN_PATH("varead"), "guard+0", "--expect", "fault", "--snapshot", 0,
  };
  char *guard_write[] = {
    XV6_USR_BIN_PATH("vawrite"), "guard+0", "0x41", "--expect", "fault", 0,
  };
  char *brk_read[] = {
    XV6_USR_BIN_PATH("varead"), "brk+4096", "--expect", "fault", 0,
  };
  char *plic_write[] = {
    XV6_USR_BIN_PATH("vawrite"), "0x000000000C000000", "0x41",
    "--expect", "fault", 0,
  };
  char *alive[] = {XV6_BIN_PATH("echo"), "shell-alive", 0};

  require_contains(run_command(guard_read, 0), "VAACCESS result=fault");
  require_contains(run_command(guard_write, 0), "VAACCESS result=fault");
  require_contains(run_command(brk_read, 0), "VAACCESS result=fault");
  require_contains(run_command(plic_write, 0), "VAACCESS result=fault");
  require_contains(run_command(alive, 0), "shell-alive");

  printf("vaaccesstest: fault isolation OK\n");
}

/** test_parsing_and_expect 验证地址解析、溢出拒绝和 --expect 状态传播。 */
static void
test_parsing_and_expect(void)
{
  char *decimal[] = {XV6_USR_BIN_PATH("varead"), "0", "--expect", "ok", 0};
  char *hex[] = {XV6_USR_BIN_PATH("varead"), "0x0", "--expect", "ok", 0};
  char *bad[] = {XV6_USR_BIN_PATH("varead"), "not-a-va", 0};
  char *overflow[] = {XV6_USR_BIN_PATH("varead"), "0xffffffffffffffff", 0};
  char *mismatch[] = {
    XV6_USR_BIN_PATH("varead"), "image+0", "--expect", "fault", 0,
  };

  require_contains(run_command(decimal, 0), "VAACCESS result=ok");
  require_contains(run_command(hex, 0), "VAACCESS result=ok");
  run_command(bad, 2);
  run_command(overflow, 2);
  run_command(mismatch, 1);

  printf("vaaccesstest: parsing and expect OK\n");
}

/** test_va_zero_follows_real_pagetable 验证 VA 0 不是写死的 NULL page 假设。 */
static void
test_va_zero_follows_real_pagetable(void)
{
  struct memviz_va_query query;
  if(vaquery(0, &query) < 0)
    fail("vaquery zero failed");

  if(query.present){
    char *args[] = {
      XV6_USR_BIN_PATH("varead"), "image+0", "--expect", "ok", 0,
    };
    require_contains(run_command(args, 0), "VAACCESS result=ok");
  } else {
    char *args[] = {
      XV6_USR_BIN_PATH("varead"), "0", "--expect", "fault", 0,
    };
    require_contains(run_command(args, 0), "VAACCESS result=fault");
  }

  printf("vaaccesstest: va0 real pagetable OK\n");
}

/**
 * main 执行用户态 VA 访问探针的 Lab3 回归。
 *
 * @param argc 参数数量；测试不接受额外参数。
 * @param argv 参数数组，仅用于保持 xv6 用户程序签名。
 * @return 通过 exit() 返回；全部断言成功为 0。
 */
int
main(int argc, char **argv)
{
  (void)argv;
  if(argc != 1)
    exit(2);

  test_mapped_access();
  test_lazy_access();
  test_cow_access();
  test_tlb_switch_isolation();
  test_faults();
  test_parsing_and_expect();
  test_va_zero_follows_real_pagetable();

  printf("vaaccesstest: OK\n");
  exit(0);
}
