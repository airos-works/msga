# The air/OS (Haiku) backend: the Be API (src/haiku/haiku_internal.h).
target_sources(plat PRIVATE
    src/haiku/haiku_app.cpp
    src/haiku/haiku_window.cpp)
target_compile_definitions(plat PUBLIC PLAT_HAS_HAIKU)
# be: the Application, Interface and Storage Kits; bnetapi: the network
# roster (the locale roster is in libbe); network: sockets; tracker: BFilePanel.
target_link_libraries(plat PUBLIC be bnetapi network tracker)
# The Be API dynamic_casts the handlers, views and windows it is given:
# classes derived from its own need their type info, which plat's
# -fno-rtti (PLAT_NO_EXCEPTIONS) would leave out.
# common.cpp holds plat::App's key function, so its type info too (the
# backend's App classes derive from it).
set_source_files_properties(src/haiku/haiku_app.cpp src/haiku/haiku_window.cpp
    src/core/common.cpp PROPERTIES COMPILE_OPTIONS "-frtti")

# The tray icon: a Deskbar add-on (deskbar_tray.cpp), loaded into Deskbar's
# own team, so a library of its own linking libbe alone. The package installs
# it as add-ons/MSGA/MSGA Deskbar item; a build's binary finds it beside
# itself as msga_deskbar.so.
add_library(msga_deskbar MODULE src/haiku/deskbar_tray.cpp)
target_include_directories(msga_deskbar PRIVATE src)
target_link_libraries(msga_deskbar PRIVATE be)
target_compile_options(msga_deskbar PRIVATE -frtti -Wall -Wextra -Wno-unused-parameter)
set_target_properties(msga_deskbar PROPERTIES PREFIX "" OUTPUT_NAME "msga_deskbar" SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
