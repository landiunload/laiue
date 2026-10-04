# laiue

Модульный движок на C17: бесконечные воксельные миры и миры из моделей,
детерминированная физика, графика, анимации, звук, ресурсы и моды.
Приложение выбирает нужные модули через общий C ABI и задаёт правила игры,
генерацию мира и порядок загрузки модов. Декодеры и основные алгоритмы
собственные; платформенные адаптеры используют системные API и SDK.

Оконные примеры работают на Windows с Direct3D 12 или Vulkan и на Linux
с X11/Vulkan. Ядро macOS, Android и iOS проверяется сборками CI; это
не подтверждает полноценный клиент на этих платформах. Для Android есть
отдельный пример NativeActivity/Vulkan. Точные границы поддержки и
зависимости сборки — в [документации по платформам](docs/portability.md).

Для Windows нужны CMake 3.28+, Ninja и Visual Studio Developer PowerShell:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release
```

Начните с [архитектуры](docs/architecture.md),
[примеров ходьбы](examples/walk/README.md) и
[мира из моделей](examples/mesh_world/README.md).
[Пример подключения SDK](tests/consumer/CMakeLists.txt) использует
`find_package(laiue)`; [правила разработки](CONTRIBUTING.md) описывают проверки.

Прикладной проект —
[simulation-of-sins](https://github.com/landiunload/simulation-of-sins).
Лицензия — [MIT](LICENSE).
