#ifndef _BUS_H_
#define _BUS_H_
#include <stdbool.h>
#include <systemd/sd-bus.h>

typedef struct bus_state Bus;

#include "service.h"

#define SD_DESTINATION "org.freedesktop.systemd1"
#define SD_IFACE(x)    "org.freedesktop.systemd1." x
#define SD_OPATH       "/org/freedesktop/systemd1"

#define BUS_CPY_PROPERTY(svc, src) {\
    free(svc->src);\
    svc->src = strdup(src);\
    if (!svc->src) \
        sm_err_set("Failed to update %s property", #src);\
}

enum bus_type {
    SYSTEM = 0,
    USER
};

struct bus_state {
    enum bus_type type;
    bool reloading;
    sd_bus *bus;
    int total_types[MAX_TYPES];
    service_list services;
};

Bus * bus_currently_displayed(void);
bool bus_system_only(void);
int bus_init(void);
int bus_invocation_id(Bus *bus, Service *svc);
int bus_operation(Bus *bus, Service *svc, enum operation op);
void bus_fetch_service_status(Bus *bus, Service *svc);
void bus_update_unit_file_state(Bus *bus, Service *svc);
#endif
