#include "sm_err.h"
#include "service.h"
#include "display.h"
#include <systemd/sd-journal.h>

const char * service_str_types[] = {
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
    "snapshot",
    "__unknown__"
};

/* Using the the end of the units name, identify its service type */
static void service_set_type(Service *svc)
{
    const char* dot = strrchr(svc->unit, '.');
    if (!dot || dot == svc->unit)
        return;

    for (int i=0; i < MAX_TYPES; i++) {
        if (strcmp(dot + 1, service_str_types[i]) == 0) {
            svc->type = i;
            return;
        }
    }

    svc->type = UNKNOWN;
    return;
}

static void service_free(Service *svc)
{
    if (!svc)
        return;

    sd_bus_slot_unref(svc->slot);
    free(svc->unit);
    free(svc->load);
    free(svc->active);
    free(svc->sub);
    free(svc->description);
    free(svc->object);
    free(svc->fragment_path);
    free(svc->unit_file_state);
    free(svc->cgroup);
    free(svc->sysfs_path);
    free(svc->mount_where);
    free(svc->mount_what);
    free(svc->bind_ipv6_only);
    free(svc);
}

/**
 * Retrieves the logs for a given service unit.
 *
 * @param svc The service unit to retrieve logs for.
 * @param lines The maximum number of log lines to retrieve.
 * @return A dynamically allocated string containing the formatted logs, or NULL on failure.
 */
char *service_logs(Service *svc, int lines) {
    sd_journal *j;
    char *out = NULL;
    char *ptr = NULL;
    int r;
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

    r = sd_journal_open(&j, SD_JOURNAL_SYSTEM | SD_JOURNAL_CURRENT_USER);
    if (r < 0) {
        sm_err_set("Cannot retrieve journal: %s", strerror(-r));
        return NULL;
    }

    snprintf(match, sizeof(match), "_SYSTEMD_INVOCATION_ID=%s", svc->invocation_id);
    sd_journal_add_match(j, match, 0);

    sd_journal_add_disjunction(j);
    snprintf(match, sizeof(match), "USER_INVOCATION_ID=%s", svc->invocation_id);
    sd_journal_add_match(j, match, 0);

    total = 0;
    SD_JOURNAL_FOREACH_BACKWARDS(j) {
        size_t sz;
        const char *val = NULL;

        if (left <= 0)
            break;

        struct logline *ll = &logs[left - 1];

        r = sd_journal_get_realtime_usec(j, &ll->stamp);
        if (r < 0)
            continue;

        r = sd_journal_get_data(j, "MESSAGE", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->msg, val + 8, sizeof(ll->msg) - 1);
        ll->msg[sizeof(ll->msg) - 1] = '\0';  // Null termination

        r = sd_journal_get_data(j, "_HOSTNAME", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->hostname, val + 10, sizeof(ll->hostname) - 1);
        ll->hostname[sizeof(ll->hostname) - 1] = '\0';

        r = sd_journal_get_data(j, "SYSLOG_IDENTIFIER", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->syslogident, val + 18, sizeof(ll->syslogident) - 1);
        ll->syslogident[sizeof(ll->syslogident) - 1] = '\0';

        r = sd_journal_get_data(j, "_PID", (const void **)&val, &sz);
        if (r < 0)
            continue;
        strncpy(ll->pid, val + 5, sizeof(ll->pid) - 1);
        ll->pid[sizeof(ll->pid) - 1] = '\0';

        /* The 64 is to over-compensate for writing in the timestamp and whitespace later */
        total += strlen(ll->msg) + strlen(ll->hostname) + strlen(ll->syslogident) + strlen(ll->pid) + 64;

        left--;
    }

    if (total == 0)
        goto fin;

    out = malloc(total + 1);  // +1 for Null termination
    if (!out) {
        sm_err_set("Cannot create logs: %s", strerror(errno));
        goto fin;
    }

    ptr = out;
    for (int i = left; i < lines; i++) {
        struct logline *ll = &logs[i];
        char strstamp[32] = {0};

        time_t t = (ll->stamp / 1000000);
        struct tm *tm = localtime(&t);
        strftime(strstamp, sizeof(strstamp), "%b %d %H:%M:%S", tm);

        int written = snprintf(ptr, total - (ptr - out), "%s %s %s[%s]: %s\n",
                               strstamp, ll->hostname, ll->syslogident, ll->pid, ll->msg);

        if (written < 0 || written >= total - (ptr - out)) {
            // Error exception in case of write error or overflow
            sm_err_set("Failed to write logs");
            free(out);
            out = NULL;
            goto fin;
        }

        ptr += written;
    }

    *ptr = '\0';  // Null termination

fin:
    sd_journal_close(j);
    return out;
}

/**
 * Formats the status of a service unit.
 *
 * @param svc The service unit to format the status for.
 * @return A dynamically allocated string containing the formatted status, or NULL on failure.
 */
static char * service_format_status(Service *svc) {
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
            if (svc->backlog == INT32_MAX || svc->backlog == UINT32_MAX)
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: Unlimited\n", "Backlog");
            else if (svc->backlog > INT16_MAX)
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: Invalid value (%u)\n", "Backlog", svc->backlog);
            else
                ptr += snprintf(ptr, sizeof(buf) - (ptr - buf), "%11s: %u\n", "Backlog", svc->backlog);
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
 * Initializes a new Service struct and returns a pointer to it.
 *
 * This function allocates memory for a new Service struct and returns a pointer to it.
 * The struct is initialized with all fields set to 0 or NULL.
 *
 * @return A pointer to the newly initialized Service struct.
 */
Service * service_init(const char *name)
{
    Service *svc = NULL;
    char *nm = NULL;

    svc = calloc(1, sizeof(Service));
    nm = strdup(name);

    if (!svc || !nm) {
        free(svc);
        free(nm);
        return NULL;
    }

    svc->unit = nm;
    service_set_type(svc);

    return svc;
}

/* Return the nth service in the list, accounting for the enabled
 * filter */
Service * service_nth(Bus *bus, int n)
{
    int i = 0;
    Service *svc;

    TAILQ_FOREACH(svc, &bus->services, e) {
        if (display_mode() != ALL && svc->type != display_mode())
            continue;

        if (i == n)
            return svc;

        i++;
    }
    return NULL;
}

/**
 * Finds the service with the specified y-position in the service list.
 *
 * @param ypos The y-position of the service to find.
 * @return The service with the specified y-position, or NULL if not found.
 */
Service * service_ypos(Bus *bus, int ypos)
{
    Service *svc;

    TAILQ_FOREACH(svc, &bus->services, e) {
        if (svc->ypos == ypos)
            return svc;
    }

    return NULL;
}

/* Insert service into the list in a sorted order */
void service_insert(Bus *bus, Service *svc)
{
    Service *node = NULL;

    bus->total_types[svc->type]++;
    bus->total_types[ALL]++;

    /* List is empty, add to the head of the list */
    if (TAILQ_EMPTY(&bus->services)) {
        TAILQ_INSERT_HEAD(&bus->services, svc, e);
        return;
    }

    /* Find the next entry lexicographically above us and insert */
    TAILQ_FOREACH(node, &bus->services, e) {
        if (strcmp(node->object, svc->object) <= 0)
            continue;

        TAILQ_INSERT_BEFORE(node, svc, e);
        return;
    }

    /* This item is the lexicographically greatest, put in tail */
    TAILQ_INSERT_TAIL(&bus->services, svc, e);
}

/* Return the service that matches this unit name */
Service * service_get_name(Bus *bus, const char *name)
{
    Service *svc = NULL;

    TAILQ_FOREACH(svc, &bus->services, e) {
        if (strcmp(name, svc->unit) == 0)
            return svc;
    }
    return NULL;
}

/* Iterate through the list, remove any that haven't been updated since
 * timestamp */
void services_prune_dead_units(Bus *bus, uint64_t ts)
{
    int removed = 0;
    Service *svc = NULL;

    svc = TAILQ_FIRST(&bus->services);
    while (svc) {
      Service *n;
      n = TAILQ_NEXT(svc, e);

      if (svc->last_update >= ts) {
          svc = n;
          continue;
      }

      TAILQ_REMOVE(&bus->services, svc, e);
      if (svc->ypos > -1)
          removed++;

      service_free(svc);
      svc = n;
    }

    /* If and only if we removed a item from our ACTIVE list, clear the screen */
    if (removed)
        display_erase();
    return;
}

/* This is used during the print services routine
 * to reset the y positions on all services, since not
 * every service is displayed at once. */
void services_invalidate_ypos(Bus *bus)
{
    Service *svc = NULL;

    TAILQ_FOREACH(svc, &bus->services, e) {
        svc->ypos = -1;
    }
}


/* Fetch the event handlers understanding of the current time */
uint64_t service_now(void)
{
    sd_event *ev = NULL;
    uint64_t now;
    int rc;

    rc = sd_event_default(&ev);
    if (rc < 0) 
        sm_err_set("Cannot find default event handler: %s\n", strerror(-rc));

    rc = sd_event_now(ev, CLOCK_MONOTONIC, &now);
    if (rc < 0)
        sm_err_set("Cannot fetch event handler timestamp: %s\n", strerror(-rc));

    sd_event_unref(ev);

    return now;
}

Service * service_next(Service *svc)
{
    if (!svc)
        return NULL;

    return TAILQ_NEXT(svc, e);
}

char * service_status_info(Bus *bus, Service *svc)
{
    char *out = NULL;
    char *logs = NULL;

    bus_fetch_service_status(bus, svc);

    out = service_format_status(svc);
    logs = service_logs(svc, 10);
    if (!logs)
        goto fin;

    out = realloc(out, strlen(out) + strlen(logs) + 1);
    if (!out)
        goto fin;

    strcat(out, logs);

fin:
    free(logs);
    return out;
}

const char * service_string_type(enum service_type type)
{
    return service_str_types[type];
}
