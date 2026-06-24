#define SBRK_ERROR ((char *)-1)

struct stat;

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int *);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int close(int);
int kill(int);
/*
 * exec(path, argv) — 把当前进程替换为 path 指定的程序。
 *
 * 参数：
 *   path — 可执行文件路径，如 "/bin/ls" 或 "cat"
 *   argv — 参数字符串数组，以 NULL 结尾，如 {"ls", "-l", NULL}
 *
 * 返回值：
 *   成功 — 永不返回（当前进程已被替换）
 *   失败 — 返回 -1，原进程继续执行
 *
 * 典型用法（shell 模式）：
 *   if (fork() == 0) {      // 子进程
 *       exec("cat", av);    // 替换成 cat
 *       exit(1);            // 只有 exec 失败才走到这里
 *   }
 *   wait(0);                // 父进程等待
 */
int exec(const char *, char **);
int open(const char *, int);
int mknod(const char *, short, short);
int unlink(const char *);
int fstat(int fd, struct stat *);
int link(const char *, const char *);
int mkdir(const char *);
int chdir(const char *);
int dup(int);
int getpid(void);
char *sys_sbrk(int, int);
int pause(int);
int uptime(void);

// ulib.c
int stat(const char *, struct stat *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, int);
char *strchr(const char *, char c);
int strcmp(const char *, const char *);
char *gets(char *, int max);
uint strlen(const char *);
void *memset(void *, int, uint);
int atoi(const char *);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
char *sbrk(int);
char *sbrklazy(int);

// printf.c
void fprintf(int, const char *, ...) __attribute__((format(printf, 2, 3)));
void printf(const char *, ...) __attribute__((format(printf, 1, 2)));

// umalloc.c
void *malloc(uint);
void free(void *);
