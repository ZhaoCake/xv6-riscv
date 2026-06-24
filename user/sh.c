// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

/*
 * xv6 shell — 命令行解释器。
 *
 * 整体工作流程：
 *   main() 循环读入一行命令 → parsecmd() 解析成命令树 → runcmd() 执行
 *
 * 命令类型通过一组结构体表示，所有结构体第一个字段都是 type（多态）：
 *   cmd         类型标记（基类型）
 *   execcmd     "ls -l" 这种普通命令
 *   redircmd    "cat > out.txt" 重定向
 *   pipecmd     "ls | grep foo" 管道
 *   listcmd     "echo a; echo b" 顺序执行
 *   backcmd     "sleep 100 &" 后台运行
 *
 * 解析器是递归下降的，调用层次：
 *   parsecmd → parseline → parsepipe → parseexec → parseredirs
 */

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

/*
 * 所有命令类型的「基类」——只用 type 字段区分子类型，
 * 运行时通过 switch/case 转型为具体的子结构体。
 */
struct cmd {
  int type;
};

/* 普通命令，如 "ls -l" */
struct execcmd {
  int type;
  char *argv[MAXARGS];  // 参数指针数组
  char *eargv[MAXARGS]; // 每个参数的结束位置（用于后续 NUL 截断）
};

/* 重定向，如 "cat > out.txt" 或 "grep < input.txt" */
struct redircmd {
  int type;
  struct cmd *cmd; // 被重定向的子命令
  char *file;      // 文件名
  char *efile;     // 文件名结束位置
  int mode;        // O_RDONLY / O_WRONLY ...
  int fd;          // 要替换的文件描述符（stdin=0 / stdout=1）
};

/* 管道连接，如 "ls | grep foo" */
struct pipecmd {
  int type;
  struct cmd *left;  // 管道左侧命令（ls）
  struct cmd *right; // 管道右侧命令（grep foo）
};

/* 顺序执行，如 "echo a; echo b" */
struct listcmd {
  int type;
  struct cmd *left;  // 先执行的
  struct cmd *right; // 后执行的
};

/* 后台运行，如 "sleep 100 &" */
struct backcmd {
  int type;
  struct cmd *cmd; // 要在后台执行的命令
};

int fork1(void); // Fork but panics on failure.
void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

/*
 * 执行命令树（递归核心）。永不返回。
 *
 * 对每种命令类型的处理：
 *
 *   EXEC  — 直接 exec()。只有 exec 失败才会返回（打印错误）。
 *   REDIR — 替换文件描述符（close + open 使 fd 指向文件），
 *           然后递归执行被重定向的子命令。
 *   LIST  — fork 子进程执行左边，父进程 wait 等它结束，
 *           然后自己在当前进程里执行右边。
 *   PIPE  — 创建管道，fork 两个子进程：
 *           左子进程：stdout → 管道写端
 *           右子进程：stdin  ← 管道读端
 *           父进程等待两边都结束。
 *   BACK  — fork 子进程在后台执行，父进程直接返回。
 */
void
runcmd(struct cmd *cmd)
{
  int p[2];
  // 初始化5种命令的结构体
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    exit(1);
  //这里写了一种退出的情况，但是如果exit，会发生什么呢？

  switch (cmd->type) {
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd *)cmd;
    if (ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  /*
   * 重定向实现技巧：close(fd) 释放一个文件描述符位置，
   * 然后 open() 总是分配最小的空闲 fd 号。
   * 所以 close(1) + open("file", O_WRONLY) 的效果就是
   * "把 stdout 重定向到 file"。
   */
  case REDIR:
    rcmd = (struct redircmd *)cmd;
    close(rcmd->fd);
    if (open(rcmd->file, rcmd->mode) < 0) {
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    if (fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  /*
   * 管道的实现：两个子进程之间建立一个临时通道。
   * pipe(p) 创建两根 fd：p[0] 读端，p[1] 写端。
   *
   * 左子进程（写数据的）：
   *   1. close(1) 释放 stdout 位置
   *   2. dup(p[1]) 使 fd 1（stdout）指向管道写端
   *   3. close(p[0]) 和 close(p[1]) 清理
   *   4. 递归执行左边的命令——它的 stdout 就是管道的写端
   *
   * 右子进程（读数据的）：
   *   1. close(0) 释放 stdin 位置
    *   2. dup(p[0]) 使 fd 0（stdin）指向管道读端
    *   3. 同理清理后递归执行右边的命令
   */
  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    if (pipe(p) < 0)
      panic("pipe");
    if (fork1() == 0) {
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if (fork1() == 0) {
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    if (fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf)
{
  write(2, "$ ", 2);
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if (buf[0] == 0) // EOF
    return -1;
  return 0;
}

int
main(void)
{
  static char buf[100];
  int fd;

  /*
   * 确保 0（stdin）、1（stdout）、2（stderr）三个 fd 都已打开。
   * xv6 内核在 exec() 新程序时不保证这三个 fd 存在，
   * 所以 shell 启动时主动打开 console，填满 0/1/2 后关闭多余的。
   */
  while ((fd = open("console", O_RDWR)) >= 0) {
    if (fd >= 3) {
      close(fd);
      break;
    }
  }

  // 主循环：读命令 → 解析 → fork → 执行 → 等待
  while (getcmd(buf, sizeof(buf)) >= 0) {
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\n') // 空行，什么也不做
      continue;
    if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' ') {
      /*
       * cd 必须由父进程（shell 自己）执行，不能在子进程里做。
       * 因为子进程的 chdir 只改变子进程自己的工作目录，
       * 父进程不受影响——下一行提示符就还在原目录。
       */
      cmd[strlen(cmd) - 1] = 0; // 去掉末尾的 \n
      if (chdir(cmd + 3) < 0)
        fprintf(2, "cannot cd %s\n", cmd + 3);
    } else {
      /*
       * 通用的 fork-exec-wait 模式：
       *   子进程解析命令并执行（永远不会返回）
       *   父进程等待子进程结束
       */
      if (fork1() == 0)
        runcmd(parsecmd(cmd));
      wait(0);
    }
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if (pid == -1)
    panic("fork");
  return pid;
}

/*
 * ============================================================
 *  命令树构造器
 *
 *  这些函数从堆上分配对应的 struct，填充 type 字段后
 *  统一转型为 struct cmd *（手动"多态"）。
 * ============================================================
 */

struct cmd *
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd *)cmd;
}

struct cmd *
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd *)cmd;
}

struct cmd *
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd *)cmd;
}
/*
 * ============================================================
 *  递归下降解析器
 *
 *  语法（简化的 BNF）：
 *    line    = pipe { ";" line }
 *    pipe    = exec { "|" pipe }
 *    exec    = { redir } ( word | "(" line ")" ) { redir }
 *    redir   = "<" word | ">" word | ">>" word
 *
 *  词法分析由 gettoken() 完成，peek() 做前瞻。
 *  每个 parseXXX 函数消费输入的一个语法单元，返回命令树节点。
 *
 *  解析入口：parsecmd() → parseline()
 * ============================================================
 */

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  if (q)
    *q = s;
  ret = *s;
  switch (*s) {
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if (*s == '>') {
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if (eq)
    *eq = s;

  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if (s != es) {
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd *
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while (peek(ps, es, "&")) {
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if (peek(ps, es, ";")) {
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd *
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if (peek(ps, es, "|")) {
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd *
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while (peek(ps, es, "<>")) {
    tok = gettoken(ps, es, 0, 0);
    if (gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch (tok) {
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE | O_TRUNC, 1);
      break;
    case '+': // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd *
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if (!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if (!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd *
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if (peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd *)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while (!peek(ps, es, "|)&;")) {
    if ((tok = gettoken(ps, es, &q, &eq)) == 0)
      break;
    if (tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if (argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd *
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    return 0;

  switch (cmd->type) {
  case EXEC:
    ecmd = (struct execcmd *)cmd;
    for (i = 0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
