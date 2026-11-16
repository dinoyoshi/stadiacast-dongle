# stadia_port.h on Linux and other POSIX hosts (pthreads, zlib), for firmware/host.
add_library(port_posix STATIC port_posix.c)
target_link_libraries(port_posix PUBLIC port_api pthread z)
