#define _GNU_SOURCE
#include "rackvm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for(pid_t child, int expected_signal)
{
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return -1;
    if (expected_signal)
        return WIFSIGNALED(status) && WTERMSIG(status) == expected_signal ? 0 : -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int main(void)
{
    pid_t allowed = fork();
    if (allowed < 0)
        return 1;
    if (allowed == 0) {
        if (rackvm_vcpu_sandbox() < 0)
            _exit(2);
        syscall(SYS_getpid);
        _exit(0);
    }
    if (wait_for(allowed, 0) < 0) {
        fputs("allowed syscall was rejected by vCPU sandbox\n", stderr);
        return 1;
    }

    pid_t denied = fork();
    if (denied < 0)
        return 1;
    if (denied == 0) {
        if (rackvm_vcpu_sandbox() < 0)
            _exit(2);
        syscall(SYS_getppid);
        _exit(0);
    }
    if (wait_for(denied, SIGSYS) < 0) {
        fputs("forbidden syscall escaped vCPU sandbox\n", stderr);
        return 1;
    }
    return 0;
}
