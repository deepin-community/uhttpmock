project('uhttpmock', 'c',
  version: '0.11.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu99',
  ],
)

# Modules
gnome = import('gnome')
pkgconfig = import('pkgconfig')

# Compiler
cc = meson.get_compiler('c')
add_project_arguments([
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
], language: 'c')

# Versioning
uhm_version_split = meson.project_version().split('.')
uhm_major_version = uhm_version_split[0].to_int()
uhm_minor_version = uhm_version_split[1].to_int()
uhm_micro_version = uhm_version_split[2].to_int()

uhm_api_version = '1.0'

uhm_soversion = 1

# Before making a release, uhm_lib_version should be modified.
# The string is of the form X.Y.Z
# - If the interface is the same as the previous version, change to X.Y.Z+1
# - If interfaces have been changed or added, but binary compatibility has
#   been preserved, change to X.Y+1.0
# - If binary compatibility has been broken (eg removed or changed interfaces)
#   change to X+1.0.0
uhm_lib_version = '@0@.2.3'.format(uhm_soversion)

uhm_version_h = configure_file(
  input: 'libuhttpmock/uhm-version.h.in',
  output: '@BASENAME@',
  configuration: {
    'UHM_VERSION_MAJOR': uhm_major_version,
    'UHM_VERSION_MINOR': uhm_minor_version,
    'UHM_VERSION_MICRO': uhm_micro_version,
  },
)

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.38')
gio_dep = dependency('gio-2.0', version: '>= 2.38')
soup_dep = dependency('libsoup-3.0', version: '>= 3.1.2')

subdir('libuhttpmock')
