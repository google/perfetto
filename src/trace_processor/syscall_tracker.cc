/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/syscall_tracker.h"

#include <utility>

#include <inttypes.h>

#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/stats.h"

namespace perfetto {
namespace trace_processor {
namespace {

// Syscall number to string.
// https://thog.github.io/syscalls-table-aarch64/latest.html
constexpr std::array<const char*, kSyscallCount> aarch64_to_syscall = {{
    "sys_io_setup",                //
    "sys_io_destroy",              //
    "sys_io_submit",               //
    "sys_io_cancel",               //
    "sys_io_getevents",            //
    "sys_setxattr",                //
    "sys_lsetxattr",               //
    "sys_fsetxattr",               //
    "sys_getxattr",                //
    "sys_lgetxattr",               //
    "sys_fgetxattr",               //
    "sys_listxattr",               //
    "sys_llistxattr",              //
    "sys_flistxattr",              //
    "sys_removexattr",             //
    "sys_lremovexattr",            //
    "sys_fremovexattr",            //
    "sys_getcwd",                  //
    "sys_lookup_dcookie",          //
    "sys_eventfd2",                //
    "sys_epoll_create1",           //
    "sys_epoll_ctl",               //
    "sys_epoll_pwait",             //
    "sys_dup",                     //
    "sys_dup3",                    //
    "sys_inotify_init1",           //
    "sys_inotify_add_watch",       //
    "sys_inotify_rm_watch",        //
    "sys_ioctl",                   //
    "sys_ioprio_set",              //
    "sys_ioprio_get",              //
    "sys_flock",                   //
    "sys_mknodat",                 //
    "sys_mkdirat",                 //
    "sys_unlinkat",                //
    "sys_symlinkat",               //
    "sys_linkat",                  //
    "sys_renameat",                //
    "sys_umount2",                 //
    "sys_mount",                   //
    "sys_pivot_root",              //
    "sys_nfsservctl",              //
    "sys_fallocate",               //
    "sys_faccessat",               //
    "sys_chdir",                   //
    "sys_fchdir",                  //
    "sys_chroot",                  //
    "sys_fchmod",                  //
    "sys_fchmodat",                //
    "sys_fchownat",                //
    "sys_fchown",                  //
    "sys_openat",                  //
    "sys_close",                   //
    "sys_vhangup",                 //
    "sys_pipe2",                   //
    "sys_quotactl",                //
    "sys_getdents64",              //
    "sys_read",                    //
    "sys_write",                   //
    "sys_readv",                   //
    "sys_writev",                  //
    "sys_pread64",                 //
    "sys_pwrite64",                //
    "sys_preadv",                  //
    "sys_pwritev",                 //
    "sys_pselect6",                //
    "sys_ppoll",                   //
    "sys_signalfd4",               //
    "sys_vmsplice",                //
    "sys_splice",                  //
    "sys_tee",                     //
    "sys_readlinkat",              //
    "sys_sync",                    //
    "sys_fsync",                   //
    "sys_fdatasync",               //
    "sys_sync_file_range2",        //
    "sys_sync_file_range",         //
    "sys_timerfd_create",          //
    "sys_timerfd_settime",         //
    "sys_timerfd_gettime",         //
    "sys_utimensat",               //
    "sys_acct",                    //
    "sys_capget",                  //
    "sys_capset",                  //
    "sys_personality",             //
    "sys_exit",                    //
    "sys_exit_group",              //
    "sys_waitid",                  //
    "sys_set_tid_address",         //
    "sys_unshare",                 //
    "sys_futex",                   //
    "sys_set_robust_list",         //
    "sys_get_robust_list",         //
    "sys_nanosleep",               //
    "sys_getitimer",               //
    "sys_setitimer",               //
    "sys_kexec_load",              //
    "sys_init_module",             //
    "sys_delete_module",           //
    "sys_timer_create",            //
    "sys_timer_gettime",           //
    "sys_timer_getoverrun",        //
    "sys_timer_settime",           //
    "sys_timer_delete",            //
    "sys_clock_settime",           //
    "sys_clock_gettime",           //
    "sys_clock_getres",            //
    "sys_clock_nanosleep",         //
    "sys_syslog",                  //
    "sys_ptrace",                  //
    "sys_sched_setparam",          //
    "sys_sched_setscheduler",      //
    "sys_sched_getscheduler",      //
    "sys_sched_getparam",          //
    "sys_sched_setaffinity",       //
    "sys_sched_getaffinity",       //
    "sys_sched_yield",             //
    "sys_sched_get_priority_max",  //
    "sys_sched_get_priority_min",  //
    "sys_sched_rr_get_interval",   //
    "sys_restart_syscall",         //
    "sys_kill",                    //
    "sys_tkill",                   //
    "sys_tgkill",                  //
    "sys_sigaltstack",             //
    "sys_rt_sigsuspend",           //
    "sys_rt_sigaction",            //
    "sys_rt_sigprocmask",          //
    "sys_rt_sigpending",           //
    "sys_rt_sigtimedwait",         //
    "sys_rt_sigqueueinfo",         //
    "sys_rt_sigreturn",            //
    "sys_setpriority",             //
    "sys_getpriority",             //
    "sys_reboot",                  //
    "sys_setregid",                //
    "sys_setgid",                  //
    "sys_setreuid",                //
    "sys_setuid",                  //
    "sys_setresuid",               //
    "sys_getresuid",               //
    "sys_setresgid",               //
    "sys_getresgid",               //
    "sys_setfsuid",                //
    "sys_setfsgid",                //
    "sys_times",                   //
    "sys_setpgid",                 //
    "sys_getpgid",                 //
    "sys_getsid",                  //
    "sys_setsid",                  //
    "sys_getgroups",               //
    "sys_setgroups",               //
    "sys_uname",                   //
    "sys_sethostname",             //
    "sys_setdomainname",           //
    "sys_getrlimit",               //
    "sys_setrlimit",               //
    "sys_getrusage",               //
    "sys_umask",                   //
    "sys_prctl",                   //
    "sys_getcpu",                  //
    "sys_gettimeofday",            //
    "sys_settimeofday",            //
    "sys_adjtimex",                //
    "sys_getpid",                  //
    "sys_getppid",                 //
    "sys_getuid",                  //
    "sys_geteuid",                 //
    "sys_getgid",                  //
    "sys_getegid",                 //
    "sys_gettid",                  //
    "sys_sysinfo",                 //
    "sys_mq_open",                 //
    "sys_mq_unlink",               //
    "sys_mq_timedsend",            //
    "sys_mq_timedreceive",         //
    "sys_mq_notify",               //
    "sys_mq_getsetattr",           //
    "sys_msgget",                  //
    "sys_msgctl",                  //
    "sys_msgrcv",                  //
    "sys_msgsnd",                  //
    "sys_semget",                  //
    "sys_semctl",                  //
    "sys_semtimedop",              //
    "sys_semop",                   //
    "sys_shmget",                  //
    "sys_shmctl",                  //
    "sys_shmat",                   //
    "sys_shmdt",                   //
    "sys_socket",                  //
    "sys_socketpair",              //
    "sys_bind",                    //
    "sys_listen",                  //
    "sys_accept",                  //
    "sys_connect",                 //
    "sys_getsockname",             //
    "sys_getpeername",             //
    "sys_sendto",                  //
    "sys_recvfrom",                //
    "sys_setsockopt",              //
    "sys_getsockopt",              //
    "sys_shutdown",                //
    "sys_sendmsg",                 //
    "sys_recvmsg",                 //
    "sys_readahead",               //
    "sys_brk",                     //
    "sys_munmap",                  //
    "sys_mremap",                  //
    "sys_add_key",                 //
    "sys_request_key",             //
    "sys_keyctl",                  //
    "sys_clone",                   //
    "sys_execve",                  //
    "sys_swapon",                  //
    "sys_swapoff",                 //
    "sys_mprotect",                //
    "sys_msync",                   //
    "sys_mlock",                   //
    "sys_munlock",                 //
    "sys_mlockall",                //
    "sys_munlockall",              //
    "sys_mincore",                 //
    "sys_madvise",                 //
    "sys_remap_file_pages",        //
    "sys_mbind",                   //
    "sys_get_mempolicy",           //
    "sys_set_mempolicy",           //
    "sys_migrate_pages",           //
    "sys_move_pages",              //
    "sys_rt_tgsigqueueinfo",       //
    "sys_perf_event_open",         //
    "sys_accept4",                 //
    "sys_recvmmsg",                //
    "sys_arch_specific_syscall",   //
    "sys_wait4",                   //
    "sys_prlimit64",               //
    "sys_fanotify_init",           //
    "sys_fanotify_mark",           //
    "sys_name_to_handle_at",       //
    "sys_open_by_handle_at",       //
    "sys_clock_adjtime",           //
    "sys_syncfs",                  //
    "sys_setns",                   //
    "sys_sendmmsg",                //
    "sys_process_vm_readv",        //
    "sys_process_vm_writev",       //
    "sys_kcmp",                    //
    "sys_finit_module",            //
    "sys_sched_setattr",           //
    "sys_sched_getattr",           //
    "sys_renameat2",               //
    "sys_seccomp",                 //
    "sys_getrandom",               //
    "sys_memfd_create",            //
    "sys_bpf",                     //
    "sys_execveat",                //
    "sys_userfaultfd",             //
    "sys_membarrier",              //
    "sys_mlock2",                  //
    "sys_copy_file_range",         //
    "sys_preadv2",                 //
    "sys_pwritev2",                //
    "sys_pkey_mprotect",           //
    "sys_pkey_alloc",              //
    "sys_pkey_free",               //
    "sys_statx",                   //
}};

// Syscall number to string.
// https://filippo.io/linux-syscall-table/
// http://blog.rchapman.org/posts/Linux_System_Call_Table_for_x86_64/
constexpr std::array<const char*, kSyscallCount> x86_64_to_syscall = {{
    "sys_read",                    //
    "sys_write",                   //
    "sys_open",                    //
    "sys_close",                   //
    "sys_newstat",                 //
    "sys_newfstat",                //
    "sys_newlstat",                //
    "sys_poll",                    //
    "sys_lseek",                   //
    "sys_mmap",                    //
    "sys_mprotect",                //
    "sys_munmap",                  //
    "sys_brk",                     //
    "sys_rt_sigaction",            //
    "sys_rt_sigprocmask",          //
    "stub_rt_sigreturn",           //
    "sys_ioctl",                   //
    "sys_pread64",                 //
    "sys_pwrite64",                //
    "sys_readv",                   //
    "sys_writev",                  //
    "sys_access",                  //
    "sys_pipe",                    //
    "sys_select",                  //
    "sys_sched_yield",             //
    "sys_mremap",                  //
    "sys_msync",                   //
    "sys_mincore",                 //
    "sys_madvise",                 //
    "sys_shmget",                  //
    "sys_shmat",                   //
    "sys_shmctl",                  //
    "sys_dup",                     //
    "sys_dup2",                    //
    "sys_pause",                   //
    "sys_nanosleep",               //
    "sys_getitimer",               //
    "sys_alarm",                   //
    "sys_setitimer",               //
    "sys_getpid",                  //
    "sys_sendfile64",              //
    "sys_socket",                  //
    "sys_connect",                 //
    "sys_accept",                  //
    "sys_sendto",                  //
    "sys_recvfrom",                //
    "sys_sendmsg",                 //
    "sys_recvmsg",                 //
    "sys_shutdown",                //
    "sys_bind",                    //
    "sys_listen",                  //
    "sys_getsockname",             //
    "sys_getpeername",             //
    "sys_socketpair",              //
    "sys_setsockopt",              //
    "sys_getsockopt",              //
    "stub_clone",                  //
    "stub_fork",                   //
    "stub_vfork",                  //
    "stub_execve",                 //
    "sys_exit",                    //
    "sys_wait4",                   //
    "sys_kill",                    //
    "sys_newuname",                //
    "sys_semget",                  //
    "sys_semop",                   //
    "sys_semctl",                  //
    "sys_shmdt",                   //
    "sys_msgget",                  //
    "sys_msgsnd",                  //
    "sys_msgrcv",                  //
    "sys_msgctl",                  //
    "sys_fcntl",                   //
    "sys_flock",                   //
    "sys_fsync",                   //
    "sys_fdatasync",               //
    "sys_truncate",                //
    "sys_ftruncate",               //
    "sys_getdents",                //
    "sys_getcwd",                  //
    "sys_chdir",                   //
    "sys_fchdir",                  //
    "sys_rename",                  //
    "sys_mkdir",                   //
    "sys_rmdir",                   //
    "sys_creat",                   //
    "sys_link",                    //
    "sys_unlink",                  //
    "sys_symlink",                 //
    "sys_readlink",                //
    "sys_chmod",                   //
    "sys_fchmod",                  //
    "sys_chown",                   //
    "sys_fchown",                  //
    "sys_lchown",                  //
    "sys_umask",                   //
    "sys_gettimeofday",            //
    "sys_getrlimit",               //
    "sys_getrusage",               //
    "sys_sysinfo",                 //
    "sys_times",                   //
    "sys_ptrace",                  //
    "sys_getuid",                  //
    "sys_syslog",                  //
    "sys_getgid",                  //
    "sys_setuid",                  //
    "sys_setgid",                  //
    "sys_geteuid",                 //
    "sys_getegid",                 //
    "sys_setpgid",                 //
    "sys_getppid",                 //
    "sys_getpgrp",                 //
    "sys_setsid",                  //
    "sys_setreuid",                //
    "sys_setregid",                //
    "sys_getgroups",               //
    "sys_setgroups",               //
    "sys_setresuid",               //
    "sys_getresuid",               //
    "sys_setresgid",               //
    "sys_getresgid",               //
    "sys_getpgid",                 //
    "sys_setfsuid",                //
    "sys_setfsgid",                //
    "sys_getsid",                  //
    "sys_capget",                  //
    "sys_capset",                  //
    "sys_rt_sigpending",           //
    "sys_rt_sigtimedwait",         //
    "sys_rt_sigqueueinfo",         //
    "sys_rt_sigsuspend",           //
    "sys_sigaltstack",             //
    "sys_utime",                   //
    "sys_mknod",                   //
    "",                            // uselib
    "sys_personality",             //
    "sys_ustat",                   //
    "sys_statfs",                  //
    "sys_fstatfs",                 //
    "sys_sysfs",                   //
    "sys_getpriority",             //
    "sys_setpriority",             //
    "sys_sched_setparam",          //
    "sys_sched_getparam",          //
    "sys_sched_setscheduler",      //
    "sys_sched_getscheduler",      //
    "sys_sched_get_priority_max",  //
    "sys_sched_get_priority_min",  //
    "sys_sched_rr_get_interval",   //
    "sys_mlock",                   //
    "sys_munlock",                 //
    "sys_mlockall",                //
    "sys_munlockall",              //
    "sys_vhangup",                 //
    "sys_modify_ldt",              //
    "sys_pivot_root",              //
    "sys_sysctl",                  //
    "sys_prctl",                   //
    "sys_arch_prctl",              //
    "sys_adjtimex",                //
    "sys_setrlimit",               //
    "sys_chroot",                  //
    "sys_sync",                    //
    "sys_acct",                    //
    "sys_settimeofday",            //
    "sys_mount",                   //
    "sys_umount",                  //
    "sys_swapon",                  //
    "sys_swapoff",                 //
    "sys_reboot",                  //
    "sys_sethostname",             //
    "sys_setdomainname",           //
    "stub_iopl",                   //
    "sys_ioperm",                  //
    "",                            // create_module
    "sys_init_module",             //
    "sys_delete_module",           //
    "",                            // get_kernel_syms
    "",                            // query_module
    "sys_quotactl",                //
    "",                            // nfsservctl
    "",                            // getpmsg
    "",                            // putpmsg
    "",                            // afs_syscall
    "",                            // tuxcall
    "",                            // security
    "sys_gettid",                  //
    "sys_readahead",               //
    "sys_setxattr",                //
    "sys_lsetxattr",               //
    "sys_fsetxattr",               //
    "sys_getxattr",                //
    "sys_lgetxattr",               //
    "sys_fgetxattr",               //
    "sys_listxattr",               //
    "sys_llistxattr",              //
    "sys_flistxattr",              //
    "sys_removexattr",             //
    "sys_lremovexattr",            //
    "sys_fremovexattr",            //
    "sys_tkill",                   //
    "sys_time",                    //
    "sys_futex",                   //
    "sys_sched_setaffinity",       //
    "sys_sched_getaffinity",       //
    "",                            // set_thread_area
    "sys_io_setup",                //
    "sys_io_destroy",              //
    "sys_io_getevents",            //
    "sys_io_submit",               //
    "sys_io_cancel",               //
    "",                            // get_thread_area
    "sys_lookup_dcookie",          //
    "sys_epoll_create",            //
    "",                            // epoll_ctl_old
    "",                            // epoll_wait_old
    "sys_remap_file_pages",        //
    "sys_getdents64",              //
    "sys_set_tid_address",         //
    "sys_restart_syscall",         //
    "sys_semtimedop",              //
    "sys_fadvise64",               //
    "sys_timer_create",            //
    "sys_timer_settime",           //
    "sys_timer_gettime",           //
    "sys_timer_getoverrun",        //
    "sys_timer_delete",            //
    "sys_clock_settime",           //
    "sys_clock_gettime",           //
    "sys_clock_getres",            //
    "sys_clock_nanosleep",         //
    "sys_exit_group",              //
    "sys_epoll_wait",              //
    "sys_epoll_ctl",               //
    "sys_tgkill",                  //
    "sys_utimes",                  //
    "",                            // vserver
    "sys_mbind",                   //
    "sys_set_mempolicy",           //
    "sys_get_mempolicy",           //
    "sys_mq_open",                 //
    "sys_mq_unlink",               //
    "sys_mq_timedsend",            //
    "sys_mq_timedreceive",         //
    "sys_mq_notify",               //
    "sys_mq_getsetattr",           //
    "sys_kexec_load",              //
    "sys_waitid",                  //
    "sys_add_key",                 //
    "sys_request_key",             //
    "sys_keyctl",                  //
    "sys_ioprio_set",              //
    "sys_ioprio_get",              //
    "sys_inotify_init",            //
    "sys_inotify_add_watch",       //
    "sys_inotify_rm_watch",        //
    "sys_migrate_pages",           //
    "sys_openat",                  //
    "sys_mkdirat",                 //
    "sys_mknodat",                 //
    "sys_fchownat",                //
    "sys_futimesat",               //
    "sys_newfstatat",              //
    "sys_unlinkat",                //
    "sys_renameat",                //
    "sys_linkat",                  //
    "sys_symlinkat",               //
    "sys_readlinkat",              //
    "sys_fchmodat",                //
    "sys_faccessat",               //
    "sys_pselect6",                //
    "sys_ppoll",                   //
    "sys_unshare",                 //
    "sys_set_robust_list",         //
    "sys_get_robust_list",         //
    "sys_splice",                  //
    "sys_tee",                     //
    "sys_sync_file_range",         //
    "sys_vmsplice",                //
    "sys_move_pages",              //
    "sys_utimensat",               //
    "sys_epoll_pwait",             //
    "sys_signalfd",                //
    "sys_timerfd_create",          //
    "sys_eventfd",                 //
    "sys_fallocate",               //
    "sys_timerfd_settime",         //
    "sys_timerfd_gettime",         //
    "sys_accept4",                 //
    "sys_signalfd4",               //
    "sys_eventfd2",                //
    "sys_epoll_create1",           //
    "sys_dup3",                    //
    "sys_pipe2",                   //
    "sys_inotify_init1",           //
    "sys_preadv",                  //
    "sys_pwritev",                 //
    "sys_rt_tgsigqueueinfo",       //
    "sys_perf_event_open",         //
    "sys_recvmmsg",                //
    "sys_fanotify_init",           //
    "sys_fanotify_mark",           //
    "sys_prlimit64",               //
    "sys_name_to_handle_at",       //
    "sys_open_by_handle_at",       //
    "sys_clock_adjtime",           //
    "sys_syncfs",                  //
    "sys_sendmmsg",                //
    "sys_setns",                   //
    "sys_getcpu",                  //
    "sys_process_vm_readv",        //
    "sys_process_vm_writev",       //
    "sys_kcmp",                    //
    "sys_finit_module",            //
}};

// When we don't know the architecture map every syscall number to
// null string.
constexpr std::array<const char*, kSyscallCount> unknown_to_syscall = {{}};

}  // namespace

SyscallTracker::SyscallTracker(TraceProcessorContext* context)
    : context_(context) {
  // This sets arch_syscall_to_string_id_
  SetArchitecture(kUnknown);
}

SyscallTracker::~SyscallTracker() = default;

void SyscallTracker::SetArchitecture(Architecture arch) {
  const std::array<const char*, kSyscallCount>* arch_to_generic_syscall_number;
  switch (arch) {
    case kAarch64:
      arch_to_generic_syscall_number = &aarch64_to_syscall;
      break;
    case kX86_64:
      arch_to_generic_syscall_number = &x86_64_to_syscall;
      break;
    case kUnknown:
      arch_to_generic_syscall_number = &unknown_to_syscall;
      break;
  }

  for (size_t i = 0; i < kSyscallCount; i++) {
    const char* name = (*arch_to_generic_syscall_number)[i];
    StringId id =
        context_->storage->InternString(name ? name : "UNKNOWN_SYSCALL");
    arch_syscall_to_string_id_[i] = id;
    if (name && !strcmp(name, "sys_write"))
      sys_write_string_id_ = id;
  }
}

void SyscallTracker::Enter(int64_t ts, UniqueTid utid, uint32_t syscall_num) {
  StringId name = SyscallNumberToStringId(syscall_num);
  if (!name) {
    context_->storage->IncrementStats(stats::sys_unknown_syscall);
    return;
  }

  context_->slice_tracker->Begin(ts, utid, 0 /* cat */, name);
}

void SyscallTracker::Exit(int64_t ts, UniqueTid utid, uint32_t syscall_num) {
  StringId name = SyscallNumberToStringId(syscall_num);
  if (!name) {
    context_->storage->IncrementStats(stats::sys_unknown_syscall);
    return;
  }

  context_->slice_tracker->End(ts, utid, 0 /* cat */, name);
}

}  // namespace trace_processor
}  // namespace perfetto
