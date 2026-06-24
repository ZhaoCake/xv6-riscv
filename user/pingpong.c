/*
 * pingpong — 两个进程通过管道来回传递一个字节，测量性能。
 *
 * 实验目标（来自 xv6 书第一章练习）：
 *   编写一个程序，用一对管道在两个进程间来回传递一个字节，
 *   测量并打印 N 次来回所需的总耗时与平均耗时。
 *
 * 为什么需要两根管道，而不是一根？
 *
 *   一根管道是单向的（p[0] 读，p[1] 写），
 *   而 pingpong 需要双向通信：
 *
 *    父进程 ──(发)──→ 子进程
 *    父进程 ←──(收)── 子进程
 *
 *   用一根管道的方案：
 *     父: write(p[1]) → read(p[1])   ❌ p[1] 是写端，不能读
 *     父: write(p[1]) → read(p[0])   ❌ p[0] 被父进程自己关了
 *
 *   所以需要两根：
 *     pipe A: 父进程写 → 子进程读
 *     pipe B: 子进程写 → 父进程读
 *
 * 结构图：
 *
 *   父进程                         子进程
 *   ──────                         ──────
 *   创建 pipeA[2], pipeB[2]
 *   fork()
 *     │                               │
 *   close(pipeA[0])                 close(pipeA[1])
 *   close(pipeB[1])                 close(pipeB[0])
 *     │                               │
 *   t0 = uptime()                    │
 *   for i in 0..N:                   for i in 0..N:
 *     write(pipeA[1], "x", 1) ────→    read(pipeA[0], buf, 1)
 *     read(pipeB[0], buf, 1)  ←────    write(pipeB[1], "y", 1)
 *   t1 = uptime()                     │
 *   print 耗时                        │
 *   wait(0)                          exit(0)
 */

#include "kernel/types.h"
#include "user/user.h"

#define NTIMES 100000 // 来回传递的次数

int
main(void)
{
  int pipeA[2], pipeB[2]; // pipeA: 父→子, pipeB: 子→父
  int pid;
  char buf[1];
  int t0, t1; // 开始和结束的 tick 数

  // 创建两根管道
  if (pipe(pipeA) < 0 || pipe(pipeB) < 0) {
    printf("pipe failed\n");
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    // ──── 子进程 ────
    // 子进程只需要：从 pipeA 读，往 pipeB 写
    close(pipeA[1]); // 关掉 pipeA 写端（不需要往父进程方向写）
    close(pipeB[0]); // 关掉 pipeB 读端（不需要从子进程方向读）

    for (int i = 0; i < NTIMES; i++) {
      // 等父进程把字节写过来
      if (read(pipeA[0], buf, 1) != 1) {
        printf("child: read failed\n");
        break;
      }
      // 把字节打回去
      if (write(pipeB[1], buf, 1) != 1) {
        printf("child: write failed\n");
        break;
      }
    }

    close(pipeA[0]);
    close(pipeB[1]);
    exit(0);

  } else {
    // ──── 父进程 ────
    // 父进程只需要：往 pipeA 写，从 pipeB 读
    close(pipeA[0]); // 关掉 pipeA 读端（不需要从子进程方向读）
    close(pipeB[1]); // 关掉 pipeB 写端（不需要往子进程方向写）

    // 开始计时
    t0 = uptime();

    buf[0] = 'x';
    for (int i = 0; i < NTIMES; i++) {
      // 发一个字节给子进程
      if (write(pipeA[1], buf, 1) != 1) {
        printf("parent: write failed\n");
        break;
      }
      // 等子进程把字节打回来
      if (read(pipeB[0], buf, 1) != 1) {
        printf("parent: read failed\n");
        break;
      }
    }

    // 结束计时
    t1 = uptime();

    close(pipeA[1]);
    close(pipeB[0]);

    // 等待子进程结束
    wait(0);

    // 打印结果
    // uptime() 返回的是 tick（时钟滴答）数，
    // xv6 每个 tick 约 10ms（由 QEMU 的 CLINT 定时器控制）。
    int elapsed = t1 - t0;
    printf(
      "pingpong: %d round-trips in %d ticks (avg %d.%d ticks/round-trip)\n",
      NTIMES, elapsed, elapsed / NTIMES, (elapsed * 10 / NTIMES) % 10);

    exit(0);
  }

  // 不会走到这里
  return 0;
}
