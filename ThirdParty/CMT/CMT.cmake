set(THIRD_PARTY_LIBS ${THIRD_PARTY_LIBS} cmt)
set(THIRD_PARTY_TRACKERS ${THIRD_PARTY_TRACKERS} CMT/src/CMT)
set(THIRD_PARTY_INCLUDE_DIRS ${THIRD_PARTY_INCLUDE_DIRS} CMT/include)
# target_link_libraries(mtf cmt)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR})
