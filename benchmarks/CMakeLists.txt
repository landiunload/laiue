# Ручной бенчмарк: в ALL не входит и в CTest не регистрируется — его
# запускают осознанно и читают глазами, а не сравнивают автоматически.
add_executable(laiue_benchmark EXCLUDE_FROM_ALL engine_benchmark.c)
target_link_libraries(laiue_benchmark PRIVATE
    laiue_audio
    laiue_mesher
    laiue_world)
# Харнесс без CRT лежит рядом с тестами: у бенчмарка та же задача —
# писать в консоль и завершаться без библиотеки C.
target_include_directories(laiue_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_benchmark EngineBenchmarkEntryPoint)
