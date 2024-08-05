uhm_test_cflags = [
  '-DTEST_FILE_DIR="@0@/"'.format(meson.current_source_dir()),
  '-DG_LOG_DOMAIN="libuhttpmock-tests"',
  '-DLIBUHTTPMOCK_DISABLE_DEPRECATED',
]

uhm_tests = [
  'server',
  'resolver',
]

foreach _test : uhm_tests
  test_bin = executable(_test,
    sources: _test + '.c',
    c_args: uhm_test_cflags,
    dependencies: libuhm_internal_dep,
  )

  test(_test, test_bin)
endforeach
