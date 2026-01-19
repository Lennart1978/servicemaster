#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <ctype.h>
#include "display.h"
#include "service.h"
#include "bus.h"
#include "sm_err.h"
#include "config.h"

// External function to reset the terminal window title
extern void reset_terminal_title(void);

#define MAX(a, b) ((a) > (b) ? (a) : (b))

// Function declarations
static void sort_services_by_header(Bus *bus);

extern char *colors[];

static uint64_t start_time = 0;
static enum service_type mode = SERVICE;
static enum bus_type type = SYSTEM;
static int index_start = 0;
static int position = 0;
static uid_t euid = INT32_MAX;
static sd_event *event = NULL;
static sd_event_source *event_source = NULL;
static bool fallback_mode = false; // Track if we're in fallback color mode

// Enum for header highlighting
typedef enum
{
    BOLD_UNIT = 0,
    BOLD_STATE,
    BOLD_ACTIVE,
    BOLD_SUB,
    BOLD_DESCRIPTION,
    BOLD_NONE
} BoldHeader;

static BoldHeader current_bold_header = BOLD_NONE;

// Indicates whether a header should be highlighted on first tab press
static bool header_highlighting_initialized = false;

// Sort direction for different columns
typedef enum
{
    SORT_ASCENDING,
    SORT_DESCENDING
} SortDirection;

// Current sort status
static SortDirection unit_sort_direction = SORT_ASCENDING;
static SortDirection state_sort_direction = SORT_ASCENDING;
static SortDirection active_sort_direction = SORT_ASCENDING;
static SortDirection sub_sort_direction = SORT_ASCENDING;
static SortDirection description_sort_direction = SORT_ASCENDING;

int colorscheme = 0;

int D_XLOAD = 84;
int D_XACTIVE = 94;
int D_XSUB = 104;
int D_XDESCRIPTION = 114;

// Sort order for STATE values
static const char *state_order[] = {
    "enabled",
    "enabled-runtime",
    "loaded",
    "generated",
    "transient",
    "static",
    "not-found",
    "disabled",
    "masked",
    NULL // End marker
};

// Sort order for ACTIVE values
static const char *active_order[] = {
    "active",
    "inactive",
    NULL // End marker
};

// Sort order for SUB values
static const char *sub_order[] = {
    "exited",
    "running",
    "mounted",
    "active",
    "dead",
    "waiting",
    "plugged",
    "listening",
    NULL // End marker
};

// Helper function: Returns the index of a string in an array
static int get_index_in_array(const char *value, const char **array)
{
    if (!value)
        return -1;

    for (int i = 0; array[i] != NULL; i++)
    {
        if (strcmp(value, array[i]) == 0)
        {
            return i;
        }
    }
    return 999; // Not found, sort to the end
}

// Generic comparison function
static int compare_services(const void *a, const void *b)
{
    Service *svc1 = *(Service **)a;
    Service *svc2 = *(Service **)b;
    int result = 0;

    switch (current_bold_header)
    {
    case BOLD_UNIT:
        result = strcasecmp(svc1->unit, svc2->unit);
        return unit_sort_direction == SORT_ASCENDING ? result : -result;

    case BOLD_STATE:
    {
        int idx1 = get_index_in_array(svc1->unit_file_state, state_order);
        int idx2 = get_index_in_array(svc2->unit_file_state, state_order);
        result = idx1 - idx2;
        return state_sort_direction == SORT_ASCENDING ? result : -result;
    }

    case BOLD_ACTIVE:
    {
        int idx1 = get_index_in_array(svc1->active, active_order);
        int idx2 = get_index_in_array(svc2->active, active_order);
        result = idx1 - idx2;
        return active_sort_direction == SORT_ASCENDING ? result : -result;
    }

    case BOLD_SUB:
    {
        int idx1 = get_index_in_array(svc1->sub, sub_order);
        int idx2 = get_index_in_array(svc2->sub, sub_order);
        result = idx1 - idx2;
        return sub_sort_direction == SORT_ASCENDING ? result : -result;
    }

    case BOLD_DESCRIPTION:
        result = strcmp(svc1->description, svc2->description);
        return description_sort_direction == SORT_ASCENDING ? result : -result;

    default:
        return 0; // No sorting if no header is highlighted
    }
}

/**
 * Calculates column widths for display based on terminal width.
 *
 * Dynamically adjusts column positions for unit names, active state,
 * sub-state, and description columns. Ensures columns fit within
 * terminal width while maintaining minimum unit name width.
 *
 * @param terminal_width Total width of the terminal screen
 */
void calculate_columns(int terminal_width)
{
    const int MIN_UNIT_WIDTH = 20; // Minimum width for unit names
    const int STATE_WIDTH = 10;    // Fixed width for state columns

    // Unit column gets 50% of the width, but at least MIN_UNIT_WIDTH
    D_XLOAD = MAX(terminal_width * 0.50, MIN_UNIT_WIDTH);

    // Each state column gets fixed width
    D_XACTIVE = D_XLOAD + STATE_WIDTH;
    D_XSUB = D_XACTIVE + STATE_WIDTH;
    D_XDESCRIPTION = D_XSUB + STATE_WIDTH;

    // Ensure we don't exceed terminal width
    if (D_XDESCRIPTION >= terminal_width - 1)
    {
        // Adjust columns to fit within terminal
        int excess = D_XDESCRIPTION - (terminal_width - 1);
        D_XLOAD = MAX(MIN_UNIT_WIDTH, D_XLOAD - excess);
        D_XACTIVE = D_XLOAD + STATE_WIDTH;
        D_XSUB = D_XACTIVE + STATE_WIDTH;
        D_XDESCRIPTION = D_XSUB + STATE_WIDTH;
    }
}

static void init_color_pairs(void)
{
    // Initialize the basic color pairs used throughout the application
    init_pair(BLACK_WHITE, COLOR_BLACK, COLOR_WHITE);
    init_pair(CYAN_BLACK, COLOR_CYAN, COLOR_BLACK);
    init_pair(WHITE_BLACK, COLOR_WHITE, COLOR_BLACK);
    init_pair(RED_BLACK, COLOR_RED, COLOR_BLACK);
    init_pair(GREEN_BLACK, COLOR_GREEN, COLOR_BLACK);
    init_pair(YELLOW_BLACK, COLOR_YELLOW, COLOR_BLACK);
    init_pair(BLUE_BLACK, COLOR_BLUE, COLOR_BLACK);
    init_pair(MAGENTA_BLACK, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(WHITE_BLUE, COLOR_WHITE, COLOR_BLUE);
    init_pair(WHITE_RED, COLOR_WHITE, COLOR_RED);
    init_pair(BLACK_GREEN, COLOR_BLACK, COLOR_GREEN);
    init_pair(RED_YELLOW, COLOR_RED, COLOR_YELLOW);
}

void set_color_scheme(int scheme)
{
    colorscheme = scheme;
    init_color_pairs();
}

// Convert RGB value to ncurses color value
static short rgb_to_ncurses(short value)
{
    return (short)((value * 1000) / 255);
}

/**
 * Applies a fallback color scheme using standard ncurses colors
 * when true color support is not available.
 */
static void apply_fallback_color_scheme(void)
{
    // Use standard ncurses colors - no custom color initialization needed
    // The color pairs defined in init_color_pairs() will use the default colors
    // This provides a basic but functional color scheme
}

/**
 * Applies the specified color scheme to the terminal.
 * If true color support is not available, falls back to standard colors
 * and displays a notification to the user.
 *
 * @param scheme The color scheme to apply
 */
static void apply_color_scheme(const ColorScheme *scheme)
{
    if (!can_change_color())
    {
        // Apply fallback color scheme using standard ncurses colors
        apply_fallback_color_scheme();
        fallback_mode = true; // Set fallback mode
        
        // Display notification to user about fallback mode
        mvprintw(0, 0, "Note: Terminal does not support custom colors. Using fallback color scheme.");
        refresh();
        
        // Wait a moment for user to see the message
        napms(2000);
        
        // Clear the notification
        move(0, 0);
        clrtoeol();
        refresh();
        
        return;
    }
    
    fallback_mode = false; // Reset fallback mode when using custom colors

    // Apply the custom color scheme
    init_color(COLOR_BLACK,
               rgb_to_ncurses(scheme->black[0]),
               rgb_to_ncurses(scheme->black[1]),
               rgb_to_ncurses(scheme->black[2]));

    init_color(COLOR_RED,
               rgb_to_ncurses(scheme->red[0]),
               rgb_to_ncurses(scheme->red[1]),
               rgb_to_ncurses(scheme->red[2]));

    init_color(COLOR_GREEN,
               rgb_to_ncurses(scheme->green[0]),
               rgb_to_ncurses(scheme->green[1]),
               rgb_to_ncurses(scheme->green[2]));

    init_color(COLOR_YELLOW,
               rgb_to_ncurses(scheme->yellow[0]),
               rgb_to_ncurses(scheme->yellow[1]),
               rgb_to_ncurses(scheme->yellow[2]));

    init_color(COLOR_BLUE,
               rgb_to_ncurses(scheme->blue[0]),
               rgb_to_ncurses(scheme->blue[1]),
               rgb_to_ncurses(scheme->blue[2]));

    init_color(COLOR_MAGENTA,
               rgb_to_ncurses(scheme->magenta[0]),
               rgb_to_ncurses(scheme->magenta[1]),
               rgb_to_ncurses(scheme->magenta[2]));

    init_color(COLOR_CYAN,
               rgb_to_ncurses(scheme->cyan[0]),
               rgb_to_ncurses(scheme->cyan[1]),
               rgb_to_ncurses(scheme->cyan[2]));

    init_color(COLOR_WHITE,
               rgb_to_ncurses(scheme->white[0]),
               rgb_to_ncurses(scheme->white[1]),
               rgb_to_ncurses(scheme->white[2]));
}

/**
 * Handles the display of a service row with all its details.
 *
 * @param svc The service to display
 * @param row The row number (relative position)
 * @param spc The spacing/offset from the top
 */
static void display_service_row(Service *svc, int row, int spc)
{
    int i;
    char short_unit[D_XLOAD - 4];
    char short_unit_file_state[10];
    char *short_description;
    size_t maxx_description = getmaxx(stdscr) - D_XDESCRIPTION - 1;

    // Clear the unit name column
    for (i = 1; i < D_XLOAD - 1; i++)
        mvaddch(row + spc, i, ' ');

    // If the unit name is too long, truncate it and add ...
    if (strlen(svc->unit) >= (size_t)(D_XLOAD - 4))
    {
        strncpy(short_unit, svc->unit, D_XLOAD - 4);
        mvaddstr(row + spc, 1, short_unit);
        mvaddstr(row + spc, D_XLOAD - 4, "...");
    }
    else
        mvaddstr(row + spc, 1, svc->unit);

    // Clear the state column
    for (i = D_XLOAD; i < D_XACTIVE - 1; i++)
        mvaddch(row + spc, i, ' ');

    // If the state is too long, truncate it (enabled-runtime will be enabled-r)
    if (!svc->unit_file_state || strlen(svc->unit_file_state) == 0)
        mvprintw(row + spc, D_XLOAD, "%s", svc->load);
    else if (strlen(svc->unit_file_state) > 9)
    {
        strncpy(short_unit_file_state, svc->unit_file_state, 9);
        short_unit_file_state[9] = '\0';
        mvaddstr(row + spc, D_XLOAD, short_unit_file_state);
    }
    else
        mvprintw(row + spc, D_XLOAD, "%s", svc->unit_file_state ? svc->unit_file_state : svc->load);

    // Clear the active column
    for (i = D_XACTIVE; i < D_XSUB - 1; i++)
        mvaddch(row + spc, i, ' ');

    mvprintw(row + spc, D_XACTIVE, "%s", svc->active);

    // Clear the sub column
    for (i = D_XSUB; i < D_XDESCRIPTION - 1; i++)
        mvaddch(row + spc, i, ' ');

    mvprintw(row + spc, D_XSUB, "%s", svc->sub);

    // Clear the description column
    for (i = D_XDESCRIPTION; i < getmaxx(stdscr) - 1; i++)
        mvaddch(row + spc, i, ' ');

    // If the description is too long, truncate it and add ...
    if (strlen(svc->description) >= maxx_description)
    {
        short_description = alloca(maxx_description + 1);
        memset(short_description, 0, maxx_description + 1);
        strncpy(short_description, svc->description, maxx_description - 3);
        mvaddstr(row + spc, D_XDESCRIPTION, short_description);
        mvaddstr(row + spc, D_XDESCRIPTION + maxx_description - 3, "...");
    }
    else
        mvaddstr(row + spc, D_XDESCRIPTION, svc->description);

    // Save the y-position of the service
    svc->ypos = row + spc;
}

/**
 * Displays the list of services on the screen.
 *
 * This function is responsible for rendering the list of services,
 * handling pagination, and applying highlighting to the selected service.
 * It also manages the display of services based on the current mode and
 * available screen space.
 *
 * @param bus Pointer to the Bus structure containing service information.
 */
static void display_services(Bus *bus)
{
    int max_rows, maxy;
    int row = 0;
    int idx = index_start;
    Service *svc;
    int headerrow = 3;
    struct winsize size;
    int visible_services = 0;
    int total_services = 0;
    int dummy_maxx;

    getmaxyx(stdscr, maxy, dummy_maxx);
    (void)dummy_maxx;

    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    if (size.ws_col < (strlen(D_FUNCTIONS) + strlen(D_SERVICE_TYPES) + 2))
    {
        headerrow = 4;
    }

    int spc = headerrow + 2;
    max_rows = maxy - spc - 1;

    // Count the total number of services of the current type
    for (int i = 0;; i++)
    {
        svc = service_nth(bus, i);
        if (!svc)
            break;
        if (mode == ALL || mode == svc->type)
            total_services++;
    }

    services_invalidate_ypos(bus);

    while (true)
    {
        svc = service_nth(bus, idx);
        if (!svc)
            break;

        if (row >= max_rows)
            break;

        if (mode != ALL && mode != svc->type)
        {
            idx++;
            continue;
        }

        if (row == position)
        {
            // Monochrome theme needs a different color pair
            !strcmp(color_schemes[colorscheme].name, "Monochrome") ? attron(COLOR_PAIR(WHITE_RED)) : attron(COLOR_PAIR(WHITE_BLUE));
            attron(A_BOLD);
        }

        display_service_row(svc, row, spc);

        if (row == position)
        {
            // Monochrome theme needs a different color pair
            !strcmp(color_schemes[colorscheme].name, "Monochrome") ? attroff(COLOR_PAIR(WHITE_RED)) : attroff(COLOR_PAIR(WHITE_BLUE));
            attroff(A_BOLD);
        }

        row++;
        idx++;
        visible_services++;
    }
}

/**
 * Prints the text and lines for the main user interface.
 * This function is responsible for rendering the header, function keys, and mode indicators
 * on the screen. It also updates the position and mode information based on the current state.
 */
static void display_text_and_lines(Bus *bus)
{
    int x = D_XLOAD / 2 - 10;
    int maxx, maxy;
    char tmptype[16] = {0};
    int headerrow = 3;
    char navigation[256]; // Buffer for the complete navigation text

    struct winsize size;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);

    if (size.ws_col < (strlen(D_FUNCTIONS) + strlen(D_SERVICE_TYPES) + 2))
    {
        headerrow++;
    }

    getmaxyx(stdscr, maxy, maxx);

    // Solarized light theme needs a different color pair
    if (fallback_mode) {
        attron(COLOR_PAIR(BLACK_WHITE));
    } else {
        !strcmp(color_schemes[colorscheme].name, "Solarized Light") ? attron(COLOR_PAIR(MAGENTA_BLACK)) : attron(COLOR_PAIR(BLACK_WHITE));
    }

    border(0, 0, 0, 0, 0, 0, 0, 0);

    // Create the navigation text with the current theme name or "Fallback"
    if (fallback_mode) {
        snprintf(navigation, sizeof(navigation), D_NAVIGATION_BASE, "Fallback");
    } else {
        snprintf(navigation, sizeof(navigation), D_NAVIGATION_BASE, color_schemes[colorscheme].name);
    }

    attron(A_BOLD);
    mvaddstr(1, 1, D_HEADLINE);
    mvaddstr(1, strlen(D_HEADLINE) + 1 + ((size.ws_col - strlen(D_HEADLINE) - strlen(D_QUIT) - strlen(navigation) - 2) / 2), navigation);
    mvaddstr(1, size.ws_col - strlen(D_QUIT) - 1, D_QUIT);

    attron(COLOR_PAIR(WHITE_RED));
    mvaddstr(2, 1, D_FUNCTIONS);
    attroff(COLOR_PAIR(WHITE_RED));

    attron(COLOR_PAIR(BLACK_GREEN));
    if (size.ws_col < (strlen(D_FUNCTIONS) + strlen(D_SERVICE_TYPES) + 2))
    {
        mvaddstr(3, 1, D_SERVICE_TYPES);
    }
    else
    {
        mvaddstr(2, size.ws_col - strlen(D_SERVICE_TYPES) - 1, D_SERVICE_TYPES);
    }
    attroff(COLOR_PAIR(BLACK_GREEN));
    attroff(A_BOLD);

    // Solarized light theme needs a different color pair
    if (fallback_mode) {
        attron(COLOR_PAIR(BLACK_WHITE));
    } else {
        !strcmp(color_schemes[colorscheme].name, "Solarized Light") ? attron(COLOR_PAIR(MAGENTA_BLACK)) : attron(COLOR_PAIR(BLACK_WHITE));
    }
    mvprintw(headerrow, D_XLOAD - 10, "Pos.:%3d", position + index_start);

    // UNIT Header
    if (current_bold_header == BOLD_UNIT)
    {
        attron(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
        mvprintw(headerrow, 1, "UNIT:");
        attroff(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
    }
    else
    {
        mvprintw(headerrow, 1, "UNIT:");
    }

    attron(COLOR_PAIR(GREEN_BLACK));
    mvprintw(headerrow, 7, "(%s)", type ? "USER" : "SYSTEM");
    attroff(COLOR_PAIR(GREEN_BLACK));

    // Solarized light theme needs a different color pair
    if (fallback_mode) {
        attron(COLOR_PAIR(BLACK_WHITE));
    } else {
        !strcmp(color_schemes[colorscheme].name, "Solarized Light") ? attron(COLOR_PAIR(MAGENTA_BLACK)) : attron(COLOR_PAIR(BLACK_WHITE));
    }

    // STATE Header
    if (current_bold_header == BOLD_STATE)
    {
        attron(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
        mvprintw(headerrow, D_XLOAD, "STATE:");
        attroff(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
    }
    else
    {
        mvprintw(headerrow, D_XLOAD, "STATE:");
    }

    // ACTIVE Header
    if (current_bold_header == BOLD_ACTIVE)
    {
        attron(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
        mvprintw(headerrow, D_XACTIVE, "ACTIVE:");
        attroff(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
    }
    else
    {
        mvprintw(headerrow, D_XACTIVE, "ACTIVE:");
    }

    // SUB Header
    if (current_bold_header == BOLD_SUB)
    {
        attron(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
        mvprintw(headerrow, D_XSUB, "SUB:");
        attroff(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
    }
    else
    {
        mvprintw(headerrow, D_XSUB, "SUB:");
    }

    // DESCRIPTION Header
    if (current_bold_header == BOLD_DESCRIPTION)
    {
        attron(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
        mvprintw(headerrow, D_XDESCRIPTION, "DESCRIPTION:");
        attroff(COLOR_PAIR(WHITE_BLUE) | A_REVERSE | A_BOLD);
    }
    else
    {
        mvprintw(headerrow, D_XDESCRIPTION, "DESCRIPTION:");
    }

    attron(COLOR_PAIR(GREEN_BLACK));
    attron(A_UNDERLINE);

    // Sets the type count
    strncpy(tmptype, service_string_type(mode), 16);
    tmptype[sizeof(tmptype) - 1] = '\0';
    tmptype[0] = toupper(tmptype[0]);
    mvprintw(headerrow, x, "%s: %d", tmptype, bus->total_types[mode]);

    attroff(COLOR_PAIR(GREEN_BLACK));
    attroff(A_UNDERLINE);
    attroff(A_BOLD);
    mvhline(headerrow + 1, 1, ACS_HLINE, maxx - 2);
    mvvline(headerrow, D_XLOAD - 1, ACS_VLINE, maxy - 3);
    mvvline(headerrow, D_XACTIVE - 1, ACS_VLINE, maxy - 3);
    mvvline(headerrow, D_XSUB - 1, ACS_VLINE, maxy - 3);
    mvvline(headerrow, D_XDESCRIPTION - 1, ACS_VLINE, maxy - 3);
}

/**
 * Handles user input and performs various operations on systemd services.
 * This function is responsible for:
 * - Handling user input from the keyboard, including navigation, service operations, and mode changes
 * - Updating the display based on the current state and user actions
 * - Calling appropriate functions to perform service operations (start, stop, restart, etc.)
 * - Reloading the service list when necessary
 *
 * @param s The event source that triggered the callback.
 * @param fd The file descriptor associated with the event source.
 * @param revents The events that occurred on the file descriptor.
 * @param data Arbitrary user data passed to the callback.
 * @return 0 to indicate the event was handled successfully.
 */
int display_key_pressed(sd_event_source *s, int fd, uint32_t revents, void *data)
{
    (void)fd;
    int c;
    char *status = NULL;
    int max_services = 0;
    int maxy;
    int maxx;

    getmaxyx(stdscr, maxy, maxx);

    int headerrow = 3;
    struct winsize size;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    if (size.ws_col < (strlen(D_FUNCTIONS) + strlen(D_SERVICE_TYPES) + 2))
    {
        headerrow = 4;
    }
    int spc = headerrow + 2;               // +2 for the separator line and a space
    int max_visible_rows = maxy - spc - 1; // Exact calculation of visible rows
    int page_scroll = max_visible_rows;    // For Page Up/Down
    bool update_state = false;
    Service *svc = NULL;
    Bus *bus = (Bus *)data;

    if ((revents & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) > 0)
        return 0;

    c = getch();

    // Count the total number of services of the current type
    for (int i = 0;; i++)
    {
        svc = service_nth(bus, i);
        if (!svc)
            break;
        if (mode == ALL || mode == svc->type)
            max_services++;
    }

    svc = service_nth(bus, position + index_start);
    set_escdelay(25);

    switch (c)
    {
    case 'f': // Search function
    {
        Service *found_service = NULL;
        int max_input_length = 50;   // Internal buffer (can be longer than the window)
        char search_query[51] = {0}; // Space for 50 characters + null termination
        int win_height = 3, win_width = 80;
        int starty = (maxy - win_height) / 2;
        int startx = (maxx - win_width) / 2;
        int cursor = 0; // Current position in search_query
        int ch;
        static bool search_in_progress = false;

        if (search_in_progress)
            break;
        search_in_progress = true;

        WINDOW *input_win = newwin(win_height, win_width, starty, startx);
        box(input_win, 0, 0);
        // Information line in window, show maximum length:
        mvwprintw(input_win, 1, 1, "Search unit: ");
        wrefresh(input_win);

        echo(); // Show characters directly (optional, since we render ourselves)
        curs_set(1);
        // Switch the key input mode to non-blocking input field
        keypad(input_win, TRUE);

        // We only show the part of the input that fits in the window.
        // Assume the visible area starts after displaying the static characters.
        int offset = 1 + strlen("Search unit: ");
        // How many characters fit in the visible area?
        int visible_length = win_width - offset - 2; // 2 for border

        // Process character-by-character input
        while ((ch = wgetch(input_win)) != KEY_RETURN)
        {
            if ((ch == KEY_BACKSPACE || ch == 127) && cursor > 0)
            {
                cursor--;
                search_query[cursor] = '\0';
            }
            else if (cursor < max_input_length && ch >= 32 && ch <= 126)
            {
                search_query[cursor++] = ch;
                search_query[cursor] = '\0';
            }
            // Rendering: We calculate the start index so that if the input is longer than visible_length,
            // only the last visible_length characters are displayed.
            int start_index = 0;
            if (cursor > visible_length)
                start_index = cursor - visible_length;

            // Clear the input area
            for (int i = offset; i < win_width - 1; i++)
            {
                mvwaddch(input_win, 1, i, ' ');
            }
            // Display the visible part of the input
            mvwprintw(input_win, 1, offset, "%s", &search_query[start_index]);
            wmove(input_win, 1, offset + ((cursor > visible_length) ? visible_length : cursor));
            wrefresh(input_win);
        }
        noecho();
        curs_set(0);

        // Clean up input window
        delwin(input_win);
        refresh();
        flushinp();

        // Clear any remaining input
        while (getch() != ERR)
            ;

        // Exit search if query is empty
        if (strlen(search_query) == 0)
        {
            search_in_progress = false;
            break;
        }

        // Store current mode and switch to ALL to search across all services
        enum service_type current_mode = mode;
        mode = ALL;

        // Search for service matching query
        for (int i = 0;; i++)
        {
            Service *svc_iter = service_nth(bus, i);
            if (!svc_iter)
                break;

            if (strcasestr(svc_iter->unit, search_query) != NULL)
            {
                found_service = svc_iter;
                break;
            }
        }

        if (found_service)
        {
            // Switch to found service's type
            mode = found_service->type;
            int filtered_pos = 0;

            // Calculate position of found service in filtered list
            for (int i = 0;; i++)
            {
                Service *svc = service_nth(bus, i);
                if (!svc)
                    break;

                if (svc->type == mode)
                {
                    if (svc == found_service)
                    {
                        // Adjust scroll position to show found service
                        if (filtered_pos >= max_visible_rows)
                        {
                            index_start = filtered_pos - max_visible_rows + 1;
                            position = max_visible_rows - 1;
                        }
                        else
                        {
                            index_start = 0;
                            position = filtered_pos;
                        }
                        break;
                    }
                    filtered_pos++;
                }
            }

            // Redraw display with found service
            clear();
            display_services(bus);
            display_text_and_lines(bus);
        }
        else
        {
            // Restore previous mode if no service found
            mode = current_mode;
            display_status_window("No matching service found.", "Search");
            clear();
            display_services(bus);
            display_text_and_lines(bus);
        }
        refresh();

        // Reset search state and update start time
        search_in_progress = false;
        start_time = service_now();
        return 0;
    }
    case KEY_F(12):
    case '?':
        // Display interactive help overlay
        display_help_overlay();
        clear();
        display_redraw(bus);
        refresh();
        break;
    case KEY_ESC:
        // If a header is highlighted, remove highlighting without sorting
        if (current_bold_header != BOLD_NONE)
        {
            current_bold_header = BOLD_NONE;
            header_highlighting_initialized = false;
            clear();
            display_redraw(bus);
            refresh();
            break;
        }

        nodelay(stdscr, TRUE); // Non-blocking mode
        int esc_timeout = 50;  // 50ms Timeout for Escape sequences
        wtimeout(stdscr, esc_timeout);
        // Buffer to store escape sequence characters
        char seq[10] = {0};
        int i = 0, c;

        // Read escape sequence characters until ERR or '~' is encountered
        while ((c = getch()) != ERR && i < 9)
        {
            seq[i++] = c;
            if (c == '~')
                break;
        }
        seq[i] = '\0';

        // Handle different escape sequences for function keys, Some terminals send escape sequences for function keys
        if (strcmp(seq, "[11~") == 0)
        {
            d_op(bus, svc, START, "Start");
        }
        else if (strcmp(seq, "[12~") == 0)
        {
            d_op(bus, svc, STOP, "Stop");
        }
        else if (strcmp(seq, "[13~") == 0)
        {
            d_op(bus, svc, RESTART, "Restart");
        }
        else if (strcmp(seq, "[14~") == 0)
        {
            d_op(bus, svc, ENABLE, "Enable");
            update_state = true;
        }
        else
        {
            // Exit if ESC was pressed and enough time has passed since start
            if ((service_now() - start_time) < D_ESCOFF_MS)
                break;
            reset_terminal_title();
            endwin();
            exit(EXIT_SUCCESS);
        }
        nodelay(stdscr, FALSE); // back to normal mode
        break;
    case KEY_F(1):
        d_op(bus, svc, START, "Start");
        break;
    case KEY_F(2):
        d_op(bus, svc, STOP, "Stop");
        break;
    case KEY_F(3):
        d_op(bus, svc, RESTART, "Restart");
        break;
    case KEY_F(4):
        d_op(bus, svc, ENABLE, "Enable");
        update_state = true;
        break;
    case KEY_F(5):
        d_op(bus, svc, DISABLE, "Disable");
        update_state = true;
        break;
    case KEY_F(6):
        d_op(bus, svc, MASK, "Mask");
        update_state = true;
        break;
    case KEY_F(7):
        d_op(bus, svc, UNMASK, "Unmask");
        update_state = true;
        break;
    case KEY_F(8):
        d_op(bus, svc, RELOAD, "Reload");
        break;
    case KEY_UP:
    case KEY_VI_U:
        if (position > 0)
        {
            // If position > 0, just move the cursor up
            position--;
        }
        else if (index_start > 0)
        {
            // If we're at the top edge and there are more entries above, scroll up
            index_start--;
        }
        break;

    case KEY_DOWN:
    case KEY_VI_D:
        if (position + index_start >= max_services - 1)
        {
            // Already at the last entry, prevent further scrolling
            break;
        }

        if (position < max_visible_rows - 1 && position + index_start < max_services - 1)
        {
            // If we're not at the bottom edge and there are more entries, move the cursor
            position++;
        }
        else if (position + index_start < max_services - 1)
        {
            // If we're at the bottom edge and there are more entries, scroll down
            index_start++;
        }
        break;

    case KEY_PPAGE: // Page Up
        if (index_start > 0)
        {
            // Scroll one page up
            index_start -= page_scroll;
            if (index_start < 0)
                index_start = 0;
            erase();
        }
        position = 0;
        break;

    case KEY_NPAGE: // Page Down
        if (index_start + max_visible_rows < max_services)
        {
            // Scroll one page down
            index_start += page_scroll;
            if (index_start + max_visible_rows > max_services)
                index_start = max_services - max_visible_rows;
            if (index_start < 0)
                index_start = 0;
            erase();
        }
        position = 0;
        break;

    case KEY_LEFT:
    case KEY_VI_L:
        // Navigation between columns when a header is highlighted
        if (current_bold_header != BOLD_NONE)
        {
            if (current_bold_header > BOLD_UNIT)
            {
                current_bold_header--;
                clear();
                display_redraw(bus);
                refresh();
            }
            break;
        }

        // Original functionality for service type navigation
        if (mode > ALL)
            D_MODE(mode - 1);
        break;

    case KEY_RIGHT:
    case KEY_VI_R:
        // Navigation between columns when a header is highlighted
        if (current_bold_header != BOLD_NONE)
        {
            if (current_bold_header < BOLD_DESCRIPTION)
            {
                current_bold_header++;
                clear();
                display_redraw(bus);
                refresh();
            }
            break;
        }

        // Original functionality for service type navigation
        if (mode < SNAPSHOT)
            D_MODE(mode + 1);
        break;

    case KEY_SPACE:
        if (bus_system_only())
        {
            display_status_window("Only system bus is available as root.", "sudo mode !");
            break;
        }

        type ^= 0x1;
        bus = bus_currently_displayed();
        sd_event_source_set_userdata(s, bus);
        erase();
        break;

    case KEY_RETURN:
        // If a header is highlighted, perform the corresponding sort
        if (current_bold_header != BOLD_NONE)
        {
            sort_services_by_header(bus);
            clear();
            display_redraw(bus);
            refresh();
            break;
        }

        // Otherwise use original functionality (show status)
        svc = service_nth(bus, position + index_start);
        if (!svc)
            break;
        status = service_status_info(bus, svc);

        display_status_window(status ? status : "No status information available.", "Status:");
        free(status);
        break;

    case 'a':
        D_MODE(ALL);
        break;

    case 'd':
        D_MODE(DEVICE);
        break;

    case 'i':
        D_MODE(SLICE);
        break;

    case 's':
        D_MODE(SERVICE);
        break;

    case 'o':
        D_MODE(SOCKET);
        break;

    case 't':
        D_MODE(TARGET);
        break;

    case 'r':
        D_MODE(TIMER);
        break;

    case 'm':
        D_MODE(MOUNT);
        break;

    case 'c':
        D_MODE(SCOPE);
        break;

    case 'n':
        D_MODE(AUTOMOUNT);
        break;

    case 'w':
        D_MODE(SWAP);
        break;

    case 'p':
        D_MODE(PATH);
        break;

    case 'H':
        D_MODE(SNAPSHOT);
        break;

    case '\t': // Tab key
        if (!header_highlighting_initialized)
        {
            current_bold_header = BOLD_UNIT;
            header_highlighting_initialized = true;
        }
        else if (current_bold_header == BOLD_DESCRIPTION)
        {
            current_bold_header = BOLD_UNIT;
        }
        else
        {
            current_bold_header++;
        }
        clear();
        display_redraw(bus);
        refresh();
        break;

    case 'q':
        endwin();
        exit(EXIT_SUCCESS);
        break;

    case '+':
        if (!fallback_mode && colorscheme < scheme_count - 1)
        {
            colorscheme++;
            apply_color_scheme(&color_schemes[colorscheme]);
            set_color_scheme(colorscheme);
            erase();
        }
        break;

    case '-':
        if (!fallback_mode && colorscheme > 0)
        {
            colorscheme--;
            apply_color_scheme(&color_schemes[colorscheme]);
            set_color_scheme(colorscheme);
            erase();
        }
        break;

    default:
        break;
    }

    if (update_state)
    {
        if (svc != NULL)
        { // NULL pointer protection
            bus_update_unit_file_state(bus, svc);
            display_redraw_row(svc);
            svc->changed = 0;
        }
        update_state = false; // reset after processing
    }

    // Make sure we are not going over the end of the list
    if (index_start + position >= max_services)
    {
        if (max_services > 0)
        {
            if (max_services > max_visible_rows)
            {
                index_start = max_services - max_visible_rows;
                position = max_visible_rows - 1;
            }
            else
            {
                index_start = 0;
                position = max_services - 1;
            }
        }
        else
        {
            index_start = 0;
            position = 0;
        }
    }

    // Full redraw of the screen
    erase();
    display_redraw(bus);
    refresh();

    return 0;
}

/**
 * Returns the current bus type.
 *
 * This function provides access to the static 'type' variable,
 * which represents the current bus type (SYSTEM or USER).
 *
 * @return The current bus type.
 */
enum bus_type display_bus_type(void)
{
    return type;
}

/**
 * Returns the current service type mode.
 *
 * This function provides access to the static 'mode' variable,
 * which represents the current service type filter (ALL, DEVICE, SERVICE, etc.).
 *
 * @return The current service type mode.
 */
enum service_type display_mode(void)
{
    return mode;
}

/**
 * Redraws the entire display with the current bus information.
 *
 * This function performs a complete redraw of the screen by:
 * 1. Displaying all services from the provided bus
 * 2. Clearing any remaining space below the services list
 * 3. Drawing the text headers and separator lines
 * 4. Refreshing the screen to show the changes
 *
 * @param bus Pointer to the Bus structure containing services to display
 */
void display_redraw(Bus *bus)
{
    struct winsize size;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);

    // Create the headline text with root marking
    char headline[100];
    snprintf(headline, sizeof(headline), "%s%s%s",
             D_HEADLINE,
             (geteuid() == 0) ? " " : "",
             (geteuid() == 0) ? "(root)" : "");

    mvaddstr(1, 1, headline);
    if (geteuid() == 0)
    {
        // Position directly after the already written text
        int root_pos = 1 + strlen(D_HEADLINE) + 1;
        move(1, root_pos);
        attron(COLOR_PAIR(RED_BLACK) | A_BOLD); // Red and bold
        printw("(root)");
        attroff(COLOR_PAIR(RED_BLACK) | A_BOLD);
    }

    display_services(bus);
    clrtobot();
    display_text_and_lines(bus);
    refresh();
}

/**
 * Refreshes the display row for the given service.
 *
 * If the service is currently displayed on the screen, this function will
 * clear the row for the service and redraw it to ensure the display is
 * up-to-date.
 *
 * @param svc The service to refresh the display row for.
 */
void display_redraw_row(Service *svc)
{
    if (svc->ypos < 0)
        return;

    int curx, cury;
    getyx(stdscr, cury, curx);
    wmove(stdscr, svc->ypos, D_XLOAD);
    wclrtoeol(stdscr);
    wmove(stdscr, cury, curx);
}

void display_erase(void)
{
    erase();
}

/**
 * Sets the current bus type for the display.
 *
 * This function updates the static 'type' variable that determines
 * whether system or user services are being displayed.
 *
 * @param ty The bus type to set (SYSTEM or USER)
 */
void display_set_bus_type(enum bus_type ty)
{
    type = ty;
}

/**
 * Sets the title of the terminal window.
 *
 * This function uses various ANSI escape sequences to change the title of
 * the terminal window in different terminal emulators.
 *
 * @param title The new title for the terminal window
 */
static void set_terminal_title(const char *title)
{
    // XTerm, GNOME Terminal, Konsole, iTerm, etc.
    printf("\033]0;%s\007", title);

    // Screen and tmux
    printf("\033k%s\033\\", title);

    // Kitty Terminal
    printf("\033]2;%s\007", title);

    // Flush stdout to ensure the sequences are sent immediately
    fflush(stdout);
}

/**
 * Resets the title of the terminal window.
 *
 * This function uses various ANSI escape sequences to reset the title of
 * the terminal window to the default value.
 */
void reset_terminal_title(void)
{
    // XTerm, GNOME Terminal, Konsole, iTerm, etc.
    printf("\033]0;\007");

    // Screen and tmux
    printf("\033k\033\\");

    // Kitty Terminal
    printf("\033]2;\007");

    // Flush stdout to ensure the sequences are sent immediately
    fflush(stdout);
}

/**
 * Signal handler for SIGWINCH (window change) events.
 *
 * This function is called when the terminal window is resized. It:
 * 1. Gets the new terminal dimensions
 * 2. Resizes the screen buffer and recalculates column widths
 * 3. Redraws the entire display
 *
 * @param sig Signal number (unused)
 */
static void handle_winch(int sig)
{
    (void)sig;
    struct winsize size;

    // Get new dimensions safely
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1)
    {
        display_status_window("Error getting window size", "Error");
        return;
    }

    // Resize terminal and check for errors
    if (resizeterm(size.ws_row, size.ws_col) == ERR)
    {
        return;
    }

    calculate_columns(size.ws_col);

    // Reset terminal properties
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    clear();

    // Set the terminal window title again
    char window_title[100];
    snprintf(window_title, sizeof(window_title), "ServiceMaster v%s", D_VERSION);
    set_terminal_title(window_title);

    // Redraw current bus if available
    Bus *current_bus = bus_currently_displayed();
    if (current_bus)
        display_redraw(current_bus);
    else
    {
        display_status_window("Error redrawing display", "Error");
    }

    refresh();
}

/**
 * Initializes the display and event handling system.
 *
 * This function:
 * 1. Sets up SIGWINCH handler for terminal window resizing
 * 2. Initializes systemd event loop and IO event handling
 * 3. Sets up ncurses display settings
 * 4. Configures color pairs for the UI
 *
 * The function handles:
 * - Window resize events through SIGWINCH
 * - Keyboard input through epoll events
 * - Basic terminal display settings
 * - Color definitions for various UI elements
 *
 * Error conditions are handled by setting error messages through sm_err_set()
 * and returning early if critical initialization fails.
 */
void display_init(void)
{
    int rc;
    Bus *bus = bus_currently_displayed();

    // Signal-Handler for SIGWINCH
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGWINCH, &sa, NULL) == -1)
    {
        sm_err_set("Cannot setup window change handler: %s\n", strerror(errno));
        return;
    }

    // initialize event loop
    rc = sd_event_default(&event);
    if (rc < 0)
    {
        sm_err_set("Cannot initialize event loop: %s\n", strerror(-rc));
        return;
    }

    // initialize event handler
    rc = sd_event_add_io(event,
                         &event_source,
                         STDIN_FILENO,
                         EPOLLIN,
                         display_key_pressed,
                         bus);
    if (rc < 0)
    {
        sm_err_set("Cannot initialize event handler: %s\n", strerror(-rc));
        return;
    }

    // activate event handler
    rc = sd_event_source_set_enabled(event_source, SD_EVENT_ON);
    if (rc < 0)
    {
        sm_err_set("Cannot enable event source: %s\n", strerror(-rc));
        return;
    }

    euid = geteuid();
    start_time = service_now();

    // initialize ncurses
    initscr();
    raw();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    set_escdelay(0);

    // Only report mouse position, ignore clicks to prevent paste issues
    mousemask(REPORT_MOUSE_POSITION, NULL);
    printf("\033[?1003h\n"); // Extended mouse events enabled

    struct winsize size;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    calculate_columns(size.ws_col);

    start_color();

    apply_color_scheme(&color_schemes[colorscheme]);
    set_color_scheme(colorscheme);

    // Set the terminal window title
    char window_title[100];
    snprintf(window_title, sizeof(window_title), "ServiceMaster v%s", D_VERSION);
    set_terminal_title(window_title);

    clear();
    border(0, 0, 0, 0, 0, 0, 0, 0);
}

/**
 * Displays a status window with the provided status message and title.
 *
 * This function creates a centered window on the screen with the given status
 * message and title. The window is displayed until the user presses a key.
 *
 * @param status The status message to display in the window.
 * @param title The title to display at the top of the window.
 */
void display_status_window(const char *status, const char *title)
{
    char status_cpy[strlen(status) + 1];
    strcpy(status_cpy, status);
    int maxx_row = 0, maxy = 0, maxx = 0;
    int current_row_length = 0;
    int rows = 0, height = 0, width = 0;
    int startx = 0, starty = 0;
    WINDOW *win = NULL;
    int text_starty, y, x;
    int line_length = 0;
    const char *line_start = NULL;
    const char *line_end = NULL;

    strcpy(status_cpy, status);

    for (int count = 0; status_cpy[count] != '\0'; count++)
    {
        if (status_cpy[count] == '\n')
        {
            rows++;
            if (current_row_length > maxx_row)
                maxx_row = current_row_length;
            current_row_length = 0;
        }
        else
            current_row_length++;
    }

    if (current_row_length > maxx_row)
        maxx_row = current_row_length;

    getmaxyx(stdscr, maxy, maxx);

    // Calculate height - ensure it doesn't exceed available screen space
    if (rows >= maxy - 2)
        height = maxy - 2;  // Leave space for borders and prevent overflow
    else
        height = rows + 2;  // +2 for top and bottom border
    if (rows == 0)
        height = 3;

    // Ensure minimum height
    if (height < 3)
        height = 3;

    // Calculate width - ensure it doesn't exceed available screen space
    if (maxx_row >= maxx - 4)
        width = maxx - 2;  // Leave space for borders
    else
        width = maxx_row + 4;

    // Ensure minimum width
    if (width < 20)
        width = 20;

    starty = (maxy - height) / 2;
    startx = (maxx - width) / 2;

    // Ensure window position is valid
    if (starty < 0)
        starty = 0;
    if (startx < 0)
        startx = 0;

    win = newwin(height, width, starty, startx);
    
    // Check if window creation was successful
    if (win == NULL)
    {
        // Fallback: try creating a smaller window
        height = maxy - 2;
        width = maxx - 2;
        starty = 1;
        startx = 1;
        win = newwin(height, width, starty, startx);
        
        if (win == NULL)
            return;  // Cannot create window, abort
    }
    
    box(win, 0, 0);
    keypad(win, TRUE);
    start_color();

    text_starty = 1;
    y = text_starty;
    x = 1;

    wattron(win, A_BOLD);
    wattron(win, A_UNDERLINE);

    mvwprintw(win, 0, (width / 2) - (strlen(title) / 2), "%s", title);
    wattroff(win, A_UNDERLINE);

    if (rows == 0)
        wattron(win, COLOR_PAIR(RED_BLACK));
    else if (fallback_mode)
        wattron(win, COLOR_PAIR(BLACK_WHITE));
    else
        !strcmp(color_schemes[colorscheme].name, "Solarized Light") ? wattron(win, COLOR_PAIR(MAGENTA_BLACK)) : wattron(win, COLOR_PAIR(BLACK_WHITE));

    line_start = status_cpy;
    while ((line_end = strchr(line_start, '\n')) != NULL)
    {
        line_length = line_end - line_start;
        if (line_length > width - 2)
            line_length = width - 2;

        // Only add line if we have space in the window
        if (y < height - 1)
            mvwaddnstr(win, y++, x, line_start, line_length);
        line_start = line_end + 1;
    }

    // Add last line if there's space
    if (y < height - 1)
        mvwprintw(win, y, x, "%s", line_start);
    wrefresh(win);
    wgetch(win);

    wattroff(win, COLOR_PAIR(RED_BLACK));
    wattroff(win, A_BOLD);

    delwin(win);
    refresh();
}

/**
 * Displays an interactive help overlay with all keyboard shortcuts.
 *
 * This function shows a comprehensive help screen organized by categories:
 * navigation, service operations, filters, sorting, and display options.
 * The help is displayed until the user presses any key.
 */
void display_help_overlay(void)
{
    const char *help_text =
        "=== NAVIGATION ===\n"
        "Arrow Keys / h,j,k,l  Navigate through services\n"
        "Page Up / Page Down   Scroll one page up/down\n"
        "Space                 Toggle between System and User units\n"
        "\n"
        "=== SERVICE OPERATIONS ===\n"
        "F1                    Start selected service\n"
        "F2                    Stop selected service\n"
        "F3                    Restart selected service\n"
        "F4                    Enable selected service\n"
        "F5                    Disable selected service\n"
        "F6                    Mask selected service\n"
        "F7                    Unmask selected service\n"
        "F8                    Reload selected service\n"
        "Enter                 Show detailed status of selected service\n"
        "\n"
        "=== QUICK FILTERS (Service Types) ===\n"
        "a                     Show ALL units\n"
        "d                     Show DEVICE units\n"
        "i                     Show SLICE units\n"
        "s                     Show SERVICE units\n"
        "o                     Show SOCKET units\n"
        "t                     Show TARGET units\n"
        "r                     Show TIMER units\n"
        "m                     Show MOUNT units\n"
        "c                     Show SCOPE units\n"
        "n                     Show AUTOMOUNT units\n"
        "w                     Show SWAP units\n"
        "p                     Show PATH units\n"
        "H                     Show SNAPSHOT units\n"
        "Left / Right          Navigate between service types\n"
        "\n"
        "=== SORTING & SEARCH ===\n"
        "Tab                   Select column to sort by\n"
        "Return (with column)  Sort by selected column\n"
        "f                     Search for units by name\n"
        "ESC (in sort mode)    Cancel column selection\n"
        "\n"
        "=== DISPLAY OPTIONS ===\n"
        "+                     Next color scheme\n"
        "-                     Previous color scheme\n"
        "\n"
        "=== EXIT ===\n"
        "q / ESC               Quit ServiceMaster\n"
        "? / F12               Show this help\n"
        "\n"
        "Press any key to return to the main view...";

    display_status_window(help_text, "ServiceMaster " D_VERSION " - Keyboard Shortcuts");
}


void d_op(Bus *bus, Service *svc, enum operation mode, const char *txt)
{
    (void)svc;
    bool success = false;

    if (bus->type == SYSTEM && euid != 0)
    {
        // Create a new window with a box
        WINDOW *win = newwin(6, 60, LINES / 2 - 3, COLS / 2 - 30);
        box(win, 0, 0);

        // Enable red color and bold
        wattron(win, COLOR_PAIR(RED_BLACK));
        wattron(win, A_BOLD);

        mvwprintw(win, 0, 2, "Info:");
        mvwprintw(win, 2, 2, "You must be root for this operation on system units.");
        mvwprintw(win, 3, 2, "Would you like to restart with sudo? (y/n)");

        // Disable bold and red color
        wattroff(win, A_BOLD);
        wattroff(win, COLOR_PAIR(3));

        wrefresh(win);

        // Clear any remaining input
        flushinp();
        nodelay(stdscr, FALSE);

        // Wait for user input
        int c = wgetch(win);

        // Clean up the window
        delwin(win);
        touchwin(stdscr);
        refresh();

        if (c == 'y' || c == 'Y')
        {
            // End ncurses mode
            endwin();

            // Attempt to reset the terminal
            if (system("reset") != 0)
                perror("system reset failed");

            char *args[] = {"sudo", program_name, "-w", "-c", fallback_mode ? "Fallback" : color_schemes[colorscheme].name, NULL};

            if (execvp("sudo", args) != 0)
            {
                // If execvp fails, print an error message
                perror("execvp failed to execute sudo");
                exit(EXIT_FAILURE);
            }
            exit(EXIT_SUCCESS);
        }

        // Return to ncurses mode
        nodelay(stdscr, TRUE);
        return;
    }

    Service *temp_svc = service_nth(bus, position + index_start);
    if (!temp_svc)
    {
        display_status_window("No valid service selected.", "Error:");
        return;
    }

    success = bus_operation(bus, temp_svc, mode);
    if (!success)
    {
        display_status_window("Command could not be executed on this unit.", txt);
    }
}

/**
 * Sorts the services based on the currently highlighted header
 * and the corresponding sort direction.
 *
 * @param bus The bus containing the services to be sorted
 */
static void sort_services_by_header(Bus *bus)
{
    if (!bus)
        return;

    // Toggle the sort direction for the current header
    switch (current_bold_header)
    {
    case BOLD_UNIT:
        unit_sort_direction = (unit_sort_direction == SORT_ASCENDING) ? SORT_DESCENDING : SORT_ASCENDING;
        break;

    case BOLD_STATE:
        state_sort_direction = (state_sort_direction == SORT_ASCENDING) ? SORT_DESCENDING : SORT_ASCENDING;
        break;

    case BOLD_ACTIVE:
        active_sort_direction = (active_sort_direction == SORT_ASCENDING) ? SORT_DESCENDING : SORT_ASCENDING;
        break;

    case BOLD_SUB:
        sub_sort_direction = (sub_sort_direction == SORT_ASCENDING) ? SORT_DESCENDING : SORT_ASCENDING;
        break;

    case BOLD_DESCRIPTION:
        description_sort_direction = (description_sort_direction == SORT_ASCENDING) ? SORT_DESCENDING : SORT_ASCENDING;
        break;

    default:
        return; // No sorting if no header is highlighted
    }

    // Sort the services
    service_sort(bus, compare_services);

    // Reset position
    index_start = 0;
    position = 0;

    // Remove header highlighting
    current_bold_header = BOLD_NONE;
    header_highlighting_initialized = false;
}
