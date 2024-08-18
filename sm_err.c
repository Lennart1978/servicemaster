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

void sm_err_window(const char *fmt, ...)
{
    int rc;
    va_list ap = {0};

    va_start(ap, fmt);

    free(errbuf);
    errbuf = NULL;

    rc = vasprintf(&errbuf, fmt, ap);
    if (rc < 0) {
        va_end(ap);
        sm_err_print_err("Cannot allocate memory attempting to set an error!");
        exit(EXIT_FAILURE);
    }
    va_end(ap);

    display_status_window(errbuf, "Error");
    return;
}


void sm_err_set(const char *fmt, ...)
{
    int rc;
    va_list ap = {0};

    va_start(ap, fmt);

    free(errbuf);
    errbuf = NULL;

    rc = vasprintf(&errbuf, fmt, ap);
    if (rc < 0) {
        va_end(ap);
        sm_err_print_err("Cannot allocate memory attempting to set an error!");
        exit(EXIT_FAILURE);
    }

    sm_err_print_err(errbuf);
    va_end(ap);
    exit(1);
    return;
}

const char * sm_err_get(void)
{
    return errbuf;
}
