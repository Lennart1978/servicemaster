#ifndef _SM_ERR_H
#define _SM_SRR_H
#include <errno.h>
#include <stdarg.h>

void sm_err_set(const char *fmt, ...);
void sm_err_window(const char *fmt, ...);

#endif
