#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <curses.h>
#include "display.h"
#include "sm_err.h"

/* Do not stack errors, one error overwites the next */
static char *errbuf = NULL;

static void sm_err_print_err(const char *errstr)
{
    if (curscr)
        endwin();

    fprintf(stderr, errstr);
    fprintf(stderr, "\n");
    return;
}

static void sm_err_setv(const char *fmt, va_list *ap)
{
    int rc;

    free(errbuf);
    errbuf = NULL;

    rc = vasprintf(&errbuf, fmt, *ap);
    if (rc < 0) {
        sm_err_print_err("Cannot allocate memory attempting to set an error!");
        exit(EXIT_FAILURE);
    }

    return;
}

void sm_err_window(const char *fmt, ...)
{
    va_list ap = {0};

    va_start(ap, fmt);
    sm_err_setv(fmt, &ap);
    va_end(ap);

    display_status_window(errbuf, "Error");
    return;
}


void sm_err_set(const char *fmt, ...)
{
    va_list ap = {0};

    va_start(ap, fmt);
    sm_err_setv(fmt, &ap);
    va_end(ap);

    sm_err_print_err(errbuf);
    exit(1);
}

const char * sm_err_get(void)
{
    return errbuf;
}
