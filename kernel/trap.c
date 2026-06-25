#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

/*
 * usertrap() — 处理来自用户空间的陷阱（系统调用/中断/异常）。
 *
 * 从 trampoline.S 的 uservec 跳转过来，此时已经在内核页表上。
 * 返回时把用户页表的 satp 传回 trampoline.S，由 userret 恢复。
 *
 * 根据 scause 区分三种情况：
 *   scause == 8       → 系统调用（用户执行了 ecall）
 *   scause == 0x80... → 中断（设备中断或定时器）
 *   scause == 13/15   → 缺页异常（懒分配页表）
 *   其他               → 未知错误，杀进程
 *
 * sstatus.SPP 检查：确保真的是从 U-mode 来的。
 * 如果是 S-mode 来的（SPP=1），那是内核 bug，直接 panic。
 *
 * RISC-V 的 sepc 在 ecall 后指向 ecall 指令本身，
 * 所以系统调用后需要 sepc += 4 跳过这条指令，
 * 否则回到用户态会无限重入。
 */
uint64
usertrap(void)
{
  int which_dev = 0;

  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // 先把 stvec 切到 kernelvec。
  // 现在在内核里了，之后若再发生陷阱，应走 kerneltrap() 而不是 uservec。
  // （比如在 syscall() 执行中发生时钟中断）
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();

  // 保存用户 PC 到 trapframe，方便返回时恢复。
  p->trapframe->epc = r_sepc();

  if (r_scause() == 8) {
    // ── 系统调用（ecall 指令） ──
    // 注意 sepc 指向 ecall 指令，返回时应跳过它。
    // RISC-V 的 ecall 是 4 字节指令，所以 +4。

    if (killed(p))
      kexit(-1);

    // 跳过 ecall 指令（4 字节），返回后执行下一条。
    p->trapframe->epc += 4;

    // 打开中断：系统调用执行期间可响应中断。
    // 之前在 uservec 中关了中断，现在才打开。
    intr_on();

    syscall();
  } else if ((which_dev = devintr()) != 0) {
    // ── 设备中断或定时器中断 ──
    // devintr() 返回 2=定时器，1=设备，0=未知
  } else if ((r_scause() == 15 || r_scause() == 13) &&
             vmfault(p->pagetable, r_stval(), (r_scause() == 13) ? 1 : 0) !=
               0) {
    // ── 缺页异常（懒分配处理） ──
    // scause 13 = 读缺页, 15 = 写缺页
    // vmfault() 尝试在运行时分配页面
  } else {
    // ── 未知陷阱，杀进程 ──
    printk("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printk("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if (killed(p))
    kexit(-1);

  // 如果本次陷阱是定时器中断，让出 CPU（进程调度）
  if (which_dev == 2)
    yield();

  // 设置好 sstatus/sepc，以便 sret 能正确返回用户态
  prepare_return();

  // 把用户页表的 satp 值传给 trampoline.S
  // trampoline的 userret 会用它切换回用户页表。
  uint64 satp = MAKE_SATP(p->pagetable);

  return satp;  // a0 中返回，trampoline.S 接收
}

/*
 * prepare_return() — 准备从内核返回用户空间。
 *
 * 这是 usertrap() 和 scheduler() 返回用户态前调用的函数。
 * 它设置好以下内容，让 trampoline.S 的 userret 和 sret 能正确跳回用户：
 *
 *   ① stvec ← uservec：下次用户态陷阱能再跳到 uservec
 *   ② trapframe 中的 kernel_* 字段：
 *      供下次陷入内核时 uservec 使用
 *   ③ sstatus.SPP ← 0：sret 回到 U-mode
 *   ④ sepc ← 用户 PC：sret 跳回正确位置
 *
 * 为什么叫 prepare_return 而不是 prepare_user_return？
 * 因为它也用于 scheduler() 首次启动进程时的返回——那个场景下
 * 不经过 usertrap，直接设好寄存器后 userret+sret。
 */
void
prepare_return(void)
{
  struct proc *p = myproc();

  // 关中断。接下来要改 stvec，如果此时来一个中断，
  // 内核代码会跳到 uservec（在 TRAMPOLINE 上），
  // 而那个页面映射在当前是内核页表还是用户页表？不确定。
  // 安全起见，先关中断。
  intr_off();

  // 把 stvec 设回 uservec（下次用户态陷阱跳到 trampoline）。
  // uservec 在 TRAMPOLINE 页面内，计算相对偏移：
  //   uservec - trampoline 是汇编代码内的偏移量
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // 填充 trapframe 中的 kernel_* 字段。
  // 下次用户程序执行 ecall 时，uservec 会直接从 trapframe
  // 加载这些字段，不经过 C 代码。
  p->trapframe->kernel_satp = r_satp();         // 当前内核页表的 satp
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // 内核栈顶
  p->trapframe->kernel_trap = (uint64)usertrap; // usertrap() 地址
  p->trapframe->kernel_hartid = r_tp();          // 当前 CPU 编号

  // 设置 sstatus，控制 sret 的行为。
  // RISC-V 的 sret 指令用 sstatus 中的两个位决定返回状态：
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP;   // SPP=0 → 返回到 U-mode（用户模式）
  x |= SSTATUS_SPIE;   // SPIE=1 → sret 后恢复中断使能
  w_sstatus(x);

  // sepc = 用户 PC，sret 将跳转到这个地址。
  w_sepc(p->trapframe->epc);
}

/*
 * kerneltrap() — 处理内核态发生的陷阱（中断/异常）。
 *
 * 从 kernelvec.S 跳转过来。与 usertrap 的区别：
 *   - 内核态中断时 stvec = kernelvec → 走这里
 *   - 用户态中断时 stvec = uservec → 走 usertrap
 *
 * 内核态只有设备中断和定时器中断是合法的。
 * 如果在内核态发生了系统调用（ecall）或缺页，那是 bug，直接 panic。
 *
 * 注意：内核态中断发生时，没有"保存所有寄存器"的步骤。
 * kernelvec 只保存了被调用者保存的寄存器（callee-saved），
 * 因为编译器生成的 C 代码会自动处理调用者保存的寄存器。
 * 这就是 kerneltrap 比 usertrap 轻量的原因。
 */
void
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  // 检查：确实是从 S-mode 来的（内核态中断）
  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  // 检查：中断发生时中断应该是关着的（xv6 在内核中默认关中断）
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0) {
    // 未知陷阱——内核 bug
    printk("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(),
           r_stval());
    panic("kerneltrap");
  }

  // 定时器中断 → 让出 CPU
  // myproc() != 0 防止在 scheduler() 线程里 yield
  if (which_dev == 2 && myproc() != 0)
    yield();

  // 恢复 sepc 和 sstatus：yield() 可能切换了进程，
  // 导致 trapframe 被覆盖。而 kernelvec 返回时还需要原始值。
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if (cpuid() == 0) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if (scause == 0x8000000000000009L) {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if (irq == UART0_IRQ) {
      uartintr();
    } else if (irq == VIRTIO0_IRQ) {
      virtio_disk_intr();
    } else if (irq) {
      printk("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if (irq)
      plic_complete(irq);

    return 1;
  } else if (scause == 0x8000000000000005L) {
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}
