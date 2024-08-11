#include <ncurses.h>
#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-journal.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/queue.h>

#define VERSION "1.3"
#define FUNCTIONS     "F1:START F2:STOP F3:RESTART F4:ENABLE F5:DISABLE F6:MASK F7:UNMASK F8:RELOAD"
#define SERVICE_TYPES "A:ALL D:DEV I:SLICE S:SERVICE O:SOCKET T:TARGET R:TIMER M:MOUNT C:SCOPE N:AMOUNT W:SWAP P:PATH H:SSHOT"

#define SD_DESTINATION "org.freedesktop.systemd1"
#define SD_IFACE(x)    "org.freedesktop.systemd1." x
#define SD_OPATH       "/org/freedesktop/systemd1"
#define FAIL(...) {\
    endwin();\
    fprintf(stderr, __VA_ARGS__);\
    exit(EXIT_FAILURE);\
}

#define KEY_RETURN 10
#define KEY_ESC 27
#define KEY_SPACE 32
#define XLOAD 104
#define XACTIVE 114
#define XSUB 124
#define XDESCRIPTION 134
#define CHARS_UNIT 134
#define CHARS_LOAD 10
#define CHARS_ACTIVE 10
#define CHARS_SUB 10
#define CHARS_DESCRIPTION 100
#define CHARS_STATUS 256
#define CHARS_OBJECT 512
#define UNIT_PROPERTY_SZ 256
#define INVOCATION_SZ 33

struct Service;
typedef struct Service Service;


void print_services();
void print_text_and_lines();
void get_unit_file_state(Service *svc, bool system);
int get_all_systemd_services(bool system);

const char *introduction = "Press Space to switch between system and user systemd units.\nFor security reasons, only root can manipulate system units and"
                     " only user user units.\nPress Return to display unit status information.\nUse left/right to toggle modes and arrow up/down, page up/down"
                     " to select units.\nPress the F keys to manipulate the units and ESC or Q to exit the program.\n"
                     "The program reacts immediately to unit changes from outside (DBus).\n"
                     "--> PRESS ANY KEY TO CONTINUE <--\n\nHave fun !\n\nLennart Martens 2024\nLicense: MIT\nmonkeynator78@gmail.com\n"
                     "https://github.com/lennart1978/servicemaster\nVersion: "VERSION;

const char *intro_title = "A quick introduction to ServiceMaster:";

int maxx, maxy, position, index_start;
size_t maxx_description;
bool is_system = false;
bool system_only = false;

enum Operations {
    START,
    STOP,
    RESTART,
    ENABLE,
    DISABLE,
    MASK,
    UNMASK,
    RELOAD,
    MAX_OPERATIONS
};

enum Type {
    ALL,
    DEVICE,
    SLICE,
    SERVICE,
    SOCKET,
    TARGET,
    TIMER,
    MOUNT,
    SCOPE,
    AUTOMOUNT,
    SWAP,
    PATH,
    SNAPSHOT,
    MAX_TYPES
};

const char * str_types[] = {
    "all",
    "device",
    "slice",
    "service",
    "socket",
    "target",
    "timer",
    "mount",
    "scope",
    "automount",
    "swap",
    "path",
    "snapshot"
};

const char *str_operations[] = {
    "StartUnit",
    "StopUnit",
    "RestartUnit",
    "EnableUnitFiles",
    "DisableUnitFiles",
    "MaskUnitFiles",
    "UnmaskUnitFiles",
    "ReloadUnit"
};

typedef struct Service {
    int ypos;
    int changed;
    uint64_t last_update;

    char unit[CHARS_UNIT];
    char load[CHARS_LOAD];
    char active[CHARS_ACTIVE];
    char sub[CHARS_SUB];
    char description[CHARS_DESCRIPTION];
    char object[CHARS_OBJECT];
    char fragment_path[UNIT_PROPERTY_SZ];
    char unit_file_state[UNIT_PROPERTY_SZ];
    char invocation_id[INVOCATION_SZ];

    uint64_t exec_main_start;
    uint32_t main_pid;
    uint64_t tasks_current;
    uint64_t tasks_max;
    uint64_t memory_current;
    uint64_t memory_peak;
    uint64_t swap_current;
    uint64_t swap_peak;
    uint64_t zswap_current;
    uint64_t zswap_peak;
    uint64_t cpu_usage;
    char cgroup[UNIT_PROPERTY_SZ];
    
    char sysfs_path[UNIT_PROPERTY_SZ];     // For DEVICE
    char mount_where[UNIT_PROPERTY_SZ];    // For MOUNT
    char mount_what[UNIT_PROPERTY_SZ];     // For MOUNT
    uint64_t next_elapse;                  // For TIMER
    char bind_ipv6_only[UNIT_PROPERTY_SZ]; // For SOCKET
    uint32_t backlog;                      // For SOCKET
    
    enum Type type;
    sd_bus_slot *slot;

    TAILQ_ENTRY(Service) e;
} Service;

TAILQ_HEAD(service_list, Service);

struct service_list system_services = TAILQ_HEAD_INITIALIZER(system_services);
struct service_list user_services = TAILQ_HEAD_INITIALIZER(user_services);

int total_user_types[MAX_TYPES];
int total_system_types[MAX_TYPES];

enum Type modus;

static inline Service * service_init(void) {
    Service *svc = NULL;
    svc = calloc(1, sizeof(Service));

    return svc;
}

/* Return the nth service in the list, accounting for the enabled
 * filter */
static inline Service * service_nth(int n) {
    int i=0;
    Service *svc;
    struct service_list *list = NULL;
    list = is_system ? &system_services: &user_services;

    TAILQ_FOREACH(svc, list, e) {
        if (i == n)
            return svc;
	if (svc->type == modus || modus == ALL)
            i++;
    }
    return TAILQ_FIRST(list);
}

/**
 * Finds the service with the specified y-position in the service list.
 *
 * @param ypos The y-position of the service to find.
 * @return The service with the specified y-position, or NULL if not found.
 */
static inline Service * service_ypos(int ypos) {
    Service *svc;
    struct service_list *list = NULL;
    list = is_system ? &system_services: &user_services;

    TAILQ_FOREACH(svc, list, e) {
        if (svc->ypos == ypos)
            return svc;
    }
    return NULL;
}


/* Insert service into the list in a sorted order */
static inline void service_insert(Service *svc, bool is_system) {
    int *total_types = NULL;
    struct service_list *list = NULL;
    Service *node = NULL;

    list = is_system ? &system_services : &user_services;
    total_types = is_system ? total_system_types : total_user_types;
    total_types[svc->type]++;
    total_types[ALL]++;

    /* List is empty, add to the head of the list */
    if (TAILQ_EMPTY(list)) {
        TAILQ_INSERT_HEAD(list, svc, e);
        return;
    }

    /* Find the next entry lexicographically above us and insert */
    TAILQ_FOREACH(node, list, e) {
        if (strcmp(svc->object, node->object) > 0)
            continue;

	TAILQ_INSERT_BEFORE(node, svc, e);
	return;
    }

    /* This item is the lexicographically greatest, put in tail */
    TAILQ_INSERT_TAIL(list, svc, e);
}

/* Return the service that matches this unit name */
static inline Service * service_get_name(const char *name, bool is_system)
{
    struct service_list *list = NULL;
    Service *svc = NULL;

    list = is_system ? &system_services: &user_services;
    TAILQ_FOREACH(svc, list, e) {
        if (strcmp(name, svc->unit) == 0)
            return svc;
    }
    return NULL;
}

static inline void service_refresh_row(Service *svc)
{
    /* If the service is on the screen, invalidate the row so it refreshes
     * correctly */
    if (svc->ypos > -1) {
        int x, y;
        getyx(stdscr, y, x);
        wmove(stdscr, svc->ypos, XLOAD);
        wclrtoeol(stdscr);
        wmove(stdscr, y, x);
    }
}

/**
 * Removes all services from the service list.
 *
 * This function iterates through the service list and removes each service,
 * freeing the associated resources. It is used to clear the service list
 * when the application is shutting down or when the service list needs to
 * be reset.
 */
static inline void services_empty(void)
{
    struct service_list *list = NULL;
    Service *svc = NULL;

    list = is_system ? &system_services: &user_services;

    while (!TAILQ_EMPTY(list)) {
        svc = TAILQ_FIRST(list);
        TAILQ_REMOVE(list, svc, e);
	sd_bus_slot_unref(svc->slot);
	free(svc);
    }
}

/* Iterate through the list, remove any that haven't been updated since
 * timestamp */
static inline void services_prune_dead_units(bool is_system, uint64_t ts)
{
    struct service_list *list = NULL;
    int removed = 0;
    Service *svc = NULL;

    list = is_system ? &system_services: &user_services;

    svc = TAILQ_FIRST(list);
    while (svc) {
      Service *n;
      n = TAILQ_NEXT(svc, e);

      if (svc->last_update >= ts) {
          svc = n;
          continue;
      }

      TAILQ_REMOVE(list, svc, e);
      if (svc->ypos > -1)
          removed++;
      sd_bus_slot_unref(svc->slot);
      free(svc);
      svc = n;
    }

    /* If and only if we removed a item from our ACTIVE list, clear the screen */
    if (removed)
        erase();
    return;
}

/* This is used during the print services routine
 * to reset the y positions on all services, since not
 * every service is displayed at once. */
static inline void services_invalidate_ypos(void) {
    struct service_list *list = NULL;
    Service *svc = NULL;

    list = is_system ? &system_services : &user_services;
    TAILQ_FOREACH(svc, list, e) {
        svc->ypos = -1;
    }
}

uint64_t get_now()
{
    sd_event *ev = NULL;
    uint64_t now;
    int rc;

    rc = sd_event_default(&ev);
    if (rc < 0)
        FAIL("Cannot find default event handler: %s\n", strerror(-rc));

    rc = sd_event_now(ev, CLOCK_MONOTONIC, &now);
    if (rc < 0)
        FAIL("Cannot fetch event handler timestamp: %s\n", strerror(-rc));

    sd_event_unref(ev);
    return now;
}

/**
 * Centers the given text by adding spaces to the beginning and end of each line to make the text centered.
 *
 * @param text The text to center.
 * @return A newly allocated string containing the centered text.
 */
char* center(const char *text) {
    char *result = NULL;
    char *line, *saveptr;
    char *input = strdup(text);
    int max_line_length = 0;
    int total_length = 0;
    int line_count = 0;

    line = strtok_r(input, "\n", &saveptr);
    while (line != NULL) {
        int len = strlen(line);
        if (len > max_line_length) {
            max_line_length = len;
        }
        total_length += len + 1;
        line_count++;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    result = (char*)malloc(total_length + line_count * max_line_length + 1);
    if (result == NULL) {
        free(input);
        return NULL;
    }

    char *output = result;
    const char *input_ptr = text;
    while (*input_ptr) {
        if (*input_ptr == '\n' && (input_ptr == text || *(input_ptr - 1) == '\n')) {
            *output++ = '\n';
            input_ptr++;
            continue;
        }

        const char *end = strchr(input_ptr, '\n');
        if (!end) end = input_ptr + strlen(input_ptr);

        int line_length = end - input_ptr;
        int padding = (max_line_length - line_length) / 2;

        for (int i = 0; i < padding; i++) {
            *output++ = ' ';
        }
        strncpy(output, input_ptr, line_length);
        output += line_length;
        *output++ = '\n';

        input_ptr = *end ? end + 1 : end;
    }
    *output = '\0';

    free(input);
    return result;
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
void show_status_window(const char *status, const char *title) {
    char status_cpy[strlen(status) + 1];
    strcpy(status_cpy, status);
    int maxx_row = 0;
    int current_row_length = 0;
    int rows = 0;

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
        {
            current_row_length++;
        }
    }

    if(current_row_length > maxx_row)
        maxx_row = current_row_length;

    int maxy, maxx;
    getmaxyx(stdscr, maxy, maxx);

    int height = 0;
    if(rows >= maxy)
        height = maxy + 2;
    else
        height = rows + 2;
    if(rows == 0)
        height = 3;

    int width;
    if(maxx_row >= maxx)
        width = maxx;
    else
        width = maxx_row + 4;

    int starty = (maxy - height) / 2;
    int startx = (maxx - width) / 2;

    WINDOW *win = newwin(height, width, starty, startx);
    box(win, 0, 0);

    keypad(win, TRUE);
    start_color();
    init_pair(13, COLOR_RED, COLOR_BLACK);

    int text_starty = 1;
    int y = text_starty;
    int x = 1;

    wattron(win, A_BOLD);
    wattron(win, A_UNDERLINE);

    mvwprintw(win, 0, (width / 2) - (strlen(title) / 2), "%s", title);
    wattroff(win, A_UNDERLINE);

    if(rows == 0)
        wattron(win, COLOR_PAIR(13));

    const char *line_start = status_cpy;
    const char *line_end;
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        int line_length = line_end - line_start;
        if (line_length > width - 2) line_length = width - 6;
        mvwaddnstr(win, y++, x, line_start, line_length);
        line_start = line_end + 1;
    }

    mvwprintw(win, y, x, "%s", line_start);

    wrefresh(win);

    wgetch(win);

    wattroff(win, COLOR_PAIR(13));
    wattroff(win, A_BOLD);

    delwin(win);
    refresh();
}

/**
 * Perform a systemd operation on a given unit.
 *
 * This function connects to the D-Bus system or user bus, depending on the value of the `is_system` global variable,
 * and calls the appropriate method on the `org.freedesktop.systemd1.Manager` interface of the `org.freedesktop.systemd1` service at the `/org/freedesktop/systemd1` object path.
 *
 * The supported operations are: START, STOP, RESTART, ENABLE, DISABLE, MASK, UNMASK, and RELOAD.
 *
 * If the operation is successful, the function returns `true`.
 * If there is an error, the function displays an error message using the `show_status_window` function and returns `false`.
 *
 * @param unit The name of the systemd unit to perform the operation on.
 * @param operation The type of operation to perform on the unit.
 * @return `true` if the operation was successful, `false` otherwise.
 */
bool start_operation(const char* unit, enum Operations operation) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL, *m = NULL;
    sd_bus *bus = NULL;
    const char *method = NULL;
    int r;

    if (unit == NULL) {
        return false;
    }

    if (is_system && geteuid() != 0) {
        show_status_window("Root privileges required for system units", "Warning:");
        return false;
    }

    r = is_system ? sd_bus_default_system(&bus) : sd_bus_default_user(&bus);
    if (r < 0)
        return false;

    if (operation < START || operation >= MAX_OPERATIONS) {
        sd_bus_unref(bus);
	show_status_window("Invalid operation", "Error:");
	return false;
    }
    method = str_operations[operation];

    if (strcmp(method, "EnableUnitFiles") == 0 || strcmp(method, "MaskUnitFiles") == 0) {
        r = sd_bus_message_new_method_call(bus, &m, SD_DESTINATION, SD_OPATH, SD_IFACE("Manager"), method);
        if (r < 0) 
            goto finish;

        r = sd_bus_message_append_strv(m, (char*[]) { (char*)unit, NULL });
        if (r < 0)
            goto finish;

        r = sd_bus_message_append(m, "bb", false, true);
        if (r < 0)
            goto finish;

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 1)
            goto finish;
        
    } else if (strcmp(method, "DisableUnitFiles") == 0 || strcmp(method, "UnmaskUnitFiles") == 0) {
        r = sd_bus_message_new_method_call(bus, &m, SD_DESTINATION, SD_OPATH, SD_IFACE("Manager"), method);
        if (r < 0)
            goto finish;

        r = sd_bus_message_append_strv(m, (char*[]) { (char*)unit, NULL });
        if (r < 0)
            goto finish;

        r = sd_bus_message_append(m, "b", false);
        if (r < 0)
            goto finish;

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0)
            goto finish;
    } else {
        r = sd_bus_call_method(bus, SD_DESTINATION, SD_OPATH, SD_IFACE("Manager"), method, &error, &reply,
                               "ss", unit, "replace");
        if (r < 0)
            goto finish;
    }

finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);

    return r >= 0;
}

bool is_root() {
    uid_t uid;
    uid = geteuid();
    if(uid == 0)
        return true;
    return false;
}

/**
 * Retrieves a unit property from the D-Bus interface.
 *
 * @param bus The D-Bus connection to use.
 * @param object The D-Bus object path to query.
 * @param iface The D-Bus interface to query.
 * @param property The property to retrieve.
 * @param fmt The format string for the property value.
 * @param result A pointer to store the retrieved property value.
 * @param sz The size of the result buffer.
 * @return 0 on success, or a negative error code on failure.
 */
bool unit_property(sd_bus *bus, const char *object, const char *iface, const char *property, const char *fmt, void *result, int sz) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    void *data = NULL;
    int r;

    r = sd_bus_get_property(bus,
                    SD_DESTINATION,
                    object,
                    iface,
                    property,
                    &error,
                    &reply,
                    fmt);

    if (r < 0) {
        goto fail;
    }

    if (sd_bus_error_is_set(&error)) {
        goto fail;
    }

    r = sd_bus_message_read(reply, fmt, &data);
    if (r < 0) {
        goto fail;
    }

    if (*fmt == 's' || *fmt== 'o')
        strncpy(result, data, sz);
    else
        memcpy(result, &data, sz);

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return true;

fail:
    strncpy(result, "", 2);
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return false;
}

/**
 * Formats the status of a service unit.
 *
 * @param svc The service unit to format the status for.
 * @return A dynamically allocated string containing the formatted status, or NULL on failure.
 */
char* format_unit_status(Service *svc) {
    char buf[2048] = {0};
    char *out = NULL;
    char *ptr = buf;
    time_t now = time(NULL);

    ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%30s - %s\n", svc->unit, svc->description);
    ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s (%s)\n", "Loaded", svc->load, svc->fragment_path);

    switch (svc->type) {
        case SERVICE:
            if (strcmp(svc->active, "active") == 0 && strcmp(svc->sub, "running") == 0)
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s (%s) since %lu seconds ago\n",
                    "Active", svc->active, svc->sub, now - (svc->exec_main_start / 1000000));
            else
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s (%s)\n",
                    "Active", svc->active, svc->sub);

            if (strcmp(svc->active, "active") == 0) {
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %u\n", "Main PID", svc->main_pid);
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %lu (limit: %lu)\n",
                    "Tasks", svc->tasks_current, svc->tasks_max);
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %.1f (peak: %.1fM swap: %.1fM swap peak: %.1fM zswap: %.1fM))\n",
                    "Memory",
                    (float)svc->memory_current / 1048576.0,
                    (float)svc->memory_peak / 1048576.0,
                    (float)svc->swap_current / 1048576.0,
                    (float)svc->swap_peak / 1048576.0,
                    (float)svc->zswap_current / 1048576.0);
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %lums\n", "CPU", svc->cpu_usage / 1000);
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "CGroup", svc->cgroup);
            }
            break;

        case DEVICE:
            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "SysFSPath", svc->sysfs_path);
            break;

        case MOUNT:
            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "Where", svc->mount_where);
            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "What", svc->mount_what);
            break;

        case TIMER:
        {
            time_t next_elapse_sec = svc->next_elapse / 1000000;
            struct tm *tm_info = localtime(&next_elapse_sec);
            char time_str[26];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "Next Elapse", time_str);

            time_t now = time(NULL);
            double diff_seconds = difftime(next_elapse_sec, now);

            if (diff_seconds > 0) {
                int days = (int)(diff_seconds / 86400);
                int hours = (int)((diff_seconds - days * 86400) / 3600);
                int minutes = (int)((diff_seconds - days * 86400 - hours * 3600) / 60);
                int seconds = (int)(diff_seconds - days * 86400 - hours * 3600 - minutes * 60);

                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: ", "Time until");
                if (days > 0) ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%d days ", days);
                if (hours > 0) ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%d hours ", hours);
                if (minutes > 0) ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%d minutes ", minutes);
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%d seconds\n", seconds);
            } else {
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: In the past\n", "Time until");
            }
        }
        break;

        case SOCKET:
        ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "BindIPv6Only", svc->bind_ipv6_only);

        if (svc->backlog == 2147483647 || svc->backlog == UINT32_MAX) {
            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: Unlimited\n", "Backlog");
        } else if (svc->backlog > 65535) {
            ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: Invalid value (%u)\n", "Backlog", svc->backlog);
        } else {
         ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %u\n", "Backlog", svc->backlog);
        }
            break;

        case PATH:
            break;

        case SLICE:
        case TARGET:
        case SCOPE:
        case AUTOMOUNT:
        case SWAP:
        case SNAPSHOT:
            break;

        default:
            break;
    }

    ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %s\n", "File State", svc->unit_file_state);
    ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "\n");

    out = strdup(buf);
    if (!out)
        return NULL;
    return out;
}

/**
 * Retrieves the logs for a given service unit.
 *
 * @param svc The service unit to retrieve logs for.
 * @param lines The maximum number of log lines to retrieve.
 * @return A dynamically allocated string containing the formatted logs, or NULL on failure.
 */
char * unit_logs(Service *svc, int lines) {
    sd_journal *j;
    char *out = NULL;
    char *ptr = NULL;
    int r;
    char ebuf[256] = {0};
    char match[256] = {0};
    int total = 0;
    int left = lines;
    struct logline {
        char msg[2048];
        char hostname[128];
        char syslogident[128];
        char pid[10];
        uint64_t stamp;
    };

    struct logline *logs = NULL;

    logs = alloca(sizeof(struct logline) * lines);
    memset(logs, 0, sizeof(*logs) * lines);

    r = sd_journal_open(&j, SD_JOURNAL_SYSTEM|SD_JOURNAL_CURRENT_USER);
    if (r < 0) {
        snprintf(ebuf, 256, "Cannot retrieve journal: %s", strerror(-r));
        goto fail;
    }

    snprintf(match, 256, "_SYSTEMD_INVOCATION_ID=%s", svc->invocation_id);
    sd_journal_add_match(j, match, 0);

    sd_journal_add_disjunction(j);
    snprintf(match, 256, "USER_INVOCATION_ID=%s", svc->invocation_id);
    sd_journal_add_match(j, match, 0);

    total = 0;
    SD_JOURNAL_FOREACH_BACKWARDS(j) {
        size_t sz;
        const char *val = NULL;

        if (left <= 0)
                break;

        struct logline *ll = &logs[left-1];

        r = sd_journal_get_realtime_usec(j, &ll->stamp);
        if (r < 0)
            continue;

        r = sd_journal_get_data(j, "MESSAGE", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->msg, val+8, sz-8);
        total += sz;

        r = sd_journal_get_data(j, "_HOSTNAME", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->hostname, val+10, sz-10);
        total += sz;

        r = sd_journal_get_data(j, "SYSLOG_IDENTIFIER", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->syslogident, val+18, sz-18);
        total += sz;

        r = sd_journal_get_data(j, "_PID", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->pid, val+5, sz-5);
        total += sz;

        /* The 64 is to over-compensate for writing in the timestamp and whitespace later */
        total += 64;
        left--;
    }

    if (!total)
        goto fin;

    out = malloc(total);
    ptr = out;
    if (!out) {
        snprintf(ebuf, 256, "Cannot create logs: %s", strerror(errno));
        goto fail;
    }

    for (int i=left; i < lines; i++) {
        struct logline *ll = &logs[i];
        char strstamp[32] = {0};

        time_t t = (ll->stamp / 1000000);
        struct tm *tm = localtime(&t);
        strftime(strstamp, 32, "%b %d %H:%M:%S", tm);

        ptr += snprintf(ptr, total - (ptr - out), "%s %s %s[%s]: %s\n",
                        strstamp, ll->hostname, ll->syslogident, ll->pid, ll->msg);
    }

fin:
    sd_journal_close(j);
    return out;

fail:
    if (out) {
        free(out);
    }
    show_status_window(ebuf, "Error");
    sd_journal_close(j);
    return NULL;
}

/**
 * Retrieves the invocation ID for the specified system or user service unit.
 *
 * @param bus The D-Bus connection to use.
 * @param svc Pointer to the service structure to work on.
 * @return 0 on success, or a negative error code on failure.
 */
bool invocation_id(sd_bus *bus, Service *svc) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    char *ptr = NULL;
    uint8_t *id = NULL;
    size_t len = 0;
    int r;

    r = sd_bus_get_property(bus,
                    SD_DESTINATION,
                    svc->object,
                    SD_IFACE("Unit"),
                    "InvocationID",
                    &error,
                    &reply,
                    "ay");

    if (r < 0)
        goto fail;

    if (sd_bus_error_is_set(&error))
        goto fail;

    r = sd_bus_message_read_array(reply, 'y', (const void **)&id, &len);
    if (r < 0)
        goto fail;

    if (len != 16) {
        r = -EINVAL;
        goto fail;
    }

    ptr = svc->invocation_id;
    for (size_t i=0; i < len; i++)
        ptr += snprintf(ptr, 33 - (ptr - svc->invocation_id), "%hhx", id[i]);

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return true;

fail:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return false;
}

/**
 * Retrieves the status information for the specified system or user service unit.
 *
 * @param svc Pointer to the service structure to work on.
 * @return A dynamically allocated string containing the status information, or NULL if an error occurred.
 *         The caller is responsible for freeing the returned string.
 */
char* get_status_info(Service *svc) {
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    char *out = NULL;
    char *logs = NULL;
    int r;

    r = is_system ? sd_bus_default_system(&bus) : sd_bus_default_user(&bus);
    if (r < 0)
        goto fin;

    if (invocation_id(bus, svc) == false)
        goto fin;

    unit_property(bus, svc->object, SD_IFACE("Unit"), "FragmentPath", "s", svc->fragment_path, sizeof(svc->fragment_path));

    switch (svc->type) {
        case SERVICE:
            unit_property(bus, svc->object, SD_IFACE("Service"), "ExecMainStartTimestamp", "t", &svc->exec_main_start, sizeof(svc->exec_main_start));
            unit_property(bus, svc->object, SD_IFACE("Service"), "ExecMainPID", "u", &svc->main_pid, sizeof(svc->main_pid));
            unit_property(bus, svc->object, SD_IFACE("Service"), "TasksCurrent", "t", &svc->tasks_current, sizeof(svc->tasks_current));
            unit_property(bus, svc->object, SD_IFACE("Service"), "TasksMax", "t", &svc->tasks_max, sizeof(svc->tasks_max));
            unit_property(bus, svc->object, SD_IFACE("Service"), "MemoryCurrent", "t", &svc->memory_current, sizeof(svc->memory_current));
            unit_property(bus, svc->object, SD_IFACE("Service"), "MemoryPeak", "t", &svc->memory_peak, sizeof(svc->memory_peak));
            unit_property(bus, svc->object, SD_IFACE("Service"), "MemorySwapCurrent", "t", &svc->swap_current, sizeof(svc->swap_current));
            unit_property(bus, svc->object, SD_IFACE("Service"), "MemorySwapPeak", "t", &svc->swap_peak, sizeof(svc->swap_peak));
            unit_property(bus, svc->object, SD_IFACE("Service"), "MemoryZSwapCurrent", "t", &svc->zswap_current, sizeof(svc->zswap_current));
            unit_property(bus, svc->object, SD_IFACE("Service"), "CPUUsageNSec", "t", &svc->cpu_usage, sizeof(svc->cpu_usage));
            unit_property(bus, svc->object, SD_IFACE("Service"), "ControlGroup", "s", &svc->cgroup, sizeof(svc->cgroup));
            break;
        case DEVICE:
            unit_property(bus, svc->object, SD_IFACE("Device"), "SysFSPath", "s", svc->sysfs_path, sizeof(svc->sysfs_path));
            break;
        case MOUNT:
            unit_property(bus, svc->object, SD_IFACE("Mount"), "Where", "s", svc->mount_where, sizeof(svc->mount_where));
            unit_property(bus, svc->object, SD_IFACE("Mount"), "What", "s", svc->mount_what, sizeof(svc->mount_what));
            break;
        case TIMER:
            unit_property(bus, svc->object, SD_IFACE("Timer"), "NextElapseUSecRealtime", "t", &svc->next_elapse, sizeof(svc->next_elapse));
            break;
        case SOCKET:
            unit_property(bus, svc->object, SD_IFACE("Socket"), "BindIPv6Only", "s", svc->bind_ipv6_only, sizeof(svc->bind_ipv6_only));
            unit_property(bus, svc->object, SD_IFACE("Socket"), "Backlog", "u", &svc->backlog, sizeof(svc->backlog));
            break;
        case PATH:
            break;
        case SLICE:
        case TARGET:
        case SCOPE:
        case AUTOMOUNT:
        case SWAP:
        case SNAPSHOT:
            break;
	default:
            break;
    }

    out = format_unit_status(svc);
    logs = unit_logs(svc, 10);
    if (!logs)
        goto fin;

    out = realloc(out, strlen(out) + strlen(logs) + 1);
    if (!out)
        goto fin;

    strcat(out, logs);

fin:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    free(logs);
    return out;
}

/**
 * Compares two Service structs based on the unit field in a case-insensitive manner.
 *
 * @param a Pointer to the first Service struct to compare.
 * @param b Pointer to the second Service struct to compare.
 * @return A negative value if the unit field of the first Service is lexicographically less than the second,
 *         a positive value if the unit field of the first Service is lexicographically greater than the second,
 *         and zero if the unit fields are equal.
 */
int compare_services(const void* a, const void* b)
{
    Service* serviceA = (Service*)a;
    Service* serviceB = (Service*)b;
    return strcasecmp(serviceA->unit, serviceB->unit);
}

/**
 * Checks if the given unit string has the specified file extension.
 *
 * @param unit The unit string to check.
 * @param extension The file extension to check for.
 * @return 1 if the unit string has the specified file extension, 0 otherwise.
 */
int test_unit_extension(const char* unit, const char* extension)
{
    const char* dot = strrchr(unit, '.');

    if (!dot || dot == unit) {
        return 0;
    }

    if(strcmp(dot + 1, extension) == 0)
    {
        return 1;
    }
    return 0;
}

/**
 * Checks if a given systemd unit type is considered "enableable", meaning it can be enabled or disabled.
 * The function checks the unit name against a list of known enableable unit types.
 *
 * @param unit The name of the systemd unit to check.
 * @return true if the unit type is considered enableable, false otherwise.
 */
bool is_enableable_unit_type(const char *unit) {
    static const char *enableable_extensions[] = {
        ".service", ".socket", ".timer", ".path", ".target", ".mount", ".automount"
    };

    for (size_t i = 0; i < sizeof(enableable_extensions) / sizeof(enableable_extensions[0]); i++) {
        if (test_unit_extension(unit, enableable_extensions[i]))
           return true;
    }

    return false;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

/**
 * Handles the daemon reload callback from the D-Bus message.
 * This function is called when the systemd daemon has been reloaded.
 * It checks the reload status, and if successful, it retrieves all the
 * systemd services and updates the screen accordingly.
 *
 * @param reply The D-Bus message containing the reload status.
 * @param data The user data passed to the callback, which is a pointer to the Service struct.
 * @param err The D-Bus error object, if any.
 * @return 0 on success, a negative error code on failure.
 */
int daemon_reloaded(sd_bus_message *reply, void *data, sd_bus_error *err)
{
    int  rc, reload_started;
    const char *scope;
    bool system = false;
    sd_bus *bus;

    if (sd_bus_error_is_set(err)) 
        FAIL("Remove unit callback failed: %s\n", err->message);

    rc = sd_bus_message_read(reply, "b", &reload_started);
    if (rc < 0) 
        FAIL("Cannot read dbus messge: %s\n", strerror(-rc));

    /* Reload daemon services for specific bus type and conditionally redraw screen) */
    if (reload_started)
        goto fin;

    bus = sd_bus_message_get_bus(reply);
    sd_bus_get_scope(bus, &scope);

    if (strcmp(scope, "system") == 0)
      system = true;

    rc = get_all_systemd_services(system);
    if (rc < 0)
        FAIL("Cannot reload system units: %s\n", strerror(-rc));
    
    if ((system && is_system) || (!system && !is_system)) {
        print_services();
        clrtobot();
        print_text_and_lines();
        refresh();
    }

fin:
    sd_bus_error_free(err);
    return 0;
}

/**
 * Updates a specific property of a service based on the received D-Bus message.
 *
 * This function is called for each dictionary entry in the D-Bus message. It
 * checks if the key is a property that needs to be updated, and if so, it
 * updates the corresponding field in the Service struct.
 *
 * @param svc The Service struct to be updated.
 * @param reply The D-Bus message containing the service property updates.
 * @return 1 if a property was updated, 0 otherwise.
 */
static int update_service_property(Service *svc, sd_bus_message *reply)
{
    /* Format of message at this point is: '{sv}' */
    int rc;
    const char *k, *str;

    /* s: Next item is the key out of the dictionary */
    rc = sd_bus_message_read(reply, "s", &k);
    if (rc < 0)
        FAIL("Cannot read dictionary key item from array: %s\n", strerror(-rc));

    /* If its a property we want to measure, update the related property */
    if (strcmp(k, "ActiveState") == 0) {
        /* v: Variant, always a string in this case */
        rc = sd_bus_message_read(reply, "v", "s", &str);
        if (rc < 0)
            FAIL("Cannot fetch value from dictionary: %s\n", strerror(-rc));
        strncpy(svc->active, str, CHARS_ACTIVE);

        return 1;
    }

    else if (strcmp(k, "SubState") == 0) {
        /* v: Variant, always a string in this case */
        rc = sd_bus_message_read(reply, "v", "s", &str);
        if (rc < 0)
            FAIL("Cannot fetch value from dictionary: %s\n", strerror(-rc));
	strncpy(svc->sub, str, CHARS_SUB);

        return 1;
    }

    else
      /* Anything else is skipped */
      sd_bus_message_skip(reply, NULL);

    return 0;
}

/**
 * Callback function that handles changes to a systemd service.
 *
 * This function is called when a change is detected in a systemd service. It reads the
 * updated properties from the D-Bus message and updates the corresponding fields in the
 * Service struct. If any properties have changed, it redraws the screen to reflect the
 * updated service status.
 *
 * @param reply The D-Bus message containing the updated service properties.
 * @param data A pointer to the Service struct to be updated.
 * @param err An error object, if an error occurred.
 * @return 0 on success, or a negative error code on failure.
 */
int changed_unit(sd_bus_message *reply, void *data, sd_bus_error *err)
{
    Service *svc = (Service *)data;
    const char *iface = NULL;
    int rc;

    /* Message format: sa{sv}as */

    if (sd_bus_error_is_set(err)) 
        FAIL("Changed unit callback failed: %s\n", err->message);

    /* s: Interface name */
    rc = sd_bus_message_read(reply, "s", &iface);
    if (rc < 0) 
        FAIL("Cannot read dbus messge: %s\n", strerror(-rc));

    /* If the interface is not a unit, we dont care */
    if (strcmp(iface, SD_IFACE("Unit")) != 0) 
        goto fin;
    
    /* a: Array of dictionaries */
    rc = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (rc < 0)
        FAIL("Cannot read array in dbus message: %s\n", strerror(-rc));

    /* Array of dictionaries */
    while (true) {
	/* {..}: Dictionary itself */
        rc = sd_bus_message_enter_container(reply, 'e', "sv");
        if (rc < 0) 
            FAIL("Cannot read dict item in dbus message: %s\n", strerror(-rc));

        /* No more array entries to read */
        if (rc == 0)
            break; 

        svc->changed += update_service_property(svc, reply);
        if (svc->changed) {
            service_refresh_row(svc);
            svc->last_update = get_now();
        }

        if (sd_bus_message_exit_container(reply) < 0)
            FAIL("Cannot exit dictionary: %s\n", strerror(-rc));
    }

    sd_bus_message_exit_container(reply);

    /* Redraw screen if something changed */
    if (svc->changed) {
	svc->changed = 0;
        print_services();
        print_text_and_lines();
        refresh();
    }

fin:
    sd_bus_error_free(err);
    return 0;
}
#pragma GCC diagnostic pop

/**
 * Retrieves a list of all systemd services on the system.
 *
 * This function connects to the systemd D-Bus interface and retrieves a list of all
 * systemd units (services, devices, slices, etc.) on the system. The retrieved information
 * includes the unit name, load state, active state, sub state, and description.
 *
 * The function populates the `services` array with the retrieved information, and updates
 * the `total_types` struct with the counts of each unit type.
 *
 * @return The number of services retrieved, or -1 on error.
 */
int get_all_systemd_services(bool is_system) {
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;    
    int r = 0;
    int *total_types = NULL;
    uint64_t now = get_now();

    total_types = is_system ? total_system_types : total_user_types;

    r = is_system ? sd_bus_default_system(&bus) : sd_bus_default_user(&bus);
    if (r < 0) {
      sd_bus_error_free(&error);
      sd_bus_unref(bus);
      return -1;
    }

    r = sd_bus_call_method(bus,
                           SD_DESTINATION,
                           SD_OPATH,
                           SD_IFACE("Manager"),
                           "ListUnits",
                           &error,
                           &reply,
                           "");
    if (r < 0) {
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return -1;
    }

    r = sd_bus_message_enter_container(reply, 'a', "(ssssssouso)");
    if (r < 0) {
        sd_bus_message_unref(reply);
        sd_bus_unref(bus);
        return -1;
    }

    const char *unit, *load, *active, *sub, *description, *object;
    while (true) {
        bool is_new = false;
        char unit_state[UNIT_PROPERTY_SZ] = {0};
        Service *svc = NULL;

        r = sd_bus_message_read(reply, "(ssssssouso)", 
                                &unit, &description, &load, &active, &sub,
                                NULL, &object, NULL, NULL, NULL);
        if (r < 0)
            FAIL("Cannot read DBUS message to get system services: %s\n", strerror(-r));
        if (r == 0)
	    break;

        svc = service_get_name(unit, is_system);
        if (!svc) {
           is_new = true;
           svc = service_init();
        }

        if (!svc) {
            sd_bus_error_free(&error);
            sd_bus_unref(bus);
            return -1;
        }
        
        svc->last_update = now;
        strncpy(svc->unit, unit, sizeof(svc->unit) - 1);
        if (strcmp(svc->load, load))
            svc->changed++;
        if (strcmp(svc->active, active))
            svc->changed++;
        if (strcmp(svc->sub, sub))
            svc->changed++;

        strncpy(svc->load, load, sizeof(svc->load) - 1);
        strncpy(svc->active, active, sizeof(svc->active) - 1);
        strncpy(svc->sub, sub, sizeof(svc->sub) - 1);
        strncpy(svc->description, description, sizeof(svc->description) - 1);
        strncpy(svc->object, object, sizeof(svc->object) - 1);
        unit_property(bus, svc->object, SD_IFACE("Unit"), "UnitFileState", "s", unit_state, sizeof(unit_state));

        if (strcmp(svc->unit_file_state, unit_state))
            svc->changed++;
        strncpy(svc->unit_file_state, unit_state, UNIT_PROPERTY_SZ);
        /* Sets the units type */
        for (int j=0; j < MAX_TYPES; j++) {
              if (test_unit_extension(svc->unit, str_types[j])) {
                  svc->type = j;
                  break;
              }
        }

        if (is_new) {
            /* Register interest in events on this object */
            r = sd_bus_match_signal(bus, 
                                    &svc->slot,
                                    SD_DESTINATION,
                                    object,
                                    "org.freedesktop.DBus.Properties",
                                    "PropertiesChanged",
                                    changed_unit,
                                   (void *)svc);
            if (r < 0)
                FAIL("Cannot register interest changed units: %s\n", strerror(-r));

            service_insert(svc, is_system);
        }

        if (svc->changed) {
            service_refresh_row(svc);
            svc->changed = 0;
        }
    }

    services_prune_dead_units(is_system, now);

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return total_types[ALL];
}

/**
 * Clears the screen, ends the ncurses window, and exits the program with a status of 0.
 * This function should be called when the program needs to terminate, such as when the user
 * requests to quit the application.
 */
void quit()
{
    clear();
    endwin();
    exit(EXIT_SUCCESS);
}

/**
 * Prints the service information for the specified index and row.
 *
 * If the current row matches the position, the service information is printed with
 * a highlighted background. Otherwise, the service information is printed with a
 * normal background.
 *
 * The service information includes the unit name, load state, active state, sub state,
 * and description. If the service information is too long to fit in the available
 * space, it is truncated and an ellipsis is added.
 *
 * @param i The index of the service to print.
 * @param row The row to print the service information on.
 */
int print_s(Service *svc, int row)
{
    if(position == row)
    {
        attron(COLOR_PAIR(8));
        attron(A_BOLD);
    }
    else
    {
        attroff(COLOR_PAIR(8));
        attroff(A_BOLD);
    }

    if (modus != ALL && modus != svc->type)
        return 0;
    // if the unit name is too long, truncate it and add ...
    if(strlen(svc->unit) >= XLOAD -3) {     
        char short_unit[XLOAD - 2];
        strncpy(short_unit, svc->unit, XLOAD - 2);
        mvaddstr(row + 4, 1, short_unit);
        mvaddstr(row + 4,XLOAD - 4, "...");
    }
    else
        mvaddstr(row + 4, 1, svc->unit);
    // if the state is too long, truncate it (enabled-runtime will be enabled-r)
    if(strlen(svc->unit_file_state) > 9)
    {
        char short_unit_file_state[9];
        strncpy(short_unit_file_state, svc->unit_file_state, 9);
        short_unit_file_state[9] = '\0';
        mvaddstr(row + 4, XLOAD, short_unit_file_state);
    }
    else
    {
        mvprintw(row + 4, XLOAD, "%s", strlen(svc->unit_file_state) ? svc->unit_file_state : svc->load);
    }
    
    mvprintw(row + 4, XACTIVE, "%s", svc->active);
    mvprintw(row + 4, XSUB, "%s", svc->sub);
    // if the description is too long, truncate it and add ...
    if(strlen(svc->description) >= maxx_description) {
        char short_description[maxx_description - 3];
        strncpy(short_description, svc->description, maxx_description - 3);
        mvaddstr(row + 4, XDESCRIPTION, short_description);
        mvaddstr(row + 4, XDESCRIPTION + maxx_description - 3, "...");
    }
    else
        mvaddstr(row + 4, XDESCRIPTION, svc->description);

    svc->ypos = row + 4;
    return 1;
}

/**
 * Initializes the screen for the application.
 * This function sets up the ncurses environment, configures the terminal, and
 * initializes the color pairs used throughout the application.
 */
void init_screen()
{
    initscr();

    getmaxyx(stdscr, maxy, maxx);
    maxx_description = maxx - XDESCRIPTION - 1;

    raw();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    start_color();
    init_pair(0, COLOR_BLACK, COLOR_WHITE);
    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    init_pair(2, COLOR_WHITE, COLOR_BLACK);
    init_pair(3, COLOR_RED, COLOR_BLACK);
    init_pair(4, COLOR_GREEN, COLOR_BLACK);
    init_pair(5, COLOR_YELLOW, COLOR_BLACK);
    init_pair(6, COLOR_BLUE, COLOR_BLACK);
    init_pair(7, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(8, COLOR_WHITE, COLOR_BLUE);
    init_pair(9, COLOR_WHITE, COLOR_RED);
    init_pair(10, COLOR_BLACK, COLOR_GREEN);
    init_pair(11, COLOR_RED, COLOR_YELLOW);
    init_pair(12, COLOR_RED, COLOR_BLUE);

    clear();
    border(0, 0, 0, 0, 0, 0, 0, 0);
    refresh();
}

/**
 * Prints the list of services based on the current display mode (modus).
 * This function iterates through the list of filtered services and prints them
 * to the screen, handling different display modes such as ALL, DEVICE, SLICE,
 * SERVICE, SOCKET, TARGET, TIMER, MOUNT, SCOPE, AUTOMOUNT, SWAP, PATH, and
 * SNAPSHOT.
 */
void print_services()
{
    int max_rows = maxy - 5;
    int row = 0;
    Service *svc;

    services_invalidate_ypos();

    svc = service_nth(index_start);
    while (svc) {    
        if (row >= max_rows)
	    break;

        row += print_s(svc, row);
	svc = TAILQ_NEXT(svc, e);
    }
}

/**
 * Prints the text and lines for the main user interface.
 * This function is responsible for rendering the header, function keys, and mode indicators
 * on the screen. It also updates the position and mode information based on the current state.
 */
void print_text_and_lines()
{
    int x = XLOAD / 2 - 10;
    char *headline = "ServiceMaster "VERSION" | Q/ESC:Quit";

    char tmptype[16] = {0};
    int *total_types = NULL;
    total_types = is_system ? total_system_types : total_user_types;
    
    attroff(COLOR_PAIR(9));
    border(0, 0, 0, 0, 0, 0, 0, 0);

    attron(A_BOLD);
    attron(COLOR_PAIR(0));
    mvaddstr(1, 1, headline);
    attroff(COLOR_PAIR(8));

    attron(COLOR_PAIR(9));        
    mvaddstr(1, strlen(headline) + 2, FUNCTIONS);
    attroff(COLOR_PAIR(9));

    attron(COLOR_PAIR(10));
    mvaddstr(1, strlen(headline) + strlen(FUNCTIONS) + 3, SERVICE_TYPES);
    attroff(COLOR_PAIR(10));

    mvprintw(2, XLOAD - 10, "Pos.:%3d", position + index_start);
    mvprintw(2, 1, "UNIT:");

    attron(COLOR_PAIR(4));
    if(is_system)
        mvprintw(2, 7, "(SYSTEM)");
    else
        mvprintw(2, 7, "(USER)");
    attroff(COLOR_PAIR(4));

    mvprintw(2, 16, "Space: User/System");
    mvprintw(2, XLOAD, "STATE:");
    mvprintw(2, XACTIVE, "ACTIVE:");
    mvprintw(2, XSUB, "SUB:");
    mvprintw(2, XDESCRIPTION, "DESCRIPTION: | Left/Right: Modus | Up/Down: Select | Return: Show status");

    attron(COLOR_PAIR(4));
    attron(A_UNDERLINE);

    /* Sets the type count */
    strncpy(tmptype, str_types[modus], 16);
    tmptype[0] = toupper(tmptype[0]);
    mvprintw(2, x, "%s: %d", tmptype, total_types[modus]);

    attroff(COLOR_PAIR(4));
    attroff(A_UNDERLINE);
    attroff(A_BOLD);
    mvhline(3, 1, ACS_HLINE, maxx - 2);
    mvvline(2, XLOAD - 1, ACS_VLINE, maxy - 3);
    mvvline(2, XACTIVE -1, ACS_VLINE, maxy - 3);
    mvvline(2, XSUB -1, ACS_VLINE, maxy - 3);
    mvvline(2, XDESCRIPTION -1, ACS_VLINE, maxy - 3);
    refresh();
}

/**
 * Handles user input and performs various operations on systemd services.
 * This function is responsible for:
 * - Handling user input from the keyboard, including navigation, service operations, and mode changes
 * - Updating the display based on the current state and user actions
 * - Calling appropriate functions to perform service operations (start, stop, restart, etc.)
 * - Reloading the service list when necessary
 */
void wait_input()
{
    int rc;
    sd_event *ev = NULL;

    rc = sd_event_default(&ev);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot fetch default event handler: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    rc = sd_event_loop(ev);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot run even loop: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    sd_event_unref(ev);
    return;
}

/**
 * Retrieves the unit file state for the given service.
 *
 * This function uses the systemd D-Bus API to fetch the current state of the
 * unit file associated with the provided service. The state is stored in the
 * `unit_file_state` field of the `Service` struct.
 *
 * @param svc The service to retrieve the unit file state for.
 * @param is_system Indicates whether the service is a system service (true) or a user service (false).
 */
void get_unit_file_state(Service *svc, bool is_system)
{
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *state = NULL;
    int rc;

    rc = is_system ? sd_bus_default_system(&bus) : sd_bus_default_user(&bus);
    if (rc < 0)
        FAIL("Cannot get unit file state for %s: %s\n", svc->unit, strerror(-rc));

    rc = sd_bus_call_method(bus,
                            SD_DESTINATION,
                            SD_OPATH,
                            SD_IFACE("Manager"),
                            "GetUnitFileState",
                            &error,
                            &reply,
			    "s",
                            svc->unit);
    if (rc < 0) {
        if (-rc == ENOENT || -rc == ENOLINK)
            return;
        FAIL("Cannot send dbus message to get unit state for %s: %s\n", svc->unit, strerror(-rc));
    }

    if (sd_bus_error_is_set(&error))
        FAIL("Bad reply to unit file state: %s\n", error.message);

    rc = sd_bus_message_read(reply, "s", &state);
    if (rc < 0)
	FAIL("Bad response reading message reply: %s\n", strerror(-rc));

    memset(svc->unit_file_state, 0, UNIT_PROPERTY_SZ);
    strncpy(svc->unit_file_state, state, UNIT_PROPERTY_SZ);

    sd_bus_unref(bus);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}

/**
 * Handles user input and performs various operations on systemd services.
 * This function is responsible for:
 * - Handling user input from the keyboard, including navigation, service operations, and mode changes
 * - Updating the display based on the current state and user actions
 * - Calling appropriate functions to perform service operations (start, stop, restart, etc.)
 * - Reloading the service list when necessary
*/
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NO_STATUS "No status information available."

#define MODE(mode) {\
    position = 0;\
    index_start = 0;\
    modus = mode;\
    clear();\
}

/**
 * Performs a systemd operation (start, stop, restart, etc.) on a service.
 * This macro checks if the operation is being performed on a system unit and the
 * user is not root, in which case it displays a status message and breaks out
 * of the operation. Otherwise, it retrieves the service at the current cursor
 * position, calls the `start_operation()` function to perform the requested
 * operation, and displays a status message if the operation fails.
 *
 * @param mode The systemd operation to perform (e.g. "start", "stop", "restart").
 * @param txt A string describing the operation (e.g. "Start", "Stop", "Restart").
 */
#define SD_OPERATION(mode, txt) {\
    bool success = false;\
    if(is_system && !is_root()) {\
        show_status_window(" You must be root for this operation on system units. Press space to toggle: System/User.", "("txt")info:");\
        break;\
    }\
    svc = service_ypos(position + 4);\
    success = start_operation(svc->unit, mode);\
    if (!success)\
        show_status_window("Command could not be executed on this unit.", txt":");\
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
int key_pressed(sd_event_source *s, int fd, uint32_t revents, void *data)
{
    int c;
    int *total_types = NULL;
    total_types = is_system ? total_system_types : total_user_types;

    if ((revents & (EPOLLHUP|EPOLLERR|EPOLLRDHUP)) > 0) {
        endwin();
        fprintf(stderr, "Error handling input: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    while ((c = getch()))
    {
        Service *svc = NULL;
        char *status = NULL;
        int max_services = 0;
        int page_scroll = maxy - 6;
        bool update_state = false;

        if (c == ERR)
            return 0;

	max_services = total_types[modus];
        switch(tolower(c))
        {
            case KEY_UP:
                if (position > 0)
                    position--;
                else if (index_start > 0)
                {
                    index_start--;
                    clear();
                }
                break;

            case KEY_DOWN:
                if (position < maxy - 6 && index_start + position < max_services - 1)
                    position++;
                else if (index_start + position < max_services - 1)
                {
                    index_start++;
                    clear();
                }
                break;

            case KEY_PPAGE: // Page Up
                if (index_start > 0)
                {
                    index_start -= page_scroll;
                    if (index_start < 0)
                    {
                        index_start = 0;
                    }
                    clear();
                }
                position = 0;
                break;

            case KEY_NPAGE: // Page Down
                if (index_start < max_services - page_scroll)
                {
                    index_start += page_scroll;
                    position = maxy - 6;
                    clear();
                }
                break;
            case KEY_LEFT:
                if(modus > ALL)
		    MODE(modus-1);
                break;

            case KEY_RIGHT:
                if(modus < SNAPSHOT)
                    MODE(modus+1);
                break;

            case KEY_SPACE:
                if (system_only)
	            break;
                is_system ^= 0x1;
		clear();
                break;

	    case KEY_RETURN:
		svc = service_ypos(position + 4);
                clear();
                if(position < 0)
                    break;
                status = get_status_info(svc);

                show_status_window(status ? status : NO_STATUS, "Status:");
                free(status);
                break;

            case KEY_F(1):
                SD_OPERATION(START, "Start");
                break;

            case KEY_F(2):
                SD_OPERATION(STOP, "Stop");
                break;

            case KEY_F(3):
                SD_OPERATION(RESTART, "Restart");
                break;

            case KEY_F(4):
                SD_OPERATION(ENABLE, "Enable");
                update_state = true;
                break;

            case KEY_F(5):
                SD_OPERATION(DISABLE, "Disable");
                update_state = true;
                break;

            case KEY_F(6):
                SD_OPERATION(MASK, "Mask");
                update_state = true;
                break;

            case KEY_F(7):
                SD_OPERATION(UNMASK, "Unmask");
                update_state = true;
                break;

            case KEY_F(8):
                SD_OPERATION(RELOAD, "Reload");
                break;

            case 'a':
                MODE(ALL);
                break;

            case 'd':
                MODE(DEVICE);
                break;

            case 'i':
                MODE(SLICE);
                break;

            case 's':
                MODE(SERVICE);
                break;

            case 'o':
                MODE(SOCKET);
                break;

            case 't':
                MODE(TARGET);
                break;

            case 'r':
                MODE(TIMER);
                break;

            case 'm':
                MODE(MOUNT);
                break;

            case 'c':
                MODE(SCOPE);
                break;

            case 'n':
                MODE(AUTOMOUNT);
                break;

            case 'w':
                MODE(SWAP);
                break;

            case 'p':
                MODE(PATH);
                break;

            case 'h':
                MODE(SNAPSHOT);
                break;

            case 'q':
            case KEY_ESC:
                quit();
                return 0;

            default:
                continue;
        }

	if (update_state)
	    get_unit_file_state(svc, is_system);

        /* redraw any lines we have invalidated */
	if (update_state) {
            service_refresh_row(svc);
            svc->changed = 0;
	}
       
        if(index_start < 0)
            index_start = 0;

        if(position < 0)
            position = 0;

        if (index_start + position >= max_services) {
            if (max_services > maxy - 6) {
                index_start = max_services - (maxy - 6);
                position = maxy - 7;
            } else {
                index_start = 0;
                position = max_services - 1;
            }
        }

        print_text_and_lines();
        print_services();
    }

    return 0;
}
#undef NO_STATUS
#undef SD_OPERATION
#undef MODE
#pragma GCC diagnostic pop

/* Configures initial dbus needed for long running systemd event handling */
void setup_dbus(sd_bus *bus)
{
    sd_event *ev = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rc = -1;

    /* Ref the bus once, this ensures it stays open. */
    sd_bus_ref(bus);

    /* Now subscribe to events in systemd */
    rc = sd_bus_call_method(bus, SD_DESTINATION, SD_OPATH, SD_IFACE("Manager"), "Subscribe", &error, NULL, NULL);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot subcribe to systemd dbus events: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    if (sd_bus_error_is_set(&error)) {
        endwin();
        fprintf(stderr, "Cannot subcribe to systemd dbus events: %s\n", error.message);
        exit(EXIT_FAILURE);
    }

    rc = sd_bus_match_signal(bus, NULL, SD_DESTINATION, SD_OPATH, SD_IFACE("Manager"), "Reloading", daemon_reloaded, NULL);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot register interest in daemon reloads: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    rc = sd_event_default(&ev);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot fetch default event handler: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    rc = sd_bus_attach_event(bus, ev, SD_EVENT_PRIORITY_NORMAL);
    if (rc < 0) {
     endwin();
        fprintf(stderr, "Unable to attach bus to event loop: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    sd_bus_error_free(&error);
}

/* Initializes the event loop and sets up the handlers for
 * recieving input */
void setup_event_loop()
{
    int rc = -1;
    sd_event *ev = NULL;

    rc = sd_event_default(&ev);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot initialize event loop: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }

    rc = sd_event_add_io(ev,
                         NULL,
                         STDIN_FILENO,
                         EPOLLIN,
                         key_pressed,
                         NULL);
    if (rc < 0) {
        endwin();
        fprintf(stderr, "Cannot initialize event handler: %s\n", strerror(-rc));
        exit(EXIT_FAILURE);
    }
}

/**
 * The main entry point of the application.
 * This function initializes the screen, retrieves all systemd services,
 * filters them, and then enters a loop to wait for user input.
 * The function returns 0 on successful exit, or -1 on error.
 */
int main()
{
    char *centered_intro = center(introduction);
    int rc;
    sd_bus *sys = NULL, *user = NULL;

    if(is_root())
        is_system = true;
    else
        is_system = false;

    modus = SERVICE;

    position = 0;  
    index_start = 0;
    
    init_screen();

    if (sd_bus_default_system(&sys) < 0)
        FAIL("Cannot initialize DBUS!\n");

    rc = sd_bus_default_user(&user);
    if (-rc == ENOMEDIUM)
        system_only = true;
    else if (rc < 0)
        FAIL("Cannot initialize DBUS: %s\n", strerror(-rc));

    setup_dbus(sys);
    if (!system_only)
        setup_dbus(user);

    rc = get_all_systemd_services(true);
    if (rc < 0) {
        endwin();
        return -1;
    }

    if (!system_only) {
      rc = get_all_systemd_services(false);
      if (rc < 0) {
          endwin();
          return -1;
      }
    }

    if (centered_intro != NULL) {
        show_status_window(center(introduction), intro_title);
        free(centered_intro);
    }

    print_text_and_lines();

    print_services();

    setup_event_loop();

    refresh();
    wait_input();

    endwin();
    return 0;
}
