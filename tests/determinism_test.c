#include <windows.h>

#include "gameplay/player_controller.h"

// Харнесс-доказательство детерминизма физики. Прогоняет фиксированную ленту
// вводов через настоящий PlayerControllerUpdate и сворачивает состояние
// каждого тика в 64-битный хеш по точным битам double. Если один и тот же
// исходник, собранный разными компиляторами (MSVC и clang-cl) и в разных
// конфигурациях, даёт один и тот же хеш — симуляция побитово детерминирована,
// а значит по сети достаточно гонять вводы, а не состояние.
//
// Сим-модули (player_controller/jump/locomotion/stance, voxel_body)
// компилируются прямо в тест и закреплены за /fp:precise без FMA-контракции:
// глобальный /fp:fast переассоциирует выражения и сливает a*b+c в FMA, из-за
// чего результат зависит от CPU и компилятора.

// --- Вывод без CRT --------------------------------------------------------

static void TestWrite(const char* text)
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

static void TestWriteHex64(uint64_t value)
{
    char text[17];
    for (uint32_t i = 0; i < 16u; ++i)
    {
        uint32_t nibble = (uint32_t)((value >> ((15u - i) * 4u)) & 0xfu);
        text[i] = (char)(nibble < 10u ? '0' + nibble : 'a' + (nibble - 10u));
    }
    text[16] = '\0';
    TestWrite(text);
}

// --- Хеш по точным битам --------------------------------------------------

static uint64_t g_hash = 14695981039346656037ULL; // FNV-1a offset

static void HashBytes(const void* data, uint32_t size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    for (uint32_t i = 0; i < size; ++i)
    {
        g_hash ^= bytes[i];
        g_hash *= 1099511628211ULL; // FNV-1a prime
    }
}

static void HashDouble(double value)
{
    HashBytes(&value, sizeof(value));
}

// --- Детерминированный мир ------------------------------------------------

// Пол при z <= 0 и стена при x >= 6 (до z = 4). Игрок раз за разом врезается
// в стену на бегу и в прыжке — столкновения усиливают любое расхождение в
// младшем бите, поэтому длинный прогон ловит недетерминизм надёжнее гладкого.
static void QueryWorld(void* context, int64_t x, int64_t y, int64_t z,
    VoxelBlockPhysics* outBlock)
{
    (void)context;
    (void)y;
    bool solid = (z <= 0) || (x >= 6 && z <= 4);
    outBlock->flags = solid ? VOXEL_BLOCK_PHYSICS_SOLID : 0u;
    outBlock->friction = 0.6f;
}

// --- Лента вводов ---------------------------------------------------------

// Полностью детерминированный узор: целочисленные периоды, никаких
// вещественных часов и никакого рандома. Направления — либо оси, либо
// заранее нормализованная диагональ, чтобы не завязываться на sqrt-нормировку.
static void ScriptCommand(uint32_t tick, PlayerControllerCommand* command)
{
    static const double diagonal = 0.70710678118654752440;

    uint32_t phase = tick % 512u;
    if (phase < 128u)
    {
        command->movementX = 1.0;
        command->movementY = 0.0;
    }
    else if (phase < 256u)
    {
        command->movementX = diagonal;
        command->movementY = diagonal;
    }
    else if (phase < 384u)
    {
        command->movementX = 0.0;
        command->movementY = -1.0;
    }
    else
    {
        command->movementX = -diagonal;
        command->movementY = diagonal;
    }

    command->jumpHeld = (tick % 137u) < 24u;
    command->jumpPressed = (tick % 137u) == 0u;
    command->sprintHeld = (tick % 200u) < 100u;
    command->crouchHeld = (tick % 90u) < 30u;
}

// --- Прогон ---------------------------------------------------------------

#define DETERMINISM_TICKS 60000u

// Замороженный сценарий: точное побитовое ожидание для этой ленты вводов и
// этого конфига. Значение снято прогоном под MSVC и clang-cl (Debug/Release,
// /fp:fast и /fp:precise, с FMA/AVX2 и без) — все дали ровно этот хеш.
// Если он изменился — physics перестала воспроизводиться 1-в-1: ищи новую
// трансценденту в шаге симуляции, переставленное выражение или возврат
// /fp:fast с реально включённым FMA. Осознанная правка формул/сценария —
// пересними значение здесь.
#define DETERMINISM_EXPECTED_HASH 0x8ea8005f3c9f0821ULL

// Конфиг заморожен в тесте, а не берётся из PlayerControllerGetDefaultConfig:
// тюнинг геймплейных дефолтов не должен ронять проверку детерминизма. Это
// фиксированный эталонный вход, как и лента вводов ниже.
static void FrozenConfig(PlayerControllerConfig* config)
{
    const PlayerControllerConfig frozen = {
        .walkingSpeed = 4.0f,
        .sprintingSpeed = 6.0f,
        .crouchingSpeed = 3.5f,
        .groundAcceleration = 20.0f,
        .groundDeceleration = 30.0f,
        .airAcceleration = 4.0f,
        .sprintJumpSpeed = 8.0f,
        .gravity = 26.0f,
        .maximumFallSpeed = 55.0f,
        .jumpBufferSeconds = 0.14f,
        .coyoteTimeSeconds = 0.10f,
        .externalVelocityDamping = 8.0f,
        .fixedStepSeconds = 1.0f / 240.0f,
        .maximumSubsteps = 32u,
        .jumpHeight = 1.275,
        .radius = 0.30,
        .standingHeight = 1.80,
        .standingEyeHeight = 1.75,
        .crouchingHeight = 1.30,
        .crouchingEyeHeight = 1.25,
        .collisionEpsilon = 0.001,
        .groundProbeDepth = 0.03,
        .sneakProbeDepth = 0.60,
        .crouchEyeDuration = 0.175,
        .crouchColliderDuration = 0.200,
        .standColliderDuration = 0.200,
        .standEyeDuration = 0.250,
    };
    *config = frozen;
}

static void FoldState(const PlayerController* controller, const Camera* camera)
{
    HashDouble(camera->position[0]);
    HashDouble(camera->position[1]);
    HashDouble(camera->position[2]);
    HashDouble(controller->locomotion.velocityX);
    HashDouble(controller->locomotion.velocityY);
    HashDouble(controller->jump.verticalVelocity);
    HashDouble(controller->jump.jumpBufferRemaining);
    HashDouble(controller->jump.coyoteTimeRemaining);
    HashDouble(controller->externalVelocityX);
    HashDouble(controller->externalVelocityY);
    HashDouble(controller->stance.colliderCrouchProgress);
    HashDouble(controller->stance.eyeCrouchProgress);
    uint8_t grounded = controller->grounded ? 1u : 0u;
    HashBytes(&grounded, sizeof(grounded));
}

void DeterminismTestEntryPoint(void)
{
    PlayerControllerConfig config;
    FrozenConfig(&config);

    PlayerController controller;
    PlayerControllerInit(&controller, &config);

    Camera camera = { { 0.5, 0.5, 3.0 }, 0.0f, 0.0f };
    PlayerCollisionSource collision = { NULL, QueryWorld };

    // Ровно один фиксированный шаг на тик: подаём deltaSeconds == fixedStep,
    // и аккумулятор исполняет один SimulateStep. Число шагов не зависит от
    // настенных часов — это условие детерминизма поверх сети.
    float step = config.fixedStepSeconds;

    for (uint32_t tick = 0; tick < DETERMINISM_TICKS; ++tick)
    {
        PlayerControllerCommand command;
        ScriptCommand(tick, &command);
        PlayerControllerUpdate(&controller, &collision, &camera, &command, step);
        FoldState(&controller, &camera);
    }

    TestWrite("determinism-hash: ");
    TestWriteHex64(g_hash);
    TestWrite("\r\n");

#ifdef DETERMINISM_DIAGNOSTIC
    // Диагностика: подтверждает, что симуляция реально двигалась (иначе хеш
    // совпал бы тривиально). Печатаем биты финального состояния.
    union { double value; uint64_t bits; } view;
    for (uint32_t i = 0; i < 3u; ++i)
    {
        view.value = camera.position[i];
        TestWrite("pos "); TestWriteHex64(view.bits); TestWrite("\r\n");
    }
    view.value = controller.jump.verticalVelocity;
    TestWrite("vvel "); TestWriteHex64(view.bits); TestWrite("\r\n");
#endif

    if (g_hash != DETERMINISM_EXPECTED_HASH)
    {
        TestWrite("ДЕТЕРМИНИЗМ НАРУШЕН: ожидался ");
        TestWriteHex64(DETERMINISM_EXPECTED_HASH);
        TestWrite("\r\n");
        ExitProcess(1);
    }
    ExitProcess(0);
}
