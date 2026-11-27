# New source files are picked up without a reconfigure. ESP-IDF also reads this file in script
# mode to collect requirements, where CONFIGURE_DEPENDS is refused.
if(CMAKE_SCRIPT_MODE_FILE)
    set(GLOB_OPTIONS "")
else()
    set(GLOB_OPTIONS CONFIGURE_DEPENDS)
endif()

file(GLOB_RECURSE SOURCES ${GLOB_OPTIONS} ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-missing-field-initializers
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>
)
