# Отчёт о сбое теста (test_runtime.c): каждый намеренный сбой harness
# laiue_crash_report_test печатает код, адрес и цепочку вызовов, а процесс
# всё так же завершается сбоем, который видит CTest.
if(NOT DEFINED HARNESS)
    message(FATAL_ERROR "HARNESS is required")
endif()
get_filename_component(module "${HARNESS}" NAME)
string(REPLACE "." "[.]" module "${module}")
set(frame "  ${module}[+]0x[0-9a-f]+")

function(run_crash kind)
    set(ENV{LAIUE_CRASH_REPORT_TEST} "${kind}")
    execute_process(COMMAND "${HARNESS}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    # Код возврата — число, а сбой CMake описывает словами.
    if(result MATCHES "^-?[0-9]+$")
        message(FATAL_ERROR "${kind}: exited with ${result} instead of crashing\n${output}${errors}")
    endif()
    set(output "${output}" PARENT_SCOPE)
endfunction()

function(expect kind pattern)
    if(NOT output MATCHES "${pattern}")
        message(FATAL_ERROR "${kind}: the crash report does not match\n${pattern}\n${output}")
    endif()
endfunction()

if(WIN32)
    # Цепочка начинается с упавшей функции, а не с самого отчёта.
    run_crash(read)
    expect(read "\nUnhandled exception 0xc0000005 [(]access violation[)] at ${module}[+]0x[0-9a-f]+, reading 0x10\nStack:\n${frame}\n${frame}\n${frame}\n")
    # От нулевого адреса стек не раскрутить; вызвавшего даёт адрес возврата.
    run_crash(call)
    expect(call "\nUnhandled exception 0xc0000005 [(]access violation[)] at 0x0, executing 0x0\nStack:\n  0x0\n${frame}\n")
    # Рекурсия сжата в одну строку.
    run_crash(overflow)
    expect(overflow "\nUnhandled exception 0xc00000fd [(]stack overflow[)] at ${module}[+]0x[0-9a-f]+\nStack:\n${frame}\n${frame} [(]repeated [0-9]+ times[)]\n")
else()
    foreach(kind read call overflow)
        run_crash(${kind})
        if(kind STREQUAL "read")
            expect(${kind} "\nFatal SIG(SEGV|BUS), address 0x10\n")
        elseif(kind STREQUAL "call")
            expect(${kind} "\nFatal SIG(SEGV|BUS), address 0x0\n")
        else()
            expect(${kind} "\nFatal SIG(SEGV|BUS), address 0x[0-9a-f]+\n")
        endif()
        # backtrace есть в glibc и macOS; цепочка проходит через сам harness.
        if(output MATCHES "\nStack:\n" AND NOT output MATCHES "\nStack:\n.*${module}")
            message(FATAL_ERROR "${kind}: the stack does not reach the harness\n${output}")
        endif()
    endforeach()
endif()
