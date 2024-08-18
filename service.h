#ifndef _SERVICE_H_
#define _SERVICE_H_
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/queue.h>
#include <systemd/sd-bus.h>

typedef struct service_list service_list;

enum operation {
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

enum service_type {
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
    UNKNOWN,
    MAX_TYPES
};

typedef struct Service {
    int ypos;
    int changed;
    uint64_t last_update;

    char *unit;
    char *load;
    char *active;
    char *sub;
    char *description;
    char *object;
    char *fragment_path;
    char *unit_file_state;
    char invocation_id[33];

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

    char *cgroup;
    char *sysfs_path;       // For DEVICE
    char *mount_where;      // For MOUNT
    char *mount_what;       // For MOUNT
    uint64_t next_elapse;   // For TIMER
    uint32_t backlog;       // For SOCKET
    char *bind_ipv6_only;   // For SOCKET

    enum service_type type;
    sd_bus_slot *slot;

    TAILQ_ENTRY(Service) e;
} Service;

TAILQ_HEAD(service_list, Service);

#include "bus.h"
Service * service_get_name(Bus *bus, const char *name);
Service * service_init(const char *name);
Service * service_next(Service *svc);
Service * service_nth(Bus *bus, int n);
Service * service_ypos(Bus *bus, int ypos);
char * service_status_info(Bus *bus, Service *svc);
const char * service_string_type(enum service_type type);
uint64_t service_now(void);
void service_insert(Bus *bus, Service *svc);
void services_invalidate_ypos(Bus *bus);
void services_prune_dead_units(Bus *bus, uint64_t ts);
#endif
