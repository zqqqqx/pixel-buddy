FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources} INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math -fno-math-errno)
target_compile_definitions(${COMPONENT_LIB} PRIVATE EYES_SWAP_BYTES=1)
