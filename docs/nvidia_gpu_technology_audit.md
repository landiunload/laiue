# NVIDIA GPU-технологии: применимость к LAIUE

Проверено 27 сентября 2026 года по официальным репозиториям и документации
производителей. Цель — использовать полезные алгоритмы, не делать NVIDIA
обязательным поставщиком и не заявлять ускорение без замера на реальной нагрузке.

## Что подходит сейчас

| Технология | Доступность и переносимость | Решение для LAIUE |
| --- | --- | --- |
| NVIDIA Image Scaling (NIS) | Исходники под MIT; spatial upscaler/sharpener; D3D12 и Vulkan; алгоритм рассчитан на NVIDIA, AMD и Intel. | Хороший кандидат для необязательного снижения внутреннего разрешения. Сейчас не подключать: рендерер не имеет render-scale/post-process контракта и GPU-таймеров, поэтому нельзя проверить выигрыш и цену качества. Сопоставимый открытый вариант — AMD FSR 1. |
| DLSS Super Resolution / Frame Generation / Ray Reconstruction | Интеграционные части частично открыты через Streamline, но DLSS-плагины и модели поставляются отдельно по лицензии NVIDIA. DLSS SDK лицензирован для работы на системах с NVIDIA GPU. | Только необязательный provider позже. SR/FG/RR требуют motion vectors, jitter, истории кадров и нужных ресурсов; движок пока их не предоставляет. Универсальный путь должен оставаться собственным рендером или открытым spatial upscaler. |
| NVIDIA Streamline | Интеграционный framework открыт, но отдельные feature plugins и бинарные артефакты имеют собственные условия и аппаратные требования. Поддержка Vulkan зависит от плагина и конкретной функции. | Не добавлять как обязательный слой в core: нет готового temporal-renderer контракта, а Vulkan и нестандартные модули должны продолжать работать независимо. |
| NVIDIA Reflex Low Latency | NVIDIA-specific путь через NVAPI/Streamline; низкая задержка доступна только на поддерживаемых GeForce, тогда как latency markers доступны шире. | Общий аналог — ограниченная очередь кадров и just-in-time pacing через DXGI/Vulkan ожидания с fallback. Это уменьшает latency и очередь, но не ускоряет рендер; без input-to-present измерений pacing может снизить throughput. Сейчас не добавлять в core. |
| AMD FidelityFX Super Resolution 1 | Исходный код под MIT; поддерживает широкий набор GPU и D3D12/Vulkan. | Прямой открытый аналог NIS для будущего optional spatial-upscale provider. FSR 2+ и XeSS temporal требуют данных кадра, которых сейчас нет. |
| Intel XeSS 3 | Runtime закрытый бинарный компонент по отдельной Intel-лицензии; репозиторий также содержит headers, документацию и примеры с отдельными условиями. SR имеет DP4a cross-vendor путь для совместимого оборудования, есть Vulkan/D3D12. | Возможен только как необязательный адаптер после появления motion-vector/history API. Не делать зависимостью модулей или сборки. |
| RTX IO / GDeflate | RTX IO использует GPU-декомпрессию; GDeflate и DirectStorage дают переносимый формат/интерфейс, но аппаратная GPU-декомпрессия остаётся опциональной. Vulkan memory-decompression — также optional feature. | Потенциально полезно для крупных потоковых миров и модов. Сейчас загрузка паков происходит при подготовке мира/перезагрузке, а не в горячем кадре; асинхронного чтения и GDeflate-формата в движке нет. Не добавлять зависимость без подтверждённой нагрузки. |
| CUDA / PhysX GPU dynamics | PhysX открыт (текущая лицензия репозитория BSD-3-Clause); GPU simulation построена на CUDA и предназначена для NVIDIA. | Не заменять детерминированную C17-физику. У текущего ragdoll максимум 16 тел; передача на GPU здесь не окупится. Рассматривать отдельный provider только для доказанной сцены с тысячами активных тел, сохраняя CPU-путь и его replay-контракт. |
| RTX Mega Geometry, RTXGI, RTXDI, NRD, RTX Memory Utility | Репозитории/SDK доступны публично, но функции опираются на аппаратные RT/AS расширения и/или лицензии NVIDIA. | Низкая применимость: текущая графика — растеризация компактных voxel-quad мешей; нет RT pipeline, BVH/TLAS и ray-traced lighting outputs. Добавление усложнит и сузит поддержку вместо ускорения текущего пути. |
| Vulkan/D3D12 indirect draw и GPU culling | Стандартные API механизмы, реализуются драйверами разных производителей; имеют capability/fallback требования. | Кандидат при замере CPU-bound кадров с большим числом видимых чанков. Сейчас draw API обновляет состояние на каждый mesh, поэтому переход потребует общей таблицы геометрии и индиректного описания команд. Сначала нужен парный CPU/GPU-профиль. |

Источники производителей: [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling),
[NVIDIA RTX SDK license](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt),
[Streamline](https://github.com/NVIDIA-RTX/Streamline),
[AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR),
[AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK),
[Intel XeSS](https://github.com/intel/xess),
[NVIDIA Reflex SDK](https://developer.nvidia.com/performance-rendering-tools/reflex),
[DXGI frame-latency waitable object](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject),
[Vulkan present wait](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_present_wait.html),
[NVIDIA RTX IO](https://developer.nvidia.com/rtx-io),
[DirectStorage GDeflate](https://github.com/microsoft/DirectStorage/tree/main/GDeflate),
[PhysX](https://github.com/NVIDIA-Omniverse/PhysX),
[RTX Kit](https://github.com/NVIDIA-RTX/RTX-Kit),
[Vulkan indirect-count](https://docs.vulkan.org/guide/latest/extensions/VK_KHR_draw_indirect_count.html),
[D3D12 ExecuteIndirect](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing).

## Внесённое изменение

Vulkan renderer создаёт изображения текстурпака с одним mip-уровнем и
копирует только mip 0. До изменения общий сборщик всё равно пересэмплировал
каждый слой в полную mip-цепочку, а Vulkan отбрасывал почти треть собранного
пиксельного буфера. Добавлен отдельный внутренний путь сборки базового уровня;
Vulkan использует его при создании мира и hot reload. D3D12 по-прежнему
строит и загружает полную цепочку.

Для текстур 256×256 полная цепочка занимает 349 524 байта на слой, базовый
уровень — 262 144 байта: **−25% пиксельного payload**. Совпадение mip 0, нормалей,
порядка кадров и расписания анимации проверяется regression-тестом. Стенд
`laiue_texture_build_benchmark` печатает рядом времена полного и одноуровневого
сборщиков, объём данных и checksum базовых пикселей.

Это не меняет поведение Vulkan-рендера, потому что его изображения и раньше
имели один mip. Число −25% относится к пиксельному payload сборщика, а не к
пиковому RSS всего процесса или VRAM. Для D3D12 экономии нет, поскольку он
потребляет всю цепочку. На Windows x64, Release/MSVC 2026, RTX 4060 стенд
прогревает оба пути и затем делает девять пар одиночных сборок с чередованием
порядка full/base и base/full. Ниже медиана прошедшего wall time на одну сборку
(таймер включает загрузку контента, выделение и заполнение памяти; это не GPU
время и не время кадра игры):

| Сценарий | Время | Изменение |
| --- | ---: | ---: |
| 32 статических слоя | 15,941 → 9,109 мс | −42,9% |
| 32 слоя с нормалями | 21,704 → 8,920 мс | −58,9% |
| 8 анимированных материалов, отдельная нормаль | 28,325 → 14,073 мс | −50,3% |
| 8 анимированных материалов, встроенные нормали | 39,307 → 17,995 мс | −54,2% |

Эти результаты относятся к четырём синтетическим сценариям стенда, а не ко
всем проектным наборам текстур. Проценты показывают выигрыш времени build path
на этой машине; они не являются обещанием такого же ускорения игры. Размер
payload сокращён детерминированно на 25%, а regression-тест сверяет базовые
пиксели и метаданные. В тех же сценариях это освобождает во временном пиксельном
буфере 2,67 МиБ для 32 обычных слоёв, 5,33 МиБ для 32 слоёв с normal maps и
10,67 МиБ для 64 анимированных слоёв с normal maps. Это размер буфера, не
измеренное изменение пикового RSS процесса.

## Добавленный переносимый GPU-профайлер

Добавлен отдельный optional-модуль `laiue.graphics.profile` и одинаковый
versioned API для D3D12 и Vulkan. Backend считывает последние завершённые
GPU timestamp queries только после fence; опрос незавершённого кадра не ждёт
GPU. Vulkan учитывает `timestampValidBits`, их переполнение и
`timestampPeriod`; D3D12 переводит ticks через частоту очереди. Если драйвер,
очередь или выделение query-ресурсов не поддерживаются, renderer продолжает
работать, а API сообщает `unsupported`. API не зависит от NVIDIA и доступен на
любом из этих backend-ов, где GPU timestamps поддержаны.

История профайлера хранит последние 120 валидных кадров и считает latest,
minimum, median/p50, nearest-rank p95 и maximum. Для добавления образца нет
выделений памяти; одна история использует фиксированный массив таймингов,
сводка сортирует копию на ограниченном стеке. Результат относится к GPU
командам кадра: это не CPU submit time, ожидание VSync или фактическая задержка
до отображения пикселя. Границы измерения немного различаются по backend-ам,
поэтому напрямую сравнивать числа D3D12 и Vulkan как идентичные интервалы не
следует.

Профайлер добавляет timestamp-команды к кадрам и крошечную readback/query
область на поддерживаемом backend-е; расход и влияние этих запросов на время
кадра пока не измерялись отдельно. Эта функция собирает данные для выбора
следующего оптимизатора, но сама по себе FPS не повышает. Статистику проверяет
тест без GPU; offscreen renderer regression проверяет необязательные флаги.
Физические GPU и validation layers отдельно не запускались.

## Что измерить перед следующим графическим апгрейдом

1. Добавить сбор CPU submit и present timing отдельно от уже доступных GPU
   timestamps.
2. Если доминирует pixel/GPU time — прототипировать render-scale с NIS или
   FSR 1, оставив native resolution значением по умолчанию и сравнив кадры.
3. Если доминирует CPU submit — измерить индиректную пакетную отрисовку на
   реальном количестве visible/culled чанков и оставить обычные draw calls
   как fallback.
4. Для temporal SR сначала определить переносимый контракт depth/motion
   vectors/jitter/history; только после этого сравнивать DLSS, FSR и XeSS как
   независимые optional providers.

Reflex Low Latency не переносится в core: его задача — latency, не throughput.
Если появится запрос на управление задержкой, сначала измерить размер очереди
и input-to-present latency, затем добавить общую pacing policy поверх
DXGI/Vulkan wait-механизмов с fallback. Reflex можно подключить отдельным
provider, когда появятся подходящий frame-loop контракт и проверка на GeForce.
