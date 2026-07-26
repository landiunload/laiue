# Моддинг

Мод — доверенная нативная библиотека в каталоге `mods/<name>.lmp`.
Полный переносимый pack может содержать бинарники нескольких платформ:

```text
my_mod.lmp/
  mod.lm
  my_mod.windows-x86_64.dll
  my_mod.linux-x86_64-gnu.so
  my_mod.linux-x86_64-musl.so
```

Мод не линкуется с внутренними DLL/SO игры. Единственная публичная
граница — таблица функций из `sdk/laiue_mod_api.h`.

## Манифест

```text
LAIUE MOD 2
id = example.my_mod
name = My Mod
version = 1.0.0
game = 0.5
side = both

[native]
entry_windows_x86_64 = my_mod.windows-x86_64.dll
entry_linux_x86_64_gnu = my_mod.linux-x86_64-gnu.so
entry_linux_x86_64_musl = my_mod.linux-x86_64-musl.so
api = 1
```

`id` состоит из ASCII-букв в нижнем регистре, цифр, `.`, `_` и `-`.
`game` — минимальная совместимая версия игры, `api` — версия SDK.
Допустимо объявить только реально поставляемые платформы. Каждый
объявленный файл обязан присутствовать, иначе pack несовместим. На текущей
платформе должен быть её entry.

`LAIUE MOD 1` продолжает читаться: его `entry` считается исключительно
`entry_windows_x86_64`. Для нового или пересобранного мода используйте v2.
Одновременно задавать `entry` и `entry_windows_x86_64` нельзя.

| `side` | Где загружается | Проверка при подключении |
|---|---|---|
| `client` | только клиент | не участвует |
| `server` | только сервер | обязан быть установлен у клиента, но там не запускается |
| `both` | клиент и сервер | обязан совпасть и запускается с обеих сторон |

Без `side` используется `both`. Клиент загружает профили из
`mods/enabled.txt`, сервер — из `mods/server_enabled.txt`. Порядок важен для
зависимостей между модами.

## Минимальный мод

```c
#include "laiue_mod_api.h"

static void OnTick(void* user, float stepSeconds)
{
    (void)user;
    (void)stepSeconds;
}

LAIUE_MOD_EXPORT int32_t LAIUE_MOD_CALL
LaiueModInit(const LaiueModApi* api)
{
    api->setFixedTickCallback(api->host, OnTick, 0);
    api->log(api->host, L"mod loaded");
    return 0;
}

LAIUE_MOD_EXPORT void LAIUE_MOD_CALL LaiueModShutdown(void) {}
```

Windows, из x64 Developer Command Prompt:

```bat
cl /nologo /W4 /O2 /utf-8 /LD /Isdk my_mod.c ^
  /Fe:my_mod.windows-x86_64.dll
```

Linux glibc:

```sh
cc -std=c17 -Wall -Wextra -Werror -O2 -fPIC -fvisibility=hidden \
  -shared -Isdk my_mod.c -o my_mod.linux-x86_64-gnu.so
```

Linux musl собирается теми же флагами с musl toolchain и получает имя
`my_mod.linux-x86_64-musl.so`. Не переименовывайте glibc-бинарник в musl:
это разные ABI-артефакты.

Для одно-платформенной локальной разработки manifest может объявлять только
текущий бинарник. Такой pack намеренно не совпадёт по fingerprint с pack
другой ОС. Release pack для совместной игры Windows/Linux должен содержать
все объявленные артефакты. CI сохраняет отдельные platform stages, затем
`cmake/AssembleModSdk.cmake` создаёт `laiue-mod-sdk-x86_64` с одним
manifest и тремя бинарниками каждого example-мода; именно этот pack имеет
одинаковый compatibility hash у Windows, glibc и musl readers.

Экспорты всегда оформляются через `LAIUE_MOD_EXPORT` и
`LAIUE_MOD_CALL`; прямой `__declspec(dllexport)` делает исходник
Windows-only.

Например, скрипты `auto_bridge` создают бинарник и готовый локальный
`mod.lm` из `mod.lm.in`:

```powershell
cd sdk/examples/auto_bridge
./build.bat
```

```sh
cd sdk/examples/auto_bridge
sh ./build.sh

# Для musl:
LAIUE_LINUX_LIBC=musl CC=musl-gcc sh ./build.sh
```

`LaiueModInit` возвращает 0 при успехе. `LaiueModShutdown` опционален.
Колбеки и API вызываются только на главном потоке; в паузе игровые хуки не
работают. Мод обязан проверять `api->structSize` перед использованием полей,
которых не было в его версии заголовка.

## Включение и перезагрузка

В главном меню изменение списка только сохраняет профиль: ModHost там не
создаётся. В активной одиночной сессии изменение состава перезагружает всю
цепочку по порядку enabled-файла. Перед пересборкой DLL выключите мод или
завершите игру, иначе Windows не даст перезаписать загруженный файл.

## API версии 1

| Группа | Возможности |
|---|---|
| мир | `getBlock`, `setBlock`, время суток |
| игрок | позиция, взгляд, импульс, grounded, режим, воздушные прыжки |
| события | frame, fixed tick `1/60`, правка блока игроком |
| зависимости | `publishInterface`, `queryInterface` |
| данные | `readModData`, `writeModData` |
| диагностика | `log` в debugger и `mods/mod_log.txt` |

Gameplay размещайте в fixed tick, визуальные действия — в frame callback.
Не выполняйте там файловый ввод-вывод, частые аллокации или логирование на
каждом кадре. Массовые `setBlock` ограничивайте: каждая серия правок создаёт
работу для remeshing.

## Интерфейсы между модами

Библиотечный мод публикует именованную таблицу функций, потребитель получает
её через `queryInterface` в `LaiueModInit`.

- библиотека должна стоять раньше потребителя;
- добавление функций выполняется в хвост таблицы с повышением версии;
- ломающий контракт получает новое имя интерфейса;
- указатель принадлежит библиотеке и недействителен после перезагрузки цепочки.

Рабочая пара: `builder_lib.lmp` и `spiral_tower.lmp` в `sdk/examples/`.

## Совместимость с сервером

До допуска сравниваются упорядоченные `id`, `version` и content fingerprint.
Для v2 начальное значение — SHA-256 точных байтов `mod.lm`, после чего к нему
последовательно подмешиваются SHA-256 объявленных файлов в фиксированном
порядке:

1. `entry_windows_x86_64`;
2. `entry_linux_x86_64_gnu`;
3. `entry_linux_x86_64_musl`.

Поэтому один полный pack имеет одинаковый fingerprint на Windows, glibc и
musl, а изменение любого платформенного бинарника меняет совместимость для
всего pack. Нормализация строк manifest не выполняется: line endings и
комментарии тоже являются его байтами.

Лишний или отсутствующий `server`/`both` мод означает несовпадение. Чисто
клиентские моды сохраняются. Если нужные паки установлены, UI предлагает
включить серверный профиль; загрузка с сервера требует отдельного
подтверждения. Подробнее: [multiplayer.md](multiplayer.md).

## Доверие и сохранения

Нативная DLL/SO имеет права процесса и не изолирована песочницей. Устанавливайте
моды и загружайте их с серверов только из доверенных источников; SHA-256
проверяет целостность передачи, а не безопасность кода.

Данные мода сохраняются в `saves/<world>/moddata/`. Читайте их в Init и
записывайте при изменении или в Shutdown. Формат мира описан в
[world_format.md](world_format.md).

Встроенные примеры: `double_jump`, `daylight_lock`, `auto_bridge`,
`builder_lib` и `spiral_tower`; CMake генерирует manifest текущей платформы
и собирает их в runtime-каталог `mods/`.
