project('Servicemaster', 'c',
  version : '1.1',
  default_options : ['warning_level=3'])

ncurses_dep = dependency('ncurses')
systemd_dep = dependency('libsystemd')

executable('servicemaster',
  'main.c',
  dependencies : [ncurses_dep, systemd_dep],
  install : true)
