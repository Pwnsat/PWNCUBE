/* linux-compat.h — DWC3 Linux compat shim (strlcat comes from the toolchain). */
#ifndef __DWC3_LINUX_COMPAT__
#define __DWC3_LINUX_COMPAT__
#define dev_WARN(dev, format, arg...)	debug(format, ##arg)
#endif
