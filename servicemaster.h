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

#define VERSION        "1.3"
#define FUNCTIONS      "F1:START F2:STOP F3:RESTART F4:ENABLE F5:DISABLE F6:MASK F7:UNMASK F8:RELOAD"
#define SERVICE_TYPES  "A:ALL D:DEV I:SLICE S:SERVICE O:SOCKET T:TARGET R:TIMER M:MOUNT C:SCOPE N:AMOUNT W:SWAP P:PATH H:SSHOT"
#define NO_STATUS      "No status information available."
#define SD_DESTINATION "org.freedesktop.systemd1"
#define SD_IFACE(x)    "org.freedesktop.systemd1." x
#define SD_OPATH       "/org/freedesktop/systemd1"

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
/**
 * Macro that prints an error message to stderr and exits the program with a failure status.
 *
 * This macro is used to handle fatal errors in the program. It prints the provided error message to stderr and then
 * exits the program with the EXIT_FAILURE status code.
 *
 * @param ... The format string and arguments for the error message to print.
 */
#define FAIL(...) {\
    endwin();\
    fprintf(stderr, __VA_ARGS__);\
    exit(EXIT_FAILURE);\
}
/**
 * Sets the current position, index start, and mode for the service display.
 * This function is used to reset the service display to a known state.
 *
 * @param mode The new mode to set, which determines how the services are displayed.
 */
#define MODE(mode) {\
    position = 0;\
    index_start = 0;\
    modus = mode;\
    clear();\
}
/**
 * Performs a systemd operation on a service unit.
 *
 * This macro checks if the user has root privileges, and if not, displays a status window informing the user that they must be root
 * to perform the operation on system units. It then retrieves the service unit at the current position in the service display,
 * and attempts to start the specified operation on that unit.
 *
 * If the operation is successful, no further action is taken. If the operation fails, a status window is displayed indicating
 * that the command could not be executed on the unit.
 *
 * @param mode The systemd operation to perform, such as START, STOP, RESTART, etc.
 * @param txt A string describing the operation, used in the status window.
 */
#define SD_OPERATION(mode, txt) {\
    bool success = false;\
    if(is_system && !is_root()) {\
        show_status_window(" You must be root for this operation on system units. Press space to toggle: System/User.", "info:");\
        break;\
    }\
    svc = service_ypos(position + 4);\
    success = start_operation(svc->unit, mode);\
    if (!success)\
        show_status_window("Command could not be executed on this unit.", txt":");\
}

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

uint64_t get_now();
char* center(const char *text);
void show_status_window(const char *status, const char *title);
bool start_operation(const char* unit, enum Operations operation);
bool is_root();
bool unit_property(sd_bus *bus, const char *object, const char *iface, const char *property, const char *fmt, void *result, int sz);
char* format_unit_status(Service *svc);
char * unit_logs(Service *svc, int lines);
bool invocation_id(sd_bus *bus, Service *svc);
char* get_status_info(Service *svc);
int compare_services(const void* a, const void* b);
int test_unit_extension(const char* unit, const char* extension);
bool is_enableable_unit_type(const char *unit);
int daemon_reloaded(sd_bus_message *reply, void *data, sd_bus_error *err);
int changed_unit(sd_bus_message *reply, void *data, sd_bus_error *err);
int get_all_systemd_services(bool is_system);
void quit();
int print_s(Service *svc, int row);
void init_screen();
void print_services();
void print_text_and_lines();
void wait_input();
void get_unit_file_state(Service *svc, bool is_system);
int key_pressed(sd_event_source *s, int fd, uint32_t revents, void *data);
void setup_dbus(sd_bus *bus);
void setup_event_loop();
