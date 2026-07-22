#include <windows.h>

#include "content/content_format.h"

// Контракт пользовательского содержимого: какие имена паков и модов движок
// вообще соглашается принять. LaiueContentNameIsSafe — единственный барьер
// между именем из чужого архива и путём на диске: его результат подставляется
// в LaiueContentBuildPath и превращается в каталог рядом с исполняемым файлом.
// Поэтому здесь проверяются не «удобные» имена, а именно враждебные — выход
// вверх по дереву, разделители путей, устройства Windows и хвостовые точки,
// которые проводник молча срезает, а файловая система — нет.
//
// content_format.c компилируется в тест напрямую: правила именования — это
// внутренний контракт laiue_content, и расширять ABI DLL ради проверок нельзя
// (тот же приём, что в protocol_test и infinite_coord_test). LAIUE_BUILD_CONTENT
// переводит экспортные макросы в dllexport, чтобы функции определялись здесь,
// а не импортировались.

static uint32_t contentFormatTestChecks;

// Тест собирается без CRT: вывод — прямая запись в stdout.
static void ContentFormatTestWrite(const char* text)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE)
    {
        return;
    }
    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    DWORD written = 0;
    WriteFile(output, text, length, &written, NULL);
}

static void ContentFormatTestWriteNumber(uint32_t value)
{
    char text[11];
    uint32_t position = sizeof(text) - 1;
    text[position] = '\0';
    do
    {
        text[--position] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (value != 0 && position != 0);
    ContentFormatTestWrite(&text[position]);
}

// Имя проверки вместо номера: список растёт, перенумеровывать его при каждой
// вставке — верный способ разойтись с отчётом CTest.
static void ContentFormatTestExpect(bool condition, const char* name)
{
    ++contentFormatTestChecks;
    if (condition)
    {
        return;
    }
    ContentFormatTestWrite("Проверка не пройдена: ");
    ContentFormatTestWrite(name);
    ContentFormatTestWrite("\r\n");
    ExitProcess(1);
}

static bool WideEquals(const wchar_t* left, const wchar_t* right)
{
    uint32_t index = 0;
    while (left[index] != L'\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == right[index];
}

// --- Таблица форматов -----------------------------------------------------

static void TestFormatTable(void)
{
    const LaiueContentFormat* mod = LaiueContentFormatGet(LAIUE_CONTENT_MOD);
    ContentFormatTestExpect(mod != NULL, "формат мода не найден");
    ContentFormatTestExpect(WideEquals(mod->extension, L".lm"),
        "расширение мода не .lm");
    ContentFormatTestExpect(WideEquals(mod->directoryName, L"mods"),
        "каталог мода не mods");

    const LaiueContentFormat* shaderPack =
        LaiueContentFormatGet(LAIUE_CONTENT_SHADER_PACK);
    ContentFormatTestExpect(WideEquals(shaderPack->extension, L".lsp"),
        "расширение шейдерпака не .lsp");
    ContentFormatTestExpect(WideEquals(shaderPack->directoryName, L"shaders"),
        "каталог шейдерпака не shaders");
    // Шейдерпак — только каталог: файлом его подсовывать нельзя.
    ContentFormatTestExpect(
        shaderPack->storageMask == LAIUE_CONTENT_STORAGE_DIRECTORY,
        "шейдерпак принимается не только каталогом");

    const LaiueContentFormat* texturePack =
        LaiueContentFormatGet(LAIUE_CONTENT_TEXTURE_PACK);
    ContentFormatTestExpect(WideEquals(texturePack->extension, L".ltp"),
        "расширение текстурпака не .ltp");
    ContentFormatTestExpect(WideEquals(texturePack->directoryName, L"textures"),
        "каталог текстурпака не textures");

    // Границы перечисления: за пределами таблицы формата нет.
    ContentFormatTestExpect(
        LaiueContentFormatGet(LAIUE_CONTENT_TYPE_COUNT) == NULL,
        "LAIUE_CONTENT_TYPE_COUNT вернул формат");
    ContentFormatTestExpect(
        LaiueContentFormatGet((LaiueContentType)(LAIUE_CONTENT_TYPE_COUNT + 1))
            == NULL,
        "тип за границей таблицы вернул формат");
}

// Документированный порядок: чётный элемент — одиночная сущность, следующий
// за ним нечётный — её пак. Тест закрепляет это, чтобы вставка нового типа
// в середину не разъехалась с описанием контракта в content_format.h.
static void TestPackFlagFollowsTypeOrder(void)
{
    for (uint32_t type = 0; type < (uint32_t)LAIUE_CONTENT_TYPE_COUNT; ++type)
    {
        bool expectedPack = (type % 2u) != 0u;
        bool actualPack = LaiueContentTypeIsPack((LaiueContentType)type);
        ContentFormatTestExpect(actualPack == expectedPack,
            "признак пака не совпал с чётностью типа");

        const LaiueContentFormat* format =
            LaiueContentFormatGet((LaiueContentType)type);
        ContentFormatTestExpect(format != NULL && format->pack == expectedPack,
            "поле pack в таблице не совпало с чётностью типа");
    }

    ContentFormatTestExpect(
        !LaiueContentTypeIsPack(LAIUE_CONTENT_TYPE_COUNT),
        "несуществующий тип объявлен паком");
}

// --- Сопоставление расширений ---------------------------------------------

static void TestNameMatches(void)
{
    ContentFormatTestExpect(
        LaiueContentNameMatches(LAIUE_CONTENT_MOD, L"cool.lm"),
        "cool.lm не опознан как мод");
    // Регистр расширения не важен: архивы приходят с любым.
    ContentFormatTestExpect(
        LaiueContentNameMatches(LAIUE_CONTENT_MOD, L"cool.LM"),
        "cool.LM не опознан как мод");
    ContentFormatTestExpect(
        LaiueContentNameMatches(LAIUE_CONTENT_MOD_PACK, L"cool.lmp"),
        "cool.lmp не опознан как модпак");
    ContentFormatTestExpect(
        LaiueContentNameMatches(LAIUE_CONTENT_SHADER_PACK, L"MyShaders.lsp"),
        "MyShaders.lsp не опознан как шейдерпак");

    // Соседние форматы не должны перекрываться: .lm и .lmp различаются.
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_MOD, L"cool.lmp"),
        "модпак ошибочно опознан как мод");
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_MOD_PACK, L"cool.lm"),
        "мод ошибочно опознан как модпак");
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_SHADER, L"pack.lsp"),
        "шейдерпак ошибочно опознан как шейдер");
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_TEXTURE, L"pack.ltp"),
        "текстурпак ошибочно опознан как текстура");

    // Голое расширение без имени — не файл содержимого.
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_MOD, L".lm"),
        "голое .lm принято за мод");
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_MOD, L""),
        "пустое имя принято за мод");
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_MOD, NULL),
        "NULL принят за мод");
    ContentFormatTestExpect(
        !LaiueContentNameMatches(LAIUE_CONTENT_TYPE_COUNT, L"cool.lm"),
        "несуществующий тип что-то опознал");
}

// --- Безопасность имени: что должно приниматься ---------------------------

static void TestNameIsSafeAccepts(void)
{
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"MyPack"),
        "обычное имя отвергнуто");
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"a"),
        "имя из одного символа отвергнуто");
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"pack.lsp"),
        "имя с расширением отвергнуто");
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"my pack"),
        "внутренний пробел отвергнут");
    // Unicode разрешён явно — движок не ограничивает автора латиницей.
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"мой пак"),
        "кириллица отвергнута");
    ContentFormatTestExpect(LaiueContentNameIsSafe(L".hidden"),
        "имя с ведущей точкой отвергнуто");

    // Похожие на устройства, но не устройства: цифра вне 1..9, лишний символ
    // и лишняя цифра выводят имя из-под запрета.
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"COM0"),
        "COM0 ошибочно принят за устройство");
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"COM10"),
        "COM10 ошибочно принят за устройство");
    ContentFormatTestExpect(LaiueContentNameIsSafe(L"CONS"),
        "CONS ошибочно принят за устройство");
}

// --- Безопасность имени: выход за каталог ---------------------------------

static void TestNameIsSafeRejectsTraversal(void)
{
    ContentFormatTestExpect(!LaiueContentNameIsSafe(NULL),
        "NULL признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L""),
        "пустое имя признано безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"."),
        "текущий каталог признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L".."),
        "родительский каталог признан безопасным");

    // Разделители в любом виде: с ними имя перестаёт быть одним элементом.
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"..\\windows"),
        "выход вверх обратным слэшем признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"../windows"),
        "выход вверх прямым слэшем признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"sub\\pack"),
        "вложенный путь признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"sub/pack"),
        "вложенный путь через слэш признан безопасным");

    // Двоеточие — это диск или альтернативный поток NTFS.
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"C:"),
        "имя диска признано безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"C:\\windows"),
        "абсолютный путь признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pack:stream"),
        "поток NTFS признан безопасным");
}

// --- Безопасность имени: причуды файловой системы Windows -----------------

static void TestNameIsSafeRejectsWindowsQuirks(void)
{
    // Хвостовые точки и пробелы Windows молча срезает при создании файла:
    // «pack.» и «pack» указывали бы на один каталог.
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pack."),
        "хвостовая точка признана безопасной");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pack "),
        "хвостовой пробел признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L" pack"),
        "ведущий пробел признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"..."),
        "имя из одних точек признано безопасным");

    // Подстановочные знаки и прочие запрещённые символы.
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa*ck"),
        "звёздочка признана безопасной");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa?ck"),
        "вопросительный знак признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa\"ck"),
        "кавычка признана безопасной");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa<ck"),
        "знак меньше признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa>ck"),
        "знак больше признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa|ck"),
        "вертикальная черта признана безопасной");

    // Управляющие символы: граница диапазона — 0x20.
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa\x01" L"ck"),
        "управляющий символ признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"pa\x1f" L"ck"),
        "символ 0x1F признан безопасным");
}

// --- Безопасность имени: зарезервированные устройства ---------------------

static void TestNameIsSafeRejectsDeviceNames(void)
{
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"CON"),
        "CON признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"con"),
        "con в нижнем регистре признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"PRN"),
        "PRN признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"AUX"),
        "AUX признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"NUL"),
        "NUL признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"COM1"),
        "COM1 признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"COM9"),
        "COM9 признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"LPT1"),
        "LPT1 признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"LPT9"),
        "LPT9 признан безопасным");

    // Расширение не спасает: CON.txt открывает то же устройство.
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"CON.txt"),
        "CON.txt признан безопасным");
    ContentFormatTestExpect(!LaiueContentNameIsSafe(L"com1.lm"),
        "com1.lm признан безопасным");
}

void ContentFormatTestEntryPoint(void)
{
    TestFormatTable();
    TestPackFlagFollowsTypeOrder();
    TestNameMatches();
    TestNameIsSafeAccepts();
    TestNameIsSafeRejectsTraversal();
    TestNameIsSafeRejectsWindowsQuirks();
    TestNameIsSafeRejectsDeviceNames();

    ContentFormatTestWrite("Проверок пройдено: ");
    ContentFormatTestWriteNumber(contentFormatTestChecks);
    ContentFormatTestWrite("\r\n");
    ExitProcess(0);
}
