configure_file(
  input: 'version.xml.in',
  output: '@BASENAME@',
  configuration: {
    'UHM_VERSION_MAJOR': uhm_major_version,
    'UHM_VERSION_MINOR': uhm_minor_version,
    'UHM_VERSION_MICRO': uhm_micro_version,
  },
)

gnome.gtkdoc('libuhttpmock',
  module_version: uhm_api_version,
  main_xml: 'libuhttpmock-' + uhm_api_version + '-docs.xml',
  src_dir: include_directories('../'),
  scan_args: [
    '--rebuild-types',
    '--deprecated-guards="LIBUHTTPMOCK_DISABLE_DEPRECATED"',
  ],
  dependencies: libuhm_internal_dep,
)
