cmake_minimum_required(VERSION 3.20)
project(laiue
    VERSION 0.1.0
    DESCRIPTION "Belaiu base - window and input"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# === авто-подхват исходников ===
file(GLOB WINDOW_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.h
)

file(GLOB INPUT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/*.h
)

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h
)

file(GLOB LAUNCHER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/launcher/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/launcher/*.h
)

file(GLOB SHARED_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
)

# === общие compile definitions ===
set(LAIUE_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)

# === общие compile options (MSVC) ===
set(LAIUE_MSVC_COMPILE
    /GS- /GR- /fp:fast /W4
    $<$<CONFIG:Debug>:/WX /Od /Z7>
    $<$<NOT:$<CONFIG:Debug>>:/GL /O2 /Ot /Oi /Ob3>
)

# === общие link options (MSVC) ===
set(LAIUE_MSVC_LINK_BASE
    /NODEFAULTLIB /SUBSYSTEM:WINDOWS
    /DYNAMICBASE:NO /HIGHENTROPYVA:NO
    /MANIFEST:NO /EMITTOOLVERSIONINFO:NO
    /MERGE:.rdata=.text /MERGE:.pdata=.text
)

set(LAIUE_MSVC_LINK_DEBUG /DEBUG /INCREMENTAL:NO)
set(LAIUE_MSVC_LINK_RELEASE /LTCG /OPT:REF /OPT:ICF)

# === общие compile options (Clang) ===
set(LAIUE_CLANG_COMPILE
    -Wno-unused-command-line-argument
    /GS- /GR- /fp:fast
    $<$<CONFIG:Debug>:-g -Werror /Od>
    $<$<NOT:$<CONFIG:Debug>>:-flto=thin -O2 /Ot /Oi /Ob3>
)

set(LAIUE_CLANG_LINK_BASE
    /NODEFAULTLIB /SUBSYSTEM:WINDOWS
    /DYNAMICBASE:NO /HIGHENTROPYVA:NO
    /MANIFEST:NO /EMITTOOLVERSIONINFO:NO
    /MERGE:.rdata=.text /MERGE:.pdata=.text
)

set(LAIUE_CLANG_LINK_DEBUG /DEBUG)
set(LAIUE_CLANG_LINK_RELEASE -flto=thin /OPT:REF /OPT:ICF)

# ========== DLL: window ==========
add_library(laiue_window SHARED ${WINDOW_SOURCES} ${SHARED_HEADERS})
target_include_directories(laiue_window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(laiue_window PRIVATE ${LAIUE_DEFINES} LAIUE_BUILD_WINDOW)
target_link_libraries(laiue_window PRIVATE kernel32 user32)

# ========== DLL: input ==========
add_library(laiue_input SHARED ${INPUT_SOURCES} ${SHARED_HEADERS})
target_include_directories(laiue_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(laiue_input PRIVATE ${LAIUE_DEFINES} LAIUE_BUILD_INPUT)
target_link_libraries(laiue_input PRIVATE kernel32 user32)

# ========== DLL: core ==========
add_library(laiue_core SHARED ${CORE_SOURCES} ${SHARED_HEADERS})
target_include_directories(laiue_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(laiue_core PRIVATE ${LAIUE_DEFINES} LAIUE_BUILD_CORE)
target_link_libraries(laiue_core PRIVATE laiue_window laiue_input)

# ========== EXE: launcher ==========
add_executable(laiue WIN32 ${LAUNCHER_SOURCES} ${SHARED_HEADERS})
target_include_directories(laiue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(laiue PRIVATE ${LAIUE_DEFINES})
target_link_libraries(laiue PRIVATE kernel32)

# ========== флаги MSVC ==========
macro(apply_msvc target)
    target_compile_options(${target} PRIVATE ${LAIUE_MSVC_COMPILE})
    target_link_options(${target} PRIVATE
        ${LAIUE_MSVC_LINK_BASE}
        $<$<CONFIG:Debug>:${LAIUE_MSVC_LINK_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${LAIUE_MSVC_LINK_RELEASE}>
    )
endmacro()

if(MSVC)
    target_compile_options(laiue PRIVATE ${LAIUE_MSVC_COMPILE})
    target_link_options(laiue PRIVATE
        ${LAIUE_MSVC_LINK_BASE}
        /ENTRY:mainCRTStartup
        $<$<CONFIG:Debug>:${LAIUE_MSVC_LINK_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${LAIUE_MSVC_LINK_RELEASE}>
    )
    apply_msvc(laiue_window)
    apply_msvc(laiue_input)
    apply_msvc(laiue_core)
endif()

# ========== флаги Clang ==========
macro(apply_clang target)
    target_compile_options(${target} PRIVATE ${LAIUE_CLANG_COMPILE})
    target_link_options(${target} PRIVATE
        ${LAIUE_CLANG_LINK_BASE}
        $<$<CONFIG:Debug>:${LAIUE_CLANG_LINK_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${LAIUE_CLANG_LINK_RELEASE}>
    )
endmacro()

if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND WIN32)
    target_compile_options(laiue PRIVATE ${LAIUE_CLANG_COMPILE})
    target_link_options(laiue PRIVATE
        ${LAIUE_CLANG_LINK_BASE}
        /ENTRY:mainCRTStartup
        $<$<CONFIG:Debug>:${LAIUE_CLANG_LINK_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${LAIUE_CLANG_LINK_RELEASE}>
    )
    apply_clang(laiue_window)
    apply_clang(laiue_input)
    apply_clang(laiue_core)
endif()

# ========== entry points (DLLs) ==========
foreach(tgt laiue_window laiue_input laiue_core)
    target_link_options(${tgt} PRIVATE
        /ENTRY:DllMain
        /STACK:262144,4096
        /HEAP:65536,4096
    )
endforeach()
