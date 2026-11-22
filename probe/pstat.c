// Diagnostic helper (probe branch only): runs a command and prints the CPU's
// own counters for it and its threads, like `perf stat`: user-mode
// instructions, cycles, branches, branch misses, last-level cache misses,
// and task clock. Prints "pstat err N" where the counters are not allowed.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int open_counter(pid_t pid, uint32_t type, uint64_t config, int user_only) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = type;
    a.config = config;
    a.disabled = 1;
    a.inherit = 1;
    a.enable_on_exec = 1;
    a.exclude_kernel = user_only;
    a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, pid, -1, -1, 0);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    int go[2];
    if (pipe(go) != 0) return 2;
    pid_t child = fork();
    if (child == 0) {
        char c;
        close(go[1]);
        if (read(go[0], &c, 1) < 0) _exit(126);
        execvp(argv[1], argv + 1);
        _exit(127);
    }
    close(go[0]);
    static const struct { uint32_t type; uint64_t config; int user; const char* name; double unit; } E[] = {
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, 1, "i", 1e6},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 1, "c", 1e6},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, 1, "b", 1e6},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, 1, "m", 1e6},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, 1, "l", 1e6},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 0, "ck", 1e6},
        {PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK, 0, "t", 1e6},
        {PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS, 0, "pf", 1e3},
    };
    enum { N = sizeof E / sizeof E[0] };
    int fd[N], first_error = 0;
    for (int k = 0; k < N; k++) {
        fd[k] = open_counter(child, E[k].type, E[k].config, E[k].user);
        if (fd[k] < 0 && !first_error) first_error = errno;
    }
    if (write(go[1], "x", 1) != 1) return 2;
    close(go[1]);
    int status = 0;
    waitpid(child, &status, 0);
    char line[512];
    int at = snprintf(line, sizeof line, "pstat");
    for (int k = 0; k < N; k++) {
        uint64_t v = 0;
        if (fd[k] >= 0 && read(fd[k], &v, 8) == 8) at += snprintf(line + at, sizeof line - at, " %s=%.1f", E[k].name, (double)v / E[k].unit);
        else at += snprintf(line + at, sizeof line - at, " %s=-", E[k].name);
    }
    if (first_error) at += snprintf(line + at, sizeof line - at, " err%d", first_error);
    fprintf(stderr, "%s\n", line);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
