#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

/*
 * pipe（管道）的内核数据结构。
 *
 * 管道是一个 512 字节的环形缓冲区，配合锁和 sleep/wakeup 实现
 * 「生产者-消费者」模型：
 *
 *   - 写端写入 data[nwrite % PIPESIZE]，然后 nwrite++
 *   - 读端从 data[nread % PIPESIZE] 读取，然后 nread++
 *   - 当 nwrite == nread + PIPESIZE 时，缓冲区满，写端 sleep
 *   - 当 nread == nwrite 时，缓冲区空，读端 sleep
 *   - readopen / writeopen 标记读端/写端是否还打开着
 */
struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;    // 已读取的字节数
  uint nwrite;   // 已写入的字节数
  int readopen;  // 读端 fd 是否仍打开
  int writeopen; // 写端 fd 是否仍打开
};

/*
 * 分配一个管道，返回两个 file 结构体指针（分别对应读端和写端）。
 *
 * 一个 pipe 结构体 + 两个 file 结构体的关系：
 *   file f0 (FD_PIPE, readable=1, writable=0) ──→ pipe pi ←── file f1 (FD_PIPE, readable=0, writable=1)
 *
 * f0 是读端（只能读），f1 是写端（只能写），
 * 两个 file 指向同一个 pipe 结构体。
 */
int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if ((pi = (struct pipe *)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

bad:
  if (pi)
    kfree((char *)pi);
  if (*f0)
    fileclose(*f0);
  if (*f1)
    fileclose(*f1);
  return -1;
}

/*
 * 关闭管道的一端。
 *
 * writable=1 → 关闭写端：标记 writeopen=0，唤醒读端（让它知道不会再写入了）
 * writable=0 → 关闭读端：标记 readopen=0，唤醒写端（让它知道没人读了）
 *
 * 如果两端都关闭了，释放 pipe 结构体本身。
 */
void
pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if (writable) {
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if (pi->readopen == 0 && pi->writeopen == 0) {
    release(&pi->lock);
    kfree((char *)pi);
  } else
    release(&pi->lock);
}

/*
 * 向管道写入 n 个字节（从用户空间地址 addr 读取）。
 *
 * 核心逻辑：
 *   1. 加锁
 *   2. 如果缓冲区满了（nwrite == nread + PIPESIZE），
 *      唤醒读端，自己 sleep 等待对方读走数据
 *   3. 否则从用户空间 copyin 一个字节到环形缓冲区
 *   4. 写完 n 个字节或遇到错误后，唤醒读端
 *
 * copyin() 是从用户进程的页表拷贝到内核空间。
 * 管道每次只传 1 个字节——这是设计选择（简单），
 * 但也是练习中"传一个字节"性能问题的根源之一。
 */
int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while (i < n) {
    if (pi->readopen == 0 || killed(pr)) {
      release(&pi->lock);
      return -1;
    }
    if (pi->nwrite == pi->nread + PIPESIZE) { //DOC: pipewrite-full
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      if (copyin(pr->pagetable, &ch, addr + i, 1) == -1)
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);

  return i;
}

/*
 * 从管道读取最多 n 个字节（写入用户空间地址 addr）。
 *
 * 核心逻辑：
 *   1. 加锁
 *   2. 如果缓冲区空了（nread == nwrite）且写端还开着，
 *      sleep 等待写端写入数据
 *   3. 否则从环形缓冲区读字节，用 copyout 写回用户空间
 *   4. 读完后唤醒写端（告诉它缓冲区有空位了）
 *
 * 注意：piperead 不会阻塞等够 n 个字节，读到多少算多少。
 * 这是 Unix 管道的语义——read 返回实际可读的数据量。
 */
int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while (pi->nread == pi->nwrite && pi->writeopen) { //DOC: pipe-empty
    if (killed(pr)) {
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for (i = 0; i < n; i++) { //DOC: piperead-copy
    if (pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread % PIPESIZE];
    if (copyout(pr->pagetable, addr + i, &ch, 1) == -1) {
      if (i == 0)
        i = -1;
      break;
    }
    pi->nread++;
  }
  wakeup(&pi->nwrite); //DOC: piperead-wakeup
  release(&pi->lock);
  return i;
}
