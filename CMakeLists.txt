project (GJSCoverage)

include (FindPkgConfig)

pkg_check_modules (GJS gjs-1.0 glib-2.0 gio-2.0)
find_library (MOZJS_LIBRARY mozjs-17.0)

if (GJS_FOUND AND MOZJS_LIBRARY)

  include_directories (${GJS_INCLUDE_DIRS})
  link_directories (${GJS_LIBRARY_DIRS})

  add_executable (gjs-coverage
                  gjs-coverage.c)

  target_link_libraries (gjs-coverage ${GJS_LIBRARIES} ${MOZJS_LIBRARY})

endif (GJS_FOUND AND MOZJS_LIBRARY)

