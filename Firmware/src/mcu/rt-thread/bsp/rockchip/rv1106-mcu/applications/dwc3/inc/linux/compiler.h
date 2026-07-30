/* ponytail: minimal Linux compiler.h shim */
#ifndef _LINUX_COMPILER_H_SHIM
#define _LINUX_COMPILER_H_SHIM
#ifndef __iomem
#define __iomem
#endif
#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef barrier
#define barrier() __asm__ __volatile__("": : :"memory")
#endif
#endif
