#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/semaphore.h"
#include "user/user.h"

#define EVENT_WAITERS 4
#define RESOURCE_WORKERS 4
#define RESOURCE_LIMIT 2
#define MUTEX_WORKERS 4
#define MUTEX_LOOPS 4
#define BUFFER_CAPACITY 3
#define BUFFER_ITEMS 12
#define POLL_TICKS 200

#define EVENT_ENTER 1
#define EVENT_EXIT 2

struct worker_event {
  int type;
  int worker;
};

/** 输出稳定失败原因并终止当前测试进程。 */
static void
fail(char *message)
{
  printf("semaphoretest: FAIL: %s\n", message);
  exit(1);
}

/**
 * 完整写入一个短消息；pipe 或文件提前失败时终止当前进程。
 *
 * @param fd 目标文件描述符。
 * @param buffer 待写入缓冲区。
 * @param size 必须写完的字节数。
 */
static void
write_exact(int fd, void *buffer, int size)
{
  char *cursor = buffer;
  int written = 0;

  while(written < size){
    int n = write(fd, cursor + written, size - written);
    if(n <= 0)
      fail("write_exact");
    written += n;
  }
}

/**
 * 完整读取一个短消息；EOF 或读取失败时终止当前进程。
 *
 * @param fd 来源文件描述符。
 * @param buffer 接收缓冲区。
 * @param size 必须读满的字节数。
 */
static void
read_exact(int fd, void *buffer, int size)
{
  char *cursor = buffer;
  int received = 0;

  while(received < size){
    int n = read(fd, cursor + received, size - received);
    if(n <= 0)
      fail("read_exact");
    received += n;
  }
}

/** 等待一个子进程以状态 0 退出。 */
static void
wait_child_ok(int pid, char *label)
{
  int status = -1;

  if(waitpid(pid, &status, 0) != pid || status != 0){
    printf("semaphoretest: child failed label=%s pid=%d status=%d\n",
           label, pid, status);
    exit(1);
  }
}

/** 等待一组直接子进程全部成功退出。 */
static void
wait_children_ok(int *pids, int count, char *label)
{
  for(int i = 0; i < count; i++)
    wait_child_ok(pids[i], label);
}

/**
 * 轮询信号量快照，直到等待者数量达到期望值。
 *
 * @param handle 信号量句柄。
 * @param expected 期望阻塞在该对象上的进程数。
 * @param info 返回命中时的快照。
 */
static void
wait_for_waiters(int handle, int expected, struct semaphore_info *info)
{
  for(int tick = 0; tick < POLL_TICKS; tick++){
    if(seminfo(handle, info) == 0 && info->waiters == expected)
      return;
    sleep(1);
  }
  fail("waiter count timeout");
}

/** 创建并写入一个固定大小的整数文件。 */
static void
create_int_file(char *path, int *values, int count)
{
  int fd = open(path, O_CREATE | O_RDWR | O_TRUNC);

  if(fd < 0)
    fail("create int file");
  write_exact(fd, values, count * sizeof(int));
  if(close(fd) < 0)
    fail("close int file");
}

/** 从文件指定整数槽位读取一个值。 */
static int
read_int_slot(char *path, int slot)
{
  int value = -1;
  int fd = open(path, O_RDONLY);

  if(fd < 0)
    fail("open int file for read");
  if(lseek(fd, (int64)slot * sizeof(int), SEEK_SET) < 0)
    fail("seek int file for read");
  read_exact(fd, &value, sizeof(value));
  if(close(fd) < 0)
    fail("close int read");
  return value;
}

/** 向文件指定整数槽位覆盖写入一个值。 */
static void
write_int_slot(char *path, int slot, int value)
{
  int fd = open(path, O_RDWR);

  if(fd < 0)
    fail("open int file for write");
  if(lseek(fd, (int64)slot * sizeof(int), SEEK_SET) < 0)
    fail("seek int file for write");
  write_exact(fd, &value, sizeof(value));
  if(close(fd) < 0)
    fail("close int write");
}

/** 验证创建边界、计数上界和非法句柄不会改变状态。 */
static void
test_count_boundaries(void)
{
  struct semaphore_info info;

  if(semcreate(-1, 1) != -1 || semcreate(2, 1) != -1 ||
     semcreate(0, 0) != -1)
    fail("invalid create accepted");
  if(semwait(-1) != -1 || sempost(-1) != -1 ||
     semdestroy(-1) != -1 || seminfo(-1, &info) != -1)
    fail("invalid handle accepted");

  int handle = semcreate(2, 2);
  if(handle < 0)
    fail("boundary create");
  if(sempost(handle) != -1)
    fail("overflow post accepted");
  if(seminfo(handle, &info) < 0 || info.value != 2 || info.limit != 2 ||
     info.posts != 0)
    fail("overflow changed state");
  if(semwait(handle) < 0 || semwait(handle) < 0)
    fail("boundary waits");
  if(seminfo(handle, &info) < 0 || info.value != 0 ||
     info.successful_waits != 2)
    fail("boundary drain mismatch");
  if(semdestroy(handle) < 0 || seminfo(handle, &info) != -1)
    fail("boundary destroy");

  printf("SEMAPHORE boundary invalid_rejected=1 overflow_rejected=1 final=0\n");
}

/**
 * 验证 post 先发生时许可会保留，以及 wait 先发生时不会丢失唤醒。
 */
static void
test_event_ordering(void)
{
  struct semaphore_info info;
  int pids[EVENT_WAITERS];
  int done[2];
  int handle = semcreate(0, EVENT_WAITERS);

  if(handle < 0)
    fail("event create");
  if(sempost(handle) < 0 || semwait(handle) < 0)
    fail("post before wait");
  if(seminfo(handle, &info) < 0 || info.value != 0 ||
     info.successful_waits != 1 || info.posts != 1)
    fail("saved permit mismatch");

  if(pipe(done) < 0)
    fail("event pipe");
  for(int i = 0; i < EVENT_WAITERS; i++){
    int pid = fork();
    if(pid < 0)
      fail("event fork");
    if(pid == 0){
      close(done[0]);
      if(semwait(handle) < 0)
        exit(1);
      write_exact(done[1], &i, sizeof(i));
      close(done[1]);
      exit(0);
    }
    pids[i] = pid;
  }
  close(done[1]);

  wait_for_waiters(handle, EVENT_WAITERS, &info);
  if(info.value != 0)
    fail("blocked event has permit");
  for(int i = 0; i < EVENT_WAITERS; i++)
    if(sempost(handle) < 0)
      fail("event post");

  for(int i = 0; i < EVENT_WAITERS; i++){
    int worker;
    read_exact(done[0], &worker, sizeof(worker));
    if(worker < 0 || worker >= EVENT_WAITERS)
      fail("event worker id");
  }
  close(done[0]);
  wait_children_ok(pids, EVENT_WAITERS, "event");

  if(seminfo(handle, &info) < 0 || info.value != 0 || info.waiters != 0 ||
     info.successful_waits != EVENT_WAITERS + 1 ||
     info.posts != EVENT_WAITERS + 1 || info.wake_calls == 0)
    fail("event final state");
  printf("SEMAPHORE event saved_permit=1 blocked=%d wake_calls=%d final=0\n",
         EVENT_WAITERS, (int)info.wake_calls);
  if(semdestroy(handle) < 0)
    fail("event destroy");
}

/**
 * 用计数信号量限制同时占用资源的进程数，并由父进程重放进入/退出事件。
 */
static void
test_resource_counting(void)
{
  struct semaphore_info resource_info;
  struct semaphore_info release_info;
  int pids[RESOURCE_WORKERS];
  int events[2];
  int resource = semcreate(RESOURCE_LIMIT, RESOURCE_LIMIT);
  int release_gate = semcreate(0, RESOURCE_WORKERS);

  if(resource < 0 || release_gate < 0 || pipe(events) < 0)
    fail("resource setup");

  for(int i = 0; i < RESOURCE_WORKERS; i++){
    int pid = fork();
    if(pid < 0)
      fail("resource fork");
    if(pid == 0){
      struct worker_event event;

      close(events[0]);
      if(semwait(resource) < 0)
        exit(1);
      event.type = EVENT_ENTER;
      event.worker = i;
      write_exact(events[1], &event, sizeof(event));
      if(semwait(release_gate) < 0)
        exit(2);
      event.type = EVENT_EXIT;
      write_exact(events[1], &event, sizeof(event));
      if(sempost(resource) < 0)
        exit(3);
      close(events[1]);
      exit(0);
    }
    pids[i] = pid;
  }
  close(events[1]);

  for(int tick = 0; tick < POLL_TICKS; tick++){
    if(seminfo(resource, &resource_info) == 0 &&
       seminfo(release_gate, &release_info) == 0 &&
       resource_info.waiters == RESOURCE_WORKERS - RESOURCE_LIMIT &&
       release_info.waiters == RESOURCE_LIMIT)
      break;
    if(tick == POLL_TICKS - 1)
      fail("resource blocked state timeout");
    sleep(1);
  }

  for(int i = 0; i < RESOURCE_WORKERS; i++)
    if(sempost(release_gate) < 0)
      fail("release gate post");

  int active = 0;
  int maximum = 0;
  for(int i = 0; i < RESOURCE_WORKERS * 2; i++){
    struct worker_event event;

    read_exact(events[0], &event, sizeof(event));
    if(event.worker < 0 || event.worker >= RESOURCE_WORKERS)
      fail("resource worker id");
    if(event.type == EVENT_ENTER){
      active++;
      if(active > maximum)
        maximum = active;
      if(active > RESOURCE_LIMIT)
        fail("resource limit exceeded");
    } else if(event.type == EVENT_EXIT){
      active--;
      if(active < 0)
        fail("resource active underflow");
    } else {
      fail("resource event type");
    }
  }
  close(events[0]);
  wait_children_ok(pids, RESOURCE_WORKERS, "resource");

  if(active != 0 || maximum != RESOURCE_LIMIT)
    fail("resource event replay mismatch");
  if(seminfo(resource, &resource_info) < 0 ||
     seminfo(release_gate, &release_info) < 0 ||
     resource_info.value != RESOURCE_LIMIT || resource_info.waiters != 0 ||
     release_info.value != 0 || release_info.waiters != 0)
    fail("resource final state");

  printf("SEMAPHORE resource limit=%d max_active=%d final_permits=%d\n",
         RESOURCE_LIMIT, maximum, resource_info.value);
  if(semdestroy(resource) < 0 || semdestroy(release_gate) < 0)
    fail("resource destroy");
}

/**
 * 把初始值为 1 的二元信号量作为临界区门票，保护跨进程文件计数器。
 */
static void
test_binary_mutex_use(void)
{
  char *path = "sem-mutex";
  int initial = 0;
  int pids[MUTEX_WORKERS];
  int mutex = semcreate(1, 1);

  if(mutex < 0)
    fail("mutex create");
  create_int_file(path, &initial, 1);

  int non_owner = fork();
  if(non_owner < 0)
    fail("non-owner fork");
  if(non_owner == 0)
    exit(semdestroy(mutex) == -1 ? 0 : 1);
  wait_child_ok(non_owner, "non-owner destroy");

  for(int worker = 0; worker < MUTEX_WORKERS; worker++){
    int pid = fork();
    if(pid < 0)
      fail("mutex worker fork");
    if(pid == 0){
      for(int round = 0; round < MUTEX_LOOPS; round++){
        int value;

        if(semwait(mutex) < 0)
          exit(1);
        value = read_int_slot(path, 0);
        sleep(1);
        write_int_slot(path, 0, value + 1);
        if(sempost(mutex) < 0)
          exit(2);
      }
      exit(0);
    }
    pids[worker] = pid;
  }

  wait_children_ok(pids, MUTEX_WORKERS, "mutex");
  int final = read_int_slot(path, 0);
  if(final != MUTEX_WORKERS * MUTEX_LOOPS)
    fail("mutex counter mismatch");
  if(unlink(path) < 0 || semdestroy(mutex) < 0)
    fail("mutex cleanup");

  printf("SEMAPHORE mutex workers=%d loops=%d final=%d non_owner_destroy=reject\n",
         MUTEX_WORKERS, MUTEX_LOOPS, final);
}

/**
 * 用 empty/full 两个计数信号量驱动单生产者、单消费者的循环缓冲区。
 */
static void
test_bounded_buffer(void)
{
  char *path = "sem-buffer";
  int slots[BUFFER_CAPACITY] = {0};
  int empty = semcreate(BUFFER_CAPACITY, BUFFER_CAPACITY);
  int full = semcreate(0, BUFFER_CAPACITY);
  struct semaphore_info empty_info;
  struct semaphore_info full_info;

  if(empty < 0 || full < 0)
    fail("buffer semcreate");
  create_int_file(path, slots, BUFFER_CAPACITY);

  int producer = fork();
  if(producer < 0)
    fail("producer fork");
  if(producer == 0){
    for(int item = 0; item < BUFFER_ITEMS; item++){
      if(semwait(empty) < 0)
        exit(1);
      write_int_slot(path, item % BUFFER_CAPACITY, 1000 + item);
      if(sempost(full) < 0)
        exit(2);
    }
    exit(0);
  }

  int consumer = fork();
  if(consumer < 0)
    fail("consumer fork");
  if(consumer == 0){
    for(int item = 0; item < BUFFER_ITEMS; item++){
      if(semwait(full) < 0)
        exit(1);
      if(read_int_slot(path, item % BUFFER_CAPACITY) != 1000 + item)
        exit(2);
      if(sempost(empty) < 0)
        exit(3);
    }
    exit(0);
  }

  wait_child_ok(producer, "producer");
  wait_child_ok(consumer, "consumer");
  if(seminfo(empty, &empty_info) < 0 || seminfo(full, &full_info) < 0 ||
     empty_info.value != BUFFER_CAPACITY || full_info.value != 0 ||
     empty_info.waiters != 0 || full_info.waiters != 0)
    fail("buffer final counts");
  if(unlink(path) < 0 || semdestroy(empty) < 0 || semdestroy(full) < 0)
    fail("buffer cleanup");

  printf("SEMAPHORE buffer capacity=%d produced=%d consumed=%d empty=%d full=%d\n",
         BUFFER_CAPACITY, BUFFER_ITEMS, BUFFER_ITEMS,
         empty_info.value, full_info.value);
}

/** 验证显式销毁会唤醒等待者，创建者退出会清理遗留对象。 */
static void
test_destroy_and_exit_cleanup(void)
{
  struct semaphore_info info;
  int blocked = semcreate(0, 1);
  int waiter;

  if(blocked < 0)
    fail("cleanup create");
  waiter = fork();
  if(waiter < 0)
    fail("cleanup waiter fork");
  if(waiter == 0)
    exit(semwait(blocked) == -1 ? 0 : 1);

  wait_for_waiters(blocked, 1, &info);
  if(semdestroy(blocked) < 0)
    fail("destroy blocked semaphore");
  wait_child_ok(waiter, "destroy wake");
  if(seminfo(blocked, &info) != -1)
    fail("destroyed handle remains visible");

  int handles[2];
  if(pipe(handles) < 0)
    fail("owner cleanup pipe");
  int owner = fork();
  if(owner < 0)
    fail("owner cleanup fork");
  if(owner == 0){
    int handle;

    close(handles[0]);
    handle = semcreate(0, 1);
    if(handle < 0)
      exit(1);
    write_exact(handles[1], &handle, sizeof(handle));
    close(handles[1]);
    exit(0);
  }
  close(handles[1]);
  int stale_handle;
  read_exact(handles[0], &stale_handle, sizeof(stale_handle));
  close(handles[0]);
  wait_child_ok(owner, "owner exit");
  if(seminfo(stale_handle, &info) != -1 || sempost(stale_handle) != -1 ||
     semwait(stale_handle) != -1)
    fail("owner exit left live handle");

  int replacement = semcreate(1, 1);
  if(replacement < 0 || replacement == stale_handle ||
     seminfo(stale_handle, &info) != -1)
    fail("stale generation reused");
  if(semdestroy(replacement) < 0)
    fail("replacement destroy");

  printf("SEMAPHORE cleanup destroy_woke_waiter=1 owner_exit_invalid=1 stale_rejected=1\n");
}

int
main(void)
{
  test_count_boundaries();
  test_event_ordering();
  test_resource_counting();
  test_binary_mutex_use();
  test_bounded_buffer();
  test_destroy_and_exit_cleanup();
  printf("semaphoretest: OK\n");
  exit(0);
}
