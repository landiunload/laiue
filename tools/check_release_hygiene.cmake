# Release не несёт ничего отладочного: ни тестов, ни бенчмарков, ни
# фаззеров, ни диагностических модулей, ни отладочной информации.
#
# Скрипт ставит SDK конфигурации Release во временный каталог и проверяет
# каждый файл, а затем смотрит в каталог релизных бинарников сборки: туда
# диагностические модули тоже не должны попадать.
#
#   cmake -DENGINE_BINARY_DIR=<build> -DRELEASE_RUNTIME_DIR=<bin/Release>
#         -DTEST_ROOT=<tmp> -P tools/check_release_hygiene.cmake

foreach(variable IN ITEMS ENGINE_BINARY_DIR RELEASE_RUNTIME_DIR TEST_ROOT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

# Имена, которым не место в релизе. Диагностические модули перечислены по
# имени: их роль DIAGNOSTICS в сборке.
set(forbidden_patterns
    "_test[^/]*$"
    "_benchmark[^/]*$"
    "_fuzz[^/]*$"
    "frame_monitor[^/]*\\.(dll|so|dylib|lib|a)$"
    "graphics_profile[^/]*\\.(dll|so|dylib|lib|a)$"
    "\\.(pdb|ilk|exp)$"
    "(^|/)tests?/")

file(REMOVE_RECURSE "${TEST_ROOT}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${ENGINE_BINARY_DIR}" --config Release
            --prefix "${TEST_ROOT}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_output)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Release install failed:\n${install_output}")
endif()

set(violations)
file(GLOB_RECURSE installed RELATIVE "${TEST_ROOT}" "${TEST_ROOT}/*")
list(LENGTH installed installed_count)
if(installed_count EQUAL 0)
    message(FATAL_ERROR "Release install produced no files")
endif()
foreach(path IN LISTS installed)
    # Документация и шаблоны SDK описывают тесты словами, а не содержат их.
    if(path MATCHES "^share/laiue/(docs|sdk)/")
        continue()
    endif()
    foreach(pattern IN LISTS forbidden_patterns)
        if(path MATCHES "${pattern}")
            list(APPEND violations "installed: ${path}")
        endif()
    endforeach()
endforeach()

if(EXISTS "${RELEASE_RUNTIME_DIR}")
    file(GLOB built RELATIVE "${RELEASE_RUNTIME_DIR}" "${RELEASE_RUNTIME_DIR}/*")
    foreach(path IN LISTS built)
        if(path MATCHES "(frame_monitor|graphics_profile)[^/]*\\.(dll|so|dylib)$")
            list(APPEND violations "built in Release: ${path}")
        endif()
    endforeach()
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
if(violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR "Release contains debug-only files:\n  ${formatted}")
endif()
message(STATUS "Release hygiene: OK (${installed_count} installed files)")
