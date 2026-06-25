# xv6 结构关系图——字段为什么存在

本文不罗列结构体，而是追踪结构体之间的**引用链**。
每个字段的存在都是为了指向另一个结构、或建立一种关联。
理解了这些"为什么"，结构体自然就记住了。

---

## 1. 中心线索：一次系统调用的全部关联

用户程序调用 `write(1, buf, 5)`，涉及的**所有结构体之间的连线**：

```
用户进程 p (struct proc)
  │
  ├── p->trapframe
  │     └── 保存了 write 调用时的寄存器：
  │           a0=1 (fd), a1=buf地址, a2=5, a7=SYS_write
  │         内核读取 trapframe->a7 找到 sys_write
  │
  ├── p->pagetable
  │     └── 映射了用户地址空间，确保 buf 地址可读
  │
  ├── p->ofile[1]  →  struct file f
  │     │
  │     ├── f->type == FD_PIPE
  │     │   └── f->pipe  →  struct pipe pi
  │     │         └── pi->data[512] 环形缓冲区
  │     │             pi->nread/nwrite 读写位置
  │     │             pi->lock 保护自身
  │     │
  │     └── f->ref (引用计数)
  │           ├── 最初由 pipealloc 分配时 = 1
  │           ├── dup() 后 = 2（父进程和子进程各自持有）
  │           └── 每次 fileclose 减 1，归零才释放
  │
  ├── p->cwd  →  struct inode (当前目录)
  │
  └── p->parent  → 另一个 struct proc（父进程）
        └── 当 p exit 时，parent 的 wait 来收尸
  
  与之并行的：文件系统路径
  write 的数据 → piperead 读出 → 另一端进程的 ofile 接收
```

**每个字段的存在都是为了回答一个问题："A 需要访问 B 的哪个实例？"**

---

## 2. 进程 → 所有其他结构

```
struct proc
  │
  │  state, pid, killed, xstate — 进程自己的属性，不指向别处
  │
  ├── pagetable  →  用户页表 (pagetable_t)
  │     这是进程的"地址空间"。每个进程有自己的页表，
  │     所以进程 A 无法访问进程 B 的内存。
  │     存在理由：隔离。
  │
  ├── sz  →  用户内存大小
  │     配合 pagetable 使用：sbrk() 增/减 sz 并调整页表映射。
  │     存在理由：跟踪堆大小。
  │
  ├── trapframe*  →  一页物理内存
  │     用户程序的寄存器（a0-a7, ra, sp, sepc...）在陷入内核时
  │     保存在这里。uservec 汇编代码直接读写这块内存。
  │     存在理由：保存/恢复用户寄存器。
  │
  ├── context  →  内核态寄存器快照
  │     只包含 callee-saved 寄存器（s0-s11, ra, sp）。
  │     调度器 swtch() 在这里保存/恢复。
  │     存在理由：进程切换时不破坏内核执行路径。
  │
  ├── kstack  →  内核栈（一页物理内存）
  │     进程进入内核后 sp 指向这里。
  │     每个进程有自己的内核栈，互不干扰。
  │     存在理由：内核函数调用需要栈。
  │
  ├── ofile[NOFILE]  →  struct file* 数组
  │     每个元素是一个指向打开文件的指针。
  │     数组下标就是文件描述符（fd）。
  │     存在理由：fd → file 结构的映射表。
  │
  ├── cwd*  →  struct inode（当前目录）
  │     相对路径从这个 inode 开始查找。
  │     存在理由：支持 "cd dir" 后 "ls ."。
  │
  ├── parent*  →  另一个 struct proc
  │     exit 后变成 ZOMBIE，等 parent 来 wait 收尸。
  │     如果 parent 先 exit 了，子进程过继给 init。
  │     存在理由：父子进程关系。
  │
  └── chan*  →  sleep/wakeup 的"通道"
        sleep(chan, lock) 让进程睡眠在 chan 上，
        wakeup(chan) 唤醒所有睡眠在 chan 上的进程。
        chan 可以是任意地址——pipe 的 nread/nwrite 地址、
        ticks 的地址、某个 proc 的地址。
        存在理由：等待某个条件发生。
```

---

## 3. file → 三种底层资源

```
struct file 是一个"多态"接口。它的 type 字段决定它指向什么：

         ┌─────────────────────────┐
         │      struct file        │
         │  type: FD_PIPE / FD_INODE / FD_DEVICE  │
         │  ref (引用计数)           │
         │  readable / writable     │
         │  off (文件偏移)           │
         └────────┬────────────────┘
                  │
     ┌────────────┼────────────┐
     │            │            │
     ▼            ▼            ▼
  FD_PIPE      FD_INODE     FD_DEVICE
     │            │            │
     ▼            ▼            ▼
  struct pipe  struct inode  major(设备号)
  ├─ data[512]  ├─ type       UART=1, 无其他
  ├─ nread      ├─ nlink
  ├─ nwrite     ├─ size       设备文件不占磁盘空间，
  ├─ readopen   ├─ addrs[]    只是让 open 能拿到
  └─ writeopen  └─ dev/inum    major 号，后续 read/
                                 write 转给设备驱动。

  file 的 ref 字段为什么存在？
    一个 file 可被多个 fd 指向（dup 之后）。
    只有最后一个 ref 归零时，才真正释放底层资源：
      FD_PIPE   → pipeclose → 两端都关则释放 pipe
      FD_INODE  → iput      → nlink 归零则释放磁盘块
      FD_DEVICE → 无操作（设备文件不占资源）
```

---

## 4. pipe → 两个 file + 一对进程

```
pipe 是进程间通信的"通道"，它的关系网：

  ┌───── 进程 A ─────┐          ┌───── 进程 B ─────┐
  │  ofile[3] = rf   │          │  ofile[3] = wf   │  ← dup/fork 后
  │  ofile[4] = wf   │          │  ofile[4] = rf   │     fd 号可能不同
  └────────┬─────────┘          └────────┬─────────┘
           │                             │
           ▼                             ▼
  ┌──────────────── file rf ────────────┐
  │  type = FD_PIPE, readable=1         │
  │  writable=0, pipe  →  ┐             │
  └────────────────────────┼─────────────┘
                           │
  ┌──────────────── file wf ────────────┐
  │  type = FD_PIPE, readable=0         │
  │  writable=1, pipe  →  ┘             │
  └────────────────────────┬─────────────┘
                           │
                           ▼
                   ┌──────────────┐
                   │  struct pipe │  ← 内核堆上分配
                   │  data[512]   │     (kalloc)
                   │  nread       │
                   │  nwrite      │
                   │  lock        │
                   │  readopen    │
                   │  writeopen   │
                   └──────────────┘

  pipealloc 的布局：
    ① kalloc 一个 pipe 结构体
    ② filealloc 两个 file 结构体
    ③ 两个 file 的 pipe 指针都指向同一个 pipe 结构体
    ④ 一个 file 只读，一个只写 → 自然形成"半双工"

  为什么这里有两个 file 而不是两个 fd？
    因为 file 是"资源"，fd 是"引用"。
    一个 file 被多个 fd 指向时，引用计数管理生命周期。
    pipealloc 创建 file，fork 后父子进程各持有它们。
```

---

## 5. 页表 → 两个视图

```
每个进程有两套地址空间：用户视图和内核视图。

  ┌── 用户视图（p->pagetable）─────────────┐
  │                                        │
  │  0x0 ───────────────────────── p->sz    │
  │  用户代码/数据/堆（通过 sbrk 增减）       │
  │                                        │
  │  p->sz ─────────────────── TRAMPOLINE   │
  │  用户栈（向低地址增长）                   │
  │                                        │
  │  TRAMPOLINE 页（高地址）                  │
  │  映射了 trampoline.S 的物理页              │
  │  权限：R+X（用户可读可执行）               │
  │                                        │
  │  TRAPFRAME 页                            │
  │  映射了 p->trapframe 的物理页             │
  │  权限：R（用户可读，不可写——               │
  │        由 uservec 在 S-mode 下写入）      │
  └────────────────────────────────────────┘

  ┌── 内核视图（kernel_pagetable）───────────┐
  │  0x80000000 ──────────── PHYSTOP        │
  │  内核代码/数据（直接映射到物理地址）        │
  │  所有进程的内核栈都映射在此                │
  │                                          │
  │  TRAMPOLINE 页                            │
  │  和用户视图中的是同一物理页                 │
  │                                          │
  │  内核栈区域（每个进程占据两页）              │
  └──────────────────────────────────────────┘

  为什么 TRAMPOLINE 和 TRAPFRAME 要双映射？
    因为 ecall 后的瞬间：
      页表 = 用户的（还没切换）
      PC   = trampoline（必须有效）
    如果 trampoline 不在用户页表中，第一条指令就崩。

    等切换到内核页表后，PC 还在 trampoline 上。
    只要内核页表也在相同虚拟地址映射了同一物理页，就继续执行。

    TRAPFRAME 同理——uservec 读取/写入 trapframe 时
    用的是 TRAPFRAME 虚拟地址，这个地址在两个页表中
    都映射到了该进程的 trapframe 物理页。
```

---

## 6. 调度器 → CPU ↔ 进程

```
每个 CPU 核运行自己的 scheduler 循环：

  ┌── CPU 0 ──┐        ┌── CPU 1 ──┐
  │ cpus[0]   │        │ cpus[1]   │
  │  .proc     │        │  .proc     │  ← 当前正在运行的进程
  │  .context  │        │  .context  │  ← 调度器自己的栈/寄存器
  └─────┬─────┘        └─────┬─────┘
        │                    │
        ▼                    ▼
  scheduler():           scheduler():
    FOR p in proc[]:       FOR p in proc[]:
      swtch(&cpus.context,   swtch(&cpus.context,
            &p.context)            &p.context)
        │                    │
        ▼                    ▼
    进程 p 运行...          进程 q 运行...
    yield():               yield():
      swtch(&p.context,      swtch(&q.context,
            &cpus.context)         &cpus.context)
        │                    │
        ▼                    ▼
    回到 scheduler          回到 scheduler

  swtch 的数据流向：
    调用前：当前 CPU 的 cpus[i].context 保存调度器的寄存器
           目标进程的 p->context 保存进程上次 yield 时的寄存器
    调用后：sp 切到了目标进程的内核栈
           ra 指向进程上次 yield 的位置（或 forkret）
           其他 callee-saved 寄存器恢复

  为什么每个 CPU 有自己的 scheduler 循环？
    因为 xv6 是多核的。CPU0 和 CPU1 同时扫 proc[] 表，
    各自选一个 RUNNABLE 的进程来跑。
    锁（p->lock）防止两个 CPU 选了同一个进程。
```

---

## 7. 文件系统 → 磁盘 ↔ 内存

```
    路径名（用户可见）
         │
         ▼
    namei(path)        ← 按路径查找 inode
         │
         ▼
    struct inode       ← 磁盘 inode 的内存副本
    ├─ dev, inum       （位置标识）
    ├─ type            T_DIR / T_FILE / T_DEVICE
    ├─ nlink           （硬链接数，=0 时删除数据）
    ├─ size
    └─ addrs[NDIRECT+1]（指向磁盘块号）
         │
         ▼
    bread(dev, blockno)  ← 通过 buffer cache 读磁盘块
         │
         ▼
    struct buf           ← 磁盘块的内存副本
    ├─ blockno
    ├─ valid / dirty
    ├─ data[1024]
    └─ refcnt（引用计数）
         │
         ▼
    virtio_disk_rw(buf)  ← 真正的磁盘 I/O

  引用链总结：
    fd(int) → ofile[] → file → inode → (dev, inum) → buf → 磁盘块

  目录的结构：
    目录就是一个特殊的 inode（type=T_DIR），
    它的 data 块里存储了多个 struct dirent：
      dirent { inum(uint16), name[DIRSIZ](char[14]) }
    所以"查找文件"就是读目录的 data 块，匹配 name，
    找到 inum，然后用 inum 去 inode 表里加载对应的 inode。
```

---

## 8. 整体引用链全景

```
                           ┌──────────────┐
                           │  磁盘块 (buf) │
                           └──────┬───────┘
                                  │ bread/bwrite
                           ┌──────┴───────┐
                           │  inode (fs)  │
                           └──────┬───────┘
                                  │ iput/ilock
                     ┌────────────┼──────────────┐
                     │            │              │
              ┌──────┴──────┐  ┌──┴─────────┐  ┌──┴──────────┐
              │ file (FD)   │  │ file (PIPE)│  │ file (DEV)  │
              │ ip → inode  │  │ pipe → pi  │  │ major       │
              └──────┬──────┘  └──────┬─────┘  └──────┬──────┘
                     │               │               │
                     └───────┬───────┘               │
                             │                       │
                      ┌──────┴──────┐                │
                      │ ofile[fd]   │                │
                      └──────┬──────┘                │
                             │                       │
                      ┌──────┴───────────────────────┴───┐
                      │          struct proc             │
                      │  trapframe → 用户寄存器           │
                      │  pagetable → 用户地址空间          │
                      │  context   → 内核执行位置          │
                      │  parent    → 另一个 proc          │
                      │  cwd       → inode（当前目录）     │
                      └──────────────────────────────────┘
                                  │
                                  │ swtch
                                  ▼
                           ┌──────────────┐
                           │ scheduler    │
                           │ 遍历 proc[]  │
                           └──────────────┘
```

---

## 9. 简单规律总结

每类字段的存在都有固定的"为什么"：

| 字段模式 | 为什么存在 | 举例 |
|----------|-----------|------|
| 指向另一个结构体 | "我需要访问那个实例" | `proc->pagetable`, `file->pipe` |
| 数组 | "我需要 0..N-1 个" | `ofile[NOFILE]`, `proc[NPROC]` |
| 锁 | "多核同时读/写这个数据" | `pipe->lock`, `tickslock` |
| 引用计数 | "不确定谁最后释放" | `file->ref`, `buf->refcnt` |
| 状态/类型 | "分情况处理" | `proc->state`, `file->type` |
| 指针 + 锁 | "要睡了，等别人来叫" | `proc->chan` + `proc->lock` |
| 回调地址 | "下次我从哪里继续" | `context->ra`, `trapframe->epc` |
