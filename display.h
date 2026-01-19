#ifndef _DISPLAY_H_
#define _DISPLAY_H_
#include <stdbool.h>
#include "service.h"
#include "bus.h"
#include "lib/toml.h"
#include "config.h"

#define KEY_RETURN 10
#define KEY_ESC 27
#define KEY_SPACE 32
#define KEY_VI_L 104
#define KEY_VI_R 108
#define KEY_VI_U 107
#define KEY_VI_D 106

#define D_ESCOFF_MS 300000LLU
#define D_VERSION "1.8.0"
#define D_FUNCTIONS "F1:START F2:STOP F3:RESTART F4:ENABLE F5:DISABLE F6:MASK F7:UNMASK F8:RELOAD"
#define D_SERVICE_TYPES "a:ALL d:DEV i:SLICE s:SERVICE o:SOCKET t:TARGET r:TIMER m:MOUNT c:SCOPE n:AMOUNT w:SWAP p:PATH H:SSHOT"
#define D_HEADLINE ""
#define D_NAVIGATION_BASE "Left/Right:Modus|Up/Down:Select|Return:Status|PageUp/Down:Scroll|f:Search|Space:Sys/Usr|Tab:Sort|+,-:Theme=%s"
#define D_QUIT "q/ESC:Quit"

extern int D_XLOAD;
extern int D_XACTIVE;
extern int D_XSUB;
extern int D_XDESCRIPTION;

#define D_MODE(m)        \
    {                    \
        position = 0;    \
        index_start = 0; \
        mode = m;        \
        clear();         \
    }

// Color pairs
#define BLACK_WHITE 0
#define CYAN_BLACK 1
#define WHITE_BLACK 2
#define RED_BLACK 3
#define GREEN_BLACK 4
#define YELLOW_BLACK 5
#define BLUE_BLACK 6
#define MAGENTA_BLACK 7
#define WHITE_BLUE 8
#define WHITE_RED 9
#define BLACK_GREEN 10
#define RED_YELLOW 11

extern char *program_name;

typedef struct
{
    short r, g, b;
} RGB;

extern int colorscheme;
extern ColorScheme *color_schemes;
extern int scheme_count;

enum bus_type display_bus_type(void);
enum service_type display_mode(void);
void display_erase(void);
void display_init(void);
void display_redraw(Bus *bus);
void display_redraw_row(Service *svc);
void display_set_bus_type(enum bus_type);
void display_status_window(const char *status, const char *title);
void display_help_overlay(void);
void d_op(Bus *bus, Service *svc, enum operation mode, const char *txt);
void set_color_scheme(int scheme);
void reset_terminal_title(void);

#endif
