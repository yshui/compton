# enable xrender
srcs += [ files('backend_common.c', 'xrender/xrender.c', 'dummy/dummy.c', 'backend.c', 'driver.c') ]

# enable opengl
if get_option('opengl')
  srcs += [ files('gl/gl_common.c', 'gl/glx.c') ]
  cflags += ['-DCONFIG_OPENGL', '-DGL_GLEXT_PROTOTYPES']
  deps += [ dependency('gl', required: true) ]
endif
