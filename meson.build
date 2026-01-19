project(
    'ServiceMaster',
    'c',
    version: '1.8.0',
    default_options: ['warning_level=3', 'buildtype=release', 'strip=true', 'c_std=c99'],
)

ncurses_dep = dependency('ncurses')
systemd_dep = dependency('libsystemd')

executable(
    'servicemaster',
    'servicemaster.c',
    'sm_err.c',
    'bus.c',
    'display.c',
    'service.c',
    'config.c',
    'lib/toml.c',
    dependencies: [ncurses_dep, systemd_dep],
    install: true,
    install_dir: get_option('bindir'),
)

install_data('servicemaster.1', install_dir: join_paths(get_option('mandir'), 'man1'))

config_dir = '/etc/servicemaster'

meson.add_install_script('mkdir', '-p', config_dir)

meson.add_install_script(
    'sh',
    '-c', 'if [ ! -f "'
    + config_dir
    + '/servicemaster.toml" ]; then cp "'
    + meson.project_source_root()
    + '/servicemaster.toml" "'
    + config_dir
    + '/servicemaster.toml"; fi',
)