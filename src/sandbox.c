#define _GNU_SOURCE
#include "rackvm.h"

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ALLOW_SYSCALL(name) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_##name, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

int rackvm_vcpu_sandbox(void)
{
#if defined(__x86_64__)
    static const struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        ALLOW_SYSCALL(ioctl),
        ALLOW_SYSCALL(write),
        ALLOW_SYSCALL(writev),
        ALLOW_SYSCALL(pread64),
        ALLOW_SYSCALL(pwrite64),
        ALLOW_SYSCALL(fdatasync),
        ALLOW_SYSCALL(futex),
#ifdef SYS_futex_waitv
        ALLOW_SYSCALL(futex_waitv),
#endif
        ALLOW_SYSCALL(sched_yield),
        ALLOW_SYSCALL(clock_gettime),
        ALLOW_SYSCALL(rt_sigreturn),
        ALLOW_SYSCALL(rt_sigprocmask),
        ALLOW_SYSCALL(restart_syscall),
        ALLOW_SYSCALL(getpid),
        ALLOW_SYSCALL(gettid),
        ALLOW_SYSCALL(tgkill),
        ALLOW_SYSCALL(exit),
        ALLOW_SYSCALL(exit_group),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
    };
    struct sock_fprog program = {
        .len = (unsigned short)(sizeof(instructions) / sizeof(instructions[0])),
        .filter = (struct sock_filter *)instructions
    };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return -1;
    return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program);
#else
    errno = ENOTSUP;
    return -1;
#endif
}
