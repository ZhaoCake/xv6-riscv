# xv6-riscv — teaching OS for RISC-V

## Stack
- **Language** — ANSI C (gnu99), with some assembly (`kernel/*.S`)
- **Architecture** — RISC-V (rv64gc), runs under QEMU (`qemu-system-riscv64`)
- **Toolchain** — `riscv64-unknown-elf-*` / `riscv64-linux-gnu-*` cross-compiler (auto-detected by Makefile)
- **Build system** — bare GNU Make (no CMake/Meson)

## Layout
| Path | Contents |
|------|----------|
| `kernel/` | OS kernel: entry/S, process mgmt, memory allocator, file system, traps, syscalls, drivers (virtio, UART, PLIC) |
| `user/` | User-space programs (cat, echo, grep, sh, ls, usertests, etc.) |
| `mkfs/` | File-system image builder (`mkfs/mkfs.c` — compiled with host gcc) |
| `docs/` | Study notes (in Chinese) — user-added, not part of upstream xv6 |
| `test-xv6.py` | Python test harness that drives qemu and checks output |

## Commands
| Command | Action |
|---------|--------|
| `make qemu` | Build kernel + user programs + fs.img, then launch in QEMU |
| `make qemu-gdb` | Same, but starts QEMU paused waiting for GDB (`:1234`) |
| `make clean` | Remove all build artifacts (.o, .d, .asm, .sym, kernel, fs.img, mkfs/mkfs, usys.S) |
| `make fmt` | Format all `kernel/*.[ch]` `user/*.[ch]` `mkfs/*.c` via clang-format |
| `./test-xv6.py <test>` | Run automated tests (e.g. `usertests`, `crash`, `log`) |
| `nix develop` | Enter Nix dev shell with RISC-V toolchain + QEMU (if using Nix) |

## Conventions
- **Naming**: kernel source uses short lowercase + underscore (`kalloc.c`, `sleeplock.c`, `virtio_disk.c`). User programs match Unix names (`cat.c`, `grep.c`, `sh.c`).
- **Headers**: Kernel declares shared functions in `kernel/defs.h`. Architecture constants in `kernel/memlayout.h`, `kernel/param.h`.
- **User programs**: Compiled with `_` prefix in the build output (`_cat`, `_echo`) and linked against `ulib.o` + `usys.o` + `printf.o` + `umalloc.o`.
- **Formatting**: `.clang-format` at root — `clang-format` with `SortIncludes: Never`, `ReflowComments: Never`, custom brace wrapping.

## Watch out for
- **Cross-compilation**: Everything except `mkfs/mkfs` uses the RISC-V cross-toolchain. Host `gcc` won't link kernel objects — use `make` or `nix develop`.
- **Committed build artifacts**: `.o`, `.d`, `.asm`, `.sym`, `kernel/kernel`, `user/_*` binaries are tracked in git — `make clean` then `make qemu` if toolchain changes.
- **Python test harness**: `test-xv6.py` drives actual QEMU — slow but definitive. Pass `-q` to `usertests` for quick subset.
- **`fs.img`** is the root filesystem — rebuilt from `mkfs/mkfs` + listed user programs + `README`. Changes to user programs require `make qemu` (not just a re-link).
