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
    target_link_libraries(laiue_world_batch_benchmark PRIVATE laiue_world)
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
    "${PROJECT_SOURCE_DIR}/src/physics/rigid_broadphase.c")
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
    ../src/physics/compound_bvh.c)
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
        "${PROJECT_SOURCE_DIR}/src/render/texture_build.c")
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
