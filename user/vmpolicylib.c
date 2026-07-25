#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/swap.h"
#include "user/user.h"
#include "user/vmpolicy.h"

struct vm_policy_frame {
  int page;
  int referenced;
};

struct vm_policy_state {
  enum vm_policy_kind kind;
  int frame_count;
  int resident_count;
  int hand;
  struct vm_policy_frame frames[VM_POLICY_MAX_FRAMES];
};

/** 将 result 复位为可安全返回的初始状态。 */
static void
result_init(struct vm_policy_result *result)
{
  memset(result, 0, sizeof(*result));
  result->error_step = -1;
  for(int i = 0; i < VM_POLICY_MAX_ACCESSES; i++)
    result->trace[i].victim = -1;
}

/** 记录第一个失败阶段，避免后续清理错误遮蔽根因。 */
static void
result_fail(struct vm_policy_result *result, int error, int step)
{
  if(result->error == VM_POLICY_ERROR_NONE){
    result->error = error;
    result->error_step = step;
  }
}

/** 校验实验规模与访问下标，确保所有固定数组访问有界。 */
static int
workload_valid(enum vm_policy_kind kind, struct vm_policy_workload *workload)
{
  if(kind != VM_POLICY_FIFO && kind != VM_POLICY_CLOCK)
    return 0;
  if(workload == 0 || workload->label == 0)
    return 0;
  if(workload->page_count <= 0 || workload->page_count > VM_POLICY_MAX_PAGES)
    return 0;
  if(workload->frame_count <= 0 ||
     workload->frame_count > VM_POLICY_MAX_FRAMES ||
     workload->frame_count > workload->page_count)
    return 0;
  if(workload->access_count <= 0 ||
     workload->access_count > VM_POLICY_MAX_ACCESSES)
    return 0;
  for(int i = 0; i < workload->access_count; i++){
    if(workload->accesses[i].page < 0 ||
       workload->accesses[i].page >= workload->page_count)
      return 0;
    if(workload->accesses[i].write != 0 &&
       workload->accesses[i].write != 1)
      return 0;
  }
  return 1;
}

/** 初始化 FIFO/CLOCK 共用的固定页框状态。 */
static void
state_init(struct vm_policy_state *state,
           enum vm_policy_kind kind,
           int frame_count)
{
  memset(state, 0, sizeof(*state));
  state->kind = kind;
  state->frame_count = frame_count;
  for(int i = 0; i < VM_POLICY_MAX_FRAMES; i++)
    state->frames[i].page = -1;
}

/** 返回目标逻辑页当前占用的页框槽位，未驻留时返回 -1。 */
static int
find_frame(struct vm_policy_state *state, int page)
{
  for(int i = 0; i < state->resident_count; i++)
    if(state->frames[i].page == page)
      return i;
  return -1;
}

/** 将页框数组压缩成页号位图，供 trace 和断言稳定比较。 */
static uint
resident_mask(struct vm_policy_state *state)
{
  uint mask = 0;
  for(int i = 0; i < state->resident_count; i++)
    mask |= 1U << state->frames[i].page;
  return mask;
}

/** 将 CLOCK 引用状态压缩成页号位图；FIFO 仅把它作为观察信息。 */
static uint
referenced_mask(struct vm_policy_state *state)
{
  uint mask = 0;
  for(int i = 0; i < state->resident_count; i++)
    if(state->frames[i].referenced)
      mask |= 1U << state->frames[i].page;
  return mask;
}

/**
 * 推进一次纯策略决策，不直接访问页面或调用内核。
 *
 * 命中只更新引用信息；未满时占用空槽；已满时 FIFO 使用 hand 指向最早
 * 进入的页，CLOCK 则清除引用位直到遇到第一次机会已耗尽的页。
 */
static void
select_page(struct vm_policy_state *state,
            struct vm_policy_access *access,
            struct vm_policy_trace *trace)
{
  memset(trace, 0, sizeof(*trace));
  trace->page = access->page;
  trace->write = access->write;
  trace->victim = -1;
  trace->hand_before = state->hand;

  int frame = find_frame(state, access->page);
  if(frame >= 0){
    trace->hit = 1;
    state->frames[frame].referenced = 1;
  } else if(state->resident_count < state->frame_count){
    frame = state->resident_count++;
    state->frames[frame].page = access->page;
    state->frames[frame].referenced = 1;
  } else if(state->kind == VM_POLICY_FIFO){
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
  trace->resident_mask = resident_mask(state);
  trace->referenced_mask = referenced_mask(state);
}

/** 为一个逻辑页生成不会被零页误判为成功的初始字节。 */
static unsigned char
initial_byte(int page)
{
  return (unsigned char)((page * 37 + 11) & 0xff);
}

/**
 * 访问一个真实匿名页并校验换入后的首尾字节。
 *
 * 首次访问负责物化 lazy 页并写入签名；后续访问先验证签名，再按 workload
 * 的 write 标志修改首字节。这样任何已换出页被重新访问时都同时验证数据保持。
 */
static int
access_real_page(char *page,
                 int logical_page,
                 int write_access,
                 int *initialized,
                 unsigned char *expected)
{
  unsigned char tail = (unsigned char)(initial_byte(logical_page) ^ 0x5a);

  if(!initialized[logical_page]){
    expected[logical_page] = initial_byte(logical_page);
    page[0] = (char)expected[logical_page];
    page[PGSIZE - 1] = (char)tail;
    initialized[logical_page] = 1;
  } else {
    if((unsigned char)page[0] != expected[logical_page] ||
       (unsigned char)page[PGSIZE - 1] != tail)
      return -1;
  }

  if(write_access){
    expected[logical_page]++;
    page[0] = (char)expected[logical_page];
  }
  return 0;
}

/** 分配一段页对齐的 lazy 匿名地址区间，并返回恢复 break 所需增长量。 */
static char *
reserve_pages(int page_count, uint64 *old_break, int *growth)
{
  *old_break = (uint64)sbrk(0);
  uint64 base = PGROUNDUP(*old_break);
  uint64 end = base + (uint64)page_count * PGSIZE;
  uint64 growth64 = end - *old_break;

  if(end < base || growth64 > 0x7fffffffULL)
    return (char *)-1;
  *growth = (int)growth64;
  if((uint64)sbrk(*growth) != *old_break)
    return (char *)-1;
  return (char *)base;
}

/** 恢复实验前 break，并确认所有非驻留 slot 已由缩容路径释放。 */
static int
release_pages(char *base,
              uint64 old_break,
              int growth,
              uint baseline_used_slots)
{
  uint64 expected_top = (uint64)base +
                        ((uint64)growth - ((uint64)base - old_break));
  if((uint64)sbrk(-growth) != expected_top)
    return -1;
  if((uint64)sbrk(0) != old_break)
    return -1;

  struct swap_info released;
  if(swapinfo(base, &released) < 0 ||
     released.page_state != SWAP_PAGE_UNMAPPED ||
     released.used_slots != baseline_used_slots)
    return -1;
  return 0;
}

int
vm_policy_run(enum vm_policy_kind kind,
              struct vm_policy_workload *workload,
              struct vm_policy_result *result)
{
  if(result == 0)
    return -1;
  result_init(result);
  if(!workload_valid(kind, workload)){
    result_fail(result, VM_POLICY_ERROR_ARGUMENT, -1);
    return -1;
  }

  uint64 old_break = 0;
  int growth = 0;
  char *base = reserve_pages(workload->page_count, &old_break, &growth);
  if(base == (char *)-1){
    result_fail(result, VM_POLICY_ERROR_SBRK, -1);
    return -1;
  }

  struct swap_info baseline;
  if(swapinfo(base, &baseline) < 0){
    result_fail(result, VM_POLICY_ERROR_SWAPINFO, -1);
    goto cleanup;
  }

  struct vm_policy_state state;
  int initialized[VM_POLICY_MAX_PAGES];
  unsigned char expected[VM_POLICY_MAX_PAGES];
  memset(initialized, 0, sizeof(initialized));
  memset(expected, 0, sizeof(expected));
  state_init(&state, kind, workload->frame_count);

  int expected_page_ins = 0;
  for(int step = 0; step < workload->access_count; step++){
    struct vm_policy_access *access = &workload->accesses[step];
    struct vm_policy_trace *trace = &result->trace[step];
    struct swap_info target_before;
    struct swap_info target_after;
    char *target = base + (uint64)access->page * PGSIZE;

    if(swapinfo(target, &target_before) < 0){
      result_fail(result, VM_POLICY_ERROR_SWAPINFO, step);
      goto cleanup;
    }

    select_page(&state, access, trace);
    result->accesses++;
    if(trace->hit)
      result->hits++;
    else
      result->misses++;

    if(trace->hit && target_before.page_state != SWAP_PAGE_RESIDENT){
      result_fail(result, VM_POLICY_ERROR_STATE, step);
      goto cleanup;
    }
    if(!trace->hit && initialized[access->page]){
      if(target_before.page_state != SWAP_PAGE_SWAPPED){
        result_fail(result, VM_POLICY_ERROR_STATE, step);
        goto cleanup;
      }
      expected_page_ins++;
    }

    if(trace->victim >= 0){
      char *victim = base + (uint64)trace->victim * PGSIZE;
      struct swap_info victim_after;
      if(swapout(victim) < 0){
        result_fail(result, VM_POLICY_ERROR_SWAPOUT, step);
        goto cleanup;
      }
      if(swapinfo(victim, &victim_after) < 0 ||
         victim_after.page_state != SWAP_PAGE_SWAPPED){
        result_fail(result, VM_POLICY_ERROR_STATE, step);
        goto cleanup;
      }
      result->evictions++;
    }

    if(access_real_page(target, access->page, access->write,
                        initialized, expected) < 0){
      result_fail(result, VM_POLICY_ERROR_DATA, step);
      goto cleanup;
    }
    if(swapinfo(target, &target_after) < 0 ||
       target_after.page_state != SWAP_PAGE_RESIDENT){
      result_fail(result, VM_POLICY_ERROR_STATE, step);
      goto cleanup;
    }
  }

  struct swap_info after;
  if(swapinfo(base, &after) < 0){
    result_fail(result, VM_POLICY_ERROR_SWAPINFO, workload->access_count);
    goto cleanup;
  }
  result->page_outs = (int)(after.page_outs - baseline.page_outs);
  result->page_ins = (int)(after.page_ins - baseline.page_ins);
  result->final_resident_mask = resident_mask(&state);
  if(result->page_outs != result->evictions ||
     result->page_ins != expected_page_ins){
    result_fail(result, VM_POLICY_ERROR_COUNTER, workload->access_count);
    goto cleanup;
  }

cleanup:
  if(release_pages(base, old_break, growth, baseline.used_slots) < 0)
    result_fail(result, VM_POLICY_ERROR_CLEANUP, workload->access_count);
  return result->error == VM_POLICY_ERROR_NONE ? 0 : -1;
}

char *
vm_policy_name(enum vm_policy_kind kind)
{
  if(kind == VM_POLICY_FIFO)
    return "fifo";
  if(kind == VM_POLICY_CLOCK)
    return "clock";
  return "unknown";
}

char *
vm_policy_error_name(int error)
{
  switch(error){
  case VM_POLICY_ERROR_NONE:
    return "none";
  case VM_POLICY_ERROR_ARGUMENT:
    return "argument";
  case VM_POLICY_ERROR_SBRK:
    return "sbrk";
  case VM_POLICY_ERROR_SWAPINFO:
    return "swapinfo";
  case VM_POLICY_ERROR_STATE:
    return "page-state";
  case VM_POLICY_ERROR_SWAPOUT:
    return "swapout";
  case VM_POLICY_ERROR_DATA:
    return "data";
  case VM_POLICY_ERROR_COUNTER:
    return "counter";
  case VM_POLICY_ERROR_CLEANUP:
    return "cleanup";
  default:
    return "unknown";
  }
}

void
vm_policy_print(struct vm_policy_workload *workload,
                enum vm_policy_kind kind,
                struct vm_policy_result *result)
{
  for(int step = 0; step < result->accesses; step++){
    struct vm_policy_trace *trace = &result->trace[step];
    printf("VMPOLICY label=%s policy=%s step=%d access=%d mode=%s result=%s victim=%d scan=%d hand=%d->%d resident=0x%x referenced=0x%x\n",
           workload->label, vm_policy_name(kind), step, trace->page,
           trace->write ? "write" : "read",
           trace->hit ? "hit" : "miss", trace->victim, trace->scanned,
           trace->hand_before, trace->hand_after, trace->resident_mask,
           trace->referenced_mask);
  }
  printf("VMPOLICY summary label=%s policy=%s frames=%d accesses=%d hits=%d misses=%d evictions=%d page_outs=%d page_ins=%d resident=0x%x error=%s step=%d\n",
         workload->label, vm_policy_name(kind), workload->frame_count,
         result->accesses, result->hits, result->misses, result->evictions,
         result->page_outs, result->page_ins, result->final_resident_mask,
         vm_policy_error_name(result->error), result->error_step);
}
