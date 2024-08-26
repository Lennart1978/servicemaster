# ServiceMaster 1.4.1

ServiceMaster is a powerful terminal-based tool for managing systemd units on Linux systems. It provides an intuitive interface for viewing and controlling system and user units, making it easier to manage your units without leaving the command line.

## Features

- View all systemd units or filter by type (services, devices, sockets, etc.)
- Start, stop, restart, enable, disable, mask, and unmask units
- View detailed status information for each unit
- Switch between system and user units
- User-friendly ncurses interface with color-coded information
- Keyboard shortcuts for quick navigation and control
- DBus event loop: Reacts immediately to external changes to units

## Requirements

- Linux system with systemd
- NCurses library
- Systemd development libraries

## Usage

After launching ServiceMaster, you can use the following controls:

- Arrow keys, page up/down: Navigate through the list of units
- Space: Toggle between system and user units
- Enter: Show detailed status of the selected unit
- F1-F8: Perform actions (start, stop, restart, etc.) on the selected unit
- A-Z: Quick filter units by type
- Q or ESC: Quit the application

## Security Note

For security reasons, only root can manipulate system units, and only user units can be manipulated when running as a regular user.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Author

Lennart Martens

## Contact

For bug reports, feature requests, or general inquiries:
- Email: monkeynator78@gmail.com

## Version

1.4.1

Build:
```bash
mkdir builddir && meson setup builddir --buildtype=release --prefix=/usr && meson compile -C builddir
```
Install:
```bash
sudo meson install -C builddir
```

For Archlinux users: There is 'servicemaster-bin' in the AUR.

ServiceMaster in 'Kitty' terminal and 'Monokai' theme:
<img src="sm-kitty-monokai.png" alt="SM-screenshot"></img>
ServiceMaster in 'Kgx' standard Gnome terminal emulator:
<img src="servicemaster.png" alt="SM-screenshot"></img>
<img src="servicemaster-logo.jpeg" alt="SM-Logo"></img>
