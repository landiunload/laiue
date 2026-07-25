#include "test_runtime.h"

// Парсер является внутренней частью laiue_mod. Как и protocol_test,
// этот тест компилирует production source напрямую, чтобы не расширять
// публичный ABI DLL исключительно ради тестирования.
#include "../src/mod/mods.c"

static uint32_t modManifestTestChecks;

static void ModManifestTestWrite(const char *text)
{
    LaiueTestRuntimeWrite(text);
}

static void ModManifestTestExpect(bool condition, const char *name)
{
    ++modManifestTestChecks;
    if (condition)
    {
        return;
    }
    ModManifestTestWrite("Проверка не пройдена: ");
    ModManifestTestWrite(name);
    ModManifestTestWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static uint32_t AsciiLength(const char *text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

static bool WideEquals(const wchar_t *left, const wchar_t *right)
{
    uint32_t index = 0;
    while (left[index] != L'\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == right[index];
}

static bool ParseManifestForTest(const char *text, ModEntry *entry, ManifestParseState *state,
                                 uint32_t *formatVersion)
{
    memset(entry, 0, sizeof(*entry));
    memset(state, 0, sizeof(*state));
    state->entry = entry;
    state->valid = true;
    *formatVersion = 0;
    return ParseModFile((const uint8_t *)text, AsciiLength(text), ManifestCallback, state,
                        formatVersion);
}

static const ModEntry *FindPack(const ModsState *mods, const wchar_t *name)
{
    for (uint32_t i = 0; i < mods->count; ++i)
    {
        if (WideEquals(mods->entries[i].fileName, name))
            return &mods->entries[i];
    }
    return NULL;
}

static bool HashIsNonzero(const uint8_t hash[MODS_CONTENT_HASH_SIZE])
{
    uint8_t combined = 0;
    for (uint32_t i = 0; i < MODS_CONTENT_HASH_SIZE; ++i)
        combined |= hash[i];
    return combined != 0;
}

static void RemoveTestPack(const wchar_t *packName, const wchar_t *currentName,
                           const wchar_t *foreignName)
{
    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    ModManifestTestExpect(path != NULL, "не удалось выделить path");
    if (LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, MOD_PACK_MANIFEST_NAME, path,
                              LAIUE_CONTENT_PATH_CAPACITY))
        PlatformDeleteFile(path);
    if (LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, currentName, path,
                              LAIUE_CONTENT_PATH_CAPACITY))
        PlatformDeleteFile(path);
    if (LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, foreignName, path,
                              LAIUE_CONTENT_PATH_CAPACITY))
        PlatformDeleteFile(path);
    if (LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, NULL, path,
                              LAIUE_CONTENT_PATH_CAPACITY))
        PlatformRemoveDirectory(path);
    PlatformFree(path);
}

static void TestAllDeclaredArtifactsAffectHash(void)
{
    static const wchar_t packName[] = L"manifest_v2_test.lmp";
#if defined(_WIN32)
    static const wchar_t currentName[] = L"current.dll";
    static const wchar_t foreignName[] = L"foreign.so";
    static const char manifest[] = "LAIUE MOD 2\n"
                                   "id = test.manifest_v2\n"
                                   "name = Manifest v2 test\n"
                                   "version = 1.0\n"
                                   "side = both\n"
                                   "[native]\n"
                                   "entry_windows_x86_64 = current.dll\n"
                                   "entry_linux_x86_64_gnu = foreign.so\n"
                                   "api = 1\n";
#elif defined(LAIUE_LINUX_LIBC_MUSL)
    static const wchar_t currentName[] = L"current.so";
    static const wchar_t foreignName[] = L"foreign.dll";
    static const char manifest[] = "LAIUE MOD 2\n"
                                   "id = test.manifest_v2\n"
                                   "name = Manifest v2 test\n"
                                   "version = 1.0\n"
                                   "side = both\n"
                                   "[native]\n"
                                   "entry_windows_x86_64 = foreign.dll\n"
                                   "entry_linux_x86_64_musl = current.so\n"
                                   "api = 1\n";
#else
    static const wchar_t currentName[] = L"current.so";
    static const wchar_t foreignName[] = L"foreign.dll";
    static const char manifest[] = "LAIUE MOD 2\n"
                                   "id = test.manifest_v2\n"
                                   "name = Manifest v2 test\n"
                                   "version = 1.0\n"
                                   "side = both\n"
                                   "[native]\n"
                                   "entry_windows_x86_64 = foreign.dll\n"
                                   "entry_linux_x86_64_gnu = current.so\n"
                                   "api = 1\n";
#endif
    static const uint8_t currentBytes[] = {0x4c, 0x41, 0x49, 0x55, 0x45};
    static const uint8_t foreignBytesA[] = {0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x41};
    static const uint8_t foreignBytesB[] = {0x66, 0x6f, 0x72, 0x65, 0x69, 0x67, 0x6e, 0x42};

    ModManifestTestWrite("  cleanup\r\n");
    RemoveTestPack(packName, currentName, foreignName);

    wchar_t *path = PlatformAllocate((size_t)LAIUE_CONTENT_PATH_CAPACITY * sizeof(wchar_t), false);
    ModManifestTestExpect(path != NULL, "не удалось выделить path");
    ModManifestTestWrite("  directories\r\n");
    ModManifestTestExpect(
        LaiueContentBuildPath(LAIUE_CONTENT_MOD, NULL, NULL, path, LAIUE_CONTENT_PATH_CAPACITY) &&
            (PlatformPathExists(path) || PlatformCreateDirectory(path)),
        "не удалось подготовить mods");
    ModManifestTestExpect(LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, NULL, path,
                                                LAIUE_CONTENT_PATH_CAPACITY) &&
                              PlatformCreateDirectory(path),
                          "не удалось создать test pack");
    ModManifestTestExpect(LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName,
                                                MOD_PACK_MANIFEST_NAME, path,
                                                LAIUE_CONTENT_PATH_CAPACITY) &&
                              PlatformWriteEntireFile(path, manifest, sizeof(manifest) - 1U),
                          "не удалось записать manifest");
    ModManifestTestExpect(LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, currentName, path,
                                                LAIUE_CONTENT_PATH_CAPACITY) &&
                              PlatformWriteEntireFile(path, currentBytes, sizeof(currentBytes)),
                          "не удалось записать current artifact");

    ModManifestTestWrite("  incomplete refresh\r\n");
    ModsState *mods = PlatformAllocate(sizeof(*mods), true);
    ModManifestTestExpect(mods != NULL, "не удалось выделить ModsState");
    ModsInit(mods, L"manifest_test_enabled.txt");
    ModManifestTestWrite("    refresh call\r\n");
    ModsRefresh(mods);
    ModManifestTestWrite("    refresh returned\r\n");
    const ModEntry *entry = FindPack(mods, packName);
    ModManifestTestExpect(entry != NULL && !entry->compatible,
                          "pack с отсутствующим declared artifact совместим");

    ModManifestTestWrite("  complete refresh\r\n");
    ModManifestTestExpect(LaiueContentBuildPath(LAIUE_CONTENT_MOD_PACK, packName, foreignName, path,
                                                LAIUE_CONTENT_PATH_CAPACITY) &&
                              PlatformWriteEntireFile(path, foreignBytesA, sizeof(foreignBytesA)),
                          "не удалось записать foreign artifact");
    ModsRefresh(mods);
    entry = FindPack(mods, packName);
    ModManifestTestExpect(entry != NULL && entry->compatible, "полный platform pack несовместим");
    ModManifestTestExpect(HashIsNonzero(entry->contentHash), "content hash пуст");
    uint8_t firstHash[MODS_CONTENT_HASH_SIZE];
    memcpy(firstHash, entry->contentHash, sizeof(firstHash));

    ModManifestTestWrite("  changed refresh\r\n");
    ModManifestTestExpect(PlatformWriteEntireFile(path, foreignBytesB, sizeof(foreignBytesB)),
                          "не удалось изменить foreign artifact");
    ModsRefresh(mods);
    entry = FindPack(mods, packName);
    ModManifestTestExpect(entry != NULL && entry->compatible, "изменённый полный pack несовместим");
    ModManifestTestExpect(
        !PlatformConstantTimeEqual(firstHash, entry->contentHash, sizeof(firstHash)),
        "foreign artifact не влияет на content hash");

    ModManifestTestWrite("  final cleanup\r\n");
    RemoveTestPack(packName, currentName, foreignName);
    PlatformFree(mods);
    PlatformFree(path);
}

static void TestLegacyManifestIsWindowsOnly(void)
{
    static const char manifest[] = "LAIUE MOD 1\n"
                                   "id = example.legacy\n"
                                   "version = 1.0\n"
                                   "[native]\n"
                                   "entry = legacy.dll\n"
                                   "api = 1\n";
    ModEntry entry;
    ManifestParseState state;
    uint32_t version;
    ModManifestTestExpect(ParseManifestForTest(manifest, &entry, &state, &version),
                          "v1 не разобран");
    ModManifestTestExpect(version == 1u, "неверная версия v1");
    ModManifestTestExpect(ManifestNativeEntriesValid(&state, version), "legacy entry отклонён");
    ModManifestTestExpect(WideEquals(entry.entryWindowsX86_64, L"legacy.dll"),
                          "legacy entry не назначен Windows");
    ModManifestTestExpect(entry.entryLinuxX86_64Gnu[0] == L'\0' &&
                              entry.entryLinuxX86_64Musl[0] == L'\0',
                          "legacy entry протёк в Linux");
}

static void TestVersionTwoEntries(void)
{
    static const char manifest[] = "LAIUE MOD 2\n"
                                   "id = example.portable\n"
                                   "version = 2.0\n"
                                   "[native]\n"
                                   "entry_linux_x86_64_musl = mod.musl.so\n"
                                   "entry_windows_x86_64 = mod.dll\n"
                                   "entry_linux_x86_64_gnu = mod.gnu.so\n"
                                   "api = 1\n";
    ModEntry entry;
    ManifestParseState state;
    uint32_t version;
    ModManifestTestExpect(ParseManifestForTest(manifest, &entry, &state, &version),
                          "v2 не разобран");
    ModManifestTestExpect(ManifestNativeEntriesValid(&state, version),
                          "platform entries v2 отклонены");
    ModManifestTestExpect(WideEquals(entry.entryWindowsX86_64, L"mod.dll"),
                          "Windows entry потерян");
    ModManifestTestExpect(WideEquals(entry.entryLinuxX86_64Gnu, L"mod.gnu.so"),
                          "GNU entry потерян");
    ModManifestTestExpect(WideEquals(entry.entryLinuxX86_64Musl, L"mod.musl.so"),
                          "musl entry потерян");
}

static void TestAmbiguousEntriesRejected(void)
{
    static const char manifest[] = "LAIUE MOD 2\n"
                                   "[native]\n"
                                   "entry = legacy.dll\n"
                                   "entry_windows_x86_64 = explicit.dll\n"
                                   "api = 1\n";
    ModEntry entry;
    ManifestParseState state;
    uint32_t version;
    ModManifestTestExpect(ParseManifestForTest(manifest, &entry, &state, &version),
                          "не удалось разобрать неоднозначный manifest");
    ModManifestTestExpect(!ManifestNativeEntriesValid(&state, version),
                          "legacy и explicit Windows entries приняты вместе");
}

static void TestInvalidVersionsRejected(void)
{
    static const char fractional[] = "LAIUE MOD 1.5\n[native]\nentry = mod.dll\n";
    static const char future[] = "LAIUE MOD 3\n[native]\nentry_windows_x86_64 = mod.dll\n";
    ModEntry entry;
    ManifestParseState state;
    uint32_t version;
    ModManifestTestExpect(!ParseManifestForTest(fractional, &entry, &state, &version),
                          "дробная версия формата принята");
    ModManifestTestExpect(!ParseManifestForTest(future, &entry, &state, &version),
                          "неподдерживаемая будущая версия принята");
}

static void TestVersionOneRejectsVersionTwoKeys(void)
{
    static const char manifest[] = "LAIUE MOD 1\n"
                                   "[native]\n"
                                   "entry_linux_x86_64_gnu = mod.so\n"
                                   "api = 1\n";
    ModEntry entry;
    ManifestParseState state;
    uint32_t version;
    ModManifestTestExpect(ParseManifestForTest(manifest, &entry, &state, &version),
                          "v1 с новым ключом не разобран");
    ModManifestTestExpect(!ManifestNativeEntriesValid(&state, version), "v1 принял ключ из v2");
}

static void RunModManifestTests(void)
{
    ModManifestTestWrite("legacy manifest\r\n");
    TestLegacyManifestIsWindowsOnly();
    ModManifestTestWrite("platform entries\r\n");
    TestVersionTwoEntries();
    ModManifestTestWrite("ambiguous entries\r\n");
    TestAmbiguousEntriesRejected();
    ModManifestTestWrite("invalid versions\r\n");
    TestInvalidVersionsRejected();
    ModManifestTestWrite("version-key boundary\r\n");
    TestVersionOneRejectsVersionTwoKeys();
    ModManifestTestWrite("artifact hash\r\n");
    TestAllDeclaredArtifactsAffectHash();

    ModManifestTestWrite("Проверки manifest v2 пройдены\r\n");
}

LAIUE_TEST_ENTRY(ModManifestTestEntryPoint)
{
    RunModManifestTests();
    LAIUE_TEST_SUCCESS();
}
