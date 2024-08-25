project('ServiceMaster', 'c',
  version : '1.4.1',
  default_options : ['warning_level=3', 'buildtype=release'])

ncurses_dep = dependency('ncurses')
systemd_dep = dependency('libsystemd')

executable('servicemaster',
  'servicemaster.c',
  'sm_err.c',
  'bus.c',
  'display.c',
  'service.c',
  dependencies : [ncurses_dep, systemd_dep],
  install : true,
  install_dir : get_option('prefix'))

# Install the man page
man1 = 'servicemaster.1'
install_man(man1, install_dir : '/usr/share/man/man1')
