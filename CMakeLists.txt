project (GJSCoverage)

include (FindPkgConfig)

pkg_check_modules (GJS gjs-1.0 glib-2.0 gio-2.0)
find_library (MOZJS_LIBRARY mozjs-17.0)

if (GJS_FOUND AND MOZJS_LIBRARY)

  include_directories (${GJS_INCLUDE_DIRS})
  link_directories (${GJS_LIBRARY_DIRS})

  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

  add_executable (gjs-coverage
                  gjs-debug-connection.cpp
                  gjs-debug-connection.h
                  gjs-debug-interrupt-register.cpp
                  gjs-debug-interrupt-register.h
                  gjs-interrupt-register.cpp
                  gjs-interrupt-register.h
                  gjs-debug-executable-linesutil.cpp
                  gjs-debug-executable-linesutil.h
                  gjs-coverage.c)

  target_link_libraries (gjs-coverage ${GJS_LIBRARIES} ${MOZJS_LIBRARY})

endif (GJS_FOUND AND MOZJS_LIBRARY)

