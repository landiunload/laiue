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


# Точечное чтение мира и заполнение регионов: WorldGetBlock в мире без правок,
# с дальними правками и с правками в зоне запроса; WorldFillRegion 66^3 с
# пустым базовым слоем, с правкой вдали, правками в центральном чанке и у
# соседей, сплошным чанком и провайдером. EXCLUDE_FROM_ALL, как и остальные
# бенчмарки: в CTest не регистрируется, запускается руками.
add_executable(laiue_world_benchmark EXCLUDE_FROM_ALL world_read_benchmark.c)
target_link_libraries(laiue_world_benchmark PRIVATE laiue_world)
target_include_directories(laiue_world_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_world_benchmark WorldReadBenchmarkEntryPoint)
if(TARGET laiue_world)
    add_executable(laiue_world_batch_benchmark EXCLUDE_FROM_ALL
        world_batch_benchmark.c)
    target_link_libraries(laiue_world_batch_benchmark PRIVATE
        laiue_world
        laiue_numeric)
    target_include_directories(laiue_world_batch_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_world_batch_benchmark WorldBatchBenchmarkEntryPoint)
endif()

# Горячий путь AABB-контроллера персонажа: обычный шаг, свипы, ground/stable/
# sneak-запросы и динамический broadphase. Отдельно от rigid-body benchmark,
# потому что VoxelBody и rigid-тела не делят ни callbacks, ни данные.
add_executable(laiue_voxel_body_benchmark EXCLUDE_FROM_ALL voxel_body_benchmark.c)
target_link_libraries(laiue_voxel_body_benchmark PRIVATE laiue_physics)
target_include_directories(laiue_voxel_body_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_voxel_body_benchmark VoxelBodyBenchmarkEntryPoint)

# Внутренние SetProxy/RemoveProxy/Query не экспортируются из модуля, поэтому
# broadphase-бенчмарк компилирует свой production-файл прямо в исполняемый
# файл. LAIUE_STATIC убирает dllimport с определений и оставляет символы
# локальными.
add_executable(laiue_broadphase_benchmark EXCLUDE_FROM_ALL
    broadphase_benchmark.c
    "${PROJECT_SOURCE_DIR}/src/simulation/physics/rigid_broadphase.c")
target_include_directories(laiue_broadphase_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(laiue_broadphase_benchmark PRIVATE LAIUE_STATIC=1)
laiue_configure_standalone_executable(
    laiue_broadphase_benchmark BroadphaseBenchmarkEntryPoint)

add_executable(laiue_rigid_solver_profile EXCLUDE_FROM_ALL rigid_solver_profile.c)
target_link_libraries(laiue_rigid_solver_profile PRIVATE laiue_physics)
target_include_directories(laiue_rigid_solver_profile PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_rigid_solver_profile RigidSolverProfileEntryPoint)


# Внутренний child AABB BVH не экспортируется из laiue_physics, поэтому его
# источник компилируется прямо в бенчмарк. Точная FP-модель повторяет
# tests/compound_bvh_test.c: сортировка и центры обязаны считаться так же.
add_executable(laiue_compound_benchmark EXCLUDE_FROM_ALL
    compound_benchmark.c
    ../src/simulation/physics/compound_bvh.c)
target_link_libraries(laiue_compound_benchmark PRIVATE laiue::physics)
target_include_directories(laiue_compound_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
if(LAIUE_PLATFORM_EXTERNAL)
    target_link_libraries(laiue_compound_benchmark PRIVATE "${LAIUE_PRECISE_FP_TARGET}")
elseif(LAIUE_PLATFORM_WINDOWS)
    target_compile_options(laiue_compound_benchmark PRIVATE
        "$<$<C_COMPILER_ID:MSVC>:/fp:strict>"
        "$<$<C_COMPILER_ID:Clang>:/clang:-ffp-model=strict>")
else()
    target_compile_options(laiue_compound_benchmark PRIVATE -fno-fast-math -ffp-contract=off)
endif()
laiue_configure_standalone_executable(
    laiue_compound_benchmark CompoundBenchmarkEntryPoint)

if(TARGET laiue_scene)
    # Ручной benchmark главного потока стриминга чанков: смена центра,
    # вытеснение, постановка заявок и разбор очереди результатов. Мир пуст,
    # рендерер фиктивный, поэтому GPU не нужен.
    add_executable(laiue_streaming_benchmark EXCLUDE_FROM_ALL
        streaming_benchmark.c)
    target_link_libraries(laiue_streaming_benchmark PRIVATE
        laiue_scene
        laiue_world)
    target_include_directories(laiue_streaming_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_streaming_benchmark StreamingBenchmarkEntryPoint)
endif()


if(TARGET laiue_scene AND TARGET laiue_world)
    add_executable(laiue_raycast_benchmark EXCLUDE_FROM_ALL raycast_benchmark.c)
    target_link_libraries(laiue_raycast_benchmark PRIVATE laiue_scene laiue_world)
    target_include_directories(laiue_raycast_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_raycast_benchmark RaycastBenchmarkEntryPoint)
endif()

# Панорама: цена построения кадрового описания на прогретом кеше и цена
# пересборки кеша при смене размера окна.
if(TARGET laiue_scene)
    add_executable(laiue_panorama_benchmark EXCLUDE_FROM_ALL
        panorama_benchmark.c)
    target_link_libraries(laiue_panorama_benchmark PRIVATE laiue_scene)
    target_include_directories(laiue_panorama_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_panorama_benchmark PanoramaBenchmarkEntryPoint)
endif()
if(TARGET laiue_audio)
    add_executable(laiue_audio_mixer_benchmark EXCLUDE_FROM_ALL audio_mixer_benchmark.c)
    target_link_libraries(laiue_audio_mixer_benchmark PRIVATE laiue_audio)
    target_include_directories(laiue_audio_mixer_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_audio_mixer_benchmark AudioMixerBenchmarkEntryPoint)
endif()

if(TARGET laiue_audio)
    # Загрузчик звукопака: разбор `.la`, кэш рядом с исходником и поиск
    # ресурса в паке. Ссылается на laiue_content ради каталога активного
    # пака, как и тест звукопака.
    add_executable(laiue_audio_pack_benchmark EXCLUDE_FROM_ALL audio_pack_benchmark.c)
    target_link_libraries(laiue_audio_pack_benchmark PRIVATE
        laiue_audio
        laiue_content)
    target_include_directories(laiue_audio_pack_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_audio_pack_benchmark AudioPackBenchmarkEntryPoint)
endif()


# Перечисление каталога содержимого на малых и крупных наборах: отдельный
# сценарий для сортировки и проверки ASCII case-collision, которые растут
# быстрее числа файлов.
add_executable(laiue_content_catalog_benchmark EXCLUDE_FROM_ALL
    content_catalog_benchmark.c)
target_link_libraries(laiue_content_catalog_benchmark PRIVATE laiue_content)
target_include_directories(laiue_content_catalog_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_content_catalog_benchmark ContentCatalogBenchmarkEntryPoint)

if(TARGET laiue_mod)
    add_executable(laiue_mod_benchmark EXCLUDE_FROM_ALL mod_benchmark.c)
    target_link_libraries(laiue_mod_benchmark PRIVATE laiue_mod)
    target_include_directories(laiue_mod_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_mod_benchmark ModBenchmarkEntryPoint)
endif()

if(TARGET laiue_render)
    # CPU-времена отправки кадра D3D12 (постановка мешей и инстансов,
    # пул геометрии). GPU-времена не измеряются: время берётся вокруг
    # записи командного списка, а Present/фенс остаются за замером.
    add_executable(laiue_renderer_submit_benchmark EXCLUDE_FROM_ALL
        renderer_submit_benchmark.c)
    target_link_libraries(laiue_renderer_submit_benchmark PRIVATE laiue_render)
    target_include_directories(laiue_renderer_submit_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_renderer_submit_benchmark RendererSubmitBenchmarkEntryPoint)
endif()

# A/B-стенд сборки текстурного пака. Внутренний texture_build.c компилируется
# в исполняемый файл напрямую: сборщик не экспортируется из laiue_render, а
# новые экспорты запрещены. В CTest не регистрируется — его гоняет A/B-скрипт.
if(TARGET laiue_render AND TARGET laiue_media_support)
    add_executable(laiue_texture_build_benchmark EXCLUDE_FROM_ALL
        texture_build_benchmark.c
        "${PROJECT_SOURCE_DIR}/src/graphics/render/texture_build.c")
    target_include_directories(laiue_texture_build_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(laiue_texture_build_benchmark PRIVATE
        laiue_content
        laiue::media_support)
    laiue_configure_standalone_executable(
        laiue_texture_build_benchmark TextureBuildBenchmarkEntryPoint)
endif()

if(TARGET laiue_media_support)
    # A/B-стенд декодера PNG: читает фикстуры из каталога окружения
    # LAIUE_PNG_BENCH_DIR и печатает мину/среднее на прогон.
    add_executable(laiue_png_decode_benchmark EXCLUDE_FROM_ALL
        png_decode_benchmark.c)
    target_include_directories(laiue_png_decode_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(laiue_png_decode_benchmark PRIVATE
        laiue::media_support)
    laiue_configure_standalone_executable(
        laiue_png_decode_benchmark PngDecodeBenchmarkEntryPoint)
endif()
# Декодирование изображений: JPEG и GIF. Фикстуры берутся из tests/, а
# крупные синтетические кадры стенд строит сам, поэтому набор образцов
# его не ограничивает.
if(TARGET laiue_media_support)
    add_executable(laiue_image_benchmark EXCLUDE_FROM_ALL image_benchmark.c)
    target_link_libraries(laiue_image_benchmark PRIVATE laiue::media_support)
    target_include_directories(laiue_image_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_image_benchmark ImageBenchmarkEntryPoint)
endif()


add_executable(laiue_image_resample_benchmark EXCLUDE_FROM_ALL
    image_resample_benchmark.c)
target_link_libraries(laiue_image_resample_benchmark PRIVATE
    laiue::media_support)
target_include_directories(laiue_image_resample_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_image_resample_benchmark ImageResampleBenchmarkEntryPoint)
# R2 A/B-стенд агента 02-numeric: тот же набор нагрузок плюс неблагоприятные
# (перенос из лимба, точное погашение, сдвиг в ноль) и печать каждого прогона.
# Линкуется с laiue_numeric, поэтому baseline/candidate сравниваются одним exe
# при подмене только DLL. В CTest не регистрируется.
add_executable(laiue_r2_02_numeric_ab EXCLUDE_FROM_ALL r2_02_numeric_ab.c)
target_link_libraries(laiue_r2_02_numeric_ab PRIVATE laiue_numeric)
target_include_directories(laiue_r2_02_numeric_ab PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_02_numeric_ab R2NumericAbEntryPoint)

# ROUND2 03-voxel: A/B-стенд обработки dynamic candidates VoxelBody. Мир блоков
# пуст, поэтому измеряется только обработка выборки коллайдеров.
add_executable(laiue_r2_03_voxel_dynamic_benchmark EXCLUDE_FROM_ALL
    r2_03_voxel_dynamic_benchmark.c)
target_link_libraries(laiue_r2_03_voxel_dynamic_benchmark PRIVATE laiue_physics)
target_include_directories(laiue_r2_03_voxel_dynamic_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_03_voxel_dynamic_benchmark R2VoxelDynamicBenchmarkEntryPoint)

# ROUND 2 (04-broadphase): сцены clustered/uniform, static/moving, small/large,
# churn; замер только времени обхода, хеши и полнота — вне замеряемого участка.
# Тот же приём, что у laiue_broadphase_benchmark: production-файл компилируется
# в исполняемый файл, потому что SetProxy/RemoveProxy/Query не экспортируются.
add_executable(laiue_r2_04_broadphase_bench EXCLUDE_FROM_ALL
    r2_04_broadphase_bench.c
    "${PROJECT_SOURCE_DIR}/src/simulation/physics/rigid_broadphase.c")
target_include_directories(laiue_r2_04_broadphase_bench PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(laiue_r2_04_broadphase_bench PRIVATE LAIUE_STATIC=1)
laiue_configure_standalone_executable(
    laiue_r2_04_broadphase_bench R2BroadphaseBenchEntryPoint)

# ROUND 2 harness агента 05-solver: нагрузки active/dense, stack и ground,
# serial (канонический) и parallel (executor + цветной решатель + кэш импульсов
# + индекс broadphase). Печатает median_ms и хеш конечного состояния, чтобы
# внешний A/B со подменой laiue_physics.dll видел и скорость, и биты.
add_executable(laiue_r2_05_solver_benchmark EXCLUDE_FROM_ALL r2_05_solver_bench.c)
target_link_libraries(laiue_r2_05_solver_benchmark PRIVATE
    laiue_physics
    laiue::task)
target_include_directories(laiue_r2_05_solver_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_05_solver_benchmark R2SolverBenchmarkEntryPoint)
# ROUND2 06-bvh: отдельный A/B стенд child BVH с проверяемыми checksum и
# сценариями tiny/degenerate/equal-centers. Private-код компилируется прямо
# в исполняемый файл, поэтому baseline и candidate — два exe из одного harness.
add_executable(laiue_r2_06_bvh_benchmark EXCLUDE_FROM_ALL
    r2_06_bvh_benchmark.c
    ../src/simulation/physics/compound_bvh.c)
target_link_libraries(laiue_r2_06_bvh_benchmark PRIVATE laiue::physics)
target_include_directories(laiue_r2_06_bvh_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
if(LAIUE_PLATFORM_EXTERNAL)
    target_link_libraries(laiue_r2_06_bvh_benchmark PRIVATE "${LAIUE_PRECISE_FP_TARGET}")
elseif(LAIUE_PLATFORM_WINDOWS)
    target_compile_options(laiue_r2_06_bvh_benchmark PRIVATE
        "$<$<C_COMPILER_ID:MSVC>:/fp:strict>"
        "$<$<C_COMPILER_ID:Clang>:/clang:-ffp-model=strict>")
else()
    target_compile_options(laiue_r2_06_bvh_benchmark PRIVATE -fno-fast-math -ffp-contract=off)
endif()
laiue_configure_standalone_executable(
    laiue_r2_06_bvh_benchmark R2_06BvhBenchmarkEntryPoint)

# ROUND2 07-compound: отдельный A/B-стенд coalescer-а compound-форм.
# VoxelRigidCompoundMergeBoxes приходит из laiue_physics (shared), поэтому
# baseline и candidate сравниваются одним и тем же exe с подменённым DLL.
add_executable(laiue_r2_07_compound_benchmark EXCLUDE_FROM_ALL
    r2_07_compound_coalescer_benchmark.c)
target_link_libraries(laiue_r2_07_compound_benchmark PRIVATE laiue::physics)
target_include_directories(laiue_r2_07_compound_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_07_compound_benchmark R2CompoundCoalescerEntryPoint)


    # R2 14-mixer: расширенный A/B-стенд микшера. Один и тот же exe
    # запускается с baseline и candidate laiue_audio.dll (интерфейс тот же),
    # поэтому сравнение не зависит от пересборки харнесса.
    add_executable(laiue_r2_14_mixer_benchmark EXCLUDE_FROM_ALL
        r2_14_mixer_benchmark.c)
    target_link_libraries(laiue_r2_14_mixer_benchmark PRIVATE laiue_audio)
    target_include_directories(laiue_r2_14_mixer_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_14_mixer_benchmark R2MixerBenchmarkEntryPoint)
if(TARGET laiue_audio AND TARGET laiue_media_support)
    # A/B-стенд ROUND2: PCM16 direct view и unaligned fallback, ADPCM decode
    # и сквозная загрузка `.la` через каталог. Один и тот же exe меряет
    # baseline и candidate DLL; ссылается на media_support ради SoundEncode
    # при сборке ADPCM-фикстуры.
    add_executable(r2_15_audio_pack_benchmark EXCLUDE_FROM_ALL
        r2_15_audio_pack_benchmark.c)
    target_link_libraries(r2_15_audio_pack_benchmark PRIVATE
        laiue_audio
        laiue_content
        laiue::media_support)
    target_include_directories(r2_15_audio_pack_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        r2_15_audio_pack_benchmark R2AudioPackBenchmarkEntryPoint)
endif()

# ROUND2 A/B harness каталога (ID 16-catalog). Один exe с baseline/candidate
# laiue_content.dll; в CTest не регистрируется. psapi нужен только ради
# measured peak committed/working set процесса.
add_executable(laiue_r2_16_catalog_benchmark EXCLUDE_FROM_ALL
    r2_16_catalog_benchmark.c)
target_link_libraries(laiue_r2_16_catalog_benchmark PRIVATE laiue_content)
target_include_directories(laiue_r2_16_catalog_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
if(WIN32)
    target_link_libraries(laiue_r2_16_catalog_benchmark PRIVATE psapi)
endif()
laiue_configure_standalone_executable(
    laiue_r2_16_catalog_benchmark R2CatalogBenchmarkEntryPoint)

# ROUND3 A/B harness задачи 37-catalog-peak: transient peak памяти
# однопроходного перечисления каталога. Один exe с baseline/candidate
# laiue_content.dll; в CTest не регистрируется. psapi — только ради measured
# peak committed/working set процесса.
add_executable(laiue_r3_37_catalog_peak_benchmark EXCLUDE_FROM_ALL
    r3_37_catalog_peak_benchmark.c)
target_link_libraries(laiue_r3_37_catalog_peak_benchmark PRIVATE laiue_content)
target_include_directories(laiue_r3_37_catalog_peak_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
if(WIN32)
    target_link_libraries(laiue_r3_37_catalog_peak_benchmark PRIVATE psapi)
endif()
laiue_configure_standalone_executable(
    laiue_r3_37_catalog_peak_benchmark R3CatalogPeakBenchmarkEntryPoint)

# ROUND 2: узкий A/B-стенд парсера манифеста и читателя пака. Тот же
# исполняемый файл запускается с baseline- и candidate-копией laiue_mod.dll,
# поэтому сравнение не зависит от сборки harness. EXCLUDE_FROM_ALL.
if(TARGET laiue_mod)
    add_executable(laiue_r2_18_manifest_benchmark EXCLUDE_FROM_ALL
        r2_18_manifest_benchmark.c)
    target_link_libraries(laiue_r2_18_manifest_benchmark PRIVATE laiue_mod)
    target_include_directories(laiue_r2_18_manifest_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_18_manifest_benchmark R2ModManifestBenchmarkEntry)
endif()

if(TARGET laiue_render)
    # ROUND 2 (21-vulkan): CPU-времена постановки работы Vulkan-бэкенда
    # offscreen. Общий exe переиспользуется с baseline- и candidate-DLL.
    add_executable(laiue_r2_21_vulkan_submit_benchmark EXCLUDE_FROM_ALL
        r2_21_vulkan_submit_benchmark.c)
    target_link_libraries(laiue_r2_21_vulkan_submit_benchmark PRIVATE laiue_render)
    target_include_directories(laiue_r2_21_vulkan_submit_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_21_vulkan_submit_benchmark R2VulkanSubmitBenchmarkEntryPoint)
endif()

# ROUND2 A/B-стенд 22-texture-build: тот же сборщик, но каждый сценарий
# измеряется REPEAT сборами подряд — короткая одиночная сборка (10-30 мс)
# тонет в шуме планировщика. Набор данных и имена сценариев совпадают с
# laiue_texture_build_benchmark, поэтому контрольные суммы обязаны совпасть.
if(TARGET laiue_render AND TARGET laiue_media_support)
    add_executable(laiue_r2_22_texture_build_repeat EXCLUDE_FROM_ALL
        r2_22_texture_build_repeat.c
        "${PROJECT_SOURCE_DIR}/src/graphics/render/texture_build.c")
    target_include_directories(laiue_r2_22_texture_build_repeat PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(laiue_r2_22_texture_build_repeat PRIVATE
        laiue_content
        laiue::media_support)
    laiue_configure_standalone_executable(
        laiue_r2_22_texture_build_repeat R2TextureBuildRepeatEntryPoint)
endif()

# A/B-стенд runtime-части текстурпака (r2 23-texture-pack): покадровый lookup
# кадра, lookup субресурса, захват расписания, имена материалов, перечисление
# и загрузка. `texture_pack.c` и `texture_build.c` компилируются в исполняемый
# файл: runtime не экспортируется из laiue_render, а новые экспорты запрещены.
# LAIUE_STATIC снимает dllimport с определений (как у broadphase-стенда).
if(TARGET laiue_render AND TARGET laiue_media_support)
    add_executable(laiue_r2_23_texture_pack_benchmark EXCLUDE_FROM_ALL
        r2_23_texture_pack_benchmark.c
        "${PROJECT_SOURCE_DIR}/src/graphics/render/content_provider.c"
        "${PROJECT_SOURCE_DIR}/src/graphics/render/texture_pack.c"
        "${PROJECT_SOURCE_DIR}/src/graphics/render/texture_build.c")
    target_include_directories(laiue_r2_23_texture_pack_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(laiue_r2_23_texture_pack_benchmark PRIVATE
        laiue_content
        laiue::media_support)
    target_compile_definitions(laiue_r2_23_texture_pack_benchmark PRIVATE LAIUE_STATIC=1)
    laiue_configure_standalone_executable(
        laiue_r2_23_texture_pack_benchmark R2TexturePackBenchmarkEntryPoint)
endif()


if(TARGET laiue_render)
    # ROUND2 24-shaders: A/B стенд discovery/load/validation шейдерпаков.
    # Один и тот же exe гоняется с baseline/candidate laiue_render.dll, потому
    # что shader_pack.c компилируется в SHARED-модуль. EXCLUDE_FROM_ALL.
    add_executable(laiue_r2_24_shaders_benchmark EXCLUDE_FROM_ALL
        r2_24_shaders_benchmark.c)
    target_link_libraries(laiue_r2_24_shaders_benchmark PRIVATE
        laiue_content
        laiue_render)
    target_include_directories(laiue_r2_24_shaders_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_24_shaders_benchmark R2ShadersBenchmarkEntryPoint)
endif()

# R2 25-png: внутрибинарный A/B PNG-декодера. Базовая (закоммиченная) копия
# src/media/png_decode.c вкомпилирована в тот же exe рядом с текущей
# production, чтобы общий inflate и раскладка бинарника не маскировали эффект.
if(TARGET laiue_media_support)
    add_executable(laiue_r2_25_png_bib_benchmark EXCLUDE_FROM_ALL
        r2_25_png_bib_bench.c)
    target_include_directories(laiue_r2_25_png_bib_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(laiue_r2_25_png_bib_benchmark PRIVATE
        laiue::media_support)
    laiue_configure_standalone_executable(
        laiue_r2_25_png_bib_benchmark R2PngBibBenchmarkEntryPoint)
endif()

# R2 A/B-стенд декодера DEFLATE: внутренний src/media/inflate.c компилируется
# прямо в исполняемый файл, чтобы один и тот же harness сравнивал baseline и
# candidate .obj без пересборки статической laiue_media_support.
add_executable(laiue_r2_26_inflate_benchmark EXCLUDE_FROM_ALL
    r2_26_inflate_benchmark.c
    "${PROJECT_SOURCE_DIR}/src/assets/media/inflate.c")
target_include_directories(laiue_r2_26_inflate_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_26_inflate_benchmark R2InflateBenchmarkEntryPoint)
# A/B-стенд декодера JPEG для round 2 (IDCT/цветность/Хаффман). Отдельный
# от image_benchmark: свои синтетические цветные входы 4:2:0/4:4:4, проверка
# усечений и формат строки R2JPEG, который читает r2_27_jpeg_ab.py.
if(TARGET laiue_media_support)
    add_executable(laiue_r2_27_jpeg_benchmark EXCLUDE_FROM_ALL
        r2_27_jpeg_benchmark.c)
    target_link_libraries(laiue_r2_27_jpeg_benchmark PRIVATE
        laiue::media_support)
    target_include_directories(laiue_r2_27_jpeg_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_27_jpeg_benchmark R2Jpeg27BenchmarkEntryPoint)
endif()
# GIF-специфичный A/B-стенд агента 28-gif: tiny/литеральный/сжатый/крупный/
# чересстрочный/прозрачный/reset. Фикстуры строит сам, поэтому набор образцов
# его не ограничивает. EXCLUDE_FROM_ALL, как и остальные ручные бенчмарки.
if(TARGET laiue_media_support)
    add_executable(laiue_r2_28_gif_benchmark EXCLUDE_FROM_ALL
        r2_28_gif_benchmark.c)
    target_link_libraries(laiue_r2_28_gif_benchmark PRIVATE laiue::media_support)
    target_include_directories(laiue_r2_28_gif_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_28_gif_benchmark R2GifBenchmarkEntryPoint)
endif()


# A/B-стенд декодера MP3 (агент 29-mp3): production-файл компилируется
# прямо в исполняемый файл, потому что декодер статический и одной
# подмены DLL для baseline/candidate не хватило бы. Входы собирает
# reports/29-mp3/gen_fixtures.py, каталог задаёт LAIUE_MP3_BENCH_DIR.
# В CTest не регистрируется — его гоняет A/B-скрипт в bench-lock.
add_executable(laiue_r2_29_mp3_benchmark EXCLUDE_FROM_ALL
    r2_29_mp3_benchmark.c
    "${PROJECT_SOURCE_DIR}/src/assets/media/mp3_decode.c")
target_include_directories(laiue_r2_29_mp3_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_29_mp3_benchmark R2Mp3BenchmarkEntryPoint)
if(TARGET laiue_media_support)
    # ROUND2 30-sound: A/B-стенд WAV decode, sound dispatch и записи `.la`
    # (PCM16/ADPCM). Три разрешённых production-файла компилируются прямо в
    # исполняемый файл в фиксированном порядке (wave_decode и sound раньше
    # la_encode): тогда изменение la_encode.c не сдвигает код decode в
    # линковке и A/B меряет именно изменение, а не раскладку секций.
    # baseline и candidate — одинаково собранные exe, отличаются только
    # la_encode.c.
    add_executable(laiue_r2_30_sound_benchmark EXCLUDE_FROM_ALL
        r2_30_sound_benchmark.c
        "${PROJECT_SOURCE_DIR}/src/assets/media/wave_decode.c"
        "${PROJECT_SOURCE_DIR}/src/assets/media/sound.c"
        "${PROJECT_SOURCE_DIR}/src/assets/media/la_encode.c"
        "${PROJECT_SOURCE_DIR}/src/assets/media/mp3_decode.c")
    target_include_directories(laiue_r2_30_sound_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_compile_definitions(laiue_r2_30_sound_benchmark PRIVATE LAIUE_STATIC=1)
    laiue_configure_standalone_executable(
        laiue_r2_30_sound_benchmark R2SoundBenchmarkEntryPoint)
endif()


if(TARGET laiue_ui)
    # A/B-стенд UI (агент 31-ui): меню из виджетов и чистые текстовые пути.
    # Слинкован с настоящей laiue_ui.dll, поэтому один и тот же исполняемый
    # файл сравнивается с baseline/candidate DLL в отдельных каталогах.
    # В CTest не регистрируется: его гоняет A/B-скрипт под bench-lock.
    add_executable(laiue_r2_31_ui_benchmark EXCLUDE_FROM_ALL
        r2_31_ui_benchmark.c)
    target_link_libraries(laiue_r2_31_ui_benchmark PRIVATE laiue_ui)
    target_include_directories(laiue_r2_31_ui_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r2_31_ui_benchmark R2UiBenchmarkEntryPoint)
    if(WIN32)
        target_link_libraries(laiue_r2_31_ui_benchmark PRIVATE psapi)
    endif()
endif()

# ROUND 2, агент 32-image: A/B-стенд общего пути ImageResample (уменьшение
# с произвольным коэффициентом, смешанный масштаб, мелкие картинки).
# Отдельный target, EXCLUDE_FROM_ALL, запускается A/B-скриптом в bench-lock.
add_executable(laiue_r2_32_image_resample_benchmark EXCLUDE_FROM_ALL
    r2_32_image_resample_benchmark.c)
target_link_libraries(laiue_r2_32_image_resample_benchmark PRIVATE
    laiue::media_support)
target_include_directories(laiue_r2_32_image_resample_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_r2_32_image_resample_benchmark R2ImageResampleBenchmarkEntryPoint)

# ROUND2 33-sha256: A/B-стенд LaiueSha256Compute. На Windows sha256.c не входит
# в laiue_platform_support (там BCrypt), поэтому production-файл компилируется
# прямо в исполняемый файл и baseline/candidate сравниваются одним harness.
# На POSIX sha256.c уже внутри platform_support — второй копии не добавляем.
set(_laiue_r2_sha256_sources "")
if(LAIUE_PLATFORM_WINDOWS)
    set(_laiue_r2_sha256_sources "${PROJECT_SOURCE_DIR}/src/platform/sha256.c")
endif()
add_executable(laiue_sha256_benchmark EXCLUDE_FROM_ALL
    r2_33_sha256_benchmark.c
    ${_laiue_r2_sha256_sources})
target_include_directories(laiue_sha256_benchmark PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests")
laiue_configure_standalone_executable(
    laiue_sha256_benchmark R2Sha256BenchmarkEntryPoint)
unset(_laiue_r2_sha256_sources)
if(TARGET laiue_render)
    # ROUND 3, задача 43-render-dispatch: CPU-цена выбора бэкенда/диспетчера и
    # реестра живых Renderer-объектов, отделённая от работы бэкенда.
    # Dispatch-only циклы RendererGetBackend не трогают GPU; покадровые стадии
    # меряют только время постановки на CPU. Один exe сравнивается с
    # baseline/candidate laiue_render.dll в отдельных каталогах.
    add_executable(laiue_r3_43_render_dispatch_benchmark EXCLUDE_FROM_ALL
        r3_43_render_dispatch_benchmark.c)
    target_link_libraries(laiue_r3_43_render_dispatch_benchmark PRIVATE laiue_render)
    target_include_directories(laiue_r3_43_render_dispatch_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    if(WIN32)
        target_link_libraries(laiue_r3_43_render_dispatch_benchmark PRIVATE psapi)
    endif()
    laiue_configure_standalone_executable(
        laiue_r3_43_render_dispatch_benchmark R3RenderDispatchBenchmarkEntryPoint)
endif()


# ROUND3 52-mod-host-alt: ordering/enumeration загруженных модов плюс реестр
# сервисов и поиск сервиса изнутри расширения. Расширение — отдельная
# shared-библиотека, потому что queryService доступен только изнутри мода.
# laiue_mod на Windows shared, поэтому baseline/candidate делят один exe и
# одну DLL расширения, а подменяется только laiue_mod.dll.
if(TARGET laiue_mod AND LAIUE_NATIVE_MODS_ENABLED)
    add_library(laiue_r3_52_mod_host_query_extension SHARED EXCLUDE_FROM_ALL
        r3_52_mod_host_query_extension.c)
    target_link_libraries(laiue_r3_52_mod_host_query_extension PRIVATE
        laiue_common)
    if(WIN32)
        target_link_options(laiue_r3_52_mod_host_query_extension PRIVATE
            /NOENTRY)
    else()
        set_target_properties(laiue_r3_52_mod_host_query_extension PROPERTIES
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES)
    endif()

    add_executable(laiue_r3_52_mod_host_benchmark EXCLUDE_FROM_ALL
        r3_52_mod_host_registry_benchmark.c)
    target_link_libraries(laiue_r3_52_mod_host_benchmark PRIVATE laiue_mod)
    target_include_directories(laiue_r3_52_mod_host_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    laiue_configure_standalone_executable(
        laiue_r3_52_mod_host_benchmark R3ModHostRegistryBenchmarkEntry)
    add_dependencies(laiue_r3_52_mod_host_benchmark
        laiue_r3_52_mod_host_query_extension)
endif()
