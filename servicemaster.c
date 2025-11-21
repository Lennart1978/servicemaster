#include "sm_err.h"
#include "display.h"
#include "bus.h"
#include "lib/toml.h"
#include <ncurses.h>
#include <getopt.h>

#define CONFIG_FILE "/etc/servicemaster/servicemaster.toml"

// Default editor
#define STANDARD_EDITOR "$EDITOR"

char *program_name = NULL;
bool show_welcome = true;
bool load_actual = true;

// External function to reset the terminal window title
extern void reset_terminal_title(void);

// Help message
const char *help = "\nUsage: servicemaster [options]\n\n"                   
                   "Options:\n"
                   "  -v  Display the version information and exit\n"
                   "  -w  Do not show the welcome message\n"
                   "  -h  Display this help message and exit\n"
                   "  -c  Set the colorscheme\n"
                   "      Names with a space must be enclosed in quotes!\n"
                   "  -l  List all available colorschemes\n"
                   "  -p  Print configuration file (with colorschemes)\n"
                   "  -e  Edit the configuration file\n\n"
                   "After launching ServiceMaster, you can use the following controls:\n"
                   "- Arrow keys (hljk), page up/down: Navigate through the list of units.\n"
                   "- Space: Toggle between system and user units.\n"
                   "- Enter: Show detailed status of the selected unit.\n"
                   "- F1-F8: Perform actions (start, stop, restart, etc.) on the selected unit.\n"
                   "- a-z: Quick filter units by type.\n"
                   "- q or ESC: Quit the application.\n"
                   "- +,-: Switch between colorschemes.\n"
                   "- f: Search for units by name.\n"
                   "- Tab: Select column to sort, Return: Sort.\n\n"
                   "Press F12 or ? for help\n"
                   "                2025 Lennart Martens\n\n"
                   "Configuration and colorschemes are stored in:\n" CONFIG_FILE "\n\n"
                   "License: MIT Version: " D_VERSION "\n"
                   "For bug reports, feature requests, or general inquiries:\n"
                   "https://github.com/lennart1978/servicemaster\n\n";

/**
 * Displays a welcome message with basic usage and security information.
 */
static void show_welcome_message()
{
    const char *welcome_text =
        "Welcome to ServiceMaster!\n\n"
        "This tool allows you to manage Systemd units through an intuitive interface.\n\n"
        "Press F12 or ? for help\n\n"
        "SECURITY GUIDELINE:\n"
        "- Only root can manage system services.\n"
        "- Regular users can only manage their own user services.\n\n"
        "All colorschemes and settings are stored in the configuration file:\n" CONFIG_FILE "\n"
        "'man servicemaster' or 'servicemaster -h' for more information.\n\n"
        "Press any key to continue...";

    display_status_window(welcome_text, "ServiceMaster " D_VERSION);
}

/**
 * Lists all available color schemes to stdout.
 *
 * Prints a formatted list of all color scheme names that are currently
 * loaded in the color_schemes array. The output includes a header with
 * the ServiceMaster version and decorative separators.
 */
static void list_colorschemes()
{
    printf("\nServiceMaster " D_VERSION "\n\n");
    printf("Available colorschemes:\n");
    printf("-----------------------\n\n");

    for (int i = 0; i < scheme_count; i++)
    {
        printf("%s\n", color_schemes[i].name);
    }
    printf("\n");
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
    if (rc < 0)
        sm_err_set("Cannot fetch default event handler: %s\n", strerror(-rc));

    rc = sd_event_loop(ev);
    if (rc < 0)
        sm_err_set("Cannot run even loop: %s\n", strerror(-rc));
    sd_event_unref(ev);
    return;
}

int main(int argc, char *argv[])
{
    setup_signal_handlers();
    program_name = argv[0];
    int option;
    scheme_count = 0;
    colorscheme = 0;
    color_schemes = NULL;
    actual_scheme = NULL;

    // Parse command line options using getopt
    // v: version, w: no welcome, h: help, c: colorscheme, l: list schemes, p: print config, e: edit config
    while ((option = getopt(argc, argv, "vwhc:lpe")) != -1)
    {
        switch (option)
        {
        case 'v':
            printf("Version: " D_VERSION "\n");
            return EXIT_SUCCESS;

        case 'h':
            printf("%s", help);
            return EXIT_SUCCESS;

        case 'l':
            // Load and display available colorschemes if not already loaded
            if (load_actual)
            {
                if (!load_color_schemes(CONFIG_FILE))
                {
                    sm_err_set("Failed to load colorschemes\n");
                    return EXIT_FAILURE;
                }
            }
            list_colorschemes();

            // Cleanup color schemes before exit
            if (color_schemes)
                free_color_schemes();

            return EXIT_SUCCESS;

        case 'w':
            show_welcome = false;
            break;

        case 'c':
            // Load colorschemes and set the specified scheme
            if (!load_color_schemes(CONFIG_FILE))
            {
                sm_err_set("Failed to load colorschemes\n");
                return EXIT_FAILURE;
            }
            // Search for matching colorscheme name
            for (int i = 0; i < scheme_count; i++)
            {
                if (strcmp(optarg, color_schemes[i].name) == 0)
                {
                    colorscheme = i;
                    break;
                }
            }
            load_actual = false;
            break;

        case 'p':
            // Display configuration file
            print_file(CONFIG_FILE);
            return EXIT_SUCCESS;

        case 'e':
            // Open configuration file in default editor with sudo
            printf("\n\nConfiguration file: " CONFIG_FILE "\n\n");
            if (system("sudo " STANDARD_EDITOR " " CONFIG_FILE) != 0)
            {
                // If system call fails, print an error message
                sm_err_set("Failed to edit configuration file\n");
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;

        default:
            printf("Wrong arguments: Type -h for help\n");
            return EXIT_FAILURE;
        }
    }

    // Load default colorscheme if none specified via command line
    if (load_actual)
    {
        if (!load_color_schemes(CONFIG_FILE))
        {
            sm_err_set("Failed to load colorschemes\n");
            return EXIT_FAILURE;
        }
        if (!load_actual_scheme(CONFIG_FILE))
        {
            sm_err_set("Failed to load actual colorscheme\n");
            return EXIT_FAILURE;
        }
        // Find and set the default colorscheme
        for (int i = 0; i < scheme_count; i++)
        {
            if (strcmp(actual_scheme, color_schemes[i].name) == 0)
            {
                colorscheme = i;
                break;
            }
        }
    }

    // Set bus type to USER for regular users, SYSTEM for root
    if (geteuid())
        display_set_bus_type(USER);
    else
        display_set_bus_type(SYSTEM);

    // Initialize the application
    bus_init();
    display_init();

    // Show welcome message if enabled
    if (show_welcome)
        show_welcome_message();

    // Draw initial display
    display_redraw(bus_currently_displayed());

    // Enter main input loop
    wait_input();

    // Cleanup and restore terminal state
    reset_terminal_title();
    endwin();
    echo();
    curs_set(1);

    // Free allocated color schemes
    free_color_schemes();
    return EXIT_SUCCESS;
}
