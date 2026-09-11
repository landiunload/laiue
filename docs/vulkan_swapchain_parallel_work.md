# Vulkan-бэкенд: вывод кадра в окно Win32

Реализация swapchain-вывода для `src/render/renderer_vulkan.c`. Offscreen-путь
(`windowHandle == NULL`) не изменён: swapchain — только вывод, все проходы
сцены, панорама, UI и `RendererCaptureFrame` продолжают работать с
`colorTargets`.

## Что сделано

- Инстанс: при `windowHandle != NULL` на Windows включаются
  `VK_KHR_surface` и `VK_KHR_win32_surface` (после проверки
  `vkEnumerateInstanceExtensionProperties`; иначе `RendererCreate_Vulkan`
  возвращает NULL, как раньше). Заголовки `windows.h` и
  `vulkan/vulkan_win32.h` подключены строго под `#if defined(_WIN32)`,
  `WIN32_LEAN_AND_MEAN` выставлен до `windows.h`.
- Surface: `vkCreateWin32SurfaceKHR` с `GetModuleHandleW(NULL)` и HWND.
- Очередь: та же семья, что графическая, если
  `vkGetPhysicalDeviceSurfaceSupportKHR` подтверждает present. Отдельная
  present-очередь не поддерживается; при её отсутствии создание рендера
  честно отказывает.
- Устройство: `VK_KHR_swapchain` включается только при наличии surface.
- Swapchain: пересоздаётся при resize и смене vsync через флаг
  `swapchainOutOfDate` (не в момент вызова). `imageUsage` —
  `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, `minImageCount` по capabilities (не
  меньше 2), `compositeAlpha` выбирается из поддерживаемых.
- Кадр: в `RendererEndFrame_Vulkan` после перехода `colorTargets[frameIndex]`
  в `TRANSFER_SRC_OPTIMAL` выполняется `vkAcquireNextImageKHR`, барьер
  `UNDEFINED → TRANSFER_DST_OPTIMAL`, копия/блит, барьер
  `TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR`, submit с ожиданием
  `imageAvailable[frameIndex]` (стадия `TRANSFER`) и сигналом
  `renderFinished[imageIndex]`, затем `vkQueuePresentKHR`, ожидающий
  `renderFinished`.
- `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` от acquire/present помечают
  swapchain устаревшим; кадр не считается ошибкой (возвращается true,
  present пропускается), пересоздание — в начале следующего кадра.
- Resize 0×0 (свёрнутое окно): кадры пропускаются без ошибок, при
  восстановлении размера цели и swapchain пересобираются.
- Уничтожение: `vkDeviceWaitIdle`, семафоры, представления образов,
  swapchain, surface, устройство, инстанс. Частично построенный swapchain
  освобождается на любом шаге.
- Linux и прочие ОС: `windowHandle != NULL` по-прежнему даёт NULL; весь
  Win32-код вырезан препроцессором.

Публичный API/ABI не менялся, новых экспортов нет. Пул геометрии
(`PoolAllocate`/`PoolFree`/`PoolBlockCreate`), `DrainDeferredReleases` и
`RendererGetStats_Vulkan` не тронуты. Новые поля `struct Renderer` добавлены
одним блоком в конец с комментарием `// swapchain`.

## Выбранные формат и present-mode (RTX 4060, драйвер Vulkan 1.4.341)

Проба (`vkGetPhysicalDeviceSurfaceCapabilitiesKHR`/`...FormatsKHR`/
`...PresentModesKHR`) на скрытом окне 640×360:

- surface-форматы: `B8G8R8A8_UNORM` (44), `B8G8R8A8_SRGB` (50),
  `R8G8B8A8_UNORM` (37), **`R8G8B8A8_SRGB` (43)**, `A2B10G10R10_UNORM_PACK32` (64).
- present-modes: `FIFO`, `FIFO_RELAXED`, `MAILBOX`, `IMMEDIATE`, плюс
  shared-present.
- capabilities: `minImageCount = 2`, `maxImageCount = 8`,
  `currentExtent = min = max` (клиентская область), `currentTransform = IDENTITY`,
  `supportedCompositeAlpha = OPAQUE | PRE_MULTIPLIED`,
  `supportedUsageFlags` включает `TRANSFER_DST`.
- семья 0: `GRAPHICS | COMPUTE | TRANSFER | SPARSE`, present поддерживается.

Итог выбора:

- **Формат: `VK_FORMAT_R8G8B8A8_SRGB`** — он совпадает с форматом
  offscreen-цели (`COLOR_FORMAT`), поэтому применяется `vkCmdCopyImage` без
  конверсии и перестановки каналов. Формат `B8G8R8A8_UNORM` был доступен и
  тоже задействован как следующий приоритет, но через `vkCmdBlitImage`: по
  спецификации blit из sRGB-источника конвертирует нелинейные значения в
  линейные, а в UNORM-приёмник пишет уже линейные, из-за чего картинка на
  экране темнеет. Точное совпадение формата этой проблемы не имеет. Порядок
  приоритетов: `R8G8B8A8_SRGB` → `B8G8R8A8_UNORM` → любой sRGB → первый
  доступный; при несовпадении формата или размера используется blit.
- **Present-mode: `FIFO` при включённом vsync; `IMMEDIATE` при выключенном**
  (он поддержан), с откатом на `MAILBOX`, затем `FIFO`.
- **Образов: 2** (`minImageCount`), размер — `currentExtent`.

## Схема кадра

```
colorTargets[i] --(проходы сцены, панорама, UI)--> TRANSFER_SRC_OPTIMAL
vkAcquireNextImageKHR -> swapchain[j], сигнал imageAvailable[i]
swapchain[j]: UNDEFINED -> TRANSFER_DST_OPTIMAL
vkCmdCopyImage (формат совпал) | vkCmdBlitImage (иначе)
swapchain[j]: TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
vkQueueSubmit(wait imageAvailable[i] @TRANSFER, signal renderFinished[j], fence[i])
vkQueuePresentKHR(wait renderFinished[j])
```

Семафоры: `imageAvailable[FRAME_COUNT]`, `renderFinished[imageCount]`
(индексируется номером образа). Переиспользование исключено: `imageAvailable`
гарантированно отработан к моменту сигнала `fence[frameIndex]`, а
`renderFinished[j]` освобождается только тогда, когда тот же образ снова
выдан `vkAcquireNextImageKHR`, что означает завершение предыдущего present.

## Результаты стенда

Стенд `build\peer\swapchain_stand.c` (сборка `build\peer\build_stand.bat`,
запуск `build\peer\run_stand.bat`): скрытое `WS_POPUP`-окно 640×360,
`RendererCreateWithBackend(HWND, VULKAN)`, один меш.

- Создание с HWND — не NULL, `RendererGetBackend == RENDERER_BACKEND_VULKAN`.
- 60 кадров без vsync: все `RendererEndFrame == true`;
  `RendererCaptureFrame` даёт нечёрный кадр (230400/230400 пикселей) и центр,
  отличный от цвета неба, — offscreen-цель жива при включённом swapchain.
- Время кадра: без vsync (`IMMEDIATE`) ≈ **1.1 мс/кадр**, с vsync (`FIFO`) ≈
  **16.2 мс/кадр**. Разница подтверждает, что смена vsync действительно
  пересоздаёт swapchain (present-mode зашит в swapchain).
- Переключение vsync on/off посреди серии — все кадры успешны.
- Resize 640×360 → 800×450 → 0×0 → 640×360: кадры после 0×0 пропускаются
  (`RendererBeginFrame` возвращает false), после восстановления рисуются
  снова.
- Создание/уничтожение в одном процессе: рост числа дескрипторов процесса
  совпадает с фоном голого Vulkan-цикла. Важно: драйвер этого GPU сам не
  освобождает ~5–7 kernel-дескрипторов на каждый цикл
  `instance+surface+device+swapchain` (проверено зондами `probe_leak.c`,
  `probe_leak2.c` без движка). Стенд измеряет этот фон отдельным голым
  циклом и сравнивает: за 3 цикла рендерера +21 дескриптор при фоне +19, то
  есть выше фона драйвера утечки нет.

Существующие тесты Vulkan (`build\vk`): **31/31**, включая
`laiue.render.offscreen_frame` — путь без окна не изменился. D3D12-сборка
(`build`, `setup.bat`) собирается без изменений.

## Что не поддержано и почему

- **Отдельная present-очередь.** Используется только та же семья, что и
  графика, если она умеет present; иначе создание с окном отказывает.
  Причина: движок работает с одной очередью (`queueFamily`, `queue`) во всех
  проходах и буферах; вторая семья потребовала бы разделения владения
  ресурсами (`VK_SHARING_MODE_CONCURRENT`/передачи владения) и заметно
  усложнила бы синхронизацию. На проверенном GPU семья 0 закрывает обе роли.
- **Оконный вывод на Linux и других ОС.** Весь код под `#if defined(_WIN32)`;
  `windowHandle != NULL` вне Windows по-прежнему означает NULL. Переносимой
  window-системы у движка пока нет (X11/Wayland surface — отдельный этап),
  и этот этап её не вводит, чтобы не ломать головную сборку.
- **Публичная диагностика пересозданий swapchain.** Внутренний счётчик
  `swapchainRecreateCount` есть, но в `RendererStats` не выведен:
  публичную структуру расширять запрещено. Работа пересоздания проверяется
  косвенно — по времени кадра (FIFO против IMMEDIATE).

## Известные ограничения

- Свободного выбора extent на Win32 нет: `minImageExtent == maxImageExtent ==
  currentExtent`, поэтому размер swapchain берётся из capabilities, а цели
  кадра при создании и resize подгоняются под клиентскую область.
- Утечка kernel-дескрипторов уровня драйвера (см. выше) не устраняется
  средствами движка; стенд отделяет её от собственных утечек.
