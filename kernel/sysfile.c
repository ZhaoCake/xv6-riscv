/*
 * sysfile.c — 文件系统相关的系统调用实现。
 *
 * 这里实现的系统调用负责「桥接」的功能：
 *   ① 从用户空间取参数（argstr / argint / argaddr）
 *   ② 做权限和合法性检查
 *   ③ 调用底层的 file.c 和 fs.c 完成实际工作
 *
 * 涉及的系统调用：open, read, write, close, dup, link, unlink,
 *                mkdir, mknod, chdir, fstat, pipe, exec
 *
 * 核心调用层次关系：
 *   sys_open → create/namei → fs.c (inode 操作)
 *            → filealloc / fdalloc → file.c (文件描述符)
 *   sys_read → fileread → file.c → fs.c
 *   sys_exec → kexec → exec.c
 *   sys_pipe → pipealloc → pipe.c
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

/*
 * 取第 n 个系统调用参数，把它当作文件描述符（fd），
 * 同时返回 fd 号和对应的 struct file 指针。
 *
 * 参数：
 *   n  — 参数序号（0 = 第一个参数）
 *   pfd — 输出：fd 号（可为 0 表示不关心）
 *   pf  — 输出：对应的 file 结构体指针
 *
 * 返回 0 成功，-1 失败（非法 fd 或对应 file 不存在）
 */
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

/*
 * 为给定的 file 结构体分配一个空闲的文件描述符。
 *
 * 进程的 ofile[] 数组保存了它打开的所有文件（最多 NOFILE 个）。
 * 遍历找到第一个空闲的槽位，把 file 指针放进去。
 *
 * 成功后，file 的引用计数由 filedup() 或 pipealloc() 等函数增加，
 * 这里只负责"占槽位"。
 */
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

/*
 * dup(OLD_FD) — 复制一个文件描述符。
 *
 * 作用：创建一个新的 fd，指向和 old_fd 相同的 file 结构体。
 * 新 fd 是当前最小的空闲 fd 号。
 *
 * 典型用途：
 *   1. 重定向：close(1) + dup(pipe[1]) → stdout 指向管道写端
 *   2. 在 fork 前备份 stdin/stdout，子进程恢复时用
 *
 * filedup() 递增 file 的引用计数，
 * 所以两个 fd 各自独立 close 时，最后一个才会真正释放 file。
 */
uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

/*
 * read(FD, BUF, N) — 从文件描述符 fd 读取最多 n 个字节到 buf。
 *
 * 参数来自用户寄存器，需要：
 *   argfd(0)  → fd + file 结构体
 *   argaddr(1) → buf 在用户空间的地址
 *   argint(2)  → 要读的字节数 n
 *
 * 实际工作委托给 fileread()（在 file.c 中），
 * 它会根据 file 的类型（管道 / inode / 设备）走不同的读路径。
 *
 * 返回值：实际读到的字节数，0 表示 EOF，-1 表示出错。
 */
uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

/*
 * write(FD, BUF, N) — 向文件描述符 fd 写入 n 个字节。
 *
 * 参数布局与 read 相同：fd / buf / n。
 *
 * pipe 的 write 走 pipewrite()——每次写一个字节到环形缓冲区，
 * 满了就 sleep 等读端腾出空间。
 * 磁盘文件的 write 走 filewrite()——写入 inode 的缓冲区。
 */
uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

/*
 * close(FD) — 关闭文件描述符。
 *
 * 两步：
 *   ① 从进程的 ofile[] 中移除这个 fd 的引用
 *   ② fileclose(f) 递减 file 的引用计数
 *      如果引用计数归零，才真正释放 file 结构体和底层资源
 *      （管道的话释放 pipe 结构体，inode 的话释放 inode 引用）
 */
uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

/*
 * fstat(FD, STAT) — 获取文件描述符 fd 指向的文件的状态信息。
 *
 * 把 stat 结构体（包含文件类型、大小、inode 号等）拷贝到
 * 用户空间的 st 指针处。
 *
 * stat 结构体的定义在 kernel/stat.h 中：
 *   dev     — 设备号
 *   ino     — inode 编号
 *   type    — 文件类型（T_DIR / T_FILE / T_DEVICE）
 *   nlink   — 硬链接数
 *   size    — 文件大小
 */
uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

/*
 * link(OLD_PATH, NEW_PATH) — 创建硬链接。
 *
 * 硬链接的本质：让一个新路径名指向同一个 inode。
 * 在文件系统中，这意味着：
 *   ① 在 NEW_PATH 的父目录中新建一个目录项
 *   ② 把那个目录项的 inum 设为 OLD_PATH 的 inode 号
 *   ③ 增加 inode 的 nlink 引用计数
 *
 * 限制：
 *   - 不能对目录创建硬链接（防止环）
 *   - 不能跨设备（不同磁盘的 inode 编号不互通）
 *
 * 如果创建新目录项成功但后续失败，需要回滚 nlink。
 */
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

/*
 * 检查目录 dp 是否为空（仅含 "." 和 ".." 两个目录项）。
 *
 * xv6 的目录项（struct dirent）是固定长度的：
 *   struct dirent { ushort inum; char name[DIRSIZ]; };
 * 每个 16 字节。前两个固定是 "." (inum=self) 和 ".." (inum=parent)。
 * 所以从第 2*sizeof(de) 字节开始扫描，看是否有 inum != 0 的项。
 */
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

/*
 * unlink(PATH) — 删除路径名对应的目录项。
 *
 * 步骤：
 *   ① nameiparent() 找到父目录和文件名
 *   ② dirlookup() 找到文件名对应的 inode
 *   ③ 如果是目录且非空 → 不允许删除
 *   ④ 把父目录中该目录项清零（writei 写一个全零的 dirent）
 *   ⑤ 递减 inode 的 nlink
 *
 * nlink 归零后，inode 的磁盘空间和数据块会在
 * iput() → itrunc() → ifree() 中真正释放。
 *
 * 注意：如果是删除目录，父目录的 nlink 也要减 1（因为失去了 ".." 的引用）。
 * 这是 inode 引用计数的一部分。
 */
uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // 不能删除 "." 和 ".."（会导致引用计数混乱）
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  // 清空目录项（写一个全零的 dirent）
  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

/*
 * create(PATH, TYPE, MAJOR, MINOR) — 创建一个新的 inode（文件/目录/设备）。
 *
 * 这是 sys_open(O_CREATE)、sys_mkdir、sys_mknod 的公共辅助函数。
 *
 * 流程：
 *   ① nameiparent(path, &name) 找到父目录 inode 和文件名
 *   ② dirlookup() 检查文件是否已存在——
 *      如果已存在且类型匹配（文件或设备），直接返回已有的 inode
 *      （open 的 O_CREATE 不要求排他，允许打开已有文件）
 *   ③ ialloc() 在磁盘上分配一个新的 inode
 *   ④ 如果是目录，创建 "." 和 ".." 两个目录项，
 *      并递增父目录的 nlink（因为 ".." 指向父目录）
 *   ⑤ dirlink() 在父目录中添加新文件名 → inum 的映射
 *
 * 如果过程中出错（比如创建 "." 失败），需要回收已分配的 inode。
 */
static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // 创建目录时，建立 "." 和 ".." 两个隐含项
    // "." 指向自己，但不递增 nlink，避免循环引用计数
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // 此时所有步骤已成功，父目录的 nlink++（因为多了个 ".."）
    dp->nlink++;
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

fail:
  // 出错了，回收已分配的 inode
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

/*
 * open(PATH, O_MODE) — 打开一个文件，返回文件描述符。
 *
 * xv6 的 open 兼顾了多种场景，看参数 omode：
 *
 *   O_CREATE → 调用 create() 创建新文件（如果已存在则打开）
 *   否则     → namei() 按路径查找已有文件
 *
 *   打开目录时只能 O_RDONLY（不能写目录本身）
 *
 *   如果文件是 T_DEVICE（设备文件），
 *   file 结构的 type 设为 FD_DEVICE，记录 major 设备号；
 *   否则设为 FD_INODE，维护当前文件偏移 off。
 *
 *   O_TRUNC 表示打开时截断文件（清空数据块）。
 *
 * 成功返回 fd；失败返回 -1。
 */
uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

/*
 * mkdir(PATH) — 创建目录。
 *
 * 委托给 create() 完成：分配 inode (T_DIR)、创建 "." 和 ".."，
 * 以及把新文件名写入父目录。
 *
 * 注意：create() 中有一个细节——目录成功创建后，
 * 父目录的 nlink++（因为新目录的 ".." 指向父目录）。
 */
uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

/*
 * mknod(PATH, MAJOR, MINOR) — 创建设备文件节点。
 *
 * 设备文件本身不占用磁盘空间，只是 inode 中记录了 major/minor 设备号。
 * 当用户程序 open 一个设备文件时，file 结构体会记录 FD_DEVICE 类型
 * 和设备号，后续 read/write 会转发到对应的设备驱动。
 *
 * xv6 只有几个设备：
 *   console（major=1）— 终端输入输出
 *   virtio disk — 块设备
 */
uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

/*
 * chdir(PATH) — 更改当前工作目录。
 *
 * 核心操作只有两步：
 *   ① 释放旧 cwd 的 inode 引用（iput）
 *   ② 把新路径对应的 inode 设为进程的 cwd
 *
 * 注意 cwd 是 struct proc 中保存的 inode 指针，
 * 而不是一个字符串路径。所以 chdir 不涉及字符串拼接——
 * 后续 namei("foo") 解析相对路径时，会从 cwd 开始查找。
 *
 * 这也是为什么 shell 必须把 cd 作为内置命令：
 * fork 出来的子进程改了 cwd，父进程不受影响。
 */
uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

/*
 * exec 系统调用的用户态→内核态桥接函数。
 *
 * 系统调用的参数来自用户寄存器，需要：
 *   ① argstr(0, path) — 从用户空间取出第 0 个参数（可执行文件路径名）
 *   ② argaddr(1, &uargv) — 取出第 1 个参数（argv 数组在用户空间的地址）
 *   ③ 遍历用户空间的 argv 数组，用 fetchstr() 逐个把字符串
 *      从用户页表拷贝到内核页表（kalloc 分配的临时缓冲区）
 *   ④ 调用 kexec() 做真正的加载工作
 *   ⑤ 释放临时分配的 argv 缓冲
 */
uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

/*
 * pipe(FDARRAY) — 创建管道，返回一对文件描述符。
 *
 * 用户空间调用：int p[2]; pipe(p);
 * 内核要做的事：
 *   ① pipealloc() 分配一个 pipe 结构体 + 两个 file 结构体
 *      （rf = 读端 file，wf = 写端 file，都指向同一个 pipe）
 *   ② fdalloc() 把这两个 file 放入进程的 ofile[] 表，分配 fd 号
 *   ③ copyout() 把 fd0（读端）和 fd1（写端）写回用户空间的 p[2] 数组
 *
 * 任何一步失败都需要回滚已经分配的资源。
 */
uint64
sys_pipe(void)
{
  uint64 fdarray; // 用户空间的 int p[2] 数组地址
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) <
        0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
