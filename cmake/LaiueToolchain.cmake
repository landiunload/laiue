include_guard(GLOBAL)

option(LAIUE_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" ON)
option(LAIUE_ENABLE_LTO "Включить оптимизацию времени линковки в Release" ON)

if(NOT WIN32)
    message(FATAL_ERROR "laiue собирается только под Windows")
endif()
if(NOT MSVC)
    message(FATAL_ERROR
        "Поддерживаются cl.exe и clang-cl.exe с интерфейсом MSVC")
endif()

get_property(LAIUE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(LAIUE_IS_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:Embedded>")
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/Ob2" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_C_STANDARD_LIBRARIES "")

add_library(laiue_compile_settings INTERFACE)
target_include_directories(laiue_compile_settings
    INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(laiue_compile_settings INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    # Версия игры доступна каждому модулю: подпись в меню и проверка
    # совместимости модов (game = MAJOR.MINOR в манифесте).
    "LAIUE_VERSION_TEXT=L\"${PROJECT_VERSION}\""
    LAIUE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    LAIUE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
)

target_compile_options(laiue_compile_settings INTERFACE
    /W4 /utf-8 /GS- /fp:fast
    $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
    $<$<CONFIG:Debug>:/Od>
    $<$<NOT:$<CONFIG:Debug>>:/O2 /Ot /Oi /Gw>
    $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/Ob3>
    $<$<C_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>
)

# --- Точная плавающая арифметика для отдельных файлов ----------------------
#
# Глобально включён /fp:fast (на clang-cl это -ffast-math), и отдельным файлам
# он противопоказан. Случаев ровно два, оба описаны ниже. Знание о том, какими
# флагами это включается, лежит здесь одним куском: раньше оно было скопировано
# в четыре CMakeLists и успело разъехаться с действительностью.
#
# clang-cl не принимает флаги в стиле GCC напрямую, их нужно передавать через
# /clang:. Без этого префикса -ffp-contract=off молча игнорировался — запрет
# слияния в FMA не действовал вообще, хотя комментарии рядом утверждали
# обратное. На нынешних настройках это ничего не меняло (без /arch:AVX2 у цели
# нет FMA, ассемблер побайтово совпадает), но защита была мёртвой: при
# включении AVX2 в коде появляется vfmadd — проверено сравнением ассемблера.
#
# Подавления предупреждений точечные и относятся к самой сути этих опций:
# clang 22 считает перекрытие глобального /fp:fast подозрительным, а здесь оно
# и есть цель. С /WX без них сборка под clang-cl падает.

# Точная модель FP для кода, который проверяет недоверенный ввод: NaN и
# бесконечности обязаны вести себя по стандарту, иначе проверки вырождаются.
set(LAIUE_PRECISE_FP_OPTIONS
    "/fp:precise"
    "$<$<C_COMPILER_ID:Clang>:-Wno-overriding-complex-range>")

# То же плюс запрет слияния a*b+c в FMA — для кода, обязанного давать
# побитово одинаковый результат на разных машинах и компиляторах.
# У MSVC /fp:precise контракции не делает, поэтому флаг нужен только clang.
set(LAIUE_DETERMINISTIC_FP_OPTIONS
    ${LAIUE_PRECISE_FP_OPTIONS}
    "$<$<C_COMPILER_ID:Clang>:/clang:-ffp-contract=off>"
    "$<$<C_COMPILER_ID:Clang>:-Wno-overriding-option>")

add_library(laiue_common INTERFACE)
target_link_libraries(laiue_common INTERFACE laiue_compile_settings)

if(LAIUE_ENABLE_LTO)
    target_compile_options(laiue_common INTERFACE
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/GL>
        $<$<AND:$<C_COMPILER_ID:Clang>,$<NOT:$<CONFIG:Debug>>>:-flto=thin>
    )
endif()

target_link_options(laiue_common INTERFACE
    /NODEFAULTLIB
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT
    /MANIFEST:NO
    /MERGE:.rdata=.text /MERGE:.pdata=.text
    $<$<C_COMPILER_ID:MSVC>:/EMITTOOLVERSIONINFO:NO>
    $<$<CONFIG:Debug>:/DEBUG /INCREMENTAL:NO>
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
)

if(LAIUE_ENABLE_LTO)
    target_link_options(laiue_common INTERFACE
        $<$<AND:$<C_COMPILER_ID:MSVC>,$<NOT:$<CONFIG:Debug>>>:/LTCG>
    )
endif()

add_library(laiue_runtime OBJECT "${PROJECT_SOURCE_DIR}/src/runtime/memory.c")
target_compile_options(laiue_runtime PRIVATE
    /W4 /utf-8 /GS- /fp:fast
    $<$<BOOL:${LAIUE_WARNINGS_AS_ERRORS}>:/WX>
    $<$<CONFIG:Debug>:/Od>
    $<$<NOT:$<CONFIG:Debug>>:/O2 /Oi>
)
