# Фаззеры разборщиков внешних данных: картинок, звука, моделей, zlib и
# манифестов.
# Собираются только с LAIUE_BUILD_FUZZERS (Clang): тогда весь код движка
# уже инструментирован -fsanitize=fuzzer-no-link и ASan/UBSan, а здесь
# лишь компонуется драйвер libFuzzer. Production-исходники сюда не
# копируются, каждый остаётся в своём модуле.
#
# CTest гоняет каждый фаззер короткой серией прогонов поверх семян:
# найденные однажды падения остаются в corpus/ регрессией. Новые входы
# libFuzzer пишет в первый каталог — он в дереве сборки, исходники при
# прогоне не меняются. Долгий поиск запускается вручную, например:
#   build/linux-clang-fuzz/bin/Debug/laiue_fuzz_image -max_total_time=600 \
#       fuzz-corpus/image tests/fuzz/corpus/image

set(LAIUE_FUZZ_RUNS 4000 CACHE STRING "Прогонов на фаззер в CTest")

# Общая цель собирает все фаззеры: CI строит её, а не перечисляет их
# поимённо, поэтому новый фаззер не останется без сборки и с тестом
# «Not Run».
add_custom_target(laiue_fuzzers)

function(laiue_add_fuzzer name source)
    add_executable(laiue_fuzz_${name} ${source})
    add_dependencies(laiue_fuzzers laiue_fuzz_${name})
    target_link_libraries(laiue_fuzz_${name} PRIVATE laiue_common ${ARGN})
    target_compile_options(laiue_fuzz_${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(laiue_fuzz_${name} PRIVATE -fsanitize=fuzzer)
    if(BUILD_TESTING)
        set(work_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/${name}")
        file(MAKE_DIRECTORY "${work_corpus}")
        add_test(NAME laiue.fuzz.${name}
            COMMAND laiue_fuzz_${name} -runs=${LAIUE_FUZZ_RUNS} -seed=1
                -rss_limit_mb=4096 -timeout=60 "${work_corpus}"
                "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")
        set_tests_properties(laiue.fuzz.${name} PROPERTIES TIMEOUT 600)
    endif()
endfunction()

laiue_add_fuzzer(image image_fuzz.c laiue::media_support)
laiue_add_fuzzer(sound sound_fuzz.c laiue::media_support)
laiue_add_fuzzer(inflate inflate_fuzz.c laiue::media_support)
laiue_add_fuzzer(model model_fuzz.c laiue::media_support)
laiue_add_fuzzer(animation animation_fuzz.c laiue::media_support)
if(TARGET laiue_mod)
    laiue_add_fuzzer(mod_manifest mod_manifest_fuzz.c laiue_mod)
endif()
laiue_add_fuzzer(module_manifest module_manifest_fuzz.c laiue_bootstrap
    laiue::platform_support)
