        #
        # trampoline.S — 用户态↔内核态切换的汇编入口。
        #
        # 这是 xv6 中最重要的汇编代码。
        # 它解答了一个关键问题：
        #
        #   用户态执行 ecall 后，CPU 切换到 S-mode，
        #   但页表还是用户的——此时 PC 在这段代码上。
        #   怎么保证执行完几行指令后，切换到内核页表时
        #   PC 不会指向非法地址？
        #
        # 答案：这段代码被同时映射到用户页表和内核页表的
        #   相同虚拟地址（TRAMPOLINE）上。
        #   切换页表前后，PC 指向的是同一块物理内存。
        #
        # 内核启动时把 uservec 的地址写入 stvec CSR，
        # 所以每次 ecall / 中断都跳到这里。
        #
        # trapframe 则映射在 TRAMPOLINE 下一页（也是两个页表共享）。
        #

#include "riscv.h"
#include "memlayout.h"

.section trampsec
.globl trampoline
.globl usertrap
trampoline:
.align 4
.globl uservec
uservec:    
	#
        # uservec — 从用户态陷入内核的入口。
        #
        # 进入此时代码在 TRAMPOLINE 虚拟地址上运行，
        # 页表还是用户的，但 CPU 已经是 S-mode 了。
        #
        # 要做的事：
        #   ① 保存所有用户寄存器到 trapframe
        #   ② 切换到内核页表
        #   ③ 跳转到 usertrap()（C 函数）
        #

        # 先保存用户 a0 到 sscratch（专用暂存 CSR）。
        # 因为马上要用 a0 来指路 trapframe。
        # RISC-V 的 sscratch CSR 就是为这种场景设计的。
        csrw sscratch, a0

        # 每个进程有独立的 trapframe 内存页，
        # 但在所有用户页表中都映射在相同虚拟地址 TRAPFRAME 上。
        # 这是另一个"共享页"技巧——和 TRAMPOLINE 同理。
        li a0, TRAPFRAME
        
        # 保存所有用户寄存器到 p->trapframe
        # （偏移量参见 proc.h 中 struct trapframe 的布局）
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# 把用户 a0（之前暂存在 sscratch 的）恢复出来，
        # 存到 trapframe+112 的位置（struct trapframe 的 a0 字段）。
        csrr t0, sscratch
        sd t0, 112(a0)

        # 加载内核栈指针（来自 trapframe 中的 kernel_sp）。
        # 从此 sp 指向进程自己的内核栈——可以在 C 函数里正常调用了。
        ld sp, 8(a0)

        # 加载 hartid（CPU 编号）到 tp 寄存器。
        # xv6 用 tp 存 hartid，这样 myproc() / cpuid() 就能用 tp 找到当前 CPU。
        ld tp, 32(a0)

        # 加载 usertrap() 函数的地址。
        # 所有准备工作完成后，跳转到 C 代码处理陷阱。
        ld t0, 16(a0)

        # 加载内核页表的 satp 值。
        ld t1, 0(a0)

        # 执行完上述 ld 后确保 TLB 中存的仍是用户页表的内容。
        sfence.vma zero, zero

        # 切换到内核页表。
        csrw satp, t1

        # flush TLB：之前缓存的用户页表项已经失效了。
        sfence.vma zero, zero

        # 跳转到 usertrap()——进入 C 世界。
        jalr t0

.globl userret
userret:
        #
        # userret — 从内核返回用户态。
        #
        # 从 usertrap() 返回后，a0 中装着用户页表的 satp 值。
        # 反向执行 uservec 的步骤：
        #   ① 切换回用户页表
        #   ② 从 trapframe 恢复所有寄存器
        #   ③ sret 回到用户态
        #

        # 切换回用户页表（a0 中传入了用户 satp）。
        sfence.vma zero, zero
        csrw satp, a0
        sfence.vma zero, zero

        # 用 a0 指路 trapframe，开始恢复寄存器。
        li a0, TRAPFRAME

        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	# 最后恢复用户 a0（从 trapframe 加载）。
        # 注意这必须最后做——前一条 ld 用 a0 指向 trapframe。
        ld a0, 112(a0)
        
        # sret 返回用户模式。
        #
        # sret 是 RISC-V 从 S-mode 返回的专用指令，它会：
        #   ① 把 PC 设为 sepc（prepare_return 已设置为用户 PC）
        #   ② 把特权级设为 SPP 字段（已清 0 = U-mode）
        #   ③ 如果 SPIE=1（已设），恢复中断使能
        #
        # 用户程序将从 ecall 的下一条指令继续执行。
        sret
