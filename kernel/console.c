//
// Console input and output, to the uart.
// Cooked mode reads a line at a time and implements the historical xv6 editing
// controls. Raw mode is a small teaching interface used only by the interactive
// shell so user space can receive every input byte and own line editing.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "console.h"
#include "jobctl.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart.
// called by printf, and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

/**
 * 保存 console 输入环形缓冲区、最小行规约和单控制台前台进程组。
 *
 * raw_owner 和 raw_file 共同标识唯一允许读取、重复设置和释放 raw mode 的 Shell。
 * fg_pgid 表示当前可读取终端输入的进程组；Ctrl-C/Ctrl-Z 只在 cons.lock 下记录
 * pending action，真正的进程表扫描由安全锁边界外的 console_apply_pending_control()
 * 完成。所有字段均受 cons.lock 保护。
 */
struct {
  struct spinlock lock;

  // input
#define INPUT_BUF 128
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
  int raw;
  int raw_owner;
  struct file *raw_file;
  int shell_pgid;
  int fg_pgid;
  int control_pgid;
  int control_action;
} cons;

/**
 * 返回当前进程指定 fd 对应的可读 console 文件对象。
 *
 * @param p 当前进程，函数只读取其打开文件表。
 * @param fd 待校验的文件描述符编号。
 * @return 校验通过时返回内部 file 指针；非 console、不可读或无效 fd 返回 0。
 */
static struct file*
console_file(struct proc *p, int fd)
{
  struct file *file;

  if(fd < 0 || fd >= NOFILE || (file = p->ofile[fd]) == 0)
    return 0;
  if(file->type != FD_DEVICE || file->major != CONSOLE || !file->readable)
    return 0;
  return file;
}

/**
 * 在持有 cons.lock 时恢复 cooked mode 并清空未消费的 raw 字节。
 *
 * 调用者必须已经验证 owner，并持有 cons.lock。函数只修改内存和执行 wakeup，
 * 不进行可能睡眠的文件系统或设备操作。
 */
static void
clear_raw_locked(void)
{
  cons.raw = 0;
  cons.raw_owner = 0;
  cons.raw_file = 0;
  // raw 字节没有 cooked 行边界，恢复时统一丢弃，避免污染下一次逐行读取。
  cons.r = cons.w = cons.e;
  wakeup(&cons.r);
}

/**
 * 在持有 cons.lock 时切换 console 输入模式。
 *
 * @param p 发起切换的 Shell 进程。
 * @param file 本次系统调用校验后的可读 console 文件对象。
 * @param mode CONSOLE_MODE_COOKED 或 CONSOLE_MODE_RAW。
 * @return 成功返回 0；owner、文件对象、输入边界或 mode 不满足约束时返回 -1。
 *
 * raw claim 只允许从空缓冲区开始，避免把内核已编辑或已提交的一部分 cooked 行
 * 交给用户态解释。重复设置只有同一 PID 和同一 file 对象可以成功；非 owner 不能
 * 释放活跃 raw mode。owner 异常退出时由 fileclose() 调用 consolefileclose() 回收。
 */
static int
set_console_mode(struct proc *p, struct file *file, int mode)
{
  int result = 0;

  acquire(&cons.lock);
  if(mode == CONSOLE_MODE_RAW){
    // 只有当前前台 Shell 可以接管逐字节输入；后台进程不能借 raw mode 绕过隔离。
    if(cons.fg_pgid != 0 && p->pgid != cons.fg_pgid){
      result = -1;
    } else if(cons.raw){
      if(cons.raw_owner != p->pid || cons.raw_file != file)
        result = -1;
    } else if(cons.r != cons.w || cons.w != cons.e){
      result = -1;
    } else {
      cons.raw = 1;
      cons.raw_owner = p->pid;
      cons.raw_file = file;
    }
  } else if(mode == CONSOLE_MODE_COOKED){
    if(cons.raw){
      if(cons.raw_owner != p->pid || cons.raw_file != file)
        result = -1;
      else
        clear_raw_locked();
    }
  } else {
    result = -1;
  }
  release(&cons.lock);
  return result;
}

/**
 * 在 owner 关闭其 console 文件对象时回收 raw mode。
 *
 * @param file 正在由 fileclose() 释放引用的文件对象。
 * @param pid 执行关闭操作的当前进程 PID。
 *
 * fileclose() 在进程正常 exit、kill 后退出及用户 trap 异常退出时都会经过此路径。
 * 非 owner、不同 file 或 cooked mode 均为无操作。该函数不持有 ftable.lock，也不睡眠。
 */
void
consolefileclose(struct file *file, int pid)
{
  acquire(&cons.lock);
  if(cons.raw && cons.raw_owner == pid && cons.raw_file == file)
    clear_raw_locked();
  release(&cons.lock);
}

/**
 * consolemode 为交互式 xv6 Shell 切换最小 raw/cooked 输入语义。
 *
 * 系统调用参数为 `(fd, mode)`。fd 必须是当前进程打开的可读 console；pipe、普通
 * 文件或其他设备会失败。首版用进程名 `sh` 限制模式所有者，这是 xv6 单 console
 * 教学约束，不等价于 POSIX controlling terminal、session 或前台进程组。
 *
 * @return 成功返回 0；参数、fd、调用者或所有权不满足约束时返回 -1。
 */
uint64
sys_consolemode(void)
{
  struct proc *p = myproc();
  struct file *file;
  int fd;
  int mode;
  int pgid;

  if(argint(0, &fd) < 0 || argint(1, &mode) < 0)
    return -1;
  if((file = console_file(p, fd)) == 0 ||
     strncmp(p->name, "sh", sizeof(p->name)) != 0)
    return -1;
  if((pgid = getpgid(0)) < 0)
    return -1;
  p->pgid = pgid;
  return set_console_mode(p, file, mode);
}

/**
 * 将单控制台的前台输入所有权交给一个进程组。
 *
 * 只有最初声明控制台所有权的交互式 sh 进程组可以切换。stdin 为 pipe/文件的
 * 非交互 Shell 返回成功但不修改 console，保持现有 pipe-driven Shell 测试隔离。
 * 目标必须是 Shell 自身或其直接子进程领导的现有进程组。
 */
uint64
sys_tcsetpgrp(void)
{
  struct proc *p = myproc();
  int pgid;
  int caller_pgid;

  if(argint(0, &pgid) < 0 || pgid <= 0 ||
     strncmp(p->name, "sh", sizeof(p->name)) != 0)
    return -1;
  if((caller_pgid = getpgid(0)) < 0)
    return -1;

  // 非交互 Shell 没有 controlling console；不允许它篡改真实交互 Shell 的 fg_pgid。
  if(console_file(p, 0) == 0)
    return 0;
  if(pgid != caller_pgid && getpgid(pgid) != pgid)
    return -1;

  acquire(&cons.lock);
  if(cons.shell_pgid == 0)
    cons.shell_pgid = caller_pgid;
  if(cons.shell_pgid != caller_pgid || cons.raw){
    release(&cons.lock);
    return -1;
  }
  cons.fg_pgid = pgid;
  release(&cons.lock);
  return 0;
}

/**
 * 在 console 锁之外应用 Ctrl-C/Ctrl-Z 记录的进程组动作。
 *
 * 任意 CPU 的 trap 路径都可以调用本函数。调用者只会竞争很短的 cons.lock，取走
 * pending action 后再扫描 proc 表，从而保持 `cons.lock -> p->lock` 不形成锁序。
 */
void
console_apply_pending_control(void)
{
  int pgid;
  int action;

  acquire(&cons.lock);
  pgid = cons.control_pgid;
  action = cons.control_action;
  cons.control_pgid = 0;
  cons.control_action = 0;
  release(&cons.lock);

  if(pgid > 0 && action != 0)
    proc_group_control(pgid, action);
}

//
// user write()s to the console go here.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  acquire(&cons.lock);
  for(i = 0; i < n; i++){
    char c;
    if(either_copyin(&c, user_src, src+i, 1) == -1)
      break;
    uartputc(c);
  }
  release(&cons.lock);

  return i;
}

/**
 * 从 console 读取 cooked 行或 raw 字节。
 *
 * @param user_dst 非零表示 dst 是用户地址，零表示内核地址。
 * @param dst 输出地址。
 * @param n 最大读取字节数。
 * @return 已复制字节数；进程被杀死、copyout 失败、后台进程组或 raw 非 owner
 *         读取返回 -1。
 *
 * cooked mode 保留原 xv6 Ctrl-D 和换行返回语义。raw mode 只允许 owner 读取，并
 * 立即返回当前可用字节；Shell 每次请求一个字节，从而在用户态维护 Escape 状态机。
 */
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;
  struct proc *p = myproc();
  int pid = p->pid;
  int pgid = getpgid(0);

  if(pgid < 0)
    return -1;

  target = n;
  acquire(&cons.lock);
  if(cons.fg_pgid != 0 && cons.fg_pgid != pgid){
    release(&cons.lock);
    return -1;
  }
  if(cons.raw && cons.raw_owner != pid){
    release(&cons.lock);
    return -1;
  }

  while(n > 0){
    // wait until interrupt handler has put some input into cons.buffer.
    while(cons.r == cons.w){
      if(p->killed){
        release(&cons.lock);
        return -1;
      }
      if(cons.fg_pgid != 0 && cons.fg_pgid != pgid){
        release(&cons.lock);
        return -1;
      }
      if(cons.raw && cons.raw_owner != pid){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF];

    if(!cons.raw && c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1){
      release(&cons.lock);
      return -1;
    }

    dst++;
    --n;

    if(cons.raw){
      // raw caller通常只请求一个字节；若请求更多，则一次返回当前已到达的连续字节。
      if(cons.r == cons.w)
        break;
    } else if(c == '\n'){
      // a whole line has arrived, return to the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

/**
 * 接收 UART 输入并按当前模式提交到 console 环形缓冲区。
 *
 * @param c UART 提供的输入字节。
 *
 * Ctrl-P 始终保留为内核 procdump 控制键。当前前台不是 Shell 时，Ctrl-C/Ctrl-Z
 * 分别记录 TERM/STOP；扫描进程组留给 trap 安全点。raw mode 不回显、不转换 CR，
 * 也不解释 Backspace、Ctrl-U 或 Ctrl-D，并在每个字节到达时唤醒 owner。
 */
void
consoleintr(int c)
{
  acquire(&cons.lock);

  if(c == C('P')){
    procdump();
    release(&cons.lock);
    return;
  }

  if((c == C('C') || c == C('Z')) && cons.fg_pgid > 0 &&
     cons.fg_pgid != cons.shell_pgid){
    int action = c == C('C') ? JOBCTL_TERM : JOBCTL_STOP;
    // TERM 优先级高于尚未执行的 STOP，避免快速 Ctrl-Z/Ctrl-C 留下永久停止作业。
    if(cons.control_action == 0 || action == JOBCTL_TERM){
      cons.control_pgid = cons.fg_pgid;
      cons.control_action = action;
    }
    release(&cons.lock);
    return;
  }

  if(cons.raw){
    if(c != 0 && cons.e-cons.r < INPUT_BUF){
      cons.buf[cons.e++ % INPUT_BUF] = c;
      cons.w = cons.e;
      wakeup(&cons.r);
    }
    release(&cons.lock);
    return;
  }

  switch(c){
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f':
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF] = c;

      if(c == '\n' || c == C('D') || cons.e == cons.r+INPUT_BUF){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }

  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
