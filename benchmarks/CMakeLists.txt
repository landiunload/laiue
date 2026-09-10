# Ручной бенчмарк: в ALL не входит и в CTest не регистрируется — его
# запускают осознанно и читают глазами, а не сравнивают автоматически.
if(TARGET laiue_mesher AND TARGET laiue_audio)
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
endif()

if(TARGET laiue_mesher)
    add_executable(laiue_mesher_benchmark EXCLUDE_FROM_ALL mesher_benchmark.c)
    target_link_libraries(laiue_mesher_benchmark PRIVATE laiue_mesher laiue_world)
    target_include_directories(laiue_mesher_benchmark PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_mesher_benchmark MesherBenchmarkEntryPoint)
endif()

add_executable(laiue_physics_benchmark EXCLUDE_FROM_ALL physics_benchmark.c)
target_link_libraries(laiue_physics_benchmark PRIVATE laiue_physics)
target_include_directories(laiue_physics_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_physics_benchmark PhysicsBenchmarkEntryPoint)

add_executable(laiue_numeric_physics_benchmark EXCLUDE_FROM_ALL numeric_physics_benchmark.c)
target_link_libraries(laiue_numeric_physics_benchmark PRIVATE laiue_numeric)
target_include_directories(laiue_numeric_physics_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_numeric_physics_benchmark NumericPhysicsBenchmarkEntryPoint)

add_executable(laiue_task_pool_benchmark EXCLUDE_FROM_ALL task_pool_benchmark.c)
target_link_libraries(laiue_task_pool_benchmark PRIVATE laiue::task)
target_include_directories(laiue_task_pool_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_task_pool_benchmark TaskPoolBenchmarkEntryPoint)
