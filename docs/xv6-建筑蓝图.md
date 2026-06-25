# xv6 建筑蓝图——从硬件搭到用户程序

本文按**依赖层次**组织。每一层只依赖下面各层已经建好的东西。
读的时候可以只关注"这一层新增了什么能力"。

---

## 第 0 层：硬件（QEMU + RISC-V）

```
开机时的状态：
  • 一个或多个 CPU（hart），都在 M-mode
  • 物理内存从 0x80000000 到 0x88000000（128MB）
  • UART 串口在 0x10000000（可 printf 输出）
  • CLINT 定时器在 0x02000000（可产生时钟中断）
  • PLIC 中断控制器在 0x0C000000（可管理设备中断）
  • VIRTIO 磁盘在 0x10001000（可读写文件系统）

硬件提供的关键机制：
  ecall     — 用户态→内核态切换
  sret      — 内核态→用户态切换
  satp CSR  — 页表基地址寄存器（开启虚拟内存）
  stvec CSR — 陷阱向量（指定陷入内核后跳哪里）
  scause    — 陷阱原因（系统调用 / 缺页 / 中断）
  sepc      — 陷阱发生时的 PC
  sscratch  — 临时暂存寄存器
```

**这一层存在之后，我们能做的：** 在 M-mode 写汇编，printf 到串口。但不够——我们不能隔离进程、不能运行用户程序。

---

## 第 1 层：物理内存分配器

```
文件：kernel/kalloc.c
提供：kalloc() / kfree()

做的事情：
  把空闲物理内存串成链表（struct run { next; }）。
  kalloc() 摘下一个页（4096 字节），kfree() 挂回去。

数据结构：
  struct kmem {
      struct spinlock lock;
      struct run *freelist;
  }

关键事实：
  • 这是内核唯一的物理内存分配器
  • 所有"分配一个 xxx 结构体"最终都调用 kalloc
  • 粒度是页（4096 字节），没有 malloc 那种小对象分配
```

**这一层存在之后，我们能做的：** 动态分配/释放物理页。这是万物之基——页表页、trapframe 页、内核栈页、pipe 结构体、磁盘缓冲区……全从这来。

---

## 第 2 层：内核虚拟内存

```
文件：kernel/vm.c
提供：kvmmap() / uvmalloc() / walk() / mappages()

做的事：
  把物理页映射到虚拟地址空间，建立页表。
  kvminit() 创建内核页表——直接映射（虚拟地址 = 物理地址 + 偏移）。
  kvminithart() 把内核页表写入 satp CSR，开启分页。

数据结构：
  pagetable_t = uint64*（指向根页表页，共 512 个 PTEs）

关键事实：
  • 这层只有"映射"，没有"进程"
  • 内核页表是所有 CPU 共享的
  • 直接映射意味着内核代码中的指针（如 &some_struct）
    就等于它的物理地址——简单，但需要预留
```

**这一层存在之后，我们能做的：** 分页开启，有了虚拟地址。但还没有进程——只有一个共享的内核地址空间。

---

## 第 3 层：CPU 抽象（每 CPU 状态 + 自旋锁）

```
文件：kernel/spinlock.c + kernel/proc.c(procinit部分)
提供：acquire() / release() / push_off() / pop_off()
     cpuid() / mycpu() / myproc()

做的事：
  自旋锁：多核并发时保护共享数据。
  push_off/pop_off：嵌套地关/开中断（防止死锁）。
  cpuid()：读 tp 寄存器获得当前 CPU 编号。

数据结构：
  struct spinlock { locked; cpu; }
  struct cpu { proc; context; noff; intena; }

关键事实：
  • tp 寄存器存的是 hartid（CPU 编号），由 start.c 在开机时设置
  • 有了 cpuid() 和 myproc()，函数才能知道"我在哪个 CPU 上"、
    "当前运行的是哪个进程"——这是后面一切的基础
  • noff 和 intena 支持 push_off/pop_off 的嵌套调用
```

**这一层存在之后，我们能做的：** 安全地在多核上访问共享数据。不再担心竞态。

---

## 第 4 层：进程表 + 调度器

```
文件：kernel/proc.c
提供：allocproc() / scheduler() / swtch() / sleep() / wakeup()

做的事：
  • 进程表 proc[NPROC] 是 64 个 struct proc 的静态数组
  • 调度器循环扫描 proc[]，挑 RUNNABLE 的进程运行
  • swtch() 保存/恢复 callee-saved 寄存器，切换进程
  • sleep/wakeup 提供等待/通知机制

新增的数据结构：
  struct proc { state; pid; context; trapframe; pagetable; kstack; ofile[]; ... }

关键事实：
  • 这一层的核心贡献是「进程」这个概念
  • 有了进程，才谈得上隔离、上下文切换
  • 但目前进程只分配了结构体和页表，没有用户代码
  • swtch 不关心切换的是进程还是调度器——它只保存/恢复寄存器
```

**这一层存在之后，我们能做的：** 创建多个进程，在它们之间切换运行。但进程里没有代码可执行（还没加载用户程序）。

---

## 第 5 层：陷阱处理

```
文件：kernel/trap.c + kernel/trampoline.S + kernel/syscall.c
提供：usertrap() / prepare_return() / kerneltrap() / syscall()

做的事：
  • 建立从用户态到内核的"门"
  • ecall → uservec → usertrap → syscall → ... → userret → sret
  • 系统调用分发表（syscall 号 → 处理函数）

新增的数据结构：
  无新增结构体。但使以下字段变得有意义：
    proc->trapframe（保存/恢复用户寄存器）
    stvec、sepc、scause、sstatus 等 CSR 的使用

关键事实：
  • 陷阱路径是 xv6 最关键的路径——所有用户请求都经过这里
  • uservec 在 TRAMPOLINE 页上，双映射到用户和内核页表
  • 这一层本身不实现任何系统调用，只负责转发
```

**这一层存在之后，我们能做的：** 用户程序可以通过 ecall 进入内核执行代码。但还没有任何有用的系统调用——进来后只能返回。

---

## 第 6 层：文件描述符 + pipe

```
文件：kernel/file.c + kernel/pipe.c
提供：filealloc() / filedup() / fileclose()
     fileread() / filewrite()
     pipealloc() / pipewrite() / piperead()

做的事：
  • file 作为"多态资源"的抽象层
  • pipe 作为进程间通信通道
  • file 引用计数管理共享生命周期

新增的数据结构：
  struct file { type; ref; readable; writable; off; pipe/ip; }
  struct pipe { data[512]; nread; nwrite; lock; readopen; writeopen; }

关键事实：
  • file 不直接暴露给用户——用户看到的是 fd（即 ofile[] 的下标）
  • pipealloc 创建 2 个 file + 1 个 pipe，两个 file 指向同一个 pipe
  • file 的 ref 归零时才真正释放底层资源
```

**这一层存在之后，我们能做的：** 打开/读/写/关闭文件和管道。但还没有文件系统——file 可以指向 pipe，但还不能指向磁盘上的文件。

---

## 第 7 层：文件系统（磁盘 + inode + 目录）

```
文件：kernel/bio.c + kernel/fs.c
提供：bread() / bwrite() / brelse()
     namei() / readi() / writei() / ialloc() / iput()

做的事：
  • buffer cache 作为磁盘和内存之间的缓存层
  • inode 作为磁盘文件的抽象
  • 目录作为文件名 → inode 号的映射

新增的数据结构：
  struct buf { blockno; valid; dirty; data[1024]; refcnt; }
  struct inode { dev; inum; type; nlink; size; addrs[NDIRECT+1]; }
  struct dirent { inum; name[DIRSIZ]; }

关键事实：
  • file->type = FD_INODE 时，file->ip 指向一个 inode
  • inode 的 nlink 归零时，它占用的磁盘块才被释放
  • buffer cache 使用 LRU 替换（实际是"时钟"算法）
```

**这一层存在之后，我们能做的：** 读写磁盘上的文件。此时 file 可以指向 inode，pipe 和磁盘文件都可以通过 fileread/filewrite 统一访问。

---

## 第 8 层：系统调用实现

```
文件：kernel/sysfile.c + kernel/sysproc.c + kernel/exec.c
提供：sys_open() / sys_read() / sys_write() / sys_fork()
     sys_exec() / sys_pipe() / sys_close() / sys_chdir() ...

做的事：
  把用户请求（通过 ecall 进来）转化为对底层各层的调用。

清单：
  sys_dup     → file.c: filedup + fdalloc
  sys_read    → file.c: fileread（派发到 pipe 或 inode）
  sys_write   → file.c: filewrite
  sys_close   → file.c: fileclose
  sys_fstat   → file.c: filestat
  sys_link    → fs.c: namei + dirlink
  sys_unlink  → fs.c: nameiparent + dirlookup + writei
  sys_open    → fs.c: namei/create + file.c: filealloc + fdalloc
  sys_mkdir   → fs.c: create(T_DIR)
  sys_mknod   → fs.c: create(T_DEVICE)
  sys_chdir   → fs.c: namei + iput
  sys_pipe    → pipe.c: pipealloc + fdalloc
  sys_exec    → exec.c: kexec
  sys_fork    → proc.c: kfork
  sys_exit    → proc.c: kexit
  sys_wait    → proc.c: kwait
  sys_kill    → proc.c: kkill
  sys_getpid  → proc.c: myproc()->pid
  sys_sbrk    → vm.c: uvmalloc/uvmdealloc
  sys_uptime  → 读 ticks 全局变量
  sys_pause   → sleep(&ticks, &tickslock)
```

**这一层存在之后，我们能做的：** 用户程序通过系统调用使用内核的全部能力。

---

## 第 9 层：用户程序

```
文件：user/init.c + user/sh.c + user/cat.c + ...

做的事：
  init: 打开 console，fork+exec("sh")
  sh:   读命令 → fork+exec
  cat:  open → read → write
  ...

关键事实：
  • /init 由 forkret() 中的 kexec("/init") 启动
  • init 永不退出——它是"一号进程"
  • sh 如果退出，init 再 fork+exec 一个
```

---

## 全景：依赖层次图

```
第 9 层  用户程序 (cat, sh, init)
            │ 通过系统调用访问内核
第 8 层  系统调用实现 (sysfile.c, sysproc.c, exec.c)
            │ 调用下层
第 7 层  文件系统 (bio.c + fs.c)        第 6 层  pipe (pipe.c)
            │ 需要 inode                      │ 需要 file
            └──────────┬──────────────────────┘
                       ▼
第 6'层  文件描述符 (file.c)
            │ 需要进程的 ofile[]
            ▼
第 5 层  陷阱处理 (trap.c + trampoline.S + syscall.c)
            │ 需要进程来执行代码
            ▼
第 4 层  进程表 + 调度器 (proc.c)
            │ 需要锁保护共享数据
            ▼
第 3 层  自旋锁 + 每 CPU 状态 (spinlock.c)
            │ 需要虚拟内存运行
            ▼
第 2 层  内核虚拟内存 (vm.c)
            │ 需要分配物理页
            ▼
第 1 层  物理内存分配器 (kalloc.c)
            │ 不需要任何内核组件
            ▼
第 0 层  硬件 (RISC-V + QEMU)
```

---

## 跨层的关键线索

有些概念贯穿多层，它们是最容易让人困惑的地方。下面列出三条最主要的。

### 线索 A：进程的一生

```
userinit  → allocproc(kalloc + vm + proc)
  → state=RUNNABLE
    → scheduler 选中它
      → swtch → forkret → kexec(/init) → *从此用户态运行*
        → sys_read / sys_write / ...（走陷阱层和文件层）
          → kexit（关 file、释放内存、变 ZOMBIE）
            → parent kwait → freeproc → proc 槽位变 UNUSED
```

跨越的层：4（进程表）→ 5（陷阱）→ 6/7（file/inode）→ 4（调度器）

### 线索 B：数据从磁盘到用户

```
磁盘物理块 → bread (bio.c, 第7层)
  → struct buf (在 buffer cache 中)
    → readi (fs.c, 第7层)
      → fileread (file.c, 第6层)
        → sys_read (sysfile.c, 第8层)
          → copyout → 用户缓冲区
```

跨越的层：7 → 6 → 8 → 5 → 用户空间

### 线索 C：一个字节的 pipe 旅程

```
用户 A write(pipe[1], "x", 1)
  → sys_write (第8层)
    → filewrite (第6层) 找到 f->type == FD_PIPE
      → pipewrite (第6层, pipe.c)
        → copyin 从用户 A 的地址空间取 1 字节
        → 放入 pi->data[pi->nwrite++ % PIPESIZE]
        → wakeup &pi->nread（唤醒在 piperead 中 sleep 的进程 B）
          → sched (第4层)

用户 B 在 piperead 中被唤醒：
  → copyout 把 1 字节放入用户 B 的地址空间
  → sys_read 返回
```

跨越的层：8 → 6 → 4（进程切换）→ 6 → 8
