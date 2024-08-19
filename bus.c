#include <systemd/sd-bus.h>
#include <stdbool.h>

#include "sm_err.h"
#include "service.h"
#include "bus.h"
#include "display.h"

#define STS state[0]
#define STSBUS state[0].bus

#define STU state[1]
#define STUBUS state[1].bus

static bool system_only = false;
struct bus_state state[2] = {0};

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
static int bus_update_service_property(Service *svc, sd_bus_message *reply)
{
    /* Format of message at this point is: '{sv}' */
    int rc;
    const char *k, *active, *sub;

    /* s: Next item is the key out of the dictionary */
    rc = sd_bus_message_read(reply, "s", &k);
    if (rc < 0)
        sm_err_set("Cannot read dictionary key item from array: %s\n", strerror(-rc));

    /* If its a property we want to measure, update the related property */
    if (strcmp(k, "ActiveState") == 0) {
        /* v: Variant, always a string in this case */
        rc = sd_bus_message_read(reply, "v", "s", &active);
        if (rc < 0)
            sm_err_set("Cannot fetch value from dictionary: %s\n", strerror(-rc));
        BUS_CPY_PROPERTY(svc, active);

        return 1;
    }

    else if (strcmp(k, "SubState") == 0) {
        /* v: Variant, always a string in this case */
        rc = sd_bus_message_read(reply, "v", "s", &sub);
        if (rc < 0)
            sm_err_set("Cannot fetch value from dictionary: %s\n", strerror(-rc));
        BUS_CPY_PROPERTY(svc, sub);

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
static int bus_unit_changed(sd_bus_message *reply, void *data, sd_bus_error *err)
{
    Service *svc = (Service *)data;
    const char *iface = NULL;
    int rc;
    
    /* Message format: sa{sv}as */

    if (sd_bus_error_is_set(err))
        sm_err_set("Changed unit callback failed: %s\n", err->message);

    /* s: Interface name */
    rc = sd_bus_message_read(reply, "s", &iface);
    if (rc < 0)
        sm_err_set("Cannot read dbus messge: %s\n", strerror(-rc));
    
    /* If the interface is not a unit, we dont care */
    if (strcmp(iface, SD_IFACE("Unit")) != 0)
        goto fin;
            
    /* a: Array of dictionaries */
    rc = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (rc < 0)
        sm_err_set("Cannot read array in dbus message: %s\n", strerror(-rc));

    /* Array of dictionaries */ 
    while (true) {
        /* {..}: Dictionary itself */
        rc = sd_bus_message_enter_container(reply, 'e', "sv");
        if (rc < 0)
            sm_err_set("Cannot read dict item in dbus message: %s\n", strerror(-rc));

        /* No more array entries to read */
        if (rc == 0)
            break;

        svc->changed += bus_update_service_property(svc, reply);
        if (svc->changed) {
            display_redraw_row(svc);
            svc->last_update = service_now();
        }

        if (sd_bus_message_exit_container(reply) < 0)
            sm_err_set("Cannot exit dictionary: %s\n", strerror(-rc));
    }

    sd_bus_message_exit_container(reply);

    /* Redraw screen if something changed */
    if (svc->changed) {
        svc->changed = 0;
        display_redraw(bus_currently_displayed());
    }

fin:
    sd_bus_error_free(err);
    return 0;
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
static int bus_unit_property(Bus *bus, const char *object, const char *iface, const char *property, const char *fmt, void *result, int sz) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    void *data = NULL;
    int rc = 0;

    rc = sd_bus_get_property(bus->bus,
                    SD_DESTINATION,
                    object,
                    iface,
                    property,
                    &error,
                    &reply,
                    fmt);

    if (rc < 0)
        sm_err_set("Cannot fetch object property: %s", strerror(-rc));

    if (sd_bus_error_is_set(&error)) 
        sm_err_set("Cannot fetch object property: %s", error.message);

    rc = sd_bus_message_read(reply, fmt, &data);
    if (rc < 0) 
        sm_err_set("Cannot read object property: %s", strerror(-rc));

    if ((*fmt == 's' || *fmt== 'o') && sz > 0)
        strncpy(result, data, sz);
    else if ((*fmt == 's' || *fmt == 'o') && sz <= 0) {
        *(char **)result = strdup(data);
    }
    else
        memcpy(result, &data, sz);

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return rc;
}

static int bus_update_service_entry(sd_bus_message *reply, struct bus_state *st, uint64_t now)
{

    Service *svc = NULL;
    int rc = 0;
    bool is_new = false;
    const char *unit, *load, *active, *sub, *description, *object; 
    char unit_file_state[32] = {0};;

    rc = sd_bus_message_read(reply, "(ssssssouso)",
                             &unit,
                             &description, 
                             &load,
                             &active,
                             &sub,
                             NULL,
                             &object,
                             NULL,
                             NULL,
                             NULL);
    if (rc < 0) 
        sm_err_set("Cannot ready service from service list: %s", strerror(-rc));

    /* There are no more entries in the list */
    if (rc == 0)
       goto fin;

    /* Find a matching service in our existing list, or if none found create a new record */
    svc = service_get_name(st, unit);
    if (!svc) {
       is_new = true;
       svc = service_init(unit);
    }

    if (!svc)
        sm_err_set("Failed to acquire a service entry: %s", strerror(errno));

    svc->last_update = now;
    bus_unit_property(st, object, SD_IFACE("Unit"), "UnitFileState", "s", unit_file_state, 32);

    /* Properties we detect for changes */
    if (!svc->load || strcmp(svc->load, load))
        svc->changed++;
    if (!svc->active || strcmp(svc->active, active))
        svc->changed++;
    if (!svc->sub || strcmp(svc->sub, sub))
        svc->changed++;
    if (!svc->unit_file_state || strcmp(svc->unit_file_state, unit_file_state))
        svc->changed++;

    /* Properties we just update, but dont indicate change */
    BUS_CPY_PROPERTY(svc, unit);
    BUS_CPY_PROPERTY(svc, load);
    BUS_CPY_PROPERTY(svc, active);
    BUS_CPY_PROPERTY(svc, sub);
    BUS_CPY_PROPERTY(svc, description);
    BUS_CPY_PROPERTY(svc, object);
    BUS_CPY_PROPERTY(svc, unit_file_state);

    if (svc->changed) {
        display_redraw_row(svc);
        svc->changed = 0;
    }

    if (!is_new) {
        rc = 1;
        goto fin;
    }

    /* Register interest in events on this object */
    rc = sd_bus_match_signal(st->bus,
                             &svc->slot,
                             SD_DESTINATION,
                             object,
                            "org.freedesktop.DBus.Properties",
                            "PropertiesChanged",
                             bus_unit_changed,
                            (void *)svc);
     if (rc < 0) 
         sm_err_set("Cannot register interest changed units: %s\n", strerror(-rc));

     //bus_update_unit_file_state(st, svc);
     service_insert(st, svc);
     rc = 1;

fin:
    return rc;
}

static int bus_get_all_systemd_services(struct bus_state *st) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc = 0;
    uint64_t now = service_now();

    sd_bus_ref(st->bus);

    rc = sd_bus_call_method(st->bus,
                           SD_DESTINATION,
                           SD_OPATH,
                           SD_IFACE("Manager"),
                           "ListUnits",
                           &error,
                           &reply,
                           NULL);
    if (rc < 0) {
        sm_err_set("Cannot call DBUS request to fetch all units: %s", strerror(-rc));
        goto fin;
    }

    if (sd_bus_error_is_set(&error)) {
        sm_err_set("Error retrieving unit list from DBUS: %s", error.message);
        rc = -1;
        goto fin;
    }

    rc = sd_bus_message_enter_container(reply, 'a', "(ssssssouso)");
    if (rc < 0) {
        sm_err_set("Cannot enter into array fetching all units: %s", strerror(-rc));
        goto fin;
    }

    while (true) {
        rc = bus_update_service_entry(reply, st, now);
        if (rc <= 0)
            break;
    }
    sd_bus_message_exit_container(reply);

    services_prune_dead_units(st, now);

fin:
    sd_bus_message_unref(reply);
    sd_bus_unref(st->bus);
    return rc;
}

/* Callback which is invoked when a reload event is captured */
static int bus_systemd_reloaded(sd_bus_message *reply, void *data, sd_bus_error *err)
{
    /* This line here is a bug. Reload should be tracked in the services list */
    int  rc;
    struct bus_state *st = (struct bus_state *)data;

    if (sd_bus_error_is_set(err)) {
        sm_err_set("Remove unit callback failed: %s\n", err->message);
        return -1;
    }

    rc = sd_bus_message_read(reply, "b", &st->reloading);
    if (rc < 0) {
        sm_err_set("Cannot read dbus mesasge: %s\n", strerror(-rc));
        return -1;
    }

    /* Reload daemon services for specific bus type and conditionally redraw screen) */
    /* The reload emits a boolean if it starts set to true, once the reload finishes
     * the callback emits again, with the boolean set to false */
    if (st->reloading)
        goto fin;

    
    rc = bus_get_all_systemd_services(st);
    if (rc < 0)
        sm_err_set("Cannot reload system units: %s\n", strerror(-rc));

    /* If the affected bus is the one being shown */
    if (((st->type == SYSTEM) && display_bus_type() == SYSTEM)
    || ((st->type != SYSTEM) && display_bus_type() != SYSTEM))
        display_redraw(st);

fin:
    sd_bus_error_free(err);
    return 0;
}


static int bus_setup_bus(struct bus_state *st)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rc = 0;

    /* Now subscribe to events in systemd */
    rc = sd_bus_call_method(st->bus,
            SD_DESTINATION,
            SD_OPATH,
            SD_IFACE("Manager"),
            "Subscribe",
            &error,
            NULL,
            NULL);
    if (rc < 0) {
        sm_err_set("Cannot subcribe to systemd dbus events: %s\n", strerror(-rc));
        goto fin;
    }

    if (sd_bus_error_is_set(&error)) {
        sm_err_set("Cannot subcribe to systemd dbus events: %s\n", error.message);
        goto fin;
    }

    /* We care about the reloading signal/event */
    rc = sd_bus_match_signal(st->bus, 
            NULL,
            SD_DESTINATION,
            SD_OPATH,
            SD_IFACE("Manager"),
            "Reloading",
            bus_systemd_reloaded,
            (void *)st);
    if (rc < 0) {
        sm_err_set("Cannot register interest in daemon reloads: %s\n", strerror(-rc));
        goto fin;
    }

fin:
    sd_bus_error_free(&error);
    return rc;
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
void bus_update_unit_file_state(Bus *bus, Service *svc)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_file_state = NULL;
    int rc;

    sd_bus_ref(bus->bus);
    rc = sd_bus_call_method(bus->bus,
                            SD_DESTINATION,
                            SD_OPATH,
                            SD_IFACE("Manager"),
                            "GetUnitFileState",
                            &error,
                            &reply,
                            "s",
                            svc->unit);
    if (-rc == ENOENT || -rc == ENOLINK)
        goto fin;

    if (rc < 0)
        sm_err_set("Cannot send dbus message to get unit state for %s: %s\n", svc->unit, strerror(-rc));

    if (sd_bus_error_is_set(&error))
        sm_err_set("Bad reply to unit file state: %s\n", error.message);

    rc = sd_bus_message_read(reply, "s", &unit_file_state);
    if (rc < 0)
        sm_err_set("Bad response reading message reply: %s\n", strerror(-rc));

    BUS_CPY_PROPERTY(svc, unit_file_state);

fin:
    sd_bus_unref(bus->bus);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}

int bus_init(void)
{
    int rc = 0;
    sd_event *ev = NULL;
    Bus *user = &state[USER], *sys = &state[SYSTEM];

    rc = sd_event_default(&ev);
    if (rc < 0) {
        sm_err_set("Cannot fetch event handler: %s\n", strerror(-rc));
        goto fin;
    }

    /* Do the system-wide systemd instance */
    rc = sd_bus_default_system(&sys->bus);
    if (rc < 0) {
        sm_err_set("Cannot initialize DBUS: %s\n", strerror(-rc));
        goto fin;
    }

    sys->type = SYSTEM;
    TAILQ_INIT(&sys->services);
    rc = bus_setup_bus(sys);
    if (rc < 0)
        goto fin;
    sd_bus_ref(sys->bus);

    rc = sd_bus_attach_event(sys->bus, ev, SD_EVENT_PRIORITY_NORMAL);
    if (rc < 0) {
        sm_err_set("Unable to attach bus to event loop: %s\n", strerror(-rc));
        goto fin;
    }

    rc = bus_get_all_systemd_services(sys);
    if (rc < 0)
        goto fin;

    /* Optionally do the user systemd instance */
    rc = sd_bus_default_user(&user->bus);
    if (-rc == ENOMEDIUM) {
        system_only = true;
        rc = 0;
        goto fin;
    }
    else if (rc < 0) {
        sm_err_set("Cannot initialize DBUS: %s\n", strerror(-rc));
        goto fin;
    }

    system_only = false;
    user->type = USER;
    TAILQ_INIT(&user->services);
    rc = bus_setup_bus(user);
    if (rc < 0)
        goto fin;
    sd_bus_ref(user->bus);

    rc = sd_bus_attach_event(user->bus, ev, SD_EVENT_PRIORITY_NORMAL);
    if (rc < 0) {
        sm_err_set("Unable to attach bus to event loop: %s\n", strerror(-rc));
        goto fin;
    }

    rc = bus_get_all_systemd_services(user);
    if (rc < 0) 
        goto fin;

fin:
    sd_event_unref(ev);
    sd_bus_unref(user->bus);
    sd_bus_unref(sys->bus);
    return rc;
}

bool bus_system_only(void) {
    return system_only;
}

Bus * bus_currently_displayed(void)
{
    Bus *bus = &state[display_bus_type()];
    return bus;
}

/**
 * Retrieves the invocation ID for the specified system or user service unit.
 *
 * @param bus The bus connection to use.
 * @param svc Pointer to the service structure to work on.
 * @return 0 on success, or a negative error code on failure.
 */
int bus_invocation_id(Bus *bus, Service *svc) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    uint8_t *id = NULL;
    size_t len = 0;
    int rc = 0;
    char *ptr = NULL;
    size_t remaining = 32; /* Max length of invocation_id is 32 chars + 1 null terminator */

    rc = sd_bus_get_property(bus->bus,
                    SD_DESTINATION,
                    svc->object,
                    SD_IFACE("Unit"),
                    "InvocationID",
                    &error,
                    &reply,
                    "ay");
    if (sd_bus_error_is_set(&error)) {
        sm_err_window("Cannot fetch invocation ID: %s", error.message);
        goto fin;
    }

    if (rc < 0) {
        sm_err_window("Cannot fetch invocation ID: %s", strerror(-rc));
        goto fin;
    }

    rc = sd_bus_message_read_array(reply, 'y', (const void **)&id, &len);
    if (rc < 0) {
        sm_err_window("Cannot fetch this invocation ID: %s", strerror(-rc));
        goto fin;
    }

    /* There is no ID */
    if (len != 16) {
        rc = -1;
        strncpy(svc->invocation_id, "00000000000000000000000000000000", 33);
        goto fin;
    }

    ptr = svc->invocation_id;
    for (size_t i = 0; i < len && remaining > 0; i++) {
        int written = snprintf(ptr, remaining + 1, "%02hhx", id[i]);

        if (written < 0) {
            /* write error */
            sm_err_set("Failed to write invocation ID");
            strncpy(svc->invocation_id, "00000000000000000000000000000000", 33);
            rc = -1;
            goto fin;
        }

        if (written > (int)remaining)
            /* The remaining buffer was not sufficient */
            break;

        ptr += written;
        remaining -= written;
    }

    *ptr = '\0';  // Null termination

fin:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return rc;
}


void bus_fetch_service_status(Bus *bus, Service *svc)
{
    bus_invocation_id(bus, svc);
    bus_unit_property(bus, svc->object, SD_IFACE("Unit"), "FragmentPath", "s", &svc->fragment_path, 0);

    switch (svc->type) {
        case SERVICE:
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "ExecMainStartTimestamp", "t", &svc->exec_main_start, sizeof(svc->exec_main_start));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "ExecMainPID", "u", &svc->main_pid, sizeof(svc->main_pid));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "TasksCurrent", "t", &svc->tasks_current, sizeof(svc->tasks_current));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "TasksMax", "t", &svc->tasks_max, sizeof(svc->tasks_max));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "MemoryCurrent", "t", &svc->memory_current, sizeof(svc->memory_current));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "MemoryPeak", "t", &svc->memory_peak, sizeof(svc->memory_peak));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "MemorySwapCurrent", "t", &svc->swap_current, sizeof(svc->swap_current));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "MemorySwapPeak", "t", &svc->swap_peak, sizeof(svc->swap_peak));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "MemoryZSwapCurrent", "t", &svc->zswap_current, sizeof(svc->zswap_current));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "CPUUsageNSec", "t", &svc->cpu_usage, sizeof(svc->cpu_usage));
            bus_unit_property(bus, svc->object, SD_IFACE("Service"), "ControlGroup", "s", &svc->cgroup, 0);
            break;
        case DEVICE:
            bus_unit_property(bus, svc->object, SD_IFACE("Device"), "SysFSPath", "s", &svc->sysfs_path, 0);
            break;
        case MOUNT:
            bus_unit_property(bus, svc->object, SD_IFACE("Mount"), "Where", "s", &svc->mount_where, 0);
            bus_unit_property(bus, svc->object, SD_IFACE("Mount"), "What", "s", &svc->mount_what, 0);
            break;
        case TIMER:
            bus_unit_property(bus, svc->object, SD_IFACE("Timer"), "NextElapseUSecRealtime", "t", &svc->next_elapse, sizeof(svc->next_elapse));
            break;
        case SOCKET:
            bus_unit_property(bus, svc->object, SD_IFACE("Socket"), "BindIPv6Only", "s", &svc->bind_ipv6_only, 0);
            bus_unit_property(bus, svc->object, SD_IFACE("Socket"), "Backlog", "u", &svc->backlog, sizeof(svc->backlog));
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
}

int bus_operation(Bus *bus, Service *svc, enum operation op) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL, *m = NULL;
    int rc = 0;

    const char *bus_str_operations[] = {
        "StartUnit",
        "StopUnit",
        "RestartUnit",
        "EnableUnitFiles",
        "DisableUnitFiles",
        "MaskUnitFiles",
        "UnmaskUnitFiles",
        "ReloadUnit"
    };

    if (op < START || op >= MAX_OPERATIONS)
        sm_err_set("Invalid operation");

    sd_bus_ref(bus->bus);

    switch (op) {
        case ENABLE:
        case MASK:
        rc = sd_bus_message_new_method_call(bus->bus,
                                            &m,
                                            SD_DESTINATION,
                                            SD_OPATH,
                                            SD_IFACE("Manager"),
                                            bus_str_operations[op]);
        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));

        rc = sd_bus_message_append_strv(m, (char*[]) { (char*)svc->unit, NULL });
        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));

        rc = sd_bus_message_append(m, "bb", false, true);
        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc)); 

        rc = sd_bus_call(bus->bus, m, 0, &error, &reply);
        if (sd_bus_error_is_set(&error)) {
            sm_err_window("%s", error.message);
            break;
        }

        if (rc < 1)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));
        break;

        case DISABLE:
        case UNMASK:
        rc = sd_bus_message_new_method_call(bus->bus,
                                            &m,
                                            SD_DESTINATION,
                                            SD_OPATH,
                                            SD_IFACE("Manager"),
                                            bus_str_operations[op]);
        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));

        rc = sd_bus_message_append_strv(m, (char*[]) { (char*)svc->unit, NULL });
        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));

        rc = sd_bus_message_append(m, "b", false);
        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));

        rc = sd_bus_call(bus->bus, m, 0, &error, &reply);
        if (sd_bus_error_is_set(&error)) {
            sm_err_window("%s", error.message);
            break;
        }

        if (rc < 0)
            sm_err_set("cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));
        break;

        default:
        rc = sd_bus_call_method(bus->bus,
                               SD_DESTINATION,
                               SD_OPATH,
                               SD_IFACE("Manager"),
                               bus_str_operations[op],
                               &error,
                               &reply,
                               "ss",
                               svc->unit,
                               "replace");

        if (sd_bus_error_is_set(&error)) {
            sm_err_window("%s", error.message);
            break;
        }

        if (rc < 0)
            sm_err_set("Cannot send operation %s to bus: %s", bus_str_operations[op], strerror(-rc));
        break;
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus->bus);

    return rc;
}
