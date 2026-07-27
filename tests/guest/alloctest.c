#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define STRESS_SLOTS 24
#define STRESS_STEPS 600

/** 输出失败阶段并以非零状态结束当前测试子进程。 */
static void
fail(char *phase, char *reason)
{
  printf("alloctest: %s: %s\n", phase, reason);
  exit(1);
}

/** 使用与槽位和偏移相关的确定性字节填充一个活跃块。 */
static void
fill_pattern(unsigned char *pointer, uint64 size, unsigned char tag)
{
  for(uint64 index = 0; index < size; index++)
    pointer[index] = (unsigned char)(tag ^ index ^ (index >> 8));
}

/** 校验活跃块的确定性字节模式未被其他分配器操作覆盖。 */
static void
verify_pattern(unsigned char *pointer, uint64 size, unsigned char tag,
               char *phase)
{
  for(uint64 index = 0; index < size; index++)
    if(pointer[index] != (unsigned char)(tag ^ index ^ (index >> 8)))
      fail(phase, "payload pattern changed");
}

/** 验证不同大小请求都保持 16 字节对齐、互不重叠且数据可读回。 */
static void
test_alignment_and_overlap(void)
{
  static uint64 sizes[] = {
    1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33,
    PGSIZE - 1, PGSIZE, PGSIZE + 1, 2 * PGSIZE + 17,
  };
  unsigned char *pointers[ARRAY_SIZE(sizes)];

  for(uint64 index = 0; index < ARRAY_SIZE(sizes); index++){
    pointers[index] = malloc(sizes[index]);
    if(pointers[index] == 0)
      fail("alignment", "malloc returned null");
    if(((uint64)pointers[index] & 0xf) != 0)
      fail("alignment", "payload is not 16-byte aligned");

    for(uint64 previous = 0; previous < index; previous++){
      uint64 left_start = (uint64)pointers[previous];
      uint64 left_end = left_start + sizes[previous];
      uint64 right_start = (uint64)pointers[index];
      uint64 right_end = right_start + sizes[index];
      if(left_start < right_end && right_start < left_end)
        fail("alignment", "live payload ranges overlap");
    }
    fill_pattern(pointers[index], sizes[index], (unsigned char)(index + 11));
  }

  for(uint64 index = 0; index < ARRAY_SIZE(sizes); index++){
    verify_pattern(pointers[index], sizes[index],
                   (unsigned char)(index + 11), "alignment");
    free(pointers[index]);
  }
}

/** 验证调用者预先制造非对齐 break 后，malloc 仍返回 16 字节对齐 payload。 */
static void
test_unaligned_initial_break(void)
{
  char *before = sbrk(1);
  void *pointer;

  if(before == (char *)-1)
    fail("unaligned-break", "sbrk(1) failed");
  pointer = malloc(7);
  if(pointer == 0)
    fail("unaligned-break", "malloc returned null");
  if(((uint64)pointer & 0xf) != 0)
    fail("unaligned-break", "allocator did not repair initial alignment");
  free(pointer);
}

/** 验证大空闲块能够被分割并由后续请求复用，且不额外增长 break。 */
static void
test_split_and_reuse(void)
{
  unsigned char *large = malloc(512);
  void *guard = malloc(64);
  char *break_before;
  void *small;
  void *remainder;

  if(large == 0 || guard == 0)
    fail("split", "setup allocation failed");
  free(large);
  break_before = sbrk(0);

  small = malloc(64);
  remainder = malloc(400);
  if(small != large)
    fail("split", "first-fit did not reuse the released block");
  if(remainder == 0)
    fail("split", "legal split remainder was not reusable");
  if(sbrk(0) != break_before)
    fail("split", "reuse unexpectedly grew program break");

  free(small);
  free(remainder);
  free(guard);
}

/** 验证不足 32 字节的尾部不会形成可破坏链表的 splinter。 */
static void
test_no_splinter(void)
{
  unsigned char *block = malloc(49);
  void *guard = malloc(32);
  char *break_before;
  unsigned char *replacement;

  if(block == 0 || guard == 0)
    fail("no-splinter", "setup allocation failed");
  free(block);
  break_before = sbrk(0);
  replacement = malloc(48);
  if(replacement != block)
    fail("no-splinter", "released 80-byte block was not reused");
  fill_pattern(replacement, 48, 0x51);
  verify_pattern(replacement, 48, 0x51, "no-splinter");
  free(replacement);

  replacement = malloc(49);
  if(replacement != block)
    fail("no-splinter", "whole-block placement damaged later reuse");
  if(sbrk(0) != break_before)
    fail("no-splinter", "whole-block reuse unexpectedly grew break");
  free(replacement);
  free(guard);
}

/** 验证前后均已分配时，释放块仍能独立复用。 */
static void
test_coalesce_neither(void)
{
  void *a = malloc(48);
  void *b = malloc(48);
  void *c = malloc(48);
  char *break_before;
  void *result;

  if(a == 0 || b == 0 || c == 0)
    fail("coalesce-neither", "setup allocation failed");
  free(b);
  break_before = sbrk(0);
  result = malloc(48);
  if(result != b || sbrk(0) != break_before)
    fail("coalesce-neither", "standalone free block was not reused");
  free(a);
  free(result);
  free(c);
}

/** 验证释放当前块时能够与后继空闲块合并。 */
static void
test_coalesce_next(void)
{
  void *a = malloc(48);
  void *b = malloc(48);
  void *c = malloc(48);
  void *d = malloc(48);
  char *break_before;
  void *result;

  if(a == 0 || b == 0 || c == 0 || d == 0)
    fail("coalesce-next", "setup allocation failed");
  free(c);
  free(b);
  break_before = sbrk(0);
  result = malloc(96);
  if(result != b || sbrk(0) != break_before)
    fail("coalesce-next", "next-only merged space was not reused");
  free(a);
  free(result);
  free(d);
}

/** 验证释放当前块时能够与前驱空闲块合并。 */
static void
test_coalesce_previous(void)
{
  void *a = malloc(48);
  void *b = malloc(48);
  void *c = malloc(48);
  void *d = malloc(48);
  char *break_before;
  void *result;

  if(a == 0 || b == 0 || c == 0 || d == 0)
    fail("coalesce-previous", "setup allocation failed");
  free(b);
  free(c);
  break_before = sbrk(0);
  result = malloc(96);
  if(result != b || sbrk(0) != break_before)
    fail("coalesce-previous", "previous-only merged space was not reused");
  free(a);
  free(result);
  free(d);
}

/** 验证释放当前块时能够同时合并前驱与后继空闲块。 */
static void
test_coalesce_both(void)
{
  void *a = malloc(48);
  void *b = malloc(48);
  void *c = malloc(48);
  void *d = malloc(48);
  void *e = malloc(48);
  char *break_before;
  void *result;

  if(a == 0 || b == 0 || c == 0 || d == 0 || e == 0)
    fail("coalesce-both", "setup allocation failed");
  free(b);
  free(d);
  free(c);
  break_before = sbrk(0);
  result = malloc(176);
  if(result != b || sbrk(0) != break_before)
    fail("coalesce-both", "three-block merged space was not reused");
  free(a);
  free(result);
  free(e);
}

/** 验证零大小 API 与 free(0) 不崩溃且不改变 program break。 */
static void
test_zero_and_null_contracts(void)
{
  char *before = sbrk(0);

  free(0);
  if(malloc(0) != 0)
    fail("zero-null", "malloc(0) did not return null");
  if(calloc(0, 8) != 0 || calloc(8, 0) != 0)
    fail("zero-null", "zero-sized calloc did not return null");
  if(realloc(0, 0) != 0)
    fail("zero-null", "realloc(NULL, 0) did not return null");
  if(sbrk(0) != before)
    fail("zero-null", "zero/null API changed program break");

  void *pointer = malloc(32);
  if(pointer == 0)
    fail("zero-null", "allocator unusable after free(NULL)");
  free(pointer);
}

/** 验证 calloc 清零、相邻块隔离及乘法溢出失败回滚。 */
static void
test_calloc_contracts(void)
{
  unsigned char *left = malloc(32);
  unsigned char *zeroed = calloc(37, 7);
  unsigned char *right = malloc(32);
  char *break_before_failure;
  uint64 maximum = ~(uint64)0;

  if(left == 0 || zeroed == 0 || right == 0)
    fail("calloc", "setup allocation failed");
  fill_pattern(left, 32, 0x21);
  fill_pattern(right, 32, 0x42);
  for(uint64 index = 0; index < 37 * 7; index++)
    if(zeroed[index] != 0)
      fail("calloc", "calloc payload was not fully zeroed");
  fill_pattern(zeroed, 37 * 7, 0x63);
  verify_pattern(left, 32, 0x21, "calloc");
  verify_pattern(right, 32, 0x42, "calloc");

  break_before_failure = sbrk(0);
  if(calloc(maximum / 3 + 1, 3) != 0)
    fail("calloc", "overflowing multiplication unexpectedly succeeded");
  if(sbrk(0) != break_before_failure)
    fail("calloc", "overflow failure changed program break");
  verify_pattern(left, 32, 0x21, "calloc");
  verify_pattern(right, 32, 0x42, "calloc");
  verify_pattern(zeroed, 37 * 7, 0x63, "calloc");

  free(left);
  free(zeroed);
  free(right);
}

/** 验证 realloc(NULL, n) 与 realloc(ptr, 0) 的标准化教学契约。 */
static void
test_realloc_null_and_zero(void)
{
  unsigned char *pointer = realloc(0, 64);
  char *break_before_reuse;
  void *replacement;

  if(pointer == 0 || ((uint64)pointer & 0xf) != 0)
    fail("realloc-null-zero", "realloc(NULL, n) did not allocate");
  fill_pattern(pointer, 64, 0x71);
  if(realloc(pointer, 0) != 0)
    fail("realloc-null-zero", "realloc(ptr, 0) did not return null");

  break_before_reuse = sbrk(0);
  replacement = malloc(64);
  if(replacement != pointer || sbrk(0) != break_before_reuse)
    fail("realloc-null-zero", "realloc(ptr, 0) did not free reusable space");
  free(replacement);
}

/** 验证 realloc 原地缩小后分割出的合法剩余块可被复用。 */
static void
test_realloc_shrink_split(void)
{
  unsigned char *pointer = malloc(256);
  void *guard = malloc(64);
  char *break_before;
  unsigned char *shrunk;
  void *reuse;

  if(pointer == 0 || guard == 0)
    fail("realloc-shrink-split", "setup allocation failed");
  fill_pattern(pointer, 256, 0x31);
  break_before = sbrk(0);
  shrunk = realloc(pointer, 32);
  if(shrunk != pointer)
    fail("realloc-shrink-split", "shrink moved the block");
  verify_pattern(shrunk, 32, 0x31, "realloc-shrink-split");
  reuse = malloc(160);
  if(reuse == 0 || sbrk(0) != break_before)
    fail("realloc-shrink-split", "split remainder was not reused");
  free(shrunk);
  free(reuse);
  free(guard);
}

/** 验证 realloc 缩小时不足最小块的剩余空间不会被错误分割。 */
static void
test_realloc_shrink_no_splinter(void)
{
  unsigned char *pointer = malloc(49);
  void *guard = malloc(32);
  unsigned char *shrunk;

  if(pointer == 0 || guard == 0)
    fail("realloc-shrink-nosplit", "setup allocation failed");
  fill_pattern(pointer, 49, 0x23);
  shrunk = realloc(pointer, 48);
  if(shrunk != pointer)
    fail("realloc-shrink-nosplit", "small shrink moved the block");
  verify_pattern(shrunk, 48, 0x23, "realloc-shrink-nosplit");
  free(shrunk);
  free(guard);
}

/** 验证 realloc 优先吞并紧邻后继空闲块并保留原数据前缀。 */
static void
test_realloc_grow_in_place(void)
{
  unsigned char *pointer = malloc(64);
  void *next = malloc(128);
  void *guard = malloc(64);
  char *break_before;
  unsigned char *grown;
  void *split_reuse;

  if(pointer == 0 || next == 0 || guard == 0)
    fail("realloc-grow-in-place", "setup allocation failed");
  fill_pattern(pointer, 64, 0x44);
  free(next);
  break_before = sbrk(0);
  grown = realloc(pointer, 160);
  if(grown != pointer)
    fail("realloc-grow-in-place", "adjacent growth moved the block");
  verify_pattern(grown, 64, 0x44, "realloc-grow-in-place");
  split_reuse = malloc(16);
  if(split_reuse == 0 || sbrk(0) != break_before)
    fail("realloc-grow-in-place", "growth remainder was not reusable");
  free(grown);
  free(split_reuse);
  free(guard);
}

/** 验证无法原地增长时 realloc 移动块并完整保留旧 payload 前缀。 */
static void
test_realloc_move(void)
{
  unsigned char *pointer = malloc(64);
  void *guard = malloc(64);
  unsigned char *moved;

  if(pointer == 0 || guard == 0)
    fail("realloc-move", "setup allocation failed");
  fill_pattern(pointer, 64, 0x55);
  moved = realloc(pointer, PGSIZE);
  if(moved == 0 || moved == pointer)
    fail("realloc-move", "blocked growth did not move to a new block");
  verify_pattern(moved, 64, 0x55, "realloc-move");
  free(moved);
  free(guard);
}

/** 验证 realloc 超大请求失败时原指针、数据与 program break 都保持有效。 */
static void
test_realloc_failure_preserves_old(void)
{
  unsigned char *pointer = malloc(128);
  char *break_before;
  void *result;

  if(pointer == 0)
    fail("realloc-failure", "setup allocation failed");
  fill_pattern(pointer, 128, 0x6a);
  break_before = sbrk(0);
  result = realloc(pointer, ~(uint64)0);
  if(result != 0)
    fail("realloc-failure", "overflowing growth unexpectedly succeeded");
  if(sbrk(0) != break_before)
    fail("realloc-failure", "overflow failure changed program break");
  verify_pattern(pointer, 128, 0x6a, "realloc-failure");

  result = realloc(pointer, 0x80000000ULL);
  if(result != 0)
    fail("realloc-failure", "sbrk-unrepresentable growth unexpectedly succeeded");
  if(sbrk(0) != break_before)
    fail("realloc-failure", "sbrk boundary failure changed program break");
  verify_pattern(pointer, 128, 0x6a, "realloc-failure");
  free(pointer);
}

/** 验证 malloc 的元数据加法、对齐溢出和 sbrk(int) 边界在初始化前安全失败。 */
static void
test_malloc_overflow(void)
{
  char *before = sbrk(0);

  if(malloc(~(uint64)0) != 0)
    fail("malloc-overflow", "maximum request unexpectedly succeeded");
  if(malloc((~(uint64)0) - 7) != 0)
    fail("malloc-overflow", "alignment overflow unexpectedly succeeded");
  if(malloc(0x80000000ULL) != 0)
    fail("malloc-overflow", "sbrk-unrepresentable request unexpectedly succeeded");
  if(sbrk(0) != before)
    fail("malloc-overflow", "overflow failure changed program break");
}

/** 返回固定 seed 的 xorshift32 下一状态，避免压力测试依赖随机时序。 */
static uint
next_random(uint *state)
{
  uint value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

/** 描述确定性压力测试中的一个活跃 payload 槽位。 */
struct stress_slot {
  unsigned char *pointer;
  uint64 size;
  unsigned char tag;
};

/** 校验所有压力测试活跃槽位仍保持各自字节模式。 */
static void
verify_live_slots(struct stress_slot *slots)
{
  for(int index = 0; index < STRESS_SLOTS; index++)
    if(slots[index].pointer != 0)
      verify_pattern(slots[index].pointer, slots[index].size,
                     slots[index].tag, "stress");
}

/** 固定 seed 反复执行 allocate/write/verify/realloc/free，覆盖链表长期演化。 */
static void
test_deterministic_stress(void)
{
  struct stress_slot slots[STRESS_SLOTS];
  uint state = 0x218c0deU;

  for(int index = 0; index < STRESS_SLOTS; index++){
    slots[index].pointer = 0;
    slots[index].size = 0;
    slots[index].tag = 0;
  }

  for(int step = 0; step < STRESS_STEPS; step++){
    uint random = next_random(&state);
    int slot_index = random % STRESS_SLOTS;
    struct stress_slot *slot = &slots[slot_index];

    verify_live_slots(slots);
    if(slot->pointer == 0){
      slot->size = 1 + (next_random(&state) % 512);
      slot->tag = (unsigned char)(1 + slot_index * 7 + step);
      slot->pointer = malloc(slot->size);
      if(slot->pointer == 0)
        fail("stress", "ordinary allocation returned null");
      fill_pattern(slot->pointer, slot->size, slot->tag);
      continue;
    }

    switch(random % 3){
    case 0:
      free(slot->pointer);
      slot->pointer = 0;
      slot->size = 0;
      break;
    case 1: {
      uint64 new_size = 1 + (next_random(&state) % 768);
      unsigned char *old_pointer = slot->pointer;
      uint64 old_size = slot->size;
      unsigned char old_tag = slot->tag;
      unsigned char *resized;

      if(step % 97 == 0)
        new_size = ~(uint64)0;
      resized = realloc(old_pointer, new_size);
      if(new_size == ~(uint64)0){
        if(resized != 0)
          fail("stress", "overflowing realloc unexpectedly succeeded");
        verify_pattern(old_pointer, old_size, old_tag, "stress");
        break;
      }
      if(resized == 0)
        fail("stress", "ordinary realloc returned null");
      verify_pattern(resized, old_size < new_size ? old_size : new_size,
                     old_tag, "stress");
      slot->pointer = resized;
      slot->size = new_size;
      slot->tag = (unsigned char)(old_tag + 29);
      fill_pattern(slot->pointer, slot->size, slot->tag);
      break;
    }
    default:
      slot->tag = (unsigned char)(slot->tag + 11);
      fill_pattern(slot->pointer, slot->size, slot->tag);
      break;
    }
  }

  verify_live_slots(slots);
  for(int index = 0; index < STRESS_SLOTS; index++)
    free(slots[index].pointer);
}

/** 验证 fork 后父子 allocator/free-list 与 COW payload 写入互不污染。 */
static void
test_fork_isolation(void)
{
  unsigned char *parent = malloc(128);
  int pid;
  int status = 0;

  if(parent == 0)
    fail("fork-isolation", "parent allocation failed");
  fill_pattern(parent, 128, 0x7d);
  pid = fork();
  if(pid < 0)
    fail("fork-isolation", "fork failed");
  if(pid == 0){
    unsigned char *child;
    free(parent);
    child = malloc(128);
    if(child == 0)
      exit(1);
    fill_pattern(child, 128, 0x2e);
    verify_pattern(child, 128, 0x2e, "fork-isolation-child");
    free(child);
    exit(0);
  }

  if(wait(&status) != pid || status != 0)
    fail("fork-isolation", "child allocator path failed");
  verify_pattern(parent, 128, 0x7d, "fork-isolation");
  free(parent);
}

/** 验证重复分配释放轮次在预热后持续复用 arena，而不单调增长 break。 */
static void
test_repeated_lifecycle(void)
{
  unsigned char *pointers[16];
  uint64 sizes[16];
  char *warm_break = 0;

  for(int round = 0; round < 40; round++){
    for(int index = 0; index < 16; index++){
      sizes[index] = 1 + ((index * 37) % 320);
      pointers[index] = malloc(sizes[index]);
      if(pointers[index] == 0)
        fail("lifecycle", "allocation failed");
      fill_pattern(pointers[index], sizes[index],
                   (unsigned char)(round + index + 1));
    }
    for(int index = 0; index < 16; index++)
      verify_pattern(pointers[index], sizes[index],
                     (unsigned char)(round + index + 1), "lifecycle");
    for(int index = 1; index < 16; index += 2)
      free(pointers[index]);
    for(int index = 0; index < 16; index += 2)
      free(pointers[index]);

    if(round == 1)
      warm_break = sbrk(0);
    else if(round > 1 && sbrk(0) != warm_break)
      fail("lifecycle", "repeated rounds stopped reusing allocator arena");
  }
}

/** 描述一个在独立子进程中执行并由退出状态持有 oracle 的测试用例。 */
struct test_case {
  char *name;
  void (*function)(void);
};

/**
 * 在独立子进程中运行一个测试，避免前一场景的 free-list 状态掩盖后续缺陷。
 *
 * @param test 要执行的测试名称和函数。
 * @return 子进程退出状态为 0 时返回 0，否则返回 -1。
 */
static int
run_test(struct test_case *test)
{
  int pid = fork();
  int status = 0;

  if(pid < 0){
    printf("alloctest: %s: fork failed\n", test->name);
    return -1;
  }
  if(pid == 0){
    test->function();
    exit(0);
  }
  if(wait(&status) != pid || status != 0){
    printf("alloctest: %s: FAILED status=%d\n", test->name, status);
    return -1;
  }
  printf("alloctest: %s: OK\n", test->name);
  return 0;
}

/** 执行 RV64 用户态动态内存分配器的完整确定性 guest 回归。 */
int
main(void)
{
  struct test_case tests[] = {
    {"alignment-and-overlap", test_alignment_and_overlap},
    {"unaligned-initial-break", test_unaligned_initial_break},
    {"split-and-reuse", test_split_and_reuse},
    {"no-splinter", test_no_splinter},
    {"coalesce-neither", test_coalesce_neither},
    {"coalesce-next", test_coalesce_next},
    {"coalesce-previous", test_coalesce_previous},
    {"coalesce-both", test_coalesce_both},
    {"zero-and-null-contracts", test_zero_and_null_contracts},
    {"calloc-contracts", test_calloc_contracts},
    {"realloc-null-and-zero", test_realloc_null_and_zero},
    {"realloc-shrink-split", test_realloc_shrink_split},
    {"realloc-shrink-no-splinter", test_realloc_shrink_no_splinter},
    {"realloc-grow-in-place", test_realloc_grow_in_place},
    {"realloc-move", test_realloc_move},
    {"realloc-failure-preserves-old", test_realloc_failure_preserves_old},
    {"malloc-overflow", test_malloc_overflow},
    {"deterministic-stress", test_deterministic_stress},
    {"fork-isolation", test_fork_isolation},
    {"repeated-lifecycle", test_repeated_lifecycle},
    {0, 0},
  };
  int failed = 0;

  for(struct test_case *test = tests; test->name != 0; test++)
    if(run_test(test) < 0)
      failed = 1;

  if(failed){
    printf("alloctest: SOME TESTS FAILED\n");
    exit(1);
  }
  printf("alloctest: ALL TESTS PASSED\n");
  exit(0);
}
