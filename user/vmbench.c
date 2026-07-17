#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/syscall.h"
#include "kernel/vmstat.h"
#include "user/user.h"

#define DEFAULT_PAGES 4096
#define MAX_PAGES 8192
#define SPARSE_STRIDE 64

struct region {
  char *start;
  char *end;
  int pages;
};

struct scenario {
  char *name;
  int (*run)(int pages);
};

static int
vmstat_call(int op, uint64 arg0, uint64 arg1, struct vmstat_snapshot *snapshot)
{
  register uint64 a0 asm("a0") = (uint64)op;
  register uint64 a1 asm("a1") = arg0;
  register uint64 a2 asm("a2") = arg1;
  register uint64 a3 asm("a3") = (uint64)snapshot;
  register uint64 a7 asm("a7") = SYS_vmstat;

  asm volatile("ecall"
               : "+r"(a0)
               : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
               : "memory");
  return (int)a0;
}

static char *
align_heap(void)
{
  uint64 current = (uint64)sbrk(0);
  uint64 aligned = PGROUNDUP(current);
  if(aligned > current && sbrk(aligned - current) == (char *)-1)
    return 0;
  return (char *)aligned;
}

static int
begin_region(int pages, struct region *region)
{
  char *start = align_heap();
  if(start == 0)
    return -1;

  uint64 bytes = (uint64)pages * PGSIZE;
  uint64 end = (uint64)start + bytes;
  if(end < (uint64)start)
    return -1;

  region->start = start;
  region->end = (char *)end;
  region->pages = pages;
  return vmstat_call(VMSTAT_BEGIN, (uint64)start, end, 0);
}

static int
reserve_region(struct region *region)
{
  uint64 oldsz = (uint64)sbrk(0);
  uint64 bytes = (uint64)region->pages * PGSIZE;
  if((uint64)region->start != oldsz)
    return -1;
  if(sbrk(bytes) == (char *)-1)
    return -1;
  return vmstat_call(VMSTAT_SAMPLE_SBRK, oldsz, oldsz + bytes, 0);
}

static void
touch_pages(char *start, int pages, int stride, char value)
{
  volatile char *memory = (volatile char *)start;
  for(int page = 0; page < pages; page += stride)
    memory[(uint64)page * PGSIZE] = value;
}

static int
check_pages(char *start, int pages, int stride, char value)
{
  volatile char *memory = (volatile char *)start;
  for(int page = 0; page < pages; page += stride)
    if(memory[(uint64)page * PGSIZE] != value)
      return -1;
  return 0;
}

static void
print_counts(char *prefix, struct vmstat_counts *counts)
{
  printf(" %s_virtual=%d", prefix, (int)counts->virtual_grow_pages);
  printf(" %s_eager_alloc=%d", prefix, (int)counts->sbrk_eager_alloc_pages);
  printf(" %s_lazy_alloc=%d", prefix, (int)counts->lazy_materialized_pages);
  printf(" %s_fork_scan=%d", prefix, (int)counts->fork_va_scan_pages);
  printf(" %s_fork_present=%d", prefix, (int)counts->fork_present_pages);
  printf(" %s_fork_copy=%d", prefix, (int)counts->fork_eager_copy_pages);
  printf(" %s_fork_shared=%d", prefix, (int)counts->fork_shared_map_pages);
  printf(" %s_cow_mark=%d", prefix, (int)counts->fork_cow_mark_pages);
  printf(" %s_cow_copy=%d", prefix, (int)counts->cow_copy_pages);
  printf(" %s_final_present=%d", prefix, (int)counts->final_present_pages);
}

static int
finish_region(char *scenario, int pages, int stride)
{
  struct vmstat_snapshot snapshot;
  if(vmstat_call(VMSTAT_END, 0, 0, &snapshot) < 0){
    printf("vmbench: VMSTAT_END failed for %s\n", scenario);
    return -1;
  }

  printf("VMRESULT scenario=%s pages=%d stride=%d abi=%d",
         scenario, pages, stride, (int)snapshot.abi_version);
  print_counts("range", &snapshot.range);
  print_counts("total", &snapshot.total);
  printf("\n");
  return 0;
}

static int
run_touch_case(char *name, int pages, int stride)
{
  struct region region;
  if(begin_region(pages, &region) < 0 || reserve_region(&region) < 0)
    return -1;

  if(stride > 0){
    touch_pages(region.start, pages, stride, 0x31);
    if(check_pages(region.start, pages, stride, 0x31) < 0)
      return -1;
  }
  return finish_region(name, pages, stride);
}

static int
run_reserve_only(int pages)
{
  return run_touch_case("reserve-only", pages, 0);
}

static int
run_sparse_touch(int pages)
{
  return run_touch_case("sparse-touch", pages, SPARSE_STRIDE);
}

static int
run_dense_touch(int pages)
{
  return run_touch_case("dense-touch", pages, 1);
}

static int
run_fork_case(char *name, int pages, int parent_stride,
              int child_write_stride, int do_exec)
{
  struct region region;
  int gate[2];
  int status = 0;

  if(begin_region(pages, &region) < 0 || reserve_region(&region) < 0)
    return -1;
  touch_pages(region.start, pages, parent_stride, 0x41);
  if(check_pages(region.start, pages, parent_stride, 0x41) < 0)
    return -1;

  if(pipe(gate) < 0)
    return -1;
  int pid = fork();
  if(pid < 0)
    return -1;

  if(pid == 0){
    char token;
    close(gate[1]);
    if(read(gate[0], &token, 1) != 1)
      exit(2);
    close(gate[0]);

    if(child_write_stride > 0)
      touch_pages(region.start, pages, child_write_stride, 0x52);
    if(vmstat_call(VMSTAT_SAMPLE_CHILD, 0, 0, 0) < 0)
      exit(3);

    if(do_exec){
      char *argv[] = { "echo", 0 };
      exec("echo", argv);
      exit(4);
    }
    exit(0);
  }

  close(gate[0]);
  if(vmstat_call(VMSTAT_SAMPLE_FORK, pid, 0, 0) < 0){
    close(gate[1]);
    wait(&status);
    return -1;
  }
  if(write(gate[1], "x", 1) != 1){
    close(gate[1]);
    wait(&status);
    return -1;
  }
  close(gate[1]);
  if(wait(&status) < 0 || status != 0)
    return -1;

  if(check_pages(region.start, pages, parent_stride, 0x41) < 0)
    return -1;
  return finish_region(name, pages,
                       child_write_stride > 0 ? child_write_stride : parent_stride);
}

static int
run_fork_exit(int pages)
{
  return run_fork_case("fork-exit", pages, 1, 0, 0);
}

static int
run_fork_exec(int pages)
{
  return run_fork_case("fork-exec", pages, 1, 0, 1);
}

static int
run_fork_write_sparse(int pages)
{
  return run_fork_case("fork-write-1of64", pages, 1, SPARSE_STRIDE, 0);
}

static int
run_fork_write_quarter(int pages)
{
  return run_fork_case("fork-write-1of4", pages, 1, 4, 0);
}

static int
run_fork_write_all(int pages)
{
  return run_fork_case("fork-write-all", pages, 1, 1, 0);
}

static int
run_sparse_fork_exec(int pages)
{
  return run_fork_case("sparse-fork-exec", pages, SPARSE_STRIDE, 0, 1);
}

static struct scenario scenarios[] = {
  { "reserve-only", run_reserve_only },
  { "sparse-touch", run_sparse_touch },
  { "dense-touch", run_dense_touch },
  { "fork-exit", run_fork_exit },
  { "fork-exec", run_fork_exec },
  { "fork-write-1of64", run_fork_write_sparse },
  { "fork-write-1of4", run_fork_write_quarter },
  { "fork-write-all", run_fork_write_all },
  { "sparse-fork-exec", run_sparse_fork_exec },
};

static int
run_one(struct scenario *scenario, int pages)
{
  int status = 0;
  int pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    if(scenario->run(pages) < 0){
      printf("VMERROR scenario=%s\n", scenario->name);
      exit(1);
    }
    exit(0);
  }
  if(wait(&status) < 0 || status != 0)
    return -1;
  return 0;
}

int
main(int argc, char **argv)
{
  char *selected = "all";
  int pages = DEFAULT_PAGES;

  if(argc >= 2)
    selected = argv[1];
  if(argc >= 3)
    pages = atoi(argv[2]);
  if(pages <= 0 || pages > MAX_PAGES){
    printf("usage: vmbench [all|scenario] [pages:1-%d]\n", MAX_PAGES);
    exit(1);
  }

  int failures = 0;
  int matched = 0;
  for(uint i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++){
    if(strcmp(selected, "all") != 0 && strcmp(selected, scenarios[i].name) != 0)
      continue;
    matched = 1;
    if(run_one(&scenarios[i], pages) < 0){
      printf("VMFAILED scenario=%s\n", scenarios[i].name);
      failures++;
    }
  }

  if(!matched){
    printf("unknown scenario: %s\n", selected);
    exit(1);
  }
  if(failures){
    printf("VMbench completed with %d failure(s)\n", failures);
    exit(1);
  }
  exit(0);
}
