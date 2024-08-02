#include <ncurses.h>
#include <systemd/sd-bus.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>

#define KEY_RETURN 10
#define KEY_ESC 27
#define KEY_SPACE 32
#define MAX_SERVICES 1000
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

char *introduction = "Press Space to switch between system and user systemd units.\nFor security reasons, only root can manipulate system units and"
                     " only user user units.\nPress Return to display unit status information. Use left/right to toggle modes and up/down"
                     " to select units.\nPress the F keys to manipulate the units and ESC or Q to exit the program.\n"
                     "I am not responsible for any damage caused by this program.\nIf you don't exactly know what you are doing here, please don't use it.\n"
                     "--> PRESS ANY KEY TO CONTINUE <--\n\nHave fun !\n\nLennart Martens 2024\nLicense: MIT\nmonkeynator78@gmail.com\n"
                     "https://github.com/lennart1978/servicemaster\nVersion: 1.0";                   


char *intro_title = "A quick introduction to ServiceMaster:";

int maxx, maxy, position, num_of_services, index_start;
size_t maxx_description;
bool is_system = false;

enum Operations {
    START,
    STOP,
    RESTART,
    ENABLE,
    DISABLE,
    MASK,
    UNMASK,
    RELOAD,
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
};

enum Type modus;

typedef struct {
    int index;
    char unit[CHARS_UNIT];
    char load[CHARS_LOAD];
    char active[CHARS_ACTIVE];
    char sub[CHARS_SUB];
    char description[CHARS_DESCRIPTION];
    enum Type type;        
} Service;

Service services[MAX_SERVICES];
Service filtered_services[MAX_SERVICES];

typedef struct {
    int devices;
    int slices;
    int services;
    int sockets;
    int targets;
    int timers;
    int mounts;
    int scopes;
    int automounts;
    int swaps;
    int paths;
    int snapshots;
} Total;

Total total_types;

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

void show_status_window(const char *status, const char *title) {
    char status_cpy[strlen(status) + 1];
    strcpy(status_cpy, status);
    int maxx_row = 0;

    if(strlen(status_cpy) > 100 && status_cpy[0] != 'P' && status_cpy[0] != 'C')
    {
        for(int i = 0; i < 4; i++)
        {
            status_cpy[i] = ' ';
        }
    }
  
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
    
    mvwprintw(win, 0, width / 2 - strlen(title) / 2, title);
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

bool daemon_reload() {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus *bus = NULL;
    const char *service = "org.freedesktop.systemd1";
    const char *object_path = "/org/freedesktop/systemd1";
    const char *interface = "org.freedesktop.systemd1.Manager";
    const char *method = "Reload";
    int r;

    r = is_system ? sd_bus_open_system(&bus) : sd_bus_open_user(&bus);
    if (r < 0) {
        show_status_window(strerror(-r), "Can't connect to bus:");
        return false;
    }

    r = sd_bus_call_method(bus, service, object_path, interface, method, &error, NULL, NULL);
    if (r < 0) {
        char status_message[256];
        snprintf(status_message, sizeof(status_message), "Failed to reload daemon: %s", error.message);
        show_status_window(status_message, "D-Bus Error:");
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return false;
    }    
    sd_bus_unref(bus);
    return true;
}

bool start_operation(const char* unit, enum Operations operation) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL, *m = NULL;
    sd_bus *bus = NULL;
    const char *method = NULL;
    const char *service = "org.freedesktop.systemd1";
    const char *object_path = "/org/freedesktop/systemd1";
    const char *interface = "org.freedesktop.systemd1.Manager";
    int r;

    if (unit == NULL) {
        show_status_window("Unit name is NULL", "Error:");
        return false;
    }

    if (is_system && geteuid() != 0) {
        show_status_window("Root privileges required for system units", "Warning:");
        return false;
    }

    r = is_system ? sd_bus_open_system(&bus) : sd_bus_open_user(&bus);
    if (r < 0) {
        show_status_window(strerror(-r), "Can't connect to bus:");
        return false;
    }

    switch (operation) {
        case START:
            method = "StartUnit";
            break;
        case STOP:
            method = "StopUnit";
            break;
        case RESTART:
            method = "RestartUnit";
            break;
        case ENABLE:
            method = "EnableUnitFiles";
            break;
        case DISABLE:
            method = "DisableUnitFiles";
            break;
        case MASK:
            method = "MaskUnitFiles";
            break;
        case UNMASK:
            method = "UnmaskUnitFiles";
            break;
        case RELOAD:
            method = "ReloadUnit";
            break;
        default:
            sd_bus_unref(bus);
            show_status_window("Invalid operation", "Error:");
            return false;
    }

    char status_message[256];    

    if (strcmp(method, "EnableUnitFiles") == 0 || strcmp(method, "MaskUnitFiles") == 0) {
        r = sd_bus_message_new_method_call(bus, &m, service, object_path, interface, method);
        if (r < 0) {
            show_status_window("Failed to create method call", "Error:");
            goto finish;
        }

        r = sd_bus_message_append_strv(m, (char*[]) { (char*)unit, NULL });
        if (r < 0) {
            show_status_window("Failed to append unit", "Error:");
            goto finish;
        }

        r = sd_bus_message_append(m, "bb", false, true);
        if (r < 0) {
            show_status_window("Failed to append arguments", "Error:");
            goto finish;
        }

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
            snprintf(status_message, sizeof(status_message), "Failed to call method: %s", error.message);
            show_status_window(status_message, "D-Bus Error:");
            goto finish;
        }
    } else if (strcmp(method, "DisableUnitFiles") == 0 || strcmp(method, "UnmaskUnitFiles") == 0) {
        r = sd_bus_message_new_method_call(bus, &m, service, object_path, interface, method);
        if (r < 0) {
            show_status_window("Failed to create method call", "Error:");
            goto finish;
        }

        r = sd_bus_message_append_strv(m, (char*[]) { (char*)unit, NULL });
        if (r < 0) {
            show_status_window("Failed to append unit", "Error:");
            goto finish;
        }

        r = sd_bus_message_append(m, "b", false);
        if (r < 0) {
            show_status_window("Failed to append arguments", "Error:");
            goto finish;
        }

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
            snprintf(status_message, sizeof(status_message), "Failed to call method: %s", error.message);
            show_status_window(status_message, "D-Bus Error:");
            goto finish;
        }
    } else {
        r = sd_bus_call_method(bus, service, object_path, interface, method, &error, &reply,
                               "ss", unit, "replace");
        if (r < 0) {
            snprintf(status_message, sizeof(status_message), "Failed to call method: %s", error.message);
            show_status_window(status_message, "D-Bus Error:");
            goto finish;
        }
    }    

    if (!daemon_reload()) {
        show_status_window("Failed to reload daemon after operation", "Warning:");
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

char* get_status_info(const char* unit) {
    char command[256];
    char* output = malloc(8000);
    if (output == NULL) {
        fprintf(stderr, "Speicherzuweisung fehlgeschlagen\n");
        return NULL;
    }
    output[0] = '\0';

    if(is_system)
        snprintf(command, sizeof(command), "systemctl status %s 2>/dev/null", unit);
    else
        snprintf(command, sizeof(command), "systemctl --user status %s 2>/dev/null", unit);

    FILE* fp = popen(command, "r");
    if (fp == NULL) {
        free(output);
        return NULL;
    }

    char buffer[128];
    size_t total_read = 0;
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t len = strlen(buffer);
        if (total_read + len >= 8000) {
            fprintf(stderr, "Ausgabe zu groß, wird abgeschnitten\n");
            break;
        }
        strcpy(output + total_read, buffer);
        total_read += len;
    }

    int status = pclose(fp);
    if (status != 0 || total_read == 0) {
        free(output);
        return NULL;
    }

    return output;
}



void print_info(const char *message)
{
    attron(A_BOLD);
    attron(COLOR_PAIR(3));
    mvprintw(2, XLOAD  - 40, message);
    refresh();
    attroff(COLOR_PAIR(3));
    attroff(A_BOLD);
}

int compare_services(const void* a, const void* b)
{
    Service* serviceA = (Service*)a;
    Service* serviceB = (Service*)b;
    return strcasecmp(serviceA->unit, serviceB->unit);
}

void sort_units_services(Service* services, int num_services) {
    
    qsort(services, num_services, sizeof(Service), compare_services);

    for (int i = 0; i < num_services; i++) {
        services[i].index = i;
    }
}

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

void filter_services()
{
    int i, k = 0;
  
    for(i = 0; i < num_of_services; i++)
    {
        filtered_services[i].unit[0] = '\0';
        filtered_services[i].load[0] = '\0';
        filtered_services[i].active[0] = '\0';
        filtered_services[i].sub[0] = '\0';
        filtered_services[i].description[0] = '\0';
        filtered_services[i].index = 0;
    }        

    for(i = 0; i < num_of_services; i++)
    {
        if(services[i].type == modus)
        {
            filtered_services[k] = services[i];
            k++;
        }
    }
    sort_units_services(filtered_services, k);
}

void delete_all_services()
{
    int i;
    for(i = 0; i < num_of_services; i++)
    {
        services[i].unit[0] = '\0';
        services[i].load[0] = '\0';
        services[i].active[0] = '\0';
        services[i].sub[0] = '\0';
        services[i].description[0] = '\0';
        services[i].index = 0;          
    }

    num_of_services = 0;

    total_types.devices = 0;
    total_types.slices = 0;
    total_types.services = 0;
    total_types.sockets = 0;
    total_types.targets = 0;
    total_types.timers = 0;
    total_types.mounts = 0;
    total_types.scopes = 0;
    total_types.automounts = 0;
    total_types.swaps = 0;
    total_types.paths = 0;
    total_types.snapshots = 0;
}

bool is_enableable_unit_type(const char *unit) {
    static const char *enableable_extensions[] = {
        ".service", ".socket", ".timer", ".path", ".target", ".mount", ".automount"
    };
    size_t unit_len = strlen(unit);
    for (size_t i = 0; i < sizeof(enableable_extensions) / sizeof(enableable_extensions[0]); i++) {
        size_t ext_len = strlen(enableable_extensions[i]);
        if (unit_len > ext_len && strcmp(unit + unit_len - ext_len, enableable_extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

int get_all_systemd_services() {
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r, i = 0;

    if (is_system) {
        r = sd_bus_open_system(&bus);
    } else {
        r = sd_bus_open_user(&bus);
    }

    if (r < 0) {        
        return -1;
    }

    r = sd_bus_call_method(bus,
                           "org.freedesktop.systemd1",
                           "/org/freedesktop/systemd1",
                           "org.freedesktop.systemd1.Manager",
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

    const char *unit, *load, *active, *sub, *description;
    while ((r = sd_bus_message_read(reply, "(ssssssouso)", 
            &unit, &load, &active, &sub, &description,
            NULL, NULL, NULL, NULL, NULL)) > 0) {
        
        if (i < MAX_SERVICES) {
            strncpy(services[i].unit, unit, sizeof(services[i].unit) - 1);
            strncpy(services[i].load, description, sizeof(services[i].load) - 1);
            strncpy(services[i].active, active, sizeof(services[i].active) - 1);
            strncpy(services[i].sub, sub, sizeof(services[i].sub) - 1);
            strncpy(services[i].description, load, sizeof(services[i].description) - 1);
            services[i].index = i;            

            if (test_unit_extension(services[i].unit, "service")) {
                total_types.services++;
                services[i].type = SERVICE;
            } else if (test_unit_extension(services[i].unit, "device")) {
                total_types.devices++;
                services[i].type = DEVICE;
            } else if (test_unit_extension(services[i].unit, "slice")) {
                total_types.slices++;
                services[i].type = SLICE;
            } else if (test_unit_extension(services[i].unit, "socket")) {
                total_types.sockets++;
                services[i].type = SOCKET;
            } else if (test_unit_extension(services[i].unit, "target")) {
                total_types.targets++;
                services[i].type = TARGET;
            } else if (test_unit_extension(services[i].unit, "timer")) {
                total_types.timers++;
                services[i].type = TIMER;
            } else if (test_unit_extension(services[i].unit, "mount")) {
                total_types.mounts++;
                services[i].type = MOUNT;
            } else if (test_unit_extension(services[i].unit, "scope")) {
                total_types.scopes++;
                services[i].type = SCOPE;
            } else if (test_unit_extension(services[i].unit, "automount")) {
                total_types.automounts++;
                services[i].type = AUTOMOUNT;
            } else if (test_unit_extension(services[i].unit, "swap")) {
                total_types.swaps++;
                services[i].type = SWAP;
            } else if (test_unit_extension(services[i].unit, "path")) {
                total_types.paths++;
                services[i].type = PATH;
            } else if (test_unit_extension(services[i].unit, "snapshot")) {
                total_types.snapshots++;
                services[i].type = SNAPSHOT;
            }

            services[i].unit[sizeof(services[i].unit) - 1] = '\0';
            services[i].load[sizeof(services[i].load) - 1] = '\0';
            services[i].active[sizeof(services[i].active) - 1] = '\0';
            services[i].sub[sizeof(services[i].sub) - 1] = '\0';
            services[i].description[sizeof(services[i].description) - 1] = '\0';

            i++;
        }
        
        if (i >= MAX_SERVICES) {            
            break;
        }
    }
    
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);

    sort_units_services(services, i);

    return i;
}

void quit()
{
    clear();
    endwin();
    exit(1);
}

void print_s(int i, int row)
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
    
    if(i <= num_of_services && modus == ALL)
    {
        if(strlen(services[i].unit) >= XLOAD -3)
        {     
            char short_unit[XLOAD - 2];
            strncpy(short_unit, services[i].unit, XLOAD - 2);
            
            mvaddstr(row + 4, 1, short_unit);
            mvaddstr(row + 4,XLOAD - 4, "...");
        }
        else
            mvaddstr(row + 4, 1, services[i].unit);
        
        mvaddstr(row + 4, XLOAD, services[i].load);
        mvaddstr(row + 4, XACTIVE, services[i].active);
        mvaddstr(row + 4, XSUB, services[i].sub);

        if(strlen(services[i].description) >= maxx_description)
        {
            char short_description[maxx_description - 3];
            strncpy(short_description, services[i].description, maxx_description - 3);

            mvaddstr(row + 4, XDESCRIPTION, short_description);
            mvaddstr(row + 4, XDESCRIPTION + maxx_description - 3, "...");
        }
        else
            mvaddstr(row + 4, XDESCRIPTION, services[i].description);                   
    } else if(i <= num_of_services && modus != ALL)
    {
        if(strlen(filtered_services[i].unit) >= XLOAD -3)
        {     
            char short_unit[XLOAD - 2];
            strncpy(short_unit, filtered_services[i].unit, XLOAD - 2);
            
            mvaddstr(row + 4, 1, short_unit);
            mvaddstr(row + 4,XLOAD - 4, "...");
        }
        else
            mvaddstr(row + 4, 1, filtered_services[i].unit);

        mvaddstr(row + 4, XLOAD, filtered_services[i].load);
        mvaddstr(row + 4, XACTIVE, filtered_services[i].active);
        mvaddstr(row + 4, XSUB, filtered_services[i].sub);

        if(strlen(filtered_services[i].description) >= maxx_description)
        {
            char short_description[maxx_description - 3];
            strncpy(short_description, filtered_services[i].description, maxx_description - 3);

            mvaddstr(row + 4, XDESCRIPTION, short_description);
            mvaddstr(row + 4, XDESCRIPTION + maxx_description - 3, "...");
        }
        else
            mvaddstr(row + 4, XDESCRIPTION, filtered_services[i].description);
    }
}

void init_screen()
{
    initscr();
    
    getmaxyx(stdscr, maxy, maxx);
    char *error_message = "Sorry, your terminal is too small to use this program !";

    if(maxx < 200 || maxy < 10)
    {
        clear();
        mvaddstr(maxy / 2, (maxx / 2) - (strlen(error_message) / 2), error_message);
        getch();
        quit();
    }
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

void print_services()
{
    int max_rows = maxy - 5;
    int row = 0;
    int i = index_start;
    
    i = index_start;
    
    while(row < max_rows && i < num_of_services)
    {
        if(i < MAX_SERVICES)
        {
            switch(modus)
            {
                case ALL:               
                print_s(i, row);
                row++; 
                break;
                case DEVICE:
                    print_s(i, row);
                    row++;
                break;
                case SLICE:
                    print_s(i, row);
                    row++;
                break;
                case SERVICE:
                    print_s(i, row);
                    row++;
                break;
                case SOCKET:
                    print_s(i, row);
                    row++;
                break;
                case TARGET:
                    print_s(i, row);
                    row++;
                break;
                case TIMER:
                    print_s(i, row);
                    row++;
                break;
                case MOUNT:
                    print_s(i, row);
                    row++;
                break;
                case SCOPE:
                    print_s(i, row);
                    row++;
                break;
                case AUTOMOUNT:
                    print_s(i, row);
                    row++;
                break;
                case SWAP:
                    print_s(i, row);
                    row++;
                break;
                case PATH:
                    print_s(i, row);
                    row++;
                break;
                case SNAPSHOT:
                    print_s(i, row);
                    row++;
                break;                
                default:
                continue;
            }
           i++;
        }
    }  
}

void print_text_and_lines()
{
    int x = XLOAD / 2 - 10;    
    char *headline = "SERVICEMASTER V1.0 | Q/ESC:Quit";
    char *functions = "F1:START F2:STOP F3:RESTART F4:ENABLE F5:DISABLE F6:MASK F7:UNMASK F8:RELOAD";
    char *types = "A:ALL D:DEV I:SLICE S:SERVICE O:SOCKET T:TARGET R:TIMER M:MOUNT C:SCOPE N:AMOUNT W:SWAP P:PATH H:SSHOT";
    
    attroff(COLOR_PAIR(9));
    border(0, 0, 0, 0, 0, 0, 0, 0);

    attron(A_BOLD);
    attron(COLOR_PAIR(0));    
    mvaddstr(1, 1, headline);
    attroff(COLOR_PAIR(8));

    attron(COLOR_PAIR(9));        
    mvaddstr(1, strlen(headline) + 2, functions);
    attroff(COLOR_PAIR(9));

    attron(COLOR_PAIR(10));
    mvaddstr(1, strlen(headline) + strlen(functions) + 3, types);
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
    mvprintw(2, XLOAD, "LOAD:");
    mvprintw(2, XACTIVE, "ACTIVE:");
    mvprintw(2, XSUB, "SUB:");
    mvprintw(2, XDESCRIPTION, "DESCRIPTION: | Left/Right: Modus | Up/Down: Select | Return: Show status");        

    attron(COLOR_PAIR(4));
    attron(A_UNDERLINE);
    switch(modus)
    {        
        case ALL:
            mvprintw(2, x, "Total: %d", num_of_services);
            break;           
        case DEVICE:
            mvprintw(2, x, "Devices: %d", total_types.devices);
            break;            
        case SLICE:
            mvprintw(2, x, "Slices: %d", total_types.slices);
            break;            
        case SERVICE:
            mvprintw(2, x, "Services: %3d", total_types.services);
            break;            
        case SOCKET:
            mvprintw(2, x, "Sockets: %d", total_types.sockets);
            break;            
        case TARGET:
            mvprintw(2, x, "Targets: %d", total_types.targets);
            break;            
        case TIMER:
            mvprintw(2, x, "Timers: %d", total_types.timers);
            break;            
        case MOUNT:
            mvprintw(2, x, "Mounts: %d", total_types.mounts);            
            break;
        case SCOPE:
            mvprintw(2, x, "Scopes: %d", total_types.scopes);
            break;            
        case AUTOMOUNT:
            mvprintw(2, x, "AutoMounts: %d", total_types.automounts);
            break;
        case SWAP:
            mvprintw(2, x, "Swaps: %d", total_types.swaps);
            break;                        
        case PATH:
            mvprintw(2, x, "Paths: %d", total_types.paths);
            break;            
        case SNAPSHOT:
            mvprintw(2, x, "Snapshots: %d", total_types.snapshots);
            break;            
        default:
            break;
    }

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

void reload_all(void)
{
    position = 0;
    index_start = 0;
    delete_all_services();
    num_of_services = get_all_systemd_services();
    filter_services();
    clear();
}

char *root_error = " You must be root for this operation on system units. Press space to toggle: System/User.";
char *status_error = " No status information available.";

void wait_input()
{
    enum Operations op;
    bool success = false;
    char *pos = "Command sent successfully.";
    char *neg = "Command could not be executed on this unit.";

    int c;

    while (1)
    {
        c = getch();
        int max_services = 0;
        for(int i = 0; i < num_of_services; i++) {
            if(services[i].type == modus) {
                max_services++;
            } else if(modus == ALL)
            {
                max_services = num_of_services;
                break;
            }
        }

        switch(c)
        {
            case KEY_UP:
                if (position > 0)
                {
                    position--;
                }
                else if (index_start > 0)
                {
                    index_start--;
                    clear();
                }
                break;
            case KEY_DOWN:
                if (position < maxy - 6 && index_start + position < max_services - 1)
                {
                    position++;
                }
                else if (index_start + position < max_services - 1)
                {
                    index_start++;
                    clear();
                }
                break;
            case KEY_LEFT:
                if(modus > ALL)
                {
                    position = 0;
                    index_start = 0;
                    modus--;
                    filter_services();
                    clear();
                }
                break;
            case KEY_RIGHT:
                if(modus < SNAPSHOT)
                {
                    position = 0;
                    index_start = 0;
                    modus++;
                    filter_services();
                    clear();
                }
                break;
            case KEY_SPACE:
                if(is_system && is_root())
                {                    
                    show_status_window(" Start Servicemaster as user to manipulate user units.", "You are now running as root !");
                    break;                   
                }
                else if(is_system && !is_root())
                {
                    is_system = false;
                }
                else
                {
                    is_system = true;
                }
                reload_all();
                break;
            case KEY_RETURN:            
                clear();
                if(modus == ALL && position >= 0 && strlen(services[position + index_start].unit) > 1)
                {
                    if(get_status_info(services[position + index_start].unit)!= NULL)
                        show_status_window(get_status_info(services[position + index_start].unit), "Status:");
                    else
                        show_status_window(status_error, "Status:");
                } else if(modus != ALL && position >= 0 && strlen(filtered_services[position + index_start].unit) > 1)
                {
                    if(get_status_info(filtered_services[position + index_start].unit)!= NULL)
                        show_status_window(get_status_info(filtered_services[position + index_start].unit), "Status:");                    
                    else
                        show_status_window(status_error, "Status:");
                }
                break;
            case KEY_F(1):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Start)info:");
                    break;
                }
                else
                {
                    op = START;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Start:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Start:");
                break;
            case KEY_F(2):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Stop)info:");
                    break;
                }
                else
                {
                    op = STOP;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Stop:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Stop:");
                break;
            case KEY_F(3):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Restart)info:");
                    break;
                }
                else
                {
                    op = RESTART;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Restart:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Restart:");
                break;
            case KEY_F(4):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Enable)info:");
                    break;
                }
                else
                {
                    op = ENABLE;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Enable:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Enable:");
                break;
            case KEY_F(5):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Disable)info:");
                    break;
                }
                else
                {
                    op = DISABLE;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Disable:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Disable:");
                break;
            case KEY_F(6):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Mask)info:");
                    break;
                }
                else
                {
                    op = MASK;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Mask:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Mask:");
                break;
            case KEY_F(7):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Unmask)info:");
                    break;
                }
                else
                {
                    op = UNMASK;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Unmask:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Unmask:");
                break;
            case KEY_F(8):
                if(is_system && !is_root())
                {
                    show_status_window(root_error, "(Reload)info:");
                    break;
                }
                else
                {
                    op = RELOAD;
                    if(modus == ALL)
                        success = start_operation(services[position + index_start].unit, op);
                    else
                        success = start_operation(filtered_services[position + index_start].unit, op);
                }
                if(success)
                {
                    show_status_window(pos, "Reload:");
                    reload_all();
                }
                else
                    show_status_window(neg, "Reload:");
                break;
            case 'a':
                position = 0;
                index_start = 0;
                modus = ALL;
                clear();
                break;
            case 'd':
                position = 0;
                index_start = 0;
                modus = DEVICE;
                filter_services();
                clear();
                break;
            case 'i':
                position = 0;
                index_start = 0;
                modus = SLICE;
                filter_services();
                clear();
                break;
            case 's':
                position = 0;
                index_start = 0;
                modus = SERVICE;
                filter_services();
                clear();
                break;
            case 'o':
                position = 0;
                index_start = 0;
                modus = SOCKET;
                filter_services();
                clear();
                break;
            case 't':
                position = 0;
                index_start = 0;
                modus = TARGET;
                filter_services();
                clear();
                break;
            case 'r':
                position = 0;
                index_start = 0;
                modus = TIMER;
                filter_services();
                clear();
                break;
            case 'm':
                position = 0;
                index_start = 0;
                modus = MOUNT;
                filter_services();
                clear();
                break;
            case 'c':
                position = 0;
                index_start = 0;
                modus = SCOPE;
                filter_services();
                clear();
                break;
            case 'n':
                position = 0;
                index_start = 0;
                modus = AUTOMOUNT;
                filter_services();
                clear();
                break;
            case 'w':
                position = 0;
                index_start = 0;
                modus = SWAP;
                filter_services();
                clear();
                break;
            case 'p':
                position = 0;
                index_start = 0;
                modus = PATH;
                filter_services();
                clear();
                break;
            case 'h':
                position = 0;
                index_start = 0;
                modus = SNAPSHOT;
                filter_services();
                clear();
                break;
            case 'q':
            case KEY_ESC:
                quit();
                return;
            default:               
                continue;
        }
       
        if(index_start < 0) {
            index_start = 0;
        }

        if(position < 0) {
            position = 0;
        }   

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
}

int main()
{
    char *centered_intro = center(introduction);

    if(is_root())
        is_system = true;
    else
        is_system = false;
    
    modus = SERVICE;
    position = 0;  
    index_start = 0;     
    
    init_screen();
    
    num_of_services = get_all_systemd_services();
    if (num_of_services < 0) {
        endwin();
        return -1;
    }
     if (centered_intro != NULL) {
        show_status_window(center(introduction), intro_title);
        free(centered_intro);
    }   

    filter_services();

    print_text_and_lines();
    
    print_services();

    wait_input();

    endwin();
    return 0;
}