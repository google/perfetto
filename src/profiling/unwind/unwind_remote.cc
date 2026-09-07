#include <libunwind-ptrace.h>
#include <libunwind.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

void UnwindRemotePid(pid_t pid);
[[noreturn]] void ChildTargetFunction(void);

[[noreturn]] void ChildTargetFunction(void) {
  while (1) {
    sleep(1);
  }
}

void UnwindRemotePid(pid_t pid) {
  // 1. Attach to the remote process using ptrace
  if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
    perror("ptrace attach failed");
    return;
  }

  // Wait for the target process to be stopped by the SIGSTOP signal
  int status;
  waitpid(pid, &status, 0);

  // 2. Create the libunwind address space backed by ptrace accessors
  // (_UPT_accessors). These accessors implement memory/register reads via
  // ptrace PEEKDATA / GETREGS.
  unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, 0);
  if (!as) {
    fprintf(stderr, "Failed to create address space\n");
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    return;
  }

  // 3. Create the UPT (Unwind-Ptrace) context bound to this specific PID
  void* upt_context = _UPT_create(pid);
  if (!upt_context) {
    fprintf(stderr, "Failed to create UPT context\n");
    unw_destroy_addr_space(as);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    return;
  }

  // 4. Initialize remote cursor
  unw_cursor_t cursor;
  if (unw_init_remote(&cursor, as, upt_context) < 0) {
    fprintf(stderr, "Failed to initialize remote cursor\n");
  } else {
    printf("Remote backtrace for PID %d:\n", pid);

    // 5. Step through the remote callstack
    while (unw_step(&cursor) > 0) {
      unw_word_t ip = 0, offset = 0;
      char sym_buffer[256];

      unw_get_reg(&cursor, UNW_REG_IP, &ip);
      if (unw_get_proc_name(&cursor, sym_buffer, sizeof(sym_buffer), &offset) ==
          0) {
        printf("  [0x%016lx] %s + 0x%lx\n", ip, sym_buffer, offset);
      } else {
        printf("  [0x%016lx] unknown/stripped\n", ip);
      }
    }
  }

  // 6. Cleanup UPT context and address space
  _UPT_destroy(upt_context);
  unw_destroy_addr_space(as);

  // Detach from the target process and let it continue running
  ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
}

int main(int argc, char** argv) {
  if (argc > 1) {
    // Unwind an external process by PID passed as an argument
    pid_t target_pid = atoi(argv[1]);
    UnwindRemotePid(target_pid);
  } else {
    // Fork a child process to demonstrate remote unwinding automatically
    pid_t child_pid = fork();
    if (child_pid == 0) {
      // In child: run a loop
      ChildTargetFunction();
    } else {
      // In parent: wait a moment for the child to enter sleep, then unwind it
      usleep(100000);  // 100ms
      UnwindRemotePid(child_pid);

      // Clean up child process
      kill(child_pid, SIGKILL);
      waitpid(child_pid, nullptr, 0);
    }
  }
  return 0;
}
