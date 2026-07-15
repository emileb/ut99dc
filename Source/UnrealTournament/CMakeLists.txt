project(UnrealTournament CXX)

set(SRC_FILES "Src/Launch.cpp")

if(TARGET_IS_DREAMCAST AND DREAMCAST_NO_WHOLE_ARCHIVE)
  list(APPEND SRC_FILES "Src/ForceLink.cpp")
endif()

if(ANDROID)
  # The app loads the game as a shared library and calls main() via the JNI glue.
  add_library(${PROJECT_NAME} SHARED ${SRC_FILES})
else()
  add_executable(${PROJECT_NAME} ${SRC_FILES})
endif()

target_include_directories(${PROJECT_NAME}
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/Inc
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

target_compile_definitions(${PROJECT_NAME} PRIVATE UNREAL_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})

# Select the appropriate viewport / window driver
if(USE_SDL)
  if(ANDROID)
    # NSDLDrv is pulled in whole-archive by the static link below; naming it
    # here as well makes lld see every symbol twice.
    target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARY})
  else()
    # SDL / NSDLDrv path (POSIX/Windows with SDL2)
    target_link_libraries(${PROJECT_NAME} ${SDL2_LIBRARY} NSDLDrv)
  endif()
  target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIR})
elseif(TARGET_IS_DREAMCAST)
  # Native Dreamcast driver (KOSDrv) – added from the top‑level CMakeLists
  target_link_libraries(${PROJECT_NAME} KOSDrv)
else()
  # Non‑Windows, non‑Dreamcast platforms currently only support the SDL path
  message(FATAL_ERROR "Platforms other than Windows and Dreamcast can only use NSDLDrv.")
endif()


if(BUILD_STATIC)
  set(LINK_PACKAGES ${INSTALL_TARGETS})
  list(REMOVE_ITEM LINK_PACKAGES ${PROJECT_NAME})
  if(BUILD_DCUTIL)
    list(REMOVE_ITEM LINK_PACKAGES DCUtil)
  endif()
  if(TARGET_IS_DREAMCAST AND DREAMCAST_NO_WHOLE_ARCHIVE)
    target_link_libraries(${PROJECT_NAME} ${LINK_PACKAGES})
  else()
    target_link_libraries(${PROJECT_NAME} -Wl,--whole-archive ${LINK_PACKAGES} -Wl,--no-whole-archive)
  endif()
else()
  target_link_libraries(${PROJECT_NAME} Engine Core)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE UNREAL_EXPORTS UPACKAGE_NAME=${PROJECT_NAME})

if(TARGET_IS_DREAMCAST AND DREAMCAST_USE_FATFS)
  target_link_libraries(${PROJECT_NAME} fatfs)
endif()

if(ANDROID)
  # Package as libut99.so (the parent scope clears the "lib" prefix).
  set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "lib" OUTPUT_NAME "ut99")

  # ENGINE_NAME names the touch-control layout XML files.
  target_compile_definitions(${PROJECT_NAME} PRIVATE ENGINE_NAME="ut99")

  # OpenTouch glue: JNI bridge from Clibs_OpenTouch, plus UT99's own touch
  # interface (mobile/touch_interface_ut99.cpp + touch_interface.h). mobile/ is
  # ahead of ${CLIBS_DIR} on the include path so the shared touch_interface_base
  # / JNI glue resolve the same TouchInterface/touch_interface.h this engine uses.
  target_include_directories(${PROJECT_NAME} PRIVATE
    ${SDL2_IMAGE_ANDROID_INCLUDE_DIR}
    ${UT99_SOURCE_DIR}/../mobile
    ${CLIBS_DIR}
    ${TOUCH_CONTROLS_DIR}
  )
  target_sources(${PROJECT_NAME} PRIVATE
    "${UT99_SOURCE_DIR}/../mobile/game_interface.cpp"
    "${UT99_SOURCE_DIR}/../mobile/touch_interface_ut99.cpp"
    "${CLIBS_DIR}/sigma/android_jni.cpp"
    "${CLIBS_DIR}/touch_interface_base.cpp"
    "${CLIBS_DIR}/LogWritter.c"
    "${CLIBS_DIR}/fts.c"
    "${CLIBS_DIR}/rand.c"
  )
  target_link_libraries(${PROJECT_NAME} SDL2_image touchcontrols saffal log)
endif()
