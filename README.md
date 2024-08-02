<img src="servicemaster-logo.jpeg" alt="SM-Logo"></img>
# Linux systemd administration tool with nice TUI written in C
Maybe the only dependency you need to install additionally: The "ncurses" C library (for the TUI graphics)

Build:
```bash
mkdir builddir && meson setup builddir && cd builddir && meson compile
```
Run:
```bash
./servicemaster
```
Have fun !

<img src="servicemaster.png" alt="SM-screenshot"></img>
