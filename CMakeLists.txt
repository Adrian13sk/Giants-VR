cmake_minimum_required(VERSION 3.22)
project(GiantsVR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(GVR_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ------------------------------------------------------------------------------------------
# Portable core: renderer, asset formats, scene, gameplay. No platform or VR dependencies.
# ------------------------------------------------------------------------------------------
add_library(gvr_core STATIC
  src/core/prof.cpp
  src/render/renderer.cpp
  src/assets/gzp.cpp
  src/assets/gbs.cpp
  src/assets/image.cpp
  src/scene/model_cache.cpp
  src/scene/asset_world.cpp
  src/scene/test_scene.cpp
  src/scene/terrain.cpp
  src/assets/gti.cpp
  src/assets/world_bin.cpp
  src/assets/skin_anim.cpp
  src/assets/object_defs.cpp
  src/assets/mission.cpp
  src/assets/flick.cpp
  src/assets/gtext.cpp
  src/assets/weapondefs.cpp
  src/assets/fx_defs.cpp
  src/render/text.cpp
  src/render/quad_batch.cpp
  src/game/cutscene.cpp
  src/scene/skinned_model.cpp
  src/scene/collision.cpp
  src/game/vr_rig.cpp
  src/game/player.cpp
  src/game/arm_ik.cpp
  src/game/weapon.cpp
  src/game/enemy.cpp
  src/game/creature.cpp
  src/game/smartie.cpp
  src/game/gore.cpp
  src/game/game_app_world.cpp
  src/game/game_app_hud.cpp
  src/game/game_app_village.cpp
  src/game/villager.cpp
  src/game/ragdoll.cpp
  src/game/game_app_ragdoll.cpp
  src/game/game_app_bodygrab.cpp
  src/game/game_app_stones.cpp
  src/game/game_app_hints.cpp
  src/game/game_app_marks.cpp
  src/game/game_app_casings.cpp
  src/game/game_app_birds.cpp
  src/game/game_app_hitchers.cpp
  src/game/game_app_fish.cpp
  src/game/game_app_seamonster.cpp
  src/game/game_app_ambience.cpp
  src/game/game_app_cinema.cpp
  src/game/vimp.cpp
  src/game/jetski.cpp
  src/game/game_app_bush.cpp
  src/game/game_app_flare.cpp
  src/game/game_app_jet.cpp
  src/game/game_app_kbpowers.cpp
  src/game/game_app_kbthrow.cpp
    src/game/test_script.cpp
  src/game/game_app_jetski.cpp
  src/game/game_app_map.cpp
  src/game/game_app_ripper.cpp
  src/game/game_app_shop.cpp
  src/game/game_app_base.cpp
  src/game/game_app_basecrew.cpp
  src/game/game_app_popup.cpp
  src/game/game_app_rpweapons.cpp
  src/game/game_app_arsenal.cpp
  src/game/game_app_grenade.cpp
  src/game/game_app_mine.cpp
  src/game/game_app_bow.cpp
  src/game/fx_player.cpp
  src/game/skirmish.cpp
  src/game/sk_net.cpp
  src/net/udp.cpp
  src/game/game_app_skirmish.cpp
  src/game/game_app_datafx.cpp
  src/game/game_app_spells.cpp
  src/game/game_app_tags.cpp
  src/game/game_app_water.cpp
  src/game/game_app_vimp.cpp
  src/game/save_game.cpp
  src/game/settings.cpp
  src/game/game_app_menu.cpp
  src/game/game_app_feel.cpp
  src/game/game_app_vr.cpp
    src/game/game_app_grass.cpp
    src/game/game_app_kbgrab.cpp
    src/game/game_app_parry.cpp
    src/game/game_app_reach.cpp
    src/game/game_app_climb.cpp
    src/game/game_app_spawners.cpp
    src/game/game_app_kbcarry.cpp
  src/game/game_app_delphi.cpp
  src/game/game_app_kabuto.cpp
  src/game/game_app_title.cpp
  src/game/hud_gauge.cpp
  src/game/destruction.cpp
  src/game/particles.cpp
  src/audio/mixer.cpp
  src/audio/ambience.cpp
  src/audio/music.cpp
  src/game/game_app.cpp
)
target_include_directories(gvr_core PUBLIC src third_party)
target_compile_options(gvr_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

if(ANDROID)
  # ----------------------------------------------------------------------------------------
  # Quest runtime: NativeActivity shared library.
  # ----------------------------------------------------------------------------------------
  set(OPENXR_LOADER_SO "" CACHE FILEPATH "Path to arm64-v8a libopenxr_loader.so (Khronos AAR)")
  if(NOT EXISTS "${OPENXR_LOADER_SO}")
    message(FATAL_ERROR "OPENXR_LOADER_SO not found: ${OPENXR_LOADER_SO}")
  endif()
  add_library(openxr_loader SHARED IMPORTED)
  set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION "${OPENXR_LOADER_SO}")

  add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

  add_library(giantsvr SHARED
    src/platform/android/android_main.cpp
    src/platform/android/autotest.cpp
    src/platform/android/audio_aaudio.cpp
    src/xr/xr_app.cpp
    src/xr/xr_input.cpp
  )
  target_include_directories(giantsvr PRIVATE src third_party third_party/openxr/include)
  target_compile_options(giantsvr PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  # ANativeActivity_onCreate must stay exported from the glue library.
  set_target_properties(giantsvr PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384")
  target_link_libraries(giantsvr PRIVATE gvr_core native_app_glue openxr_loader
                        android log EGL GLESv3 aaudio)
else()
  # ----------------------------------------------------------------------------------------
  # Host tools: headless renderer (Mesa EGL surfaceless), asset tool, unit tests.
  # ----------------------------------------------------------------------------------------
  target_include_directories(gvr_core PUBLIC third_party/khronos)
  target_compile_definitions(gvr_core PUBLIC EGL_NO_X11)
  find_library(HOST_EGL NAMES EGL libEGL.so.1 PATHS /usr/lib/x86_64-linux-gnu REQUIRED)
  find_library(HOST_GLES NAMES GLESv2 libGLESv2.so.2 PATHS /usr/lib/x86_64-linux-gnu REQUIRED)

  add_executable(headless_render tools/headless/headless_main.cpp)
  target_link_libraries(headless_render PRIVATE gvr_core ${HOST_EGL} ${HOST_GLES})

  add_executable(charsheet tools/headless/charsheet.cpp)
  target_link_libraries(charsheet PRIVATE gvr_core ${HOST_EGL} ${HOST_GLES})
  add_executable(modelsheet tools/headless/modelsheet.cpp)
  target_link_libraries(modelsheet PRIVATE gvr_core ${HOST_EGL} ${HOST_GLES})

  add_executable(gzptool tools/gzptool/gzptool.cpp)
  target_link_libraries(gzptool PRIVATE gvr_core ${HOST_EGL} ${HOST_GLES})

  add_executable(gvr_tests tests/test_main.cpp)
  target_link_libraries(gvr_tests PRIVATE gvr_core ${HOST_EGL} ${HOST_GLES})
  enable_testing()
  add_test(NAME unit COMMAND gvr_tests)
endif()
