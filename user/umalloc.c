#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define ALIGNMENT 16ULL
#define WORD_SIZE 8ULL
#define OVERHEAD (2ULL * WORD_SIZE)
#define MIN_BLOCK_SIZE 32ULL
#define ALLOCATED_FLAG 0x1ULL
#define FLAG_MASK 0xFULL
#define MAX_SBRK_INCREMENT 0x7ffffff0ULL

static char *heap_listp;
static void *free_list_head;

/** 读取一个 8 字节块元数据 word。 */
static uint64
load_word(void *address)
{
  return *(uint64 *)address;
}

/** 写入一个 8 字节块元数据 word。 */
static void
store_word(void *address, uint64 value)
{
  *(uint64 *)address = value;
}

/** 将字节数向上取整到 16 字节边界。 */
static uint64
align_up(uint64 value)
{
  return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/** 返回 payload 指针对应的 header 地址。 */
static char *
header_of(void *payload)
{
  return (char *)payload - WORD_SIZE;
}

/** 返回块总大小，不包含低四位状态标志。 */
static uint64
block_size(void *payload)
{
  return load_word(header_of(payload)) & ~FLAG_MASK;
}

/** 判断块是否处于已分配状态。 */
static int
block_allocated(void *payload)
{
  return (load_word(header_of(payload)) & ALLOCATED_FLAG) != 0;
}

/** 返回物理相邻的后继块 payload。 */
static void *
next_block(void *payload)
{
  return (char *)payload + block_size(payload);
}

/** 返回物理相邻的前驱块 payload。 */
static void *
previous_block(void *payload)
{
  uint64 previous_size = load_word((char *)payload - OVERHEAD) & ~FLAG_MASK;
  return (char *)payload - previous_size;
}

/** 读取空闲块 payload 中保存的前驱空闲块指针。 */
static void *
free_previous(void *payload)
{
  return *(void **)payload;
}

/** 读取空闲块 payload 中保存的后继空闲块指针。 */
static void *
free_next(void *payload)
{
  return *(void **)((char *)payload + WORD_SIZE);
}

/** 设置空闲块 payload 中保存的前驱空闲块指针。 */
static void
set_free_previous(void *payload, void *previous)
{
  *(void **)payload = previous;
}

/** 设置空闲块 payload 中保存的后继空闲块指针。 */
static void
set_free_next(void *payload, void *next)
{
  *(void **)((char *)payload + WORD_SIZE) = next;
}

/**
 * 同时写入块的 header 与 footer。
 *
 * @param payload 块 payload 起始地址，必须保持 16 字节对齐。
 * @param size 块总大小，必须是 16 的倍数。
 * @param allocated 非零表示已分配，零表示空闲。
 */
static void
write_block(void *payload, uint64 size, int allocated)
{
  uint64 value = size | (allocated ? ALLOCATED_FLAG : 0);
  store_word(header_of(payload), value);
  store_word((char *)payload + size - OVERHEAD, value);
}

/** 将空闲块插入显式双向链表头部。 */
static void
insert_free_block(void *payload)
{
  set_free_previous(payload, 0);
  set_free_next(payload, free_list_head);
  if(free_list_head != 0)
    set_free_previous(free_list_head, payload);
  free_list_head = payload;
}

/**
 * 从显式双向空闲链表中摘除一个块。
 *
 * @param payload 必须恰好位于空闲链表一次；函数不修改物理块元数据。
 */
static void
remove_free_block(void *payload)
{
  void *previous = free_previous(payload);
  void *next = free_next(payload);

  if(previous != 0)
    set_free_next(previous, next);
  else
    free_list_head = next;
  if(next != 0)
    set_free_previous(next, previous);
}

/**
 * 将空闲块与物理相邻空闲块立即合并，并只保留一个 free-list 入口。
 *
 * @param payload 已写为空闲状态、但尚未插入 free list 的块。
 * @return 合并后块的 payload 起始地址。
 */
static void *
coalesce(void *payload)
{
  void *previous = previous_block(payload);
  void *next = next_block(payload);
  int previous_is_allocated = block_allocated(previous);
  int next_is_allocated = block_allocated(next);
  uint64 size = block_size(payload);

  if(!previous_is_allocated){
    remove_free_block(previous);
    size += block_size(previous);
    payload = previous;
  }
  if(!next_is_allocated){
    remove_free_block(next);
    size += block_size(next);
  }

  write_block(payload, size, 0);
  insert_free_block(payload);
  return payload;
}

/**
 * 初始化一页用户堆，建立 padding、prologue、首个空闲块和 epilogue。
 *
 * @return 成功返回 0；sbrk 无法一次增长一页时返回 -1，且全局状态不改变。
 */
static int
initialize_heap(void)
{
  char *current = sbrk(0);
  char *start;
  char *base;
  void *first_free;
  uint64 leading;
  uint64 first_free_size = PGSIZE - 4 * WORD_SIZE;

  if(current == (char *)-1)
    return -1;
  leading = (ALIGNMENT - ((uint64)current & (ALIGNMENT - 1))) &
            (ALIGNMENT - 1);
  start = sbrk((int)(PGSIZE + leading));
  if(start == (char *)-1)
    return -1;
  base = start + leading;

  // leading 只修正调用者可能先用 sbrk 造成的非对齐 break；正式 arena 仍为一页。
  store_word(base, 0);
  store_word(base + WORD_SIZE, OVERHEAD | ALLOCATED_FLAG);
  store_word(base + 2 * WORD_SIZE, OVERHEAD | ALLOCATED_FLAG);

  first_free = base + 4 * WORD_SIZE;
  write_block(first_free, first_free_size, 0);
  store_word(base + PGSIZE - WORD_SIZE, ALLOCATED_FLAG);

  heap_listp = base + 2 * WORD_SIZE;
  free_list_head = 0;
  insert_free_block(first_free);
  return 0;
}

/**
 * 按页级 chunk 扩展用户堆，并把旧 epilogue 改写为新空闲块 header。
 *
 * @param minimum_size 新块至少需要容纳的总字节数。
 * @return 成功返回已合并空闲块；当前 sbrk(int) 无法安全表达增长时返回 0。
 */
static void *
extend_heap(uint64 minimum_size)
{
  uint64 size = minimum_size < PGSIZE ? PGSIZE : align_up(minimum_size);
  char *old_break;
  void *payload;

  if(size > MAX_SBRK_INCREMENT)
    return 0;
  old_break = sbrk((int)size);
  if(old_break == (char *)-1)
    return 0;

  payload = old_break;
  write_block(payload, size, 0);
  store_word(old_break + size - WORD_SIZE, ALLOCATED_FLAG);
  return coalesce(payload);
}

/**
 * 将用户请求换算为包含 header/footer 的合法块大小。
 *
 * @param requested 用户请求的 payload 字节数，必须大于 0。
 * @param adjusted 接收 16 字节对齐后的块总大小。
 * @return 计算成功返回 0；加法或对齐上整溢出时返回 -1。
 */
static int
adjust_request(uint64 requested, uint64 *adjusted)
{
  uint64 maximum = ~(uint64)0;

  if(requested > maximum - OVERHEAD - (ALIGNMENT - 1))
    return -1;
  *adjusted = align_up(requested + OVERHEAD);
  if(*adjusted < MIN_BLOCK_SIZE)
    *adjusted = MIN_BLOCK_SIZE;
  return 0;
}

/** 在空闲链表中按 first-fit 查找第一个足够大的块。 */
static void *
find_first_fit(uint64 adjusted_size)
{
  for(void *payload = free_list_head; payload != 0; payload = free_next(payload))
    if(block_size(payload) >= adjusted_size)
      return payload;
  return 0;
}

/**
 * 从空闲块中放置一个已分配块；合法剩余空间会形成新的空闲块。
 *
 * @param payload 已从 first-fit 找到且仍位于 free list 的空闲块。
 * @param adjusted_size 需要放置的块总大小。
 */
static void
place_block(void *payload, uint64 adjusted_size)
{
  uint64 current_size = block_size(payload);
  uint64 remainder = current_size - adjusted_size;

  remove_free_block(payload);
  if(remainder >= MIN_BLOCK_SIZE){
    void *split = (char *)payload + adjusted_size;
    write_block(payload, adjusted_size, 1);
    write_block(split, remainder, 0);
    insert_free_block(split);
  } else {
    write_block(payload, current_size, 1);
  }
}

/**
 * 申请一段 16 字节对齐的用户态动态内存。
 *
 * @param size 需要的 payload 字节数；0 固定返回 0。
 * @return 成功返回由调用者持有的 payload；溢出或 sbrk 失败返回 0。
 */
void *
malloc(uint64 size)
{
  uint64 adjusted_size;
  void *payload;

  if(size == 0 || adjust_request(size, &adjusted_size) < 0)
    return 0;

  // 未初始化时，单块请求若已超过 sbrk(int) 的安全增长上限，不能先改变 break。
  if(heap_listp == 0 && adjusted_size > MAX_SBRK_INCREMENT)
    return 0;
  if(heap_listp == 0 && initialize_heap() < 0)
    return 0;

  payload = find_first_fit(adjusted_size);
  if(payload == 0){
    payload = extend_heap(adjusted_size);
    if(payload == 0)
      return 0;
  }

  place_block(payload, adjusted_size);
  return payload;
}

/**
 * 释放 malloc/calloc/realloc 返回的块并立即合并物理相邻空闲块。
 *
 * @param pointer 需要释放的 payload；0 为安全空操作。非法指针与 double free 未定义。
 */
void
free(void *pointer)
{
  uint64 size;

  if(pointer == 0)
    return;
  size = block_size(pointer);
  write_block(pointer, size, 0);
  coalesce(pointer);
}

/**
 * 申请 nmemb 个元素并把请求的全部 payload 字节清零。
 *
 * @param nmemb 元素数量；0 固定返回 0。
 * @param size 单个元素字节数；0 固定返回 0。
 * @return 成功返回清零后的 payload；乘法溢出或分配失败返回 0。
 */
void *
calloc(uint64 nmemb, uint64 size)
{
  uint64 total;
  unsigned char *payload;

  if(nmemb == 0 || size == 0)
    return 0;
  if(nmemb > (~(uint64)0) / size)
    return 0;
  total = nmemb * size;
  payload = malloc(total);
  if(payload == 0)
    return 0;

  for(uint64 index = 0; index < total; index++)
    payload[index] = 0;
  return payload;
}

/**
 * 调整已分配块的 payload 容量，优先原地缩小或吞并紧邻后继空闲块。
 *
 * @param pointer 原 payload；0 时等价于 malloc(size)。
 * @param size 新 payload 字节数；0 时释放原块并返回 0。
 * @return 成功返回新 payload；失败返回 0，原块及其数据仍保持有效。
 */
void *
realloc(void *pointer, uint64 size)
{
  uint64 adjusted_size;
  uint64 current_size;
  uint64 old_payload_size;
  uint64 remainder;
  void *next;
  void *replacement;

  if(pointer == 0)
    return malloc(size);
  if(size == 0){
    free(pointer);
    return 0;
  }
  if(adjust_request(size, &adjusted_size) < 0)
    return 0;

  current_size = block_size(pointer);
  old_payload_size = current_size - OVERHEAD;
  if(adjusted_size <= current_size){
    remainder = current_size - adjusted_size;
    if(remainder >= MIN_BLOCK_SIZE){
      void *split = (char *)pointer + adjusted_size;
      write_block(pointer, adjusted_size, 1);
      write_block(split, remainder, 0);
      coalesce(split);
    }
    return pointer;
  }

  next = next_block(pointer);
  if(!block_allocated(next) && current_size + block_size(next) >= adjusted_size){
    uint64 combined_size = current_size + block_size(next);
    remove_free_block(next);
    remainder = combined_size - adjusted_size;
    if(remainder >= MIN_BLOCK_SIZE){
      void *split = (char *)pointer + adjusted_size;
      write_block(pointer, adjusted_size, 1);
      write_block(split, remainder, 0);
      coalesce(split);
    } else {
      write_block(pointer, combined_size, 1);
    }
    return pointer;
  }

  // 先完成新分配再释放旧块，确保失败不会丢失原指针、原数据或 free-list 状态。
  replacement = malloc(size);
  if(replacement == 0)
    return 0;

  uint64 copy_size = old_payload_size < size ? old_payload_size : size;
  for(uint64 index = 0; index < copy_size; index++)
    ((unsigned char *)replacement)[index] = ((unsigned char *)pointer)[index];
  free(pointer);
  return replacement;
}
