#ifndef XV6_MEMVIZTEST_POLICY_H
#define XV6_MEMVIZTEST_POLICY_H

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/swap.h"
#include "user/user.h"

#define POLICY_MAX_FRAMES 3
#define POLICY_MAX_PAGES 5
#define POLICY_MAX_ACCESSES 9

#define POLICY_FIFO 0
#define POLICY_CLOCK 1

struct policy_access {
  int page;
  int write;
};

struct policy_frame {
  int page;
  int referenced;
};

struct policy_state {
  int kind;
  int frame_count;
  int resident_count;
  int hand;
  struct policy_frame frames[POLICY_MAX_FRAMES];
};

struct policy_trace {
  int page;
  int write;
  int hit;
  int victim;
  int scanned;
  int hand_before;
  int hand_after;
  uint resident_mask;
  uint referenced_mask;
};

struct policy_result {
  int accesses;
  int hits;
  int misses;
  int evictions;
  int page_outs;
  int page_ins;
  uint final_resident_mask;
  struct policy_trace trace[POLICY_MAX_ACCESSES];
};

static struct policy_result policy_fifo_result;
static struct policy_result policy_clock_result;
static struct policy_result policy_fit_result;
static struct policy_result policy_pressure_result;
static struct policy_result policy_repeat_result;

/** 输出稳定失败原因并以非零状态终止 guest 测试。 */
static void
policy_fail(char *message)
{
  printf("vmpolicytest: FAIL: %s\n", message);
  exit(1);
}

/** 返回稳定策略名称，供逐步 trace 使用。 */
static char *
policy_name(int kind)
{
  return kind == POLICY_FIFO ? "fifo" : "clock";
}

/** 初始化固定页框状态；hand 同时承担 FIFO 队首和 CLOCK 指针。 */
static void
policy_state_init(struct policy_state *state, int kind, int frame_count)
{
  memset(state, 0, sizeof(*state));
  state->kind = kind;
  state->frame_count = frame_count;
  for(int i = 0; i < POLICY_MAX_FRAMES; i++)
    state->frames[i].page = -1;
}

/** 返回逻辑页当前所在页框槽位，未驻留时返回 -1。 */
static int
policy_find_frame(struct policy_state *state, int page)
{
  for(int frame = 0; frame < state->resident_count; frame++)
    if(state->frames[frame].page == page)
      return frame;
  return -1;
}

/** 将驻留页集合压缩成页号位图，便于稳定比较每一步状态。 */
static uint
policy_resident_mask(struct policy_state *state)
{
  uint mask = 0;
  for(int frame = 0; frame < state->resident_count; frame++)
    mask |= 1U << state->frames[frame].page;
  return mask;
}

/** 将 CLOCK 引用状态压缩成页号位图；FIFO 只把它作为观察信息。 */
static uint
policy_referenced_mask(struct policy_state *state)
{
  uint mask = 0;
  for(int frame = 0; frame < state->resident_count; frame++)
    if(state->frames[frame].referenced)
      mask |= 1U << state->frames[frame].page;
  return mask;
}

/**
 * 只推进策略状态，不执行磁盘 I/O。
 *
 * FIFO 命中不改变 hand；CLOCK 命中设置引用位。缺页且页框已满时，函数公开
 * victim、扫描次数与指针变化，调用者随后用 swapout() 执行真实淘汰。
 */
static void
policy_select(struct policy_state *state,
              struct policy_access *access,
              struct policy_trace *trace)
{
  memset(trace, 0, sizeof(*trace));
  trace->page = access->page;
  trace->write = access->write;
  trace->victim = -1;
  trace->hand_before = state->hand;

  int frame = policy_find_frame(state, access->page);
  if(frame >= 0){
    trace->hit = 1;
    state->frames[frame].referenced = 1;
  } else if(state->resident_count < state->frame_count){
    frame = state->resident_count++;
    state->frames[frame].page = access->page;
    state->frames[frame].referenced = 1;
  } else if(state->kind == POLICY_FIFO){
    frame = state->hand;
    trace->victim = state->frames[frame].page;
    state->frames[frame].page = access->page;
    state->frames[frame].referenced = 1;
    state->hand = (state->hand + 1) % state->frame_count;
  } else {
    while(state->frames[state->hand].referenced){
      state->frames[state->hand].referenced = 0;
      state->hand = (state->hand + 1) % state->frame_count;
      trace->scanned++;
    }
    frame = state->hand;
    trace->victim = state->frames[frame].page;
    state->frames[frame].page = access->page;
    state->frames[frame].referenced = 1;
    state->hand = (state->hand + 1) % state->frame_count;
  }

  trace->hand_after = state->hand;
  trace->resident_mask = policy_resident_mask(state);
  trace->referenced_mask = policy_referenced_mask(state);
}

/** 为每个逻辑页生成不等于匿名零页的确定性签名。 */
static unsigned char
policy_initial_byte(int page)
{
  return (unsigned char)((page * 37 + 11) & 0xff);
}

/**
 * 访问真实匿名页，并在换入后验证首尾签名。
 *
 * 首次访问物化 lazy 页；后续访问若目标已换出，第一处读操作会触发内核 page-in。
 * write 标志只描述工作负载，不伪造当前 xv6 尚未提供的硬件 dirty-bit 策略输入。
 */
static void
policy_access_page(char *page,
                   int logical_page,
                   int write_access,
                   int *initialized,
                   unsigned char *expected)
{
  unsigned char tail = (unsigned char)(policy_initial_byte(logical_page) ^ 0x5a);

  if(!initialized[logical_page]){
    expected[logical_page] = policy_initial_byte(logical_page);
    page[0] = (char)expected[logical_page];
    page[PGSIZE - 1] = (char)tail;
    initialized[logical_page] = 1;
  } else if((unsigned char)page[0] != expected[logical_page] ||
            (unsigned char)page[PGSIZE - 1] != tail){
    policy_fail("page data preservation");
  }

  if(write_access){
    expected[logical_page]++;
    page[0] = (char)expected[logical_page];
  }
}

/** 分配一段页对齐的 lazy 匿名区间，并返回恢复 break 所需增长量。 */
static char *
policy_reserve_pages(int page_count, uint64 *old_break, int *growth)
{
  *old_break = (uint64)sbrk(0);
  uint64 base = PGROUNDUP(*old_break);
  uint64 end = base + (uint64)page_count * PGSIZE;
  uint64 growth64 = end - *old_break;

  if(end < base || growth64 > 0x7fffffffULL)
    policy_fail("address range");
  *growth = (int)growth64;
  if((uint64)sbrk(*growth) != *old_break)
    policy_fail("sbrk reserve");
  return (char *)base;
}

/** 恢复实验前 break，并验证缩容释放了全部非驻留 swap slot。 */
static void
policy_release_pages(char *base,
                     uint64 old_break,
                     int growth,
                     uint baseline_used_slots)
{
  uint64 expected_top = old_break + (uint64)growth;
  if((uint64)sbrk(-growth) != expected_top ||
     (uint64)sbrk(0) != old_break)
    policy_fail("sbrk cleanup");

  struct swap_info released;
  if(swapinfo(base, &released) < 0 ||
     released.page_state != SWAP_PAGE_UNMAPPED ||
     released.used_slots != baseline_used_slots)
    policy_fail("swap slot cleanup");
}

/** 打印策略输入、选择结果和真实换入换出计数。 */
static void
policy_print_result(char *label, int kind, int frames, struct policy_result *result)
{
  for(int step = 0; step < result->accesses; step++){
    struct policy_trace *trace = &result->trace[step];
    printf("VMPOLICY label=%s policy=%s step=%d access=%d mode=%s result=%s victim=%d scan=%d hand=%d->%d resident=0x%x referenced=0x%x\n",
           label, policy_name(kind), step, trace->page,
           trace->write ? "write" : "read",
           trace->hit ? "hit" : "miss", trace->victim, trace->scanned,
           trace->hand_before, trace->hand_after, trace->resident_mask,
           trace->referenced_mask);
  }
  printf("VMPOLICY summary label=%s policy=%s frames=%d accesses=%d hits=%d misses=%d evictions=%d page_outs=%d page_ins=%d resident=0x%x\n",
         label, policy_name(kind), frames, result->accesses, result->hits,
         result->misses, result->evictions, result->page_outs,
         result->page_ins, result->final_resident_mask);
}

/**
 * 在真实 swap 机制上执行一组确定性访问。
 *
 * 策略先从用户态维护的显式状态选 victim，随后 swapout() 负责写后备文件、修改
 * PTE、释放物理页；访问已换出目标时由 page fault 完成换入。全局计数与 slot
 * 清理把策略结果和内核机制连接成同一条可验证链。
 */
static void
policy_run(char *label,
           int kind,
           int page_count,
           int frame_count,
           struct policy_access *accesses,
           int access_count,
           struct policy_result *result)
{
  if(label == 0 || (kind != POLICY_FIFO && kind != POLICY_CLOCK) ||
     page_count <= 0 || page_count > POLICY_MAX_PAGES ||
     frame_count <= 0 || frame_count > POLICY_MAX_FRAMES ||
     frame_count > page_count || access_count <= 0 ||
     access_count > POLICY_MAX_ACCESSES)
    policy_fail("invalid workload");

  memset(result, 0, sizeof(*result));
  for(int step = 0; step < POLICY_MAX_ACCESSES; step++)
    result->trace[step].victim = -1;

  uint64 old_break;
  int growth;
  char *base = policy_reserve_pages(page_count, &old_break, &growth);
  struct swap_info baseline;
  if(swapinfo(base, &baseline) < 0){
    sbrk(-growth);
    policy_fail("baseline swapinfo");
  }

  struct policy_state state;
  int initialized[POLICY_MAX_PAGES];
  unsigned char expected[POLICY_MAX_PAGES];
  int expected_page_ins = 0;
  memset(initialized, 0, sizeof(initialized));
  memset(expected, 0, sizeof(expected));
  policy_state_init(&state, kind, frame_count);

  for(int step = 0; step < access_count; step++){
    if(accesses[step].page < 0 || accesses[step].page >= page_count ||
       (accesses[step].write != 0 && accesses[step].write != 1))
      policy_fail("invalid access");

    char *target = base + (uint64)accesses[step].page * PGSIZE;
    struct swap_info target_before;
    if(swapinfo(target, &target_before) < 0)
      policy_fail("target swapinfo");

    struct policy_trace *trace = &result->trace[step];
    policy_select(&state, &accesses[step], trace);
    result->accesses++;
    if(trace->hit)
      result->hits++;
    else
      result->misses++;

    if(trace->hit && target_before.page_state != SWAP_PAGE_RESIDENT)
      policy_fail("hit page not resident");
    if(!trace->hit && initialized[accesses[step].page]){
      if(target_before.page_state != SWAP_PAGE_SWAPPED)
        policy_fail("miss page not swapped");
      expected_page_ins++;
    }

    if(trace->victim >= 0){
      char *victim = base + (uint64)trace->victim * PGSIZE;
      struct swap_info victim_after;
      if(swapout(victim) < 0)
        policy_fail("swapout victim");
      if(swapinfo(victim, &victim_after) < 0 ||
         victim_after.page_state != SWAP_PAGE_SWAPPED)
        policy_fail("victim state");
      result->evictions++;
    }

    policy_access_page(target, accesses[step].page, accesses[step].write,
                       initialized, expected);
    struct swap_info target_after;
    if(swapinfo(target, &target_after) < 0 ||
       target_after.page_state != SWAP_PAGE_RESIDENT)
      policy_fail("target resident state");
  }

  struct swap_info after;
  if(swapinfo(base, &after) < 0)
    policy_fail("final swapinfo");
  result->page_outs = (int)(after.page_outs - baseline.page_outs);
  result->page_ins = (int)(after.page_ins - baseline.page_ins);
  result->final_resident_mask = policy_resident_mask(&state);
  if(result->page_outs != result->evictions ||
     result->page_ins != expected_page_ins)
    policy_fail("mechanism counter mismatch");

  policy_print_result(label, kind, frame_count, result);
  policy_release_pages(base, old_break, growth, baseline.used_slots);
}

/** 验证同一序列下 FIFO 与 CLOCK 的第二个 victim 可被稳定区分。 */
static void
policy_test_contrast(void)
{
  struct policy_access accesses[] = {
    {0, 1}, {1, 0}, {2, 0}, {3, 0}, {1, 0}, {4, 1},
  };

  policy_run("policy-contrast", POLICY_FIFO, 5, 3, accesses,
             sizeof(accesses) / sizeof(accesses[0]), &policy_fifo_result);
  policy_run("policy-contrast", POLICY_CLOCK, 5, 3, accesses,
             sizeof(accesses) / sizeof(accesses[0]), &policy_clock_result);

  if(policy_fifo_result.evictions != 2 ||
     policy_clock_result.evictions != 2 ||
     policy_fifo_result.trace[3].victim != 0 ||
     policy_clock_result.trace[3].victim != 0)
    policy_fail("first replacement");
  if(!policy_fifo_result.trace[4].hit ||
     policy_fifo_result.trace[4].victim != -1 ||
     !policy_clock_result.trace[4].hit ||
     policy_clock_result.trace[4].victim != -1)
    policy_fail("hit caused eviction");
  if(policy_fifo_result.trace[5].victim != 1 ||
     policy_clock_result.trace[5].victim != 2)
    policy_fail("policy distinction");

  printf("VMPOLICY contrast first_victim=0 fifo_second=1 clock_second=2 hit_no_evict=OK\n");
}

/** 用缺页与 I/O 计数比较工作集可驻留和容量不足两种状态。 */
static void
policy_test_locality(void)
{
  struct policy_access accesses[] = {
    {0, 1}, {1, 0}, {2, 0}, {0, 0}, {1, 0}, {2, 0},
    {0, 0}, {1, 0}, {2, 0},
  };

  policy_run("working-set-fits", POLICY_FIFO, 3, 3, accesses,
             sizeof(accesses) / sizeof(accesses[0]), &policy_fit_result);
  policy_run("working-set-exceeds", POLICY_FIFO, 3, 2, accesses,
             sizeof(accesses) / sizeof(accesses[0]), &policy_pressure_result);

  if(policy_fit_result.misses != 3 || policy_fit_result.hits != 6 ||
     policy_fit_result.evictions != 0 || policy_fit_result.page_outs != 0 ||
     policy_fit_result.page_ins != 0)
    policy_fail("fitting working set");
  if(policy_pressure_result.misses != 9 || policy_pressure_result.hits != 0 ||
     policy_pressure_result.evictions != 7 ||
     policy_pressure_result.page_outs != 7 ||
     policy_pressure_result.page_ins != 6)
    policy_fail("over-capacity working set");

  printf("VMPOLICY locality fit_misses=3 pressure_misses=9 pressure_page_ins=6 timing_oracle=unused\n");
}

/** 重复 CLOCK 序列，验证决策确定且上一轮缩容未泄漏 slot。 */
static void
policy_test_repeatability(void)
{
  struct policy_access accesses[] = {
    {0, 1}, {1, 0}, {2, 0}, {3, 0}, {1, 0}, {4, 1},
  };

  policy_run("policy-repeat", POLICY_CLOCK, 5, 3, accesses,
             sizeof(accesses) / sizeof(accesses[0]), &policy_repeat_result);
  if(policy_repeat_result.trace[3].victim !=
       policy_clock_result.trace[3].victim ||
     policy_repeat_result.trace[5].victim !=
       policy_clock_result.trace[5].victim ||
     policy_repeat_result.page_outs != policy_clock_result.page_outs)
    policy_fail("repeatability");
  printf("VMPOLICY repeat clock_victims=0,2 cleanup=OK\n");
}

/** 运行策略、工作集和资源回收的完整确定性实验。 */
static void
memviztest_policy(void)
{
  policy_test_contrast();
  policy_test_locality();
  policy_test_repeatability();
  printf("VMPOLICY boundary trigger=user-space dirty_bit=absent anonymous_pageout=full-page kernel_auto_replacement=absent\n");
  printf("vmpolicytest: OK\n");
}

#undef POLICY_MAX_FRAMES
#undef POLICY_MAX_PAGES
#undef POLICY_MAX_ACCESSES
#undef POLICY_FIFO
#undef POLICY_CLOCK

#endif
