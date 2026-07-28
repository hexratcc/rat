#ifndef _RATCC_STDARG_H
#define _RATCC_STDARG_H

#define __NEED_va_list
#include <bits/alltypes.h>

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#define va_end(ap) __builtin_va_end(ap)

#endif
