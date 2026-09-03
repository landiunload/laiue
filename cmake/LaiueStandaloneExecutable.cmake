include_guard(GLOBAL)

# Тесты и бенчмарки — обычные исполняемые файлы движка, а не приложения:
# на Windows они собираются с /NODEFAULTLIB, поэтому им нужен явный
# entry point и консольная подсистема. Правило вынесено сюда, чтобы
# бенчмарки не зависели от каталога тестов ради одной функции.
function(laiue_configure_standalone_executable target_name windows_entry)
    target_link_libraries(${target_name} PRIVATE
        laiue_common
        laiue::platform_support)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE kernel32)
        target_link_options(${target_name} PRIVATE
            "/ENTRY:${windows_entry}"
            /SUBSYSTEM:CONSOLE)
    endif()
endfunction()
