
#define SYSCALL(no)  _SYSCALL2(no)
#define _SYSCALL2(no) \
    __asm("li a7, " #no "\n" \
          "ecall")

#define SYSCALL_RET(no, ret)  _SYSCALL_RET2(no, ret)
#define _SYSCALL_RET2(no, ret) \
    __asm("li a7, " #no "\n" \
          "ecall" \
          : "=r"(ret))

#define __NR_exit    93

void exit(int code) {
  SYSCALL(__NR_exit);
}

int x;

int main() {
  return 123;
}

void _start(void) {
  __asm("lw a0, 0(sp)\n"  // argc
        "addi a1, sp, 8\n"  // argv
        // "slli a2, a0, 3\n"
        // "addi a2, a2, 8\n"
        // "add  a2, a2, a1\n"  // envp
        "call main\n"
        "j exit\n"
        );
}
