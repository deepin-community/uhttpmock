libuhm_sources = files(
  'uhm-resolver.c',
  'uhm-server.c',
  'uhm-message.c'
)

libuhm_source_headers = files(
  'uhm-resolver.h',
  'uhm-server.h',
  'uhm-message.h'
)

libuhm_headers = [
  libuhm_source_headers,
  uhm_version_h,
  files('uhm.h'),
]

install_headers(libuhm_headers,
  subdir: 'libuhttpmock-@0@'.format(uhm_api_version) / 'uhttpmock',
)

libuhm_public_deps = [
  glib_dep,
  gio_dep,
  soup_dep,
]

libuhm_private_deps = [
]

libuhm = library('uhttpmock-@0@'.format(uhm_api_version),
  libuhm_sources,
  dependencies: libuhm_public_deps + libuhm_private_deps,
  c_args: [
    '-DG_LOG_DOMAIN="libuhttpmock"',
  ],
  link_args: cc.get_supported_link_arguments(
    '-Wl,--version-script,@0@'.format(meson.current_source_dir() / 'libuhttpmock.map'),
  ),
  soversion: uhm_soversion,
  version: uhm_lib_version,
  install: true,
)

libuhm_internal_dep = declare_dependency(
  link_with: libuhm,
  dependencies: libuhm_public_deps,
  include_directories: include_directories('.'),
)

# pkg-config
pkgconfig.generate(libuhm,
  name: 'libuhttpmock',
  filebase: 'libuhttpmock-@0@'.format(uhm_api_version),
  description: 'HTTP web service mocking library',
  subdirs: 'libuhttpmock-@0@'.format(uhm_api_version),
  requires: libuhm_public_deps,
)

# Introspection
if get_option('introspection')
  libuhm_gir = gnome.generate_gir(libuhm,
    sources: [ libuhm_sources, libuhm_source_headers, uhm_version_h, ],
    namespace: 'Uhm',
    nsversion: uhm_api_version,
    includes: [ 'GObject-2.0', 'Soup-3.0' ],
    header: 'uhttpmock/uhm.h',
    export_packages: 'libuhttpmock',
    install: true,
  )

  vapigen = find_program('vapigen', required: get_option('vapi'))

  if vapigen.found()
    libuhm_vapi = gnome.generate_vapi('libuhttpmock-@0@'.format(uhm_api_version),
      sources: libuhm_gir[0],
      metadata_dirs: meson.current_source_dir(),
      packages: [ 'gio-2.0', 'libsoup-3.0', 'libxml-2.0' ],
      install: true,
    )
  endif
endif

subdir('tests')

if get_option('gtk_doc')
  subdir('docs')
endif
