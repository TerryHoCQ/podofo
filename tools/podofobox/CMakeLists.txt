file(GLOB SOURCE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "[^.]*.h" "[^.]*.cpp")
source_group("" FILES ${SOURCE_FILES})
add_executable(podofobox ${SOURCE_FILES})
target_link_libraries(podofobox ${PODOFO_LIBRARIES} tools_private)
install(TARGETS podofobox RUNTIME DESTINATION "bin")
