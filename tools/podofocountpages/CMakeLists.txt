file(GLOB SOURCE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "[^.]*.h" "[^.]*.cpp")
source_group("" FILES ${SOURCE_FILES})
add_executable(podofocountpages ${SOURCE_FILES})
target_link_libraries(podofocountpages ${PODOFO_LIBRARIES} tools_private)
install(TARGETS podofocountpages RUNTIME DESTINATION "bin")