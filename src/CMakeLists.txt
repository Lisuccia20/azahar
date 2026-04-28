# Enable modules to include each other's files
include_directories(.)

include(GenerateSettingKeys)

# --- INIZIO CONFIGURAZIONE GSTREAMER ---
# Cerca GStreamer e i moduli necessari per lo streaming e WebRTC
find_package(PkgConfig REQUIRED)

if (APPLE)
    # Definiamo i percorsi dove Homebrew installa i file .pc su M1/M2/M3
    set(HB_GST_PATH "/opt/homebrew/opt/gstreamer/lib/pkgconfig")
    set(HB_BASE_PATH "/opt/homebrew/lib/pkgconfig")
    set(HB_LIBFFI_PATH "/opt/homebrew/opt/libffi/lib/pkgconfig")
    set(HB_ZLIB_PATH "/opt/homebrew/opt/zlib/lib/pkgconfig")

    # Aggiorniamo la variabile d'ambiente per questa sessione di CMake
    set(ENV{PKG_CONFIG_PATH} "${HB_GST_PATH}:${HB_BASE_PATH}:${HB_LIBFFI_PATH}:${HB_ZLIB_PATH}:$ENV{PKG_CONFIG_PATH}")

    # Debug: stampa il percorso per verifica (lo vedrai nei log di CMake)
    message(STATUS "PKG_CONFIG_PATH impostato a: $ENV{PKG_CONFIG_PATH}")
endif()


pkg_check_modules(GSTREAMER REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-webrtc-1.0
    gstreamer-sdp-1.0
    gstreamer-video-1.0
)

# Aggiunge i percorsi degli header di GStreamer globalmente
include_directories(${GSTREAMER_INCLUDE_DIRS})
# --- FINE CONFIGURAZIONE GSTREAMER ---

# CMake seems to only define _DEBUG on Windows
set_property(DIRECTORY APPEND PROPERTY
    COMPILE_DEFINITIONS $<$<CONFIG:Debug>:_DEBUG> $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

if (ENABLE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Set compilation flags
if (MSVC)
    set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS)
    add_definitions(-DNOMINMAX)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-DUNICODE -D_UNICODE)

    add_compile_options(
        /MP /Zm200 /Zo /permissive- /EHsc /utf-8 /volatile:iso /Zc:externConstexpr
        /Zc:inline /Zc:throwingNew /GT /experimental:deterministic
        /d1trimfile:"${CMAKE_SOURCE_DIR}" /experimental:external /external:anglebrackets /external:W0 /W3
        /we4062 /we4189 /we4265 /we4388 /we4389 /we4456 /we4457 /we4459 /we4505 /we4547
        /we4549 /we4555 /we4826 /we5038 /we5233 /we5245 /wd4100 /wd4324
    )

    if (CITRA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()

    if (CMAKE_C_COMPILER_LAUNCHER STREQUAL "ccache" OR CMAKE_CXX_COMPILER_LAUNCHER STREQUAL "ccache"
            OR CITRA_USE_PRECOMPILED_HEADERS)
        add_compile_options(/Z7)
    else()
        add_compile_options(/Zi)
    endif()

    add_compile_options("$<$<CONFIG:Release>:/GS->")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG   "/DEBUG /MANIFEST:NO" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/DEBUG /MANIFEST:NO /INCREMENTAL:NO /OPT:REF,ICF /PDBALTPATH:%_PDB%" CACHE STRING "" FORCE)
else()
    add_compile_options(
        -Wall
        -Wno-unused-command-line-argument
        -fstack-protector-strong
    )
    if (NOT ENABLE_LIBRETRO)
        add_compile_options(
            -Wp,-D_GLIBCXX_ASSERTIONS
            -fstack-clash-protection
        )
    endif()

    string(FIND "-D_FORTIFY_SOURCE" "${CMAKE_CXX_FLAGS} " FORTIFY_SOURCE_DEFINED)
    if (NOT CMAKE_BUILD_TYPE STREQUAL Debug AND NOT FORTIFY_SOURCE_DEFINED)
        add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2)
    endif()

    if (CITRA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()

    if (APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        add_compile_options("-stdlib=libc++")
    endif()

    if (CMAKE_CXX_COMPILER_ID STREQUAL GNU)
        add_compile_options("-Wno-attributes")
        add_compile_options("-Wno-interference-size")
        add_compile_options("-Wno-psabi")
        add_link_options("-Wno-psabi")
    endif()

    if (MINGW)
        add_definitions(-DMINGW_HAS_SECURE_API)
        if (COMPILE_WITH_DWARF)
            add_compile_options("-gdwarf")
        endif()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
        add_definitions(-D_FILE_OFFSET_BITS=64)
        set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qcTP <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_C_ARCHIVE_APPEND   "<CMAKE_AR> qTP  <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcTP <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qTP  <TARGET> <LINK_FLAGS> <OBJECTS>")
    endif()
endif()

if(ENABLE_SOFTWARE_RENDERER)
    add_compile_definitions(ENABLE_SOFTWARE_RENDERER)
endif()
if(ENABLE_OPENGL)
    add_compile_definitions(ENABLE_OPENGL)
endif()
if(ENABLE_VULKAN)
    add_compile_definitions(ENABLE_VULKAN)
endif()
if(ENABLE_DEVELOPER_OPTIONS)
    add_compile_definitions(ENABLE_DEVELOPER_OPTIONS)
endif()
if(ENABLE_BUILTIN_KEYBLOB)
    add_compile_definitions(ENABLE_BUILTIN_KEYBLOB)
endif()

# Aggiunta delle directory dei sottoprogetti
add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(video_core)
add_subdirectory(audio_core)
add_subdirectory(network)
add_subdirectory(input_common)

target_link_libraries(video_core PUBLIC input_common)

# --- LINK LIBRERIE GSTREAMER ---
# Colleghiamo le librerie di GStreamer ai target che usano lo streamer.
# Di solito citra_core e video_core.
target_link_directories(citra_core PUBLIC ${GSTREAMER_LIBRARY_DIRS})
target_link_libraries(citra_core PUBLIC ${GSTREAMER_LIBRARIES})
target_link_directories(video_core PUBLIC ${GSTREAMER_LIBRARY_DIRS})
target_link_libraries(video_core PUBLIC ${GSTREAMER_LIBRARIES})
# ------------------------------

if (ENABLE_TESTS)
    add_subdirectory(tests)
endif()

if (ENABLE_QT)
    add_subdirectory(citra_qt)
endif()

if (ENABLE_QT)
    add_subdirectory(citra_meta)
endif()

if (ENABLE_LIBRETRO)
    add_subdirectory(citra_libretro)
endif()

if (ENABLE_ROOM)
    add_subdirectory(citra_room)
endif()

if (ENABLE_ROOM_STANDALONE)
    add_subdirectory(citra_room_standalone)
endif()

if (ANDROID AND NOT ENABLE_LIBRETRO)
    add_subdirectory(android/app/src/main/jni)
    target_include_directories(citra-android PRIVATE android/app/src/main)
endif()

if (ENABLE_WEB_SERVICE)
    add_subdirectory(web_service)
endif()
