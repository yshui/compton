libev = dependency('libev', required: false)
if not libev.found()
	libev = cc.find_library('ev')
endif
base_deps = [
	cc.find_library('m'),
	libev
]

srcs = [ files('compton.c', 'win.c', 'c2.c', 'x.c', 'config.c', 'utils.c',
               'diagnostic.c', 'string_utils.c', 'kernel.c', 'log.c',
               'options.c', 'event.c', 'cache.c', 'atom.c') ]
compton_inc = include_directories('.')

cflags = []

required_xcb_packages = [
	'xcb-render', 'xcb-damage', 'xcb-randr', 'xcb-sync', 'xcb-composite',
	'xcb-shape', 'xcb-xinerama', 'xcb-xfixes', 'xcb-present', 'xcb'
]

required_packages = [
	'x11', 'x11-xcb', 'xcb-renderutil', 'xcb-image', 'xext', 'pixman-1'
]

foreach i : required_packages
	base_deps += [dependency(i, required: true)]
endforeach

foreach i : required_xcb_packages
	base_deps += [dependency(i, version: '>=1.12.0', required: true)]
endforeach

if not cc.has_header('uthash.h')
  error('Dependency uthash not found')
endif

deps = []

if get_option('config_file')
	deps += [dependency('libconfig', version: '>=1.4', required: true),
	         dependency('libxdg-basedir', required: true)]
	cflags += ['-DCONFIG_LIBCONFIG']
	srcs += [ 'config_libconfig.c' ]
endif
if get_option('regex')
	pcre = dependency('libpcre', required: true)
	cflags += ['-DCONFIG_REGEX_PCRE']
	if pcre.version().version_compare('>=8.20')
		cflags += ['-DCONFIG_REGEX_PCRE_JIT']
	endif
	deps += [pcre]
endif

if get_option('dbus')
	cflags += ['-DCONFIG_DBUS']
	deps += [dependency('dbus-1', required: true)]
	srcs += [ 'dbus.c' ]
endif

if get_option('xrescheck')
	cflags += ['-DDEBUG_XRC']
	srcs += [ 'xrescheck.c' ]
endif

if get_option('unittest')
	cflags += ['-DUNIT_TEST']
endif

subdir('backend')

compton = executable('compton', srcs, c_args: cflags,
  dependencies: [ base_deps, deps, test_h_dep ],
  install: true, include_directories: compton_inc)

if get_option('unittest')
	test('compton unittest', compton, args: [ '--unittest' ])
endif
