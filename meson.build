project(
  'libdrm-mirror',
  'c',
  version : '1.0.0',
  meson_version : '>=0.47.0',
  default_options: ['buildtype=release', 'warning_level=3'],
)

pkgconfig = import('pkgconfig')

cc = meson.get_compiler('c')

drm_dep = dependency('libdrm', version : '>= 2.4.0')
dl_dep = cc.find_library('dl', required : false)
mirror_deps = [drm_dep, dl_dep]

libdrm_mirror = shared_library(
  'drm-mirror',
  ['drm_mirror.c'],
  dependencies : mirror_deps,
  version : meson.project_version(),
  install : true,
)

pkgconfig.generate(
  libdrm_mirror,
  version : meson.project_version(),
  description : 'A hook of legacy DRM APIs to mirror connectors.',
)

configure_file(
  input : 'drm-mirror.conf.sample',
  output : 'drm-mirror.conf',
  install_dir : get_option('sysconfdir'),
  copy : true,
)
