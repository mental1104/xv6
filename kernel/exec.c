#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "vma.h"
#include "fcntl.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

/**
 * 将一个以空指针结尾的用户字符串向量复制到内核临时页。
 *
 * @param user_vector 用户地址空间中的指针数组；0 表示空向量。
 * @param vector 接收内核字符串指针，容量必须为 maximum + 1。
 * @param maximum 允许复制的非空字符串数量。
 * @return 成功返回 0；地址非法、超过数量上限或内存不足返回 -1。
 *
 * 成功时由调用者通过 free_kernel_vector() 释放。失败时本函数已经释放所有已分配页。
 */
static int
fetch_user_vector(uint64 user_vector, char **vector, int maximum)
{
  uint64 user_string;
  int i;

  memset(vector, 0, (maximum + 1) * sizeof(char *));
  if(user_vector == 0)
    return 0;

  for(i = 0; ; i++){
    if(i > maximum)
      goto bad;
    if(fetchaddr(user_vector + sizeof(uint64) * i, &user_string) < 0)
      goto bad;
    if(user_string == 0){
      vector[i] = 0;
      return 0;
    }
    // 下标 maximum 只为读取结尾空指针保留，不能再接受非空字符串。
    if(i == maximum)
      goto bad;
    vector[i] = kalloc();
    if(vector[i] == 0)
      goto bad;
    if(fetchstr(user_string, vector[i], PGSIZE) < 0)
      goto bad;
  }

 bad:
  for(i = 0; i < maximum && vector[i] != 0; i++){
    kfree(vector[i]);
    vector[i] = 0;
  }
  return -1;
}

/**
 * 释放 fetch_user_vector() 产生的内核字符串向量。
 *
 * @param vector 以空指针结尾的内核字符串指针数组。
 * @param maximum 数组允许持有的最大非空元素数量。
 */
static void
free_kernel_vector(char **vector, int maximum)
{
  int i;

  for(i = 0; i < maximum && vector[i] != 0; i++){
    kfree(vector[i]);
    vector[i] = 0;
  }
}

/**
 * 用 ELF 文件替换当前进程用户镜像，并同时建立 argv 与 envp。
 *
 * @param path ELF 文件路径。
 * @param argv 以空指针结尾的参数字符串向量，最多 MAXARG 项。
 * @param envp 以空指针结尾的环境字符串向量，最多 MAXENV 项。
 * @return 成功返回 argc，并通过 trapframe 的 a1/a2 传递 argv/envp；失败返回 -1。
 *
 * 所有字符串和两个指针数组都放入新用户栈。完成提交前不修改当前进程页表，失败时
 * 释放临时镜像并保留旧进程可继续运行。
 */
int
execve(char *path, char **argv, char **envp)
{
  char *s, *last;
  int i, off;
  uint64 argc, envc, sz = 0, sp, stackbase;
  uint64 uargv[MAXARG + 1], uenvp[MAXENV + 1];
  uint64 argv_address, envp_address;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  pagetable_t kpagetable = 0, oldkpagetable;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr || ph.vaddr + ph.memsz > USERMAX)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    sz = sz1;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if(sz > USERMAX - 2*PGSIZE)
    goto bad;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // 先复制 argv 字符串；每个字符串后重新对齐 RISC-V 栈指针。
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    uargv[argc] = sp;
  }
  uargv[argc] = 0;

  // envp 与 argv 共享同一用户栈页，但拥有独立的数量上限和指针数组。
  for(envc = 0; envp[envc]; envc++) {
    if(envc >= MAXENV)
      goto bad;
    sp -= strlen(envp[envc]) + 1;
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, envp[envc], strlen(envp[envc]) + 1) < 0)
      goto bad;
    uenvp[envc] = sp;
  }
  uenvp[envc] = 0;

  // 先压入 envp 指针数组，再压入 argv；两者地址分别交给 a2 和 a1。
  sp -= (envc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)uenvp,
             (envc + 1) * sizeof(uint64)) < 0)
    goto bad;
  envp_address = sp;

  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)uargv,
             (argc + 1) * sizeof(uint64)) < 0)
    goto bad;
  argv_address = sp;

  // 为新用户镜像构造独立的进程内核页表。失败路径尚未替换 p 的任何状态，
  // 因而可以直接释放临时页表，不会留下指向已释放用户页的别名。
  if((kpagetable = kvmcreate()) == 0)
    goto bad;
  if(u2kvmcopy(pagetable, kpagetable, 0, sz) < 0)
    goto bad;

  // main(argc, argv, envp) 分别通过 a0、a1、a2 接收三个入口参数。
  p->trapframe->a1 = argv_address;
  p->trapframe->a2 = envp_address;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image. 先释放旧 VMA 的文件引用和写回状态，再切换到
  // 新页表；切换 satp 后才能安全释放当前正在使用的旧 kpagetable。
  vma_unmap_all(p);
  oldpagetable = p->pagetable;
  oldkpagetable = p->kpagetable;
  p->pagetable = pagetable;
  p->kpagetable = kpagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter
  p->trapframe->sp = sp; // initial stack pointer
  w_satp(MAKE_SATP(p->kpagetable));
  sfence_vma();
  proc_freepagetable(oldpagetable, oldsz);
  kvmfree(oldkpagetable);

  return argc; // this ends up in a0, the first argument to main(argc, argv, envp)

 bad:
  if(kpagetable)
    kvmfree(kpagetable);
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

/**
 * 保留原 exec(path, argv) ABI，并为新镜像提供空环境向量。
 *
 * @param path ELF 文件路径。
 * @param argv 以空指针结尾的参数向量。
 * @return 与 execve() 相同；成功返回 argc，失败返回 -1。
 */
int
exec(char *path, char **argv)
{
  char *empty_environment[] = {0};

  return execve(path, argv, empty_environment);
}

/**
 * 从用户地址空间读取 argv/envp，并调用内核 execve() 完成镜像替换。
 *
 * @return 成功返回 argc；路径、向量、字符串、数量或内存非法时返回 -1。
 */
uint64
sys_execve(void)
{
  char path[MAXPATH];
  char *argv[MAXARG + 1];
  char *envp[MAXENV + 1];
  uint64 user_argv;
  uint64 user_envp;
  int result;

  if(argstr(0, path, MAXPATH) < 0 ||
     argaddr(1, &user_argv) < 0 ||
     argaddr(2, &user_envp) < 0)
    return -1;
  if(fetch_user_vector(user_argv, argv, MAXARG) < 0)
    return -1;
  if(fetch_user_vector(user_envp, envp, MAXENV) < 0){
    free_kernel_vector(argv, MAXARG);
    return -1;
  }

  result = execve(path, argv, envp);
  free_kernel_vector(argv, MAXARG);
  free_kernel_vector(envp, MAXENV);
  return result;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  if(va % PGSIZE != 0)
    panic("loadseg: va must be page aligned");

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }

  return 0;
}
