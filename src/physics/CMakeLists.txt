laiue_add_module(physics
    SOURCES
        voxel_body.c
        voxel_body.h
        ../api.h
)

# Детерминизм симуляции: коллизии и движение обязаны воспроизводиться
# побитово на любом x64 и в обоих компиляторах, иначе по сети пришлось бы
# гонять состояние физики каждый тик. Набор флагов и причины — в
# cmake/LaiueToolchain.cmake. Страж — tests/determinism_test.c, он стережёт
# результат хешем. Флаги оптимизации не тронуты.
set_source_files_properties(voxel_body.c PROPERTIES COMPILE_OPTIONS
    "${LAIUE_DETERMINISTIC_FP_OPTIONS}")
