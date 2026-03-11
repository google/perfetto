/* Provide a real file - not a symlink - as it would cause multiarch conflicts
   when multiple different arch releases are installed simultaneously.  */

#if defined __aarch64__
#include "libunwind-aarch64.h"  // no-include-violation-check
#elif defined __arm__
#include "libunwind-arm.h"  // no-include-violation-check
#elif defined __hppa__
#include "libunwind-hppa.h"  // no-include-violation-check
#elif defined __ia64__
#include "libunwind-ia64.h"  // no-include-violation-check
#elif defined __mips__
#include "libunwind-mips.h"  // no-include-violation-check
#elif defined __powerpc__ && !defined __powerpc64__
#include "libunwind-ppc32.h"  // no-include-violation-check
#elif defined __powerpc64__
#include "libunwind-ppc64.h"  // no-include-violation-check
#elif defined __sh__
#include "libunwind-sh.h"  // no-include-violation-check
#elif defined __i386__
#include "libunwind-x86.h"  // no-include-violation-check
#elif defined __x86_64__
#include "libunwind-x86_64.h"  // no-include-violation-check
#elif defined __s390x__
#include "libunwind-s390x.h"  // no-include-violation-check
#elif defined __riscv || defined __riscv__
#include "libunwind-riscv.h"  // no-include-violation-check
#elif defined __loongarch64
#include "libunwind-loongarch64.h"  // no-include-violation-check
#else
#error "Unsupported arch"
#endif
