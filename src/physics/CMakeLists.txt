laiue_add_module(physics
    SOURCES
        voxel_body.c
        voxel_body.h
        ../api.h
)

# Детерминизм симуляции: коллизии и движение обязаны воспроизводиться
# побитово на любом x64 и в обоих компиляторах, иначе по сети пришлось бы
# гонять состояние физики каждый тик. /fp:precise убирает переассоциацию,
# -ffp-contract=off (clang-cl) запрещает слияние a*b+c в FMA (у MSVC
# /fp:precise контракции не делает). Замер подтвердил идентичность бит;
# tests/determinism_test.c стережёт это хешем. Флаги оптимизации не тронуты.
set_source_files_properties(voxel_body.c PROPERTIES COMPILE_OPTIONS
    "/fp:precise;$<$<C_COMPILER_ID:Clang>:-ffp-contract=off>")
