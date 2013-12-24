project (GJSCoverage)

include (FindPkgConfig)

pkg_check_modules (GJS gjs-1.0 glib-2.0 gio-2.0)
find_library (MOZJS_LIBRARY mozjs-17.0)

if (GJS_FOUND AND MOZJS_LIBRARY)

  include_directories (${GJS_INCLUDE_DIRS})
  link_directories (${GJS_LIBRARY_DIRS})

  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
  
  add_library (gjsdebugger STATIC
               gjs-debug-connection.cpp
               gjs-debug-connection.h
               gjs-debug-coverage.cpp
               gjs-debug-coverage.h
               gjs-debug-interrupt-register.cpp
               gjs-debug-interrupt-register.h
               gjs-interrupt-register.cpp
               gjs-interrupt-register.h
               gjs-debug-executable-linesutil.cpp
               gjs-debug-executable-linesutil.h)
 
  target_link_libraries (gjsdebugger
                         ${GJS_LIBRARIES} ${MOZJS_LIBRARY})

  add_executable (gjs-coverage
                  gjs-coverage.c)

  target_link_libraries (gjs-coverage
                         gjsdebugger)
  
  add_executable (test-gjs-debug-connection
                  test-gjs-debug-connection.c)

  target_link_libraries (test-gjs-debug-connection
                         gjsdebugger)

  add_executable (test-gjs-debug-register
                  test-gjs-debug-register.c)

  target_link_libraries (test-gjs-debug-register
                         gjsdebugger)

  add_executable (test-gjs-debug-coverage
                  test-gjs-debug-coverage.c)

  target_link_libraries (test-gjs-debug-coverage
                         gjsdebugger)

endif (GJS_FOUND AND MOZJS_LIBRARY)

