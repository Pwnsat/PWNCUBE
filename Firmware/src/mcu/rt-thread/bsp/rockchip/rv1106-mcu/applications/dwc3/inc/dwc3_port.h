/*
 * dwc3_port.h — bare-metal glue for the U-Boot DWC3 gadget stack on RT-Thread.
 *
 * The U-Boot files (dwc3_core.c / dwc3_gadget.c / dwc3_ep0.c and the composite
 * gadget core) expect Linux/U-Boot facilities: readl/writel, DMA + cache ops,
 * malloc, spinlocks, logging, timers. On the SCR1 MCU with DCACHE OFF and flat
 * physical memory most of these collapse to trivial or no-op forms. This one
 * header provides them; the stub headers under inc/ (common.h, malloc.h,
 * asm/*.h, linux/*.h, dm.h ...) just pull this in. Force-included via -include.
 */
#ifndef _DWC3_PORT_H_
#define _DWC3_PORT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

/* --- fixed-width + U-Boot/Linux type aliases --- */
typedef uint8_t   u8;   typedef int8_t   s8;   typedef uint8_t  __u8;
typedef uint16_t  u16;  typedef int16_t  s16;  typedef uint16_t __u16;
typedef uint32_t  u32;  typedef int32_t  s32;  typedef uint32_t __u32;
typedef uint64_t  u64;  typedef int64_t  s64;  typedef uint64_t __u64;
typedef uint16_t  __le16; typedef uint16_t __be16;
typedef uint32_t  __le32; typedef uint32_t __be32;
typedef uint64_t  __le64; typedef uint64_t __be64;
typedef unsigned long  ulong;
typedef unsigned int   uint;
typedef unsigned int   gfp_t;
typedef unsigned long  dma_addr_t;
typedef unsigned long  phys_addr_t;

/* markers that mean nothing bare-metal */
#define __iomem
#define __force
#define __user
#define __must_check
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#define __deprecated

/* dummy devicetree node handle: we set dr_mode/speed directly, never parse OF */
typedef int ofnode;

/* U-Boot Kconfig machinery: CONFIG_IS_ENABLED(x)==0 unless CONFIG_x defined.
 * We want gadget on, host/DM/PHY off. */
#define CONFIG_USB_DWC3_GADGET 1
#define CONFIG_USB_XHCI_HCD 1
#define CONFIG_USB_MAX_CONTROLLER_COUNT 1
#define CONFIG_SYS_HZ 1000
#define CONFIG_USB_GADGET_VBUS_DRAW 2
#include <linux/kconfig.h>
#define uninitialized_var(x) x
#ifndef NULL
#define NULL ((void *)0)
#endif

/* --- MMIO (io.h subtracts the register base for us) --- */
static inline u32  readl(const volatile void *a)  { return *(const volatile u32 *)a; }
static inline void writel(u32 v, volatile void *a){ *(volatile u32 *)a = v; }
static inline u16  readw(const volatile void *a)  { return *(const volatile u16 *)a; }
static inline void writew(u16 v, volatile void *a){ *(volatile u16 *)a = v; }
static inline u8   readb(const volatile void *a)  { return *(const volatile u8 *)a; }
static inline void writeb(u8 v, volatile void *a) { *(volatile u8 *)a = v; }

/* --- endian (RISC-V is little-endian -> identity) --- */
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
#define __constant_cpu_to_le16(x) ((__le16)(x))
#define __constant_cpu_to_le32(x) ((__le32)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define cpu_to_le16s(p) do {} while (0)
#define le16_to_cpus(p) do {} while (0)
#define cpu_to_be16(x) ((__be16)__builtin_bswap16(x))
#define be16_to_cpu(x) ((u16)__builtin_bswap16(x))

static inline u16 get_unaligned_le16(const void *p){ const u8*b=p; return b[0]|(b[1]<<8); }
static inline u32 get_unaligned_le32(const void *p){ const u8*b=p; return b[0]|(b[1]<<8)|(b[2]<<16)|((u32)b[3]<<24); }
static inline void put_unaligned_le16(u16 v, void *p){ u8*b=p; b[0]=v; b[1]=v>>8; }
#define get_unaligned(p)    (*(p))
#define put_unaligned(v,p)  (*(p) = (v))
#define lower_32_bits(n) ((u32)(n))
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))

/* --- small helpers --- */
#define offsetof_(t,m) __builtin_offsetof(t,m)
#ifndef offsetof
#define offsetof(t,m) __builtin_offsetof(t,m)
#endif
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define clamp(v,lo,hi) max(lo, min(v, hi))
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) (((~0UL) << (l)) & (~0UL >> (32 - 1 - (h))))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ALIGN(x,a) (((x) + (a) - 1) & ~((unsigned long)(a) - 1))
#define ROUND(a,b) (((a) + (b) - 1) & ~((unsigned long)(b) - 1))
#define PTR_ALIGN(p,a) ((void *)ALIGN((unsigned long)(p), (a)))
#define IS_ALIGNED(x,a) (((x) & ((typeof(x))(a) - 1)) == 0)
#define roundup(x,y) ((((x) + (y) - 1) / (y)) * (y))
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#define __must_be_array(a) 0
#define BUILD_BUG_ON_NOT_POWER_OF_2(n)
#define BUILD_BUG_ON(c)
#define __le16_to_cpu(x) le16_to_cpu(x)
#define __le32_to_cpu(x) le32_to_cpu(x)
#define __cpu_to_le16(x) cpu_to_le16(x)
#define dev_set_name(dev, ...) do {} while (0)
#define dma_mapping_error(dev, addr) (0)

/* --- bitops (single-word bitmaps: 32-bit longs on rv32) --- */
#define BITS_PER_LONG 32
#define BIT_MASK(nr)  (1UL << ((nr) % 32))
#define BIT_WORD(nr)  ((nr) / 32)
#define DECLARE_BITMAP(name, bits) unsigned long name[((bits) + 31) / 32]
static inline void generic_set_bit(int nr, volatile unsigned long *a){ a[BIT_WORD(nr)] |= BIT_MASK(nr); }
static inline void generic_clear_bit(int nr, volatile unsigned long *a){ a[BIT_WORD(nr)] &= ~BIT_MASK(nr); }
static inline int  test_bit(int nr, const volatile unsigned long *a){ return (a[BIT_WORD(nr)] >> ((nr) % 32)) & 1; }
#define set_bit(nr, a)   generic_set_bit(nr, a)
#define clear_bit(nr, a) generic_clear_bit(nr, a)
#define __set_bit(nr, a) generic_set_bit(nr, a)
static inline void bitmap_zero(unsigned long *dst, int nbits){ memset(dst, 0, (((nbits) + 31) / 32) * sizeof(unsigned long)); }
#define ffs(x)  __builtin_ffs(x)
#define fls(x)  ((x) ? (32 - __builtin_clz(x)) : 0)

/* --- irq return codes (our poller reads them) --- */
typedef int irqreturn_t;
#define IRQ_NONE        0
#define IRQ_HANDLED     1
#define IRQ_WAKE_THREAD 2

/* --- mutex: single-threaded poller -> no-ops --- */
struct mutex { int _x; };
#define DEFINE_MUTEX(m)   struct mutex m
#define mutex_init(m)     do {} while (0)
#define mutex_lock(m)     do {} while (0)
#define mutex_unlock(m)   do {} while (0)

/* --- errno codes the toolchain header lacks --- */
#ifndef ESHUTDOWN
#define ESHUTDOWN 108
#endif
#ifndef EISNAM
#define EISNAM 120
#endif
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif

/* --- errno + error pointers --- */
#include <errno.h>
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) ((unsigned long)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long e){ return (void *)e; }
static inline long  PTR_ERR(const void *p){ return (long)p; }
static inline int   IS_ERR(const void *p){ return IS_ERR_VALUE((unsigned long)p); }
static inline int   IS_ERR_OR_NULL(const void *p){ return !p || IS_ERR(p); }

/* --- heap (Ponytail: route to RT-Thread heap) ---
 * ONE consistent aligned allocator. rt_free cannot free an rt_malloc_align
 * pointer (align stores the base below the aligned addr), so mixing memalign->
 * rt_malloc_align with free->rt_free corrupts the heap. Instead every alloc
 * over-allocates, stores the rt_malloc base just below the returned (aligned)
 * pointer, and a single port_free() recovers it — so free() works for plain
 * AND aligned allocations alike. */
void *rt_malloc(unsigned int);
void  rt_free(void *);
static inline void *port_alloc(unsigned long n, unsigned long align)
{
	char *base, *a;
	if (align < sizeof(void *)) align = sizeof(void *);
	base = (char *)rt_malloc((unsigned int)(n + align + sizeof(void *)));
	if (!base) return 0;
	a = (char *)(((uintptr_t)base + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1));
	((void **)a)[-1] = base;
	return a;
}
static inline void port_free(void *p){ if (p) rt_free(((void **)p)[-1]); }
#define malloc(n)        port_alloc((n), sizeof(void *))
#define free(p)          port_free(p)
#define memalign(a,n)    port_alloc((n), (a))
static inline void *calloc(size_t n, size_t s){ void*p=port_alloc((unsigned long)n*s, sizeof(void*)); if(p) memset(p,0,n*s); return p; }
static inline void *kzalloc(size_t n, gfp_t f){ (void)f; void*p=port_alloc(n, sizeof(void*)); if(p) memset(p,0,n); return p; }
static inline void *kmalloc(size_t n, gfp_t f){ (void)f; return port_alloc(n, sizeof(void*)); }
#define kcalloc(n,s,f)   kzalloc((n)*(s), f)
#define kmalloc_array(n,s,f) kmalloc((n)*(s), f)
#define kfree(p)         port_free((void *)(p))
#define devm_kzalloc(dev,n,f) kzalloc(n,f)
#define devm_kfree(dev,p)     port_free(p)
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define GFP_DMA    0

/* --- DMA + cache: flat physical, cache OFF -> identity / no-op --- */
#define CONFIG_SYS_CACHELINE_SIZE 64
#define ARCH_DMA_MINALIGN 64
static inline void flush_dcache_range(unsigned long s, unsigned long e){ (void)s;(void)e; }
static inline void invalidate_dcache_range(unsigned long s, unsigned long e){ (void)s;(void)e; }
static inline dma_addr_t dma_map_single(void *v, size_t n, unsigned long dir){ (void)n;(void)dir; return (dma_addr_t)(uintptr_t)v; }
static inline void dma_unmap_single(dma_addr_t a, size_t n, unsigned long dir){ (void)a;(void)n;(void)dir; }
static inline void *dma_alloc_coherent(size_t n, unsigned long *pa){ void*v=port_alloc(n,64); if(v){memset(v,0,n); if(pa)*pa=(unsigned long)(uintptr_t)v;} return v; }
static inline void dma_free_coherent(void *v){ port_free(v); }
#define DMA_TO_DEVICE   0
#define DMA_FROM_DEVICE 1
#define DMA_BIDIRECTIONAL 2

/* --- locks: single core, IRQs handled by our poller -> no-ops --- */
typedef int spinlock_t;
#define spin_lock_init(l)             do {} while (0)
#define spin_lock(l)                  do {} while (0)
#define spin_unlock(l)                do {} while (0)
#define spin_lock_irqsave(l, f)       do { (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f)  do { (void)(f); } while (0)

/* --- time --- */
unsigned long rt_tick_get(void);
void rt_thread_mdelay(int);
extern unsigned long g_dwc3_tick_hz;   /* set by init to RT_TICK_PER_SECOND */
static inline unsigned long get_timer(unsigned long base){ return rt_tick_get() * (1000UL / (g_dwc3_tick_hz ? g_dwc3_tick_hz : 1000)) - base; }
void udelay(unsigned long us);
static inline void mdelay(unsigned long ms){ rt_thread_mdelay((int)ms); }

/* --- logging: console is off on this build -> drop it (Ponytail) --- */
#define printf(...)        do {} while (0)
#define debug(...)         do {} while (0)
#define pr_debug(...)      do {} while (0)
#define pr_err(...)        do {} while (0)
#define dev_dbg(dev, ...)  do {} while (0)
#define dev_vdbg(dev, ...) do {} while (0)
#define dev_info(dev, ...) do {} while (0)
#define dev_err(dev, ...)  do {} while (0)
#define dev_WARN(dev, ...) do {} while (0)
#define WARN(cond, ...)    (!!(cond))
#define WARN_ON(cond)      (!!(cond))
#define WARN_ON_ONCE(cond) (!!(cond))
#define BUG()              do { for(;;); } while (0)
#define BUG_ON(cond)       do { if (cond) for(;;); } while (0)
#define assert(x)          do {} while (0)

/* --- dummy driver-model types (we bypass DM entirely) --- */
struct udevice;
struct device { void *priv; void *driver_data; struct device *parent; void (*release)(struct device *); void *class; };
struct resource { unsigned long start; unsigned long end; };
#define LIST_POISON1 ((void *)0x00100100)
#define LIST_POISON2 ((void *)0x00200200)
/* usb_init_type now comes from the real U-Boot <usb.h> (host port) */
#define dev_get_priv(dev)    (NULL)
#define DECLARE_GLOBAL_DATA_PTR  extern int __gd_unused

/* --- misc --- */
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_ALIAS(x)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(a,b)
#define __init
#define __exit
#define KERN_DEBUG
#define ENOTSUPP 524


/* --- compiler attribute macros (host port) --- */
#ifndef __weak
#define __weak __attribute__((weak))
#endif
#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif
#ifndef noinline
#define noinline __attribute__((noinline))
#endif
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif


/* --- U-Boot RMW + misc helpers (host port) --- */
#define setbits_le32(a, set)         writel(readl(a) | (set), (void *)(a))
#define clrbits_le32(a, clr)         writel(readl(a) & ~(u32)(clr), (void *)(a))
#define clrsetbits_le32(a, clr, set) writel((readl(a) & ~(u32)(clr)) | (set), (void *)(a))
#ifndef clamp_val
#define clamp_val(v, lo, hi) clamp(v, lo, hi)
#endif
#ifndef cpu_to_le64
#define cpu_to_le64(x) ((u64)(x))
#define le64_to_cpu(x) ((u64)(x))
#endif

#define HDBG(v) (*(volatile unsigned int *)0xff6ff9c0U = (unsigned int)(v))
#endif /* _DWC3_PORT_H_ */
