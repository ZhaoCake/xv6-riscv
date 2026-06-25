# xv6 伪代码——关键结构与算法

本文把 xv6 的内核抽象成伪代码，去掉 C 语言的细节和 RISC-V 硬件特性，
只保留结构和算法骨架。适合在读完源码后用来整理思路。

---

## 1. 进程管理

### 进程结构体

```
struct proc {
    state       // UNUSED / USED / RUNNABLE / RUNNING / SLEEPING / ZOMBIE
    pid
    parent      // 父进程指针
    pagetable   // 用户页表
    trapframe   // 保存用户寄存器的内存页
    context     // 内核态上下文（swtch 用）
    kstack      // 内核栈
    sz          // 用户内存大小
    ofile[]     // 打开的文件描述符表
    cwd         // 当前工作目录 inode
    chan        // sleep 等待的通道
    killed      // 是否被 kill
    xstate      // 退出码（ZOMBIE 后给父进程）
}
```

### 进程生命周期

```
// 分配新进程
FUNCTION allocproc():
    扫描 proc[] 找到 UNUSED 槽位
    分配 PID
    分配 trapframe 页
    创建用户页表（含 TRAMPOLINE + TRAPFRAME 映射）
    设置 context:
        context.ra = forkret    // 第一次被调度时从这里开始
        context.sp = kstack 顶
    RETURN  proc 指针

// 第一个进程（系统启动时）
FUNCTION userinit():
    p = allocproc()
    p.cwd = 根目录 "/"
    p.state = RUNNABLE          // 让调度器选中它

// 调度器首次运行该进程时 → forkret:
FUNCTION forkret():
    IF 首次运行:
        fsinit(ROOTDEV)         // 读磁盘超级块
        kexec("/init")          // 直接在内核加载 /init
    // kexec 成功后 → trampoline userret → sret → 用户态运行 /init

// fork（创建子进程）
FUNCTION kfork():
    np = allocproc()
    COPY 父进程的用户内存到子进程新页表
    COPY trapframe（子进程 trapframe.a0 = 0，表示是子进程）
    FOR each open fd:
        filedup(fd)             // 共享文件描述符
    np.cwd = p.cwd
    iput(p.cwd)
    np.state = RUNNABLE
    RETURN np.pid               // 父进程收子进程 PID

// exit（进程自我终止）
FUNCTION kexit(status):
    关闭所有打开的文件
    释放 cwd
    把子进程过继给 init
    唤醒父进程（它在 wait 中）
    xstate = status
    state = ZOMBIE
    让出 CPU（sched）

// wait（父进程等子进程结束）
FUNCTION kwait():
    LOOP:
        扫 proc[] 表找 state == ZOMBIE 且 parent == 自己的进程
        IF 找到:
            拷出 xstate 到用户空间
            freeproc(pp)        // 释放页表、trapframe、proc 槽位
            RETURN pid
        IF 没有子进程:
            RETURN -1
        sleep(当前进程, &wait_lock)  // 等子进程 exit 来 wakeup
```

### 调度器（最外层循环）

```
// 每个 CPU 核各自运行 scheduler
FUNCTION scheduler():
    LOOP forever:
        FOR each proc in proc[]:
            IF proc.state == RUNNABLE:
                proc.state = RUNNING
                swtch(当前 CPU context, proc.context)
                // 从 swtch 返回意味着其他进程切回来了
                proc.state = RUNNABLE

// swtch（汇编，核心 3 条指令）
FUNCTION swtch(old_context, new_context):
    保存 callee-saved 寄存器到 old_context
    从 new_context 恢复 callee-saved 寄存器
    ret  → 跳转到 new_context.ra
    // 首次调度时 target 是 forkret
    // 之后切换时 target 是 yield/sched 回来的位置
```

---

## 2. 陷阱处理（系统调用入口）

### 完整路径（用户态 write → 内核）

```
用户程序调用 write(fd, buf, n)
  │
  │  C 编译器：a0=fd, a1=buf_addr, a2=n
  ▼
usys.S 桩函数:
    li a7, SYS_write    // 系统调用号
    ecall               // 陷入内核
  │
  ▼ 硬件自动：
  │   CPU 切换到 S-mode
  │   sepc = ecall 指令地址
  │   scause = 8（系统调用）
  │   跳转到 stvec（= uservec）
  ▼
uservec（汇编, 在 TRAMPOLINE 页上）:
    // 此时还在用户页表上
    ① sscratch = a0（暂存用户 a0）
    ② a0 = TRAPFRAME（指向当前进程的 trapframe）
    ③ 保存所有用户寄存器到 p->trapframe
    ④ 从 trapframe 加载：
        sp   = kernel_sp         // 切到内核栈
        tp   = kernel_hartid     // CPU 编号
        t0   = kernel_trap       // = usertrap 函数地址
        t1   = kernel_satp       // 内核页表
    ⑤ sfence.vma + csrw satp, t1  // 切到内核页表
    ⑥ sfence.vma + jalr t0       // 跳转到 usertrap
  │
  ▼
usertrap（C 语言）:
    ① 切换 stvec → kernelvec（内核态陷阱走 kerneltrap）
    ② 保存 sepc 到 p->trapframe->epc
    ③ 检查 scause：
        IF scause == 8:              // 系统调用
            epc += 4                 // 返回时跳过 ecall
            开中断
            syscall()                // 查表转发
        ELSE IF devintr() != 0:      // 设备/定时器中断
            处理即可
        ELSE IF scause == 13/15:     // 缺页
            vmfault()                // 懒分配处理
        ELSE:
            printk + setkilled(p)    // 未知陷阱，杀进程
    ④ IF 定时器中断:
        yield()                      // 让出 CPU
    ⑤ prepare_return()              // 设好 sepc/sstatus
    ⑥ RETURN 用户页表的 satp 值      // 给 trampoline 用
  │
  ▼
userret（汇编）:
    ① sfence.vma + csrw satp, a0    // 切回用户页表
    ② 从 trapframe 恢复所有用户寄存器
    ③ sret                          // 回到用户态
  │
  ▼
用户程序从 ecall 下一条继续执行
```

### 系统调用分发表

```
FUNCTION syscall():
    num = trapframe->a7      // 系统调用号
    IF num 在合法范围内:
        trapframe->a0 = syscalls[num]()  // 调用对应处理函数，返回值放 a0
    ELSE:
        printk "unknown syscall"
        trapframe->a0 = -1
```

---

## 3. 虚拟内存布局

```
物理地址空间:
  0x00001000  — boot ROM
  0x02000000  — CLINT（定时器）
  0x0C000000  — PLIC（中断控制器）
  0x10000000  — UART0（串口）
  0x10001000  — VIRTIO（磁盘）
  0x80000000  — kernel 加载地址
  0x88000000  — PHYSTOP（128MB RAM 上限）

内核虚拟地址空间:
  [0x80000000, PHYSTOP)     — 内核代码+数据（直接映射到物理地址）
  [TRAMPOLINE, TRAMPOLINE+PGSIZE) — trampoline.S 代码
  [TRAPFRAME, TRAPFRAME+PGSIZE)   — 当前进程的 trapframe
  KSTACK(pid)               — 每个进程的内核栈（高位）

用户虚拟地址空间:
  [0x0, p->sz)              — 用户代码+数据+堆
  [p->sz, TRAPFRAME)        — 用户栈（从 TRAPFRAME 向下增长）
  [TRAMPOLINE]              — trampoline.S（只读+执行）
  [TRAPFRAME]               — trapframe 页（只读）
  // TRAMPOLINE 和 TRAPFRAME 在用户页表和内核页表中映射到相同物理页
```

---

## 4. 文件系统

### 三层抽象

```
文件描述符层 (file.c)
  struct file { type(FD_PIPE/FD_INODE/FD_DEVICE), readable, writable, off, ip, pipe }
  filealloc / filedup / fileclose
  fileread / filewrite → 根据 type 分派

inode 层 (fs.c)
  struct inode { type(T_DIR/T_FILE/T_DEVICE), dev, inum, nlink, size, addrs[] }
  namei / dirlookup / readi / writei / ialloc / iput

磁盘块层 (bio.c)
  struct buf { blockno, valid, dirty, data[] }
  bread / bwrite / brelse
  buffer cache：LRU 替换，所有磁盘操作通过它
```

### pipe 实现

```
struct pipe {
    data[512]       // 环形缓冲区
    nread, nwrite   // 读/写位置
    readopen, writeopen  // 两端是否打开
    lock            // 互斥锁
}

// 写端
FUNCTION pipewrite(pi, user_addr, n):
    加锁
    FOR i = 0 to n:
        WHILE 缓冲区满:
            唤醒读端
            sleep 等读端腾出空间
        从用户空间复制 1 字节到环形缓冲区
    唤醒读端
    解锁
    RETURN 写入字节数

// 读端
FUNCTION piperead(pi, user_addr, n):
    加锁
    WHILE 缓冲区空且写端还开着:
        sleep 等写端写入
    FOR i = 0 to min(n, 可读数):
        从缓冲区复制 1 字节到用户空间
    唤醒写端
    解锁
    RETURN 读出字节数
```

---

## 5. shell 命令循环

```
// main 循环
WHILE getcmd() >= 0:
    IF 命令 == "cd XXX":
        chdir("XXX")            // 父进程执行
    ELSE:
        IF fork() == 0:         // 子进程
            runcmd(parsecmd(cmd))
        wait(0)                 // 父进程等待

// 递归执行命令树
FUNCTION runcmd(cmd):
    SWITCH cmd.type:
        EXEC:   exec(cmd.argv[0], cmd.argv)
        REDIR:  close(cmd.fd); open(cmd.file, cmd.mode); runcmd(cmd.cmd)
        LIST:   fork() + runcmd(left); wait(0); runcmd(right)
        PIPE:   pipe(p);
                fork() child1: close(1); dup(p[1]); close(p[0,1]); runcmd(left)
                fork() child2: close(0); dup(p[0]); close(p[0,1]); runcmd(right)
                close(p[0,1]); wait(0); wait(0)
        BACK:   fork() + runcmd(cmd.cmd)    // 不 wait
```

---

## 6. 系统启动序列

```
上电 → M-mode
  _entry: 设栈指针
  start:  ① 设 PMP（S-mode 可访问全部物理内存）
          ② medeleg/mideleg（把异常/中断委托给 S-mode）
          ③ 设定时器
          ④ mret 切 S-mode → main()

main():
  CPU0:                     其他核:
    consoleinit()             等待 CPU0 完成
    kinit()                   然后：
    kvminit()                    kvminithart()
    kvminithart()                trapinithart()
    procinit()                   plicinithart()
    trapinit()
    trapinithart()
    plicinit()
    plicinithart()
    binit()
    iinit()
    fileinit()
    virtio_disk_init()
    userinit()               ← 创建第一个进程
    started = 1
                             启动完成

  scheduler()  ← 所有核都进入调度循环
```

---

## 7. 关键设计与哲学

| 设计 | 为什么 |
|------|--------|
| **TRAMPOLINE+TRAPFRAME 双映射** | 切换页表前后 PC 和 trapframe 地址不变 |
| **ecall → sepc+4** | sepc 指向 ecall 指令本身，需要跳过 |
| **fork → 子进程 trapframe.a0=0** | fork 在父进程返回 pid，子进程返回 0，靠 a0 区分 |
| **close+dup = 重定向** | open 总是分配最小空闲 fd 号 |
| **pipe = 两个 file 指向一个 pipe** | 读/写端是不同的 file，方便 close 独立跟踪 |
| **ZOMBIE 状态** | 子进程先死，父进程后收尸，避免孤儿进程丢失退出码 |
| **scheduler 永不返回** | 每个 CPU 核的最后一件事——跑调度循环到永远 |
