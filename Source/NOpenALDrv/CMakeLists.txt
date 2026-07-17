project(NOpenALDrv C CXX)

set(SRC_FILES "NOpenALDrv.cpp")

add_library(${PROJECT_NAME} ${LIB_TYPE} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
  ${OPENAL_INCLUDE_DIR}
  ${LIBXMP_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} ${OPENAL_LIBRARY} ${LIBXMP_LIBRARY})

target_link_libraries(${PROJECT_NAME} Engine Core)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOPENALDRV_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})
