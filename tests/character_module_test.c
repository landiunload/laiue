#include "character/character_service.h"
#include "mod/module_host.h"
#include "platform/system.h"
#include "test_runtime.h"

#include <stdbool.h>
#include <string.h>

#if defined(_WIN32)
#define CHARACTER_PROVIDER_NAME L"laiue_character.dll"
#elif defined(__APPLE__)
#define CHARACTER_PROVIDER_NAME L"liblaiue_character.dylib"
#else
#define CHARACTER_PROVIDER_NAME L"liblaiue_character.so"
#endif

static void Expect(bool condition, const char *message)
{
    if (!condition)
    {
        LaiueTestRuntimeWrite(message);
        LaiueTestRuntimeWrite("\n");
        LaiueTestRuntimeExit(1);
    }
}

static bool Join(wchar_t output[LAIUE_PLATFORM_PATH_CAPACITY],
                 const wchar_t *root, const wchar_t *name)
{
    uint32_t index = 0u;
    while (root[index] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
    {
        output[index] = root[index];
        ++index;
    }
    if (root[index] != L'\0')
        return false;
    if (index != 0u && output[index - 1u] != L'/' && output[index - 1u] != L'\\')
        output[index++] = L'/';
    uint32_t nameIndex = 0u;
    while (name[nameIndex] != L'\0' && index + 1u < LAIUE_PLATFORM_PATH_CAPACITY)
        output[index++] = name[nameIndex++];
    if (name[nameIndex] != L'\0')
        return false;
    output[index] = L'\0';
    return true;
}

static uint32_t SweepFlatFloor(const LaiueCharacterCollisionV1 *collision,
                               const LaiueCharacterPositionV1 *position,
                               int64_t halfExtent, int64_t deltaX, int64_t deltaY,
                               int64_t deltaZ, LaiueCharacterPositionV1 *outPosition,
                               uint32_t *outGrounded)
{
    (void)collision;
    if (position == NULL || outPosition == NULL || outGrounded == NULL)
        return 0u;
    *outPosition = *position;
    outPosition->localX += deltaX;
    outPosition->localY += deltaY;
    outPosition->localZ += deltaZ;
    const int64_t floor = INT64_C(2000) + halfExtent;
    if (outPosition->localZ <= floor)
    {
        outPosition->localZ = floor;
        *outGrounded = 1u;
    }
    else
        *outGrounded = 0u;
    return 1u;
}

static void RunSequence(const LaiueCharacterServiceV1 *character,
                        LaiueCharacterControllerV1 *controller,
                        LaiueCharacterPositionV1 *outPosition)
{
    LaiueCharacterInputV1 input = {
        .moveX = 1,
        .moveY = -1,
        .flags = LAIUE_CHARACTER_INPUT_SPRINT,
    };
    for (uint32_t tick = 0u; tick < 256u; ++tick)
    {
        if (tick == 1u)
            input.flags |= LAIUE_CHARACTER_INPUT_JUMP;
        else
            input.flags &= ~LAIUE_CHARACTER_INPUT_JUMP;
        Expect(character->step(controller, &input) != 0u,
               "character step remains deterministic and in range");
    }
    Expect(character->getPosition(controller, outPosition) != 0u,
           "character position is readable");
}

LAIUE_TEST_ENTRY(CharacterModuleTestEntryPoint)
{
    static wchar_t directory[LAIUE_PLATFORM_PATH_CAPACITY];
    static wchar_t providerPath[LAIUE_PLATFORM_PATH_CAPACITY];
    static LaiueModuleHostConfigV1 config;
    static LaiueModuleDiagnostic diagnostic;

    Expect(PlatformExecutableDirectory(directory, LAIUE_PLATFORM_PATH_CAPACITY),
           "character test executable directory is available");
    Expect(Join(providerPath, directory, CHARACTER_PROVIDER_NAME),
           "character provider path fits");
    LaiueModuleHostConfigInitialize(&config);
    LaiueModuleHost *host = LaiueModuleHostCreate(&config, &diagnostic);
    Expect(host != NULL, "character module host creates");
    LaiueModuleBinaryV1 binary = {providerPath, 0u, NULL};
    Expect(LaiueModuleHostLoad(host, &binary, 1u, &diagnostic) == LAIUE_MODULE_OK,
           diagnostic.message);

    uint32_t version = 0u;
    uint32_t size = 0u;
    const LaiueCharacterServiceV1 *character =
        (const LaiueCharacterServiceV1 *)LaiueModuleHostQueryService(
            host, LAIUE_CHARACTER_SERVICE_NAME, LAIUE_CHARACTER_SERVICE_ABI_VERSION_1,
            sizeof(LaiueCharacterServiceV1), &version, &size);
    Expect(character != NULL && version == LAIUE_CHARACTER_SERVICE_ABI_VERSION_1 &&
               size >= sizeof(*character) && character->create != NULL &&
               character->step != NULL && character->destroy != NULL,
           "character service table is published");

    LaiueCharacterCollisionV1 collision = {
        .structSize = sizeof(collision),
        .abiVersion = LAIUE_CHARACTER_ABI_VERSION_1,
        .sweepAabb = SweepFlatFloor,
    };
    LaiueCharacterControllerV1 *controller = NULL;
    Expect(character->create(&collision, 500, &controller) != 0u && controller != NULL,
           "character controller creates");
    const LaiueCharacterPositionV1 initial = {
        .cellX = INT64_C(1) << 40,
        .cellY = -(INT64_C(1) << 39),
        .localX = LAIUE_CHARACTER_LOCAL_CELL_SIZE - 2,
        .localY = 0,
        .localZ = 2500,
    };
    Expect(character->setPosition(controller, &initial, 1u) != 0u,
           "character accepts infinite starting coordinates");
    LaiueCharacterPositionV1 first;
    RunSequence(character, controller, &first);
    Expect(character->setPosition(controller, &initial, 1u) != 0u,
           "character can be reset for replay");
    LaiueCharacterPositionV1 second;
    RunSequence(character, controller, &second);
    Expect(memcmp(&first, &second, sizeof(first)) == 0,
           "same input sequence produces identical character state");
    Expect(first.cellX == initial.cellX + 1,
           "horizontal rebasing crosses the cell boundary exactly");
    Expect(character->isGrounded(controller) != 0u,
           "floor collision restores grounded state");
    character->destroy(controller);

    LaiueModuleHostUnloadAll(host);
    Expect(LaiueModuleHostQueryService(host, LAIUE_CHARACTER_SERVICE_NAME, 1u, 1u,
                                       NULL, NULL) == NULL,
           "character service disappears after unload");
    LaiueModuleHostDestroy(host);
    LAIUE_TEST_SUCCESS();
}
