from pathlib import Path
from textwrap import dedent


def replace_function(path: str, name: str, replacement: str) -> None:
    file_path = Path(path)
    text = file_path.read_text()
    marker = f"\n{name}("

    if text.count(marker) != 1:
        raise SystemExit(f"{path}: expected one definition marker for {name}")

    name_start = text.index(marker) + 1
    function_start = text.rfind("\n", 0, name_start - 1) + 1
    body_start = text.index("{", name_start)

    depth = 0
    function_end = None
    for index in range(body_start, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                function_end = index + 1
                break

    if function_end is None:
        raise SystemExit(f"{path}: unmatched braces while locating {name}")

    if function_end < len(text) and text[function_end] == "\n":
        function_end += 1

    replacement = dedent(replacement).lstrip("\n")
    if not replacement.endswith("\n"):
        replacement += "\n"

    file_path.write_text(text[:function_start] + replacement + text[function_end:])


replace_function(
    "kernel/sysfile.c",
    "sys_open",
    r'''
// Caller must hold ip's inode lock.
static int
read_symlink_target_locked(struct inode *ip, char *path)
{
  int len;

  if(readi(ip, 0, (uint64)&len, 0, sizeof(len)) < sizeof(len))
    return -1;
  if(readi(ip, 0, (uint64)path, sizeof(len), len + 1) < len + 1)
    return -1;

  return 0;
}

// Follow symbolic links and return an unlocked inode reference.
static struct inode*
follow_symlinks(struct inode *ip, char *path)
{
  int remaining = MAXSYMLINK;

  while(remaining-- > 0){
    ilock(ip);

    if(ip->type != T_SYMLINK){
      iunlock(ip);
      break;
    }

    if(read_symlink_target_locked(ip, path) < 0){
      iunlockput(ip);
      return 0;
    }

    iunlockput(ip);
    ip = namei(path);
    if(ip == 0)
      return 0;
  }

  if(remaining <= 0){
    iput(ip);
    return 0;
  }

  return ip;
}

// Return the inode locked on success.
static struct inode*
open_inode_locked(char *path, int omode)
{
  struct inode *ip;

  if(omode & O_CREATE)
    return create(path, T_FILE, 0, 0);

  ip = namei(path);
  if(ip == 0)
    return 0;

  if((omode & O_NOFOLLOW) == 0){
    ip = follow_symlinks(ip, path);
    if(ip == 0)
      return 0;
  }

  ilock(ip);
  return ip;
}

// Caller must hold ip's inode lock.
static int
can_open_inode_locked(struct inode *ip, int omode)
{
  if(ip->type == T_DIR && omode != O_RDONLY)
    return 0;
  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
    return 0;

  return 1;
}

static int
alloc_open_file(struct file **out)
{
  struct file *f;
  int fd;

  f = filealloc();
  if(f == 0)
    return -1;

  fd = fdalloc(f);
  if(fd < 0){
    fileclose(f);
    return -1;
  }

  *out = f;
  return fd;
}

// Caller must hold ip's inode lock.
static void
init_open_file(struct file *f, struct inode *ip, int omode)
{
  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }

  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, path, MAXPATH) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  ip = open_inode_locked(path, omode);
  if(ip == 0)
    goto fail_transaction;

  if(!can_open_inode_locked(ip, omode))
    goto fail_inode;

  fd = alloc_open_file(&f);
  if(fd < 0)
    goto fail_inode;

  init_open_file(f, ip, omode);

  if((omode & O_TRUNC) && ip->type == T_FILE)
    itrunc(ip);

  iunlock(ip);
  end_op();
  return fd;

fail_inode:
  iunlockput(ip);
fail_transaction:
  end_op();
  return -1;
}
''',
)

replace_function(
    "kernel/sysfile.c",
    "sys_symlink",
    r'''
// Caller must hold ip's inode lock.
static int
write_symlink_target_locked(struct inode *ip, char *target)
{
  int len = strlen(target);

  if(writei(ip, 0, (uint64)&len, 0, sizeof(len)) < sizeof(len))
    return -1;
  if(writei(ip, 0, (uint64)target, sizeof(len), len + 1) < 0)
    return -1;

  return 0;
}

uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;
  int result;

  if(argstr(0, target, MAXPATH) < 0 ||
     argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();

  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }

  result = write_symlink_target_locked(ip, target);

  iunlockput(ip);
  end_op();
  return result;
}
''',
)

replace_function(
    "kernel/log.c",
    "begin_op",
    r'''
// Caller must hold log.lock.
static int
log_has_space_for_new_op_locked(void)
{
  int reserved;

  reserved = log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS;
  return reserved <= LOGSIZE;
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);

  while(log.committing || !log_has_space_for_new_op_locked())
    sleep(&log, &log.lock);

  log.outstanding += 1;
  release(&log.lock);
}
''',
)

replace_function(
    "kernel/log.c",
    "end_op",
    r'''
// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  acquire(&log.lock);
  log.outstanding -= 1;

  if(log.committing)
    panic("log.committing");

  if(log.outstanding != 0){
    // A waiting begin_op() may now fit in the reserved log space.
    wakeup(&log);
    release(&log.lock);
    return;
  }

  // Block new operations before dropping log.lock for disk I/O.
  log.committing = 1;
  release(&log.lock);

  commit();

  acquire(&log.lock);
  log.committing = 0;
  wakeup(&log);
  release(&log.lock);
}
''',
)
