#include <string.h>

#include "network/network.h"
#include "network/protocol.h"
#include "test_runtime.h"

// Кодек протокола — чистая логика без сокетов и Windows-состояния, поэтому
// protocol.c компилируется прямо в тест. Экспортировать его из laiue_network
// ради проверок нельзя: это расширило бы ABI DLL без второго потребителя.

static uint32_t protocolTestChecks;

static void ProtocolTestWrite(const char* text)
{
    LaiueTestRuntimeWrite(text);
}

static void ProtocolTestWriteNumber(uint32_t value)
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

    ProtocolTestWrite(&text[position]);
}

// Имя проверки вместо номера: список растёт, а перенумеровывать его при
// каждой вставке — верный способ разойтись с текстом отчёта CTest.
static void ProtocolTestExpect(bool condition, const char* name)
{
    ++protocolTestChecks;
    if (condition)
    {
        return;
    }

    ProtocolTestWrite("Проверка не пройдена: ");
    ProtocolTestWrite(name);
    ProtocolTestWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static void TestWriteU16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void TestWriteU32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void TestWriteU64(uint8_t* output, uint64_t value)
{
    TestWriteU32(output, (uint32_t)value);
    TestWriteU32(output + 4, (uint32_t)(value >> 32));
}

static bool NearlyEqual(float actual, float expected, float tolerance)
{
    float difference = actual - expected;
    if (difference < 0.0f)
    {
        difference = -difference;
    }
    return difference <= tolerance;
}

// Позиции идут по проводу как биты double, поэтому проверяется точное
// совпадение представления, а не приблизительное равенство.
static bool ExactlyEqualDouble(double left, double right)
{
    union
    {
        double floating;
        uint64_t integer;
    } leftBits, rightBits;

    leftBits.floating = left;
    rightBits.floating = right;
    return leftBits.integer == rightBits.integer;
}

static void CopyAscii(char* destination, uint32_t capacity, const char* text)
{
    uint32_t index = 0;
    while (index + 1u < capacity && text[index] != '\0')
    {
        destination[index] = text[index];
        ++index;
    }
    destination[index] = '\0';
}

static bool EqualAscii(const char* left, const char* right)
{
    uint32_t index = 0;
    while (left[index] != '\0' && left[index] == right[index])
    {
        ++index;
    }
    return left[index] == right[index];
}

static void BuildHeader(uint8_t* bytes, uint16_t type,
    uint32_t payloadSize, uint32_t sequence)
{
    TestWriteU32(bytes, LAIUE_PROTOCOL_MAGIC);
    TestWriteU16(bytes + 4, (uint16_t)LAIUE_PROTOCOL_VERSION);
    TestWriteU16(bytes + 6, type);
    TestWriteU32(bytes + 8, payloadSize);
    TestWriteU32(bytes + 12, sequence);
}

// Каждый объявленный тип сообщения обязан проходить и запись, и чтение
// заголовка. Регрессия: типы дропа, инвентаря и выбора слота добавили после
// SERVER_CONTENT_END, а границу проверки оставили на нём. WriteHeader
// возвращал 0, ChannelQueuePayload — false, и вызывающий код рвал соединение
// как OVERFLOW вместо отправки. Цикл до LAIUE_MESSAGE_COUNT покрывает и те
// типы, которые добавят позже.
static void TestHeaderAcceptsEveryDeclaredType(void)
{
    for (uint32_t type = LAIUE_MESSAGE_CLIENT_HELLO;
         type < (uint32_t)LAIUE_MESSAGE_COUNT; ++type)
    {
        uint8_t bytes[LAIUE_PROTOCOL_HEADER_SIZE];
        uint32_t written = LaiueProtocolWriteHeader(bytes, sizeof(bytes),
            (LaiueMessageType)type, 1u, 0u);
        ProtocolTestExpect(written == LAIUE_PROTOCOL_HEADER_SIZE,
            "WriteHeader отверг объявленный тип сообщения");

        LaiueProtocolFrame frame;
        ProtocolTestExpect(
            LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
            "ReadHeader отверг объявленный тип сообщения");
        ProtocolTestExpect((uint32_t)frame.type == type,
            "ReadHeader вернул чужой тип сообщения");
    }
}

static void TestHeaderRoundTripAndRejections(void)
{
    uint8_t bytes[LAIUE_PROTOCOL_HEADER_SIZE];
    LaiueProtocolFrame frame;

    ProtocolTestExpect(
        LaiueProtocolWriteHeader(bytes, sizeof(bytes),
            LAIUE_MESSAGE_PLAYER_INPUT, 77u, 9u)
        == LAIUE_PROTOCOL_HEADER_SIZE,
        "WriteHeader не записал корректный заголовок");
    ProtocolTestExpect(LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader не принял собственный заголовок");
    ProtocolTestExpect(frame.type == LAIUE_MESSAGE_PLAYER_INPUT
        && frame.sequence == 77u && frame.payloadSize == 9u
        && frame.payload == bytes + LAIUE_PROTOCOL_HEADER_SIZE,
        "ReadHeader вернул неверные поля");

    ProtocolTestExpect(
        LaiueProtocolWriteHeader(bytes, sizeof(bytes),
            LAIUE_MESSAGE_PLAYER_INPUT, 0u, 0u) == 0,
        "WriteHeader принял нулевой sequence");
    ProtocolTestExpect(
        LaiueProtocolWriteHeader(bytes, sizeof(bytes),
            (LaiueMessageType)0, 1u, 0u) == 0,
        "WriteHeader принял тип 0");
    ProtocolTestExpect(
        LaiueProtocolWriteHeader(bytes, sizeof(bytes),
            (LaiueMessageType)LAIUE_MESSAGE_COUNT, 1u, 0u) == 0,
        "WriteHeader принял тип за границей диапазона");
    ProtocolTestExpect(
        LaiueProtocolWriteHeader(bytes, sizeof(bytes),
            LAIUE_MESSAGE_PLAYER_INPUT, 1u,
            LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE + 1u) == 0,
        "WriteHeader принял payload сверх предела");
    ProtocolTestExpect(
        LaiueProtocolWriteHeader(bytes, LAIUE_PROTOCOL_HEADER_SIZE - 1u,
            LAIUE_MESSAGE_PLAYER_INPUT, 1u, 0u) == 0,
        "WriteHeader принял недостаточную ёмкость");

    BuildHeader(bytes, (uint16_t)LAIUE_MESSAGE_PLAYER_INPUT, 9u, 1u);
    ProtocolTestExpect(
        !LaiueProtocolReadHeader(bytes, LAIUE_PROTOCOL_HEADER_SIZE - 1u, &frame),
        "ReadHeader принял усечённый заголовок");

    BuildHeader(bytes, (uint16_t)LAIUE_MESSAGE_PLAYER_INPUT, 9u, 1u);
    TestWriteU32(bytes, LAIUE_PROTOCOL_MAGIC ^ 1u);
    ProtocolTestExpect(!LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader принял чужой magic");

    BuildHeader(bytes, (uint16_t)LAIUE_MESSAGE_PLAYER_INPUT, 9u, 1u);
    TestWriteU16(bytes + 4, (uint16_t)(LAIUE_PROTOCOL_VERSION - 1u));
    ProtocolTestExpect(!LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader принял чужую версию протокола");

    BuildHeader(bytes, (uint16_t)LAIUE_MESSAGE_COUNT, 9u, 1u);
    ProtocolTestExpect(!LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader принял неизвестный тип");

    BuildHeader(bytes, 0u, 9u, 1u);
    ProtocolTestExpect(!LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader принял тип 0");

    BuildHeader(bytes, (uint16_t)LAIUE_MESSAGE_PLAYER_INPUT,
        LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE + 1u, 1u);
    ProtocolTestExpect(!LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader принял payload сверх предела");

    BuildHeader(bytes, (uint16_t)LAIUE_MESSAGE_PLAYER_INPUT, 9u, 0u);
    ProtocolTestExpect(!LaiueProtocolReadHeader(bytes, sizeof(bytes), &frame),
        "ReadHeader принял нулевой sequence");
}

static void TestWriteFrame(void)
{
    uint8_t frameBytes[LAIUE_PROTOCOL_HEADER_SIZE + 4u];
    const uint8_t payload[4] = { 1u, 2u, 3u, 4u };

    ProtocolTestExpect(
        LaiueProtocolWriteFrame(frameBytes, sizeof(frameBytes),
            LAIUE_MESSAGE_BLOCK_DELTA, 3u, payload, sizeof(payload))
        == sizeof(frameBytes),
        "WriteFrame не уложился в точную ёмкость");
    ProtocolTestExpect(
        memcmp(frameBytes + LAIUE_PROTOCOL_HEADER_SIZE, payload,
            sizeof(payload)) == 0,
        "WriteFrame записал payload неверно");

    ProtocolTestExpect(
        LaiueProtocolWriteFrame(frameBytes, sizeof(frameBytes) - 1u,
            LAIUE_MESSAGE_BLOCK_DELTA, 3u, payload, sizeof(payload)) == 0,
        "WriteFrame принял ёмкость на байт меньше кадра");
    ProtocolTestExpect(
        LaiueProtocolWriteFrame(frameBytes, sizeof(frameBytes),
            LAIUE_MESSAGE_BLOCK_DELTA, 3u, NULL, sizeof(payload)) == 0,
        "WriteFrame принял NULL при ненулевом размере");
    ProtocolTestExpect(
        LaiueProtocolWriteFrame(frameBytes, sizeof(frameBytes),
            LAIUE_MESSAGE_SERVER_CONTENT_END, 3u, NULL, 0u)
        == LAIUE_PROTOCOL_HEADER_SIZE,
        "WriteFrame не записал кадр без payload");
}

static void TestHelloAndWelcome(void)
{
    uint8_t payload[32];
    uint64_t nonce = 0;

    ProtocolTestExpect(
        LaiueProtocolEncodeHello(payload, sizeof(payload),
            0x0123456789abcdefULL) == 12u,
        "EncodeHello вернул неожиданный размер");
    ProtocolTestExpect(
        LaiueProtocolDecodeHello(payload, 12u, &nonce)
        && nonce == 0x0123456789abcdefULL,
        "DecodeHello не восстановил nonce");
    ProtocolTestExpect(!LaiueProtocolDecodeHello(payload, 11u, &nonce)
        && !LaiueProtocolDecodeHello(payload, 13u, &nonce),
        "DecodeHello принял неточный размер");

    // Нулевой nonce не годится для подтверждения сессии.
    TestWriteU64(payload + 4, 0u);
    ProtocolTestExpect(!LaiueProtocolDecodeHello(payload, 12u, &nonce),
        "DecodeHello принял нулевой nonce");

    TestWriteU16(payload, (uint16_t)(LAIUE_VERSION_MAJOR + 1));
    TestWriteU64(payload + 4, 1u);
    ProtocolTestExpect(!LaiueProtocolDecodeHello(payload, 12u, &nonce),
        "DecodeHello принял чужую версию игры");

    uint32_t peerId = 0;
    int64_t worldSeed = 0;
    uint64_t clientNonce = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeWelcome(payload, sizeof(payload), 7u,
            -1234567890123LL, 0xfeedfacecafebeefULL) == 20u,
        "EncodeWelcome вернул неожиданный размер");
    ProtocolTestExpect(
        LaiueProtocolDecodeWelcome(payload, 20u, &peerId, &worldSeed,
            &clientNonce)
        && peerId == 7u && worldSeed == -1234567890123LL
        && clientNonce == 0xfeedfacecafebeefULL,
        "DecodeWelcome не восстановил поля");
    ProtocolTestExpect(
        LaiueProtocolEncodeWelcome(payload, sizeof(payload), 0u, 1LL, 1ULL) == 0,
        "EncodeWelcome принял нулевой peerId");
    ProtocolTestExpect(
        LaiueProtocolEncodeWelcome(payload, sizeof(payload), 1u, 1LL, 0ULL) == 0,
        "EncodeWelcome принял нулевой nonce");
    ProtocolTestExpect(
        !LaiueProtocolDecodeWelcome(payload, 19u, &peerId, &worldSeed,
            &clientNonce),
        "DecodeWelcome принял неточный размер");
}

static void TestModNegotiation(void)
{
    uint8_t payload[128];
    uint32_t count = 0;
    uint8_t flags = 0;

    ProtocolTestExpect(
        LaiueProtocolEncodeModList(payload, sizeof(payload),
            LAIUE_PROTOCOL_MAX_MODS, 0x03u) == 3u,
        "EncodeModList вернул неожиданный размер");
    ProtocolTestExpect(
        LaiueProtocolDecodeModList(payload, 3u, &count, &flags)
        && count == LAIUE_PROTOCOL_MAX_MODS && flags == 0x03u,
        "DecodeModList не восстановил поля");
    ProtocolTestExpect(
        LaiueProtocolEncodeModList(payload, sizeof(payload),
            LAIUE_PROTOCOL_MAX_MODS + 1u, 0u) == 0,
        "EncodeModList принял список сверх предела");

    TestWriteU16(payload, (uint16_t)(LAIUE_PROTOCOL_MAX_MODS + 1u));
    ProtocolTestExpect(!LaiueProtocolDecodeModList(payload, 3u, &count, &flags),
        "DecodeModList принял список сверх предела");
    ProtocolTestExpect(!LaiueProtocolDecodeModList(payload, 4u, &count, &flags),
        "DecodeModList принял неточный размер");

    LaiueProtocolMod mod;
    memset(&mod, 0, sizeof(mod));
    CopyAscii(mod.id, sizeof(mod.id), "core.blocks");
    CopyAscii(mod.version, sizeof(mod.version), "1.0.0");
    for (uint32_t i = 0; i < LAIUE_PROTOCOL_MOD_HASH_SIZE; ++i)
    {
        mod.contentHash[i] = (uint8_t)(i + 1u);
    }

    const uint32_t expectedSize = 2u + LAIUE_PROTOCOL_MOD_HASH_SIZE + 11u + 5u;
    uint32_t size = LaiueProtocolEncodeMod(payload, sizeof(payload), &mod);
    ProtocolTestExpect(size == expectedSize,
        "EncodeMod вернул неожиданный размер");

    LaiueProtocolMod decoded;
    ProtocolTestExpect(LaiueProtocolDecodeMod(payload, size, &decoded),
        "DecodeMod отверг собственную запись");
    ProtocolTestExpect(EqualAscii(decoded.id, mod.id)
        && EqualAscii(decoded.version, mod.version)
        && memcmp(decoded.contentHash, mod.contentHash,
            LAIUE_PROTOCOL_MOD_HASH_SIZE) == 0,
        "DecodeMod не восстановил поля");

    ProtocolTestExpect(!LaiueProtocolDecodeMod(payload, size - 1u, &decoded)
        && !LaiueProtocolDecodeMod(payload, size + 1u, &decoded),
        "DecodeMod принял неточный размер");
    ProtocolTestExpect(LaiueProtocolEncodeMod(payload, size - 1u, &mod) == 0,
        "EncodeMod принял недостаточную ёмкость");

    // Идентификатор мода — только строчные буквы, цифры и «._-»: заглавные
    // сделали бы совпадение наборов модов зависимым от регистра.
    LaiueProtocolMod invalid = mod;
    CopyAscii(invalid.id, sizeof(invalid.id), "Core.Blocks");
    ProtocolTestExpect(
        LaiueProtocolEncodeMod(payload, sizeof(payload), &invalid) == 0,
        "EncodeMod принял заглавные буквы в идентификаторе");

    invalid = mod;
    memset(invalid.contentHash, 0, sizeof(invalid.contentHash));
    ProtocolTestExpect(
        LaiueProtocolEncodeMod(payload, sizeof(payload), &invalid) == 0,
        "EncodeMod принял нулевой хеш содержимого");

    invalid = mod;
    invalid.id[0] = '\0';
    ProtocolTestExpect(
        LaiueProtocolEncodeMod(payload, sizeof(payload), &invalid) == 0,
        "EncodeMod принял пустой идентификатор");

    uint8_t reason = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeReject(payload, sizeof(payload), 5u) == 1u,
        "EncodeReject вернул неожиданный размер");
    ProtocolTestExpect(LaiueProtocolDecodeReject(payload, 1u, &reason)
        && reason == 5u,
        "DecodeReject не восстановил причину");
    ProtocolTestExpect(
        LaiueProtocolEncodeReject(payload, sizeof(payload), 0u) == 0,
        "EncodeReject принял нулевую причину");
    payload[0] = 0u;
    ProtocolTestExpect(!LaiueProtocolDecodeReject(payload, 1u, &reason),
        "DecodeReject принял нулевую причину");
}

static void TestContentBegin(void)
{
    uint8_t payload[64];
    uint8_t hash[LAIUE_PROTOCOL_CONTENT_HASH_SIZE];
    for (uint32_t i = 0; i < sizeof(hash); ++i)
    {
        hash[i] = (uint8_t)(0xa0u + i);
    }

    ProtocolTestExpect(
        LaiueProtocolEncodeContentBegin(payload, sizeof(payload),
            123456789ULL, hash) == 40u,
        "EncodeContentBegin вернул неожиданный размер");

    uint64_t size = 0;
    uint8_t decodedHash[LAIUE_PROTOCOL_CONTENT_HASH_SIZE];
    ProtocolTestExpect(
        LaiueProtocolDecodeContentBegin(payload, 40u, &size, decodedHash)
        && size == 123456789ULL
        && memcmp(decodedHash, hash, sizeof(hash)) == 0,
        "DecodeContentBegin не восстановил поля");
    ProtocolTestExpect(
        LaiueProtocolEncodeContentBegin(payload, sizeof(payload), 0ULL, hash) == 0,
        "EncodeContentBegin принял нулевой размер");
    ProtocolTestExpect(
        !LaiueProtocolDecodeContentBegin(payload, 39u, &size, decodedHash),
        "DecodeContentBegin принял неточный размер");

    TestWriteU64(payload, 0u);
    ProtocolTestExpect(
        !LaiueProtocolDecodeContentBegin(payload, 40u, &size, decodedHash),
        "DecodeContentBegin принял нулевой размер содержимого");
}

static void TestInput(void)
{
    uint8_t payload[16];
    LaiueProtocolInput input = {
        .sequence = 73u,
        .movementX = 0.5f,
        .movementY = -0.5f,
        .yaw = 1.0f,
        .pitch = 0.5f,
        .jumpPressed = true,
        .jumpHeld = false,
        .sprintHeld = true,
        .crouchHeld = false,
        .useHeld = true,
    };

    ProtocolTestExpect(
        LaiueProtocolEncodeInput(payload, sizeof(payload), &input) == 13u,
        "EncodeInput вернул неожиданный размер");

    LaiueProtocolInput decoded;
    ProtocolTestExpect(LaiueProtocolDecodeInput(payload, 13u, &decoded),
        "DecodeInput отверг собственную запись");
    // Допуск — шаг квантования: движение 1/32767, углы — предел/32767.
    ProtocolTestExpect(NearlyEqual(decoded.movementX, 0.5f, 1.0e-4f)
        && NearlyEqual(decoded.movementY, -0.5f, 1.0e-4f)
        && NearlyEqual(decoded.yaw, 1.0f, 2.0e-4f)
        && NearlyEqual(decoded.pitch, 0.5f, 2.0e-4f),
        "DecodeInput потерял точность сверх шага квантования");
    ProtocolTestExpect(decoded.sequence == 73u
        && decoded.jumpPressed && !decoded.jumpHeld
        && decoded.sprintHeld && !decoded.crouchHeld && decoded.useHeld,
        "DecodeInput потерял sequence или перепутал флаги");

    ProtocolTestExpect(!LaiueProtocolDecodeInput(payload, 12u, &decoded)
        && !LaiueProtocolDecodeInput(payload, 14u, &decoded),
        "DecodeInput принял неточный размер");

    // Старшие три бита зарезервированы и обязаны быть нулевыми.
    uint8_t reserved[13];
    memcpy(reserved, payload, sizeof(reserved));
    reserved[12] |= 0x20u;
    ProtocolTestExpect(!LaiueProtocolDecodeInput(reserved, 13u, &decoded),
        "DecodeInput принял установленные резервные биты");

    memcpy(reserved, payload, sizeof(reserved));
    TestWriteU32(reserved, 0u);
    ProtocolTestExpect(!LaiueProtocolDecodeInput(reserved, 13u, &decoded),
        "DecodeInput принял зарезервированный нулевой sequence");

    LaiueProtocolInput outOfRange = input;
    outOfRange.movementX = 1.5f;
    ProtocolTestExpect(
        LaiueProtocolEncodeInput(payload, sizeof(payload), &outOfRange) == 0,
        "EncodeInput принял движение вне диапазона");

    outOfRange = input;
    outOfRange.yaw = 4.0f;
    ProtocolTestExpect(
        LaiueProtocolEncodeInput(payload, sizeof(payload), &outOfRange) == 0,
        "EncodeInput принял рыскание вне диапазона");

    // Вектор длиннее единицы — попытка ускориться по диагонали.
    uint8_t forged[13];
    memset(forged, 0, sizeof(forged));
    TestWriteU32(forged, 1u);
    TestWriteU16(forged + 4, 32767u);
    TestWriteU16(forged + 6, 32767u);
    ProtocolTestExpect(!LaiueProtocolDecodeInput(forged, 13u, &decoded),
        "DecodeInput принял вектор движения длиннее единицы");
    // 23265/32767 ~= 0.7100: squared length is about 1.0082. The old
    // 1.01 tolerance accepted this crafted diagonal and granted ~0.4% speed.
    TestWriteU16(forged + 4, 23265u);
    TestWriteU16(forged + 6, 23265u);
    ProtocolTestExpect(!LaiueProtocolDecodeInput(forged, 13u, &decoded),
        "DecodeInput принял crafted diagonal сверх quantization tolerance");

    NetworkInputCommand command = {
        .sequence = 73u,
        .movementX = 0.5f,
        .movementY = -0.5f,
        .yaw = 1.0f,
        .pitch = 0.5f,
        .jumpPressed = true,
        .sprintHeld = true,
        .useHeld = true,
    };
    NetworkInputCommand canonical;
    ProtocolTestExpect(
        NetworkInputCanonicalize(&command, &canonical)
        && canonical.sequence == 73u
        && NearlyEqual(canonical.movementX, 0.5f, 1.0e-4f)
        && NearlyEqual(canonical.movementY, -0.5f, 1.0e-4f)
        && NearlyEqual(canonical.yaw, 1.0f, 2.0e-4f)
        && NearlyEqual(canonical.pitch, 0.5f, 2.0e-4f)
        && canonical.jumpPressed && canonical.sprintHeld
        && canonical.useHeld,
        "NetworkInputCanonicalize не повторил wire-квантование");
    ProtocolTestExpect(
        NetworkInputCanonicalize(&canonical, &canonical)
        && canonical.sequence == 73u,
        "NetworkInputCanonicalize не поддержал in-place canonical input");
    command.sequence = 0u;
    ProtocolTestExpect(
        !NetworkInputCanonicalize(&command, &canonical),
        "NetworkInputCanonicalize принял нулевой sequence");
}

static void TestEditIntent(void)
{
    uint8_t payload[16];
    const float down[3] = { 0.0f, 0.0f, -1.0f };
    const float east[3] = { 1.0f, 0.0f, 0.0f };
    bool breakBlock = false;
    bool placeBlock = false;
    uint8_t placementBlock = 0;
    float direction[3];

    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), true, false,
            0u, down) == 8u,
        "EncodeEditIntent вернул неожиданный размер для ломания");
    ProtocolTestExpect(
        LaiueProtocolDecodeEditIntent(payload, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction)
        && breakBlock && !placeBlock && placementBlock == 0u
        && NearlyEqual(direction[2], -1.0f, 1.0e-4f),
        "DecodeEditIntent не восстановил ломание");

    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), false, true,
            LAIUE_PROTOCOL_MAX_INVENTORY_ITEM, east) == 8u,
        "EncodeEditIntent вернул неожиданный размер для установки");
    ProtocolTestExpect(
        LaiueProtocolDecodeEditIntent(payload, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction)
        && !breakBlock && placeBlock
        && placementBlock == LAIUE_PROTOCOL_MAX_INVENTORY_ITEM
        && NearlyEqual(direction[0], 1.0f, 1.0e-4f),
        "DecodeEditIntent не восстановил установку");

    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), true, true,
            1u, down) == 0,
        "EncodeEditIntent принял одновременно ломание и установку");
    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), false, false,
            0u, down) == 0,
        "EncodeEditIntent принял намерение без действия");
    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), false, true,
            LAIUE_PROTOCOL_MAX_INVENTORY_ITEM + 1U, down) == 0,
        "EncodeEditIntent принял неизвестный блок для установки");
    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), false, true,
            0u, down) == 0,
        "EncodeEditIntent принял установку без блока");

    const float halfLength[3] = { 0.5f, 0.0f, 0.0f };
    ProtocolTestExpect(
        LaiueProtocolEncodeEditIntent(payload, sizeof(payload), true, false,
            0u, halfLength) == 0,
        "EncodeEditIntent принял неединичное направление");

    uint8_t forged[8];
    memset(forged, 0, sizeof(forged));
    forged[0] = 1u;
    TestWriteU16(forged + 5, (uint16_t)(int16_t)-32767);
    forged[7] = 1u;
    ProtocolTestExpect(
        !LaiueProtocolDecodeEditIntent(forged, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction),
        "DecodeEditIntent принял ломание с указанным блоком");

    forged[0] = 2u;
    forged[7] = 0u;
    ProtocolTestExpect(
        !LaiueProtocolDecodeEditIntent(forged, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction),
        "DecodeEditIntent принял установку без блока");

    forged[0] = 3u;
    forged[7] = 1u;
    ProtocolTestExpect(
        !LaiueProtocolDecodeEditIntent(forged, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction),
        "DecodeEditIntent принял неизвестное действие");

    forged[0] = 2u;
    forged[7] = LAIUE_PROTOCOL_MAX_INVENTORY_ITEM + 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeEditIntent(forged, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction),
        "DecodeEditIntent принял неизвестный предмет");
}

static void TestPlayerStateAndBlockDelta(void)
{
    uint8_t payload[160];
    LaiueProtocolPlayerState state = {
        .serverTick = 42u,
        .peerId = 3u,
        .lastProcessedInputSequence = 91u,
        .position = { 1.5, -2.25, 64.0 },
        .yaw = 0.25f,
        .pitch = -0.5f,
        .locomotionVelocityX = 3.25,
        .locomotionVelocityY = -1.5,
        .verticalVelocity = 7.75,
        .externalVelocityX = 0.125,
        .externalVelocityY = -0.25,
        .jumpBufferRemaining = 0.14,
        .coyoteTimeRemaining = 0.08,
        .colliderCrouchProgress = 0.4,
        .eyeCrouchProgress = 0.6,
        .airJumpsRemaining = 2,
        .crouchingRequested = true,
        .grounded = true,
    };

    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload), &state) == 117u,
        "EncodePlayerState вернул неожиданный размер");

    LaiueProtocolPlayerState decodedState;
    ProtocolTestExpect(
        LaiueProtocolDecodePlayerState(payload, 117u, &decodedState),
        "DecodePlayerState отверг собственную запись");
    ProtocolTestExpect(decodedState.serverTick == 42u
        && decodedState.peerId == 3u
        && decodedState.lastProcessedInputSequence == 91u
        && decodedState.grounded && decodedState.crouchingRequested
        && ExactlyEqualDouble(decodedState.position[0], 1.5)
        && ExactlyEqualDouble(decodedState.position[1], -2.25)
        && ExactlyEqualDouble(decodedState.position[2], 64.0)
        && ExactlyEqualDouble(decodedState.locomotionVelocityX, 3.25)
        && ExactlyEqualDouble(decodedState.locomotionVelocityY, -1.5)
        && ExactlyEqualDouble(decodedState.verticalVelocity, 7.75)
        && ExactlyEqualDouble(decodedState.externalVelocityX, 0.125)
        && ExactlyEqualDouble(decodedState.externalVelocityY, -0.25)
        && ExactlyEqualDouble(decodedState.jumpBufferRemaining, 0.14)
        && ExactlyEqualDouble(decodedState.coyoteTimeRemaining, 0.08)
        && ExactlyEqualDouble(decodedState.colliderCrouchProgress, 0.4)
        && ExactlyEqualDouble(decodedState.eyeCrouchProgress, 0.6)
        && decodedState.airJumpsRemaining == 2
        && NearlyEqual(decodedState.yaw, 0.25f, 2.0e-4f)
        && NearlyEqual(decodedState.pitch, -0.5f, 2.0e-4f),
        "DecodePlayerState не восстановил поля");
    ProtocolTestExpect(
        !LaiueProtocolDecodePlayerState(payload, 116u, &decodedState)
        && !LaiueProtocolDecodePlayerState(payload, 118u, &decodedState),
        "DecodePlayerState принял неточный размер");

    payload[116] = 4u;
    ProtocolTestExpect(
        !LaiueProtocolDecodePlayerState(payload, 117u, &decodedState),
        "DecodePlayerState принял зарезервированный флаг");

    LaiueProtocolPlayerState invalidState = state;
    invalidState.peerId = 0u;
    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload),
            &invalidState) == 0,
        "EncodePlayerState принял нулевой peerId");

    invalidState = state;
    invalidState.pitch = 3.0f;
    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload),
            &invalidState) == 0,
        "EncodePlayerState принял тангаж вне диапазона");

    invalidState = state;
    invalidState.colliderCrouchProgress = 1.01;
    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload),
            &invalidState) == 0,
        "EncodePlayerState принял crouch progress вне диапазона");

    invalidState = state;
    invalidState.airJumpsRemaining = -1;
    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload),
            &invalidState) == 0,
        "EncodePlayerState принял отрицательный остаток air jumps");

    invalidState = state;
    invalidState.jumpBufferRemaining = -0.001;
    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload),
            &invalidState) == 0,
        "EncodePlayerState принял отрицательный jump buffer");

    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload), &state)
            == 117u,
        "EncodePlayerState не восстановился после invalid cases");
    TestWriteU64(payload + 40u, UINT64_C(0x7ff8000000000000));
    ProtocolTestExpect(
        !LaiueProtocolDecodePlayerState(payload, 117u, &decodedState),
        "DecodePlayerState принял NaN locomotion velocity");

    LaiueProtocolBlockDelta delta = {
        .serverTick = 7u,
        .revision = 81u,
        .block = { -100000LL, 5LL, 63LL },
        .replacement = 2u,
    };
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDelta(payload, sizeof(payload), &delta) == 37u,
        "EncodeBlockDelta вернул неожиданный размер");

    LaiueProtocolBlockDelta decodedDelta;
    ProtocolTestExpect(
        LaiueProtocolDecodeBlockDelta(payload, 37u, &decodedDelta)
        && decodedDelta.serverTick == 7u
        && decodedDelta.revision == 81u
        && decodedDelta.block[0] == -100000LL && decodedDelta.block[1] == 5LL
        && decodedDelta.block[2] == 63LL && decodedDelta.replacement == 2u,
        "DecodeBlockDelta не восстановил поля");
    ProtocolTestExpect(
        !LaiueProtocolDecodeBlockDelta(payload, 36u, &decodedDelta)
        && !LaiueProtocolDecodeBlockDelta(payload, 38u, &decodedDelta),
        "DecodeBlockDelta принял неточный размер");

    payload[36] = 3u;
    ProtocolTestExpect(!LaiueProtocolDecodeBlockDelta(payload, 37u, &decodedDelta),
        "DecodeBlockDelta принял неизвестный блок");

    LaiueProtocolBlockDelta invalidDelta = delta;
    invalidDelta.replacement = 3u;
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDelta(payload, sizeof(payload),
            &invalidDelta) == 0,
        "EncodeBlockDelta принял неизвестный блок");

    invalidDelta = delta;
    invalidDelta.revision = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDelta(payload, sizeof(payload),
            &invalidDelta) == 0,
        "EncodeBlockDelta принял нулевую ревизию");
}

static void TestDropsAndInventory(void)
{
    uint8_t payload[128];
    LaiueProtocolBlockDrop drop = {
        .id = 9u,
        .position = { 1.0, 2.0, 3.0 },
        .block = 1u,
    };

    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDrop(payload, sizeof(payload), &drop) == 29u,
        "EncodeBlockDrop вернул неожиданный размер");

    LaiueProtocolBlockDrop decodedDrop;
    ProtocolTestExpect(
        LaiueProtocolDecodeBlockDrop(payload, 29u, &decodedDrop)
        && decodedDrop.id == 9u && decodedDrop.block == 1u
        && ExactlyEqualDouble(decodedDrop.position[0], 1.0)
        && ExactlyEqualDouble(decodedDrop.position[1], 2.0)
        && ExactlyEqualDouble(decodedDrop.position[2], 3.0),
        "DecodeBlockDrop не восстановил поля");
    ProtocolTestExpect(
        !LaiueProtocolDecodeBlockDrop(payload, 28u, &decodedDrop)
        && !LaiueProtocolDecodeBlockDrop(payload, 30u, &decodedDrop),
        "DecodeBlockDrop принял неточный размер");

    payload[4] = 3u;
    ProtocolTestExpect(!LaiueProtocolDecodeBlockDrop(payload, 29u, &decodedDrop),
        "DecodeBlockDrop принял неизвестный блок");

    LaiueProtocolBlockDrop invalidDrop = drop;
    invalidDrop.id = 0u;
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDrop(payload, sizeof(payload), &invalidDrop) == 0,
        "EncodeBlockDrop принял нулевой идентификатор");

    invalidDrop = drop;
    invalidDrop.block = 0u;
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDrop(payload, sizeof(payload), &invalidDrop) == 0,
        "EncodeBlockDrop принял пустой блок");

    uint32_t dropId = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeDropRemove(payload, sizeof(payload), 5u) == 4u,
        "EncodeDropRemove вернул неожиданный размер");
    ProtocolTestExpect(
        LaiueProtocolDecodeDropRemove(payload, 4u, &dropId) && dropId == 5u,
        "DecodeDropRemove не восстановил идентификатор");
    ProtocolTestExpect(
        LaiueProtocolEncodeDropRemove(payload, sizeof(payload), 0u) == 0,
        "EncodeDropRemove принял нулевой идентификатор");
    ProtocolTestExpect(!LaiueProtocolDecodeDropRemove(payload, 5u, &dropId),
        "DecodeDropRemove принял неточный размер");

    LaiueProtocolInventory inventory;
    memset(&inventory, 0, sizeof(inventory));
    inventory.selectedHotbarSlot = 8u;
    inventory.slots[0].item = 1u;
    inventory.slots[0].count = 64u;
    inventory.slots[2].item = LAIUE_PROTOCOL_MAX_INVENTORY_ITEM;
    inventory.slots[2].count = 8u;
    inventory.slots[LAIUE_PROTOCOL_INVENTORY_SLOTS - 1u].item = 2u;
    inventory.slots[LAIUE_PROTOCOL_INVENTORY_SLOTS - 1u].count = 1u;

    const uint32_t inventorySize = 1u + LAIUE_PROTOCOL_INVENTORY_SLOTS * 3u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload), &inventory)
        == inventorySize,
        "EncodeInventory вернул неожиданный размер");

    LaiueProtocolInventory decodedInventory;
    ProtocolTestExpect(
        LaiueProtocolDecodeInventory(payload, inventorySize, &decodedInventory)
        && decodedInventory.selectedHotbarSlot == 8u
        && decodedInventory.slots[0].item == 1u
        && decodedInventory.slots[0].count == 64u
        && decodedInventory.slots[2].item ==
            LAIUE_PROTOCOL_MAX_INVENTORY_ITEM
        && decodedInventory.slots[2].count == 8u
        && decodedInventory.slots[LAIUE_PROTOCOL_INVENTORY_SLOTS - 1u].item == 2u
        && decodedInventory.slots[LAIUE_PROTOCOL_INVENTORY_SLOTS - 1u].count == 1u,
        "DecodeInventory не восстановил слоты");
    ProtocolTestExpect(
        !LaiueProtocolDecodeInventory(payload, inventorySize - 1u,
            &decodedInventory)
        && !LaiueProtocolDecodeInventory(payload, inventorySize + 1u,
            &decodedInventory),
        "DecodeInventory принял неточный размер");

    payload[0] = 9u;
    ProtocolTestExpect(
        !LaiueProtocolDecodeInventory(payload, inventorySize, &decodedInventory),
        "DecodeInventory принял слот хотбара вне диапазона");

    LaiueProtocolInventory invalidInventory = inventory;
    invalidInventory.selectedHotbarSlot = 9u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload),
            &invalidInventory) == 0,
        "EncodeInventory принял слот хотбара вне диапазона");

    // Пара «предмет/количество» согласована: нет предмета — нет и счётчика.
    invalidInventory = inventory;
    invalidInventory.slots[1].item = 0u;
    invalidInventory.slots[1].count = 5u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload),
            &invalidInventory) == 0,
        "EncodeInventory принял количество без предмета");

    invalidInventory = inventory;
    invalidInventory.slots[1].item = 1u;
    invalidInventory.slots[1].count = 0u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload),
            &invalidInventory) == 0,
        "EncodeInventory принял предмет без количества");

    invalidInventory = inventory;
    invalidInventory.slots[1].item = 1u;
    invalidInventory.slots[1].count = 65u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload),
            &invalidInventory) == 0,
        "EncodeInventory принял стопку сверх предела");

    invalidInventory = inventory;
    invalidInventory.slots[1].item =
        LAIUE_PROTOCOL_MAX_INVENTORY_ITEM + 1U;
    invalidInventory.slots[1].count = 1u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload),
            &invalidInventory) == 0,
        "EncodeInventory принял неизвестный предмет");

    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload), &inventory)
            == inventorySize,
        "EncodeInventory не восстановился после invalid cases");
    payload[1U + 2U * 3U] =
        LAIUE_PROTOCOL_MAX_INVENTORY_ITEM + 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeInventory(
            payload, inventorySize, &decodedInventory),
        "DecodeInventory принял неизвестный предмет");
}

// Неконечное значение float собирается из битов и проносится через
// volatile: иначе компилятор с /fp:fast вправе доказать неконечность на
// этапе компиляции и свернуть проверку, и тест мерил бы не то. Значение
// обязано дойти до валидатора «живым», как приходит из сети.
static float ProtocolTestBitsToFloat(uint32_t bits)
{
    volatile uint32_t source = bits;
    union
    {
        uint32_t integer;
        float floating;
    } conversion;

    conversion.integer = source;
    return conversion.floating;
}

static double ProtocolTestBitsToDouble(uint64_t bits)
{
    volatile uint64_t source = bits;
    union
    {
        uint64_t integer;
        double floating;
    } conversion;

    conversion.integer = source;
    return conversion.floating;
}

// Кодеки ввода и правки принимают float напрямую от клиента. NaN и
// ±Infinity обязаны отвергаться до квантования: иначе они попадут в
// позицию, угол или направление на авторитетном сервере. Проверка
// намеренно гоняет реальные битовые представления, а не литералы.
static void TestNonFiniteRejected(void)
{
    static const uint32_t nonFinitePatterns[3] = {
        0x7f800000u, // +Infinity
        0xff800000u, // -Infinity
        0x7fc00000u, // тихий NaN
    };

    uint8_t payload[16];

    // Каждое из четырёх полей ввода по очереди травится неконечным
    // значением, остальные остаются валидными.
    for (uint32_t pattern = 0; pattern < 3u; ++pattern)
    {
        float poison = ProtocolTestBitsToFloat(nonFinitePatterns[pattern]);
        for (uint32_t field = 0; field < 4u; ++field)
        {
            LaiueProtocolInput input = {
                .sequence = 1u,
                .movementX = 0.0f,
                .movementY = 0.0f,
                .yaw = 0.0f,
                .pitch = 0.0f,
            };
            switch (field)
            {
                case 0: input.movementX = poison; break;
                case 1: input.movementY = poison; break;
                case 2: input.yaw = poison; break;
                default: input.pitch = poison; break;
            }
            ProtocolTestExpect(
                LaiueProtocolEncodeInput(payload, sizeof(payload), &input) == 0,
                "EncodeInput принял неконечное поле ввода");
        }
    }

    // Направление правки — тот же контракт: неконечная компонента
    // отвергается до нормировки и квантования.
    for (uint32_t pattern = 0; pattern < 3u; ++pattern)
    {
        float poison = ProtocolTestBitsToFloat(nonFinitePatterns[pattern]);
        for (uint32_t axis = 0; axis < 3u; ++axis)
        {
            float direction[3] = { 1.0f, 0.0f, 0.0f };
            direction[axis] = poison;
            ProtocolTestExpect(
                LaiueProtocolEncodeEditIntent(payload, sizeof(payload),
                    true, false, 0u, direction) == 0,
                "EncodeEditIntent принял неконечное направление");
        }
    }
}

static void TestEndpointParser(void)
{
    NetworkEndpoint endpoint;
    ProtocolTestExpect(
        NetworkEndpointParse("192.0.2.10",
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV4
        && endpoint.port == LAIUE_NETWORK_DEFAULT_PORT
        && EqualAscii(endpoint.host, "192.0.2.10"),
        "Endpoint parser не принял IPv4 со стандартным портом");
    ProtocolTestExpect(
        NetworkEndpointParse("192.0.2.10:30000",
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV4
        && endpoint.port == 30000u,
        "Endpoint parser не принял IPv4 с портом");
    ProtocolTestExpect(
        NetworkEndpointParse("2001:db8::10",
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV6
        && endpoint.port == LAIUE_NETWORK_DEFAULT_PORT,
        "Endpoint parser не принял bare IPv6");
    ProtocolTestExpect(
        NetworkEndpointParse("[2001:DB8::10]:30000",
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV6
        && endpoint.port == 30000u
        && EqualAscii(endpoint.host, "2001:db8::10"),
        "Endpoint parser не принял bracketed IPv6");
    ProtocolTestExpect(
        NetworkEndpointParse("Example.COM:443",
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_DNS
        && endpoint.port == 443u
        && EqualAscii(endpoint.host, "example.com"),
        "Endpoint parser не канонизировал DNS endpoint");
    ProtocolTestExpect(
        NetworkEndpointParse("::ffff:192.0.2.1",
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV6,
        "Endpoint parser не принял IPv4-mapped IPv6");

    static const char* invalid[] = {
        "",
        "http://example.com",
        "user@example.com",
        "192.0.2.1:0",
        "192.0.2.1:65536",
        "192.0.2.1:notaport",
        "999.0.0.1",
        "01.2.3.4",
        "[2001:db8::1",
        "[2001:db8::1]garbage",
        "[192.0.2.1]:27180",
        "2001:::1",
        "2001:db8::1::2",
        "-example.test",
        "example..test",
        "*.example.test",
    };
    for (uint32_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
         ++index)
    {
        ProtocolTestExpect(
            NetworkEndpointParse(invalid[index],
                LAIUE_NETWORK_DEFAULT_PORT, &endpoint) !=
                NETWORK_ENDPOINT_PARSE_OK,
            "Endpoint parser принял некорректный endpoint");
    }

    ProtocolTestExpect(
        NetworkListenEndpointParse("*", NETWORK_ADDRESS_FAMILY_DUAL,
            LAIUE_NETWORK_DEFAULT_PORT, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_WILDCARD
        && EqualAscii(endpoint.host, "::"),
        "Listener parser не создал dual-stack wildcard");
    ProtocolTestExpect(
        NetworkListenEndpointParse("0.0.0.0",
            NETWORK_ADDRESS_FAMILY_IPV4, 30000u, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV4,
        "Listener parser не принял IPv4 bind");
    ProtocolTestExpect(
        NetworkListenEndpointParse("::1",
            NETWORK_ADDRESS_FAMILY_IPV6, 30000u, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && endpoint.kind == NETWORK_ENDPOINT_IPV6,
        "Listener parser не принял IPv6 bind");
    ProtocolTestExpect(
        NetworkListenEndpointParse("127.0.0.1",
            NETWORK_ADDRESS_FAMILY_DUAL, 30000u, &endpoint) ==
            NETWORK_ENDPOINT_PARSE_FAMILY_MISMATCH,
        "Listener parser принял конкретный IPv4 в dual режиме");
}

static void TestTrustParser(void)
{
    NetworkTrustMode mode;
    uint8_t pin[LAIUE_NETWORK_CERTIFICATE_PIN_SIZE];
    ProtocolTestExpect(NetworkTrustParse("system", &mode, pin)
        && mode == NETWORK_TRUST_SYSTEM && pin[0] == 0 && pin[31] == 0,
        "Trust parser не принял system");

    const char* pinned =
        "sha256:"
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f";
    ProtocolTestExpect(NetworkTrustParse(pinned, &mode, pin)
        && mode == NETWORK_TRUST_SHA256
        && pin[0] == 0x00u && pin[1] == 0x01u
        && pin[30] == 0x1eu && pin[31] == 0x1fu,
        "Trust parser не принял SHA-256 pin");
    ProtocolTestExpect(!NetworkTrustParse("", &mode, pin)
        && !NetworkTrustParse("sha", &mode, pin)
        && !NetworkTrustParse("sha256:1234", &mode, pin)
        && !NetworkTrustParse("insecure", &mode, pin),
        "Trust parser принял короткий, неполный или insecure trust");
    ProtocolTestExpect(
        !NetworkTrustParse(
            "sha256:"
            "00000000000000000000000000000000"
            "00000000000000000000000000000000",
            &mode, pin),
        "Trust parser принял вырожденный нулевой pin");
}

static void TestNetworkConfigurations(void)
{
    NetworkClientConfiguration client;
    NetworkClientConfigurationInitialize(&client);
    ProtocolTestExpect(
        client.structureSize == sizeof(client)
        && client.addressFamily == NETWORK_ADDRESS_FAMILY_AUTO
        && client.trustMode == NETWORK_TRUST_SYSTEM,
        "ClientConfigurationInitialize не выставил безопасные defaults");
    ProtocolTestExpect(
        NetworkEndpointParse("example.test",
            LAIUE_NETWORK_DEFAULT_PORT, &client.endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && NetworkClientConfigurationIsValid(&client),
        "Client configuration отвергла system trust");

    client.addressFamily = NETWORK_ADDRESS_FAMILY_IPV6;
    ProtocolTestExpect(NetworkClientConfigurationIsValid(&client),
        "Client configuration отвергла IPv6 preference для DNS");
    ProtocolTestExpect(
        NetworkEndpointParse("192.0.2.1",
            LAIUE_NETWORK_DEFAULT_PORT, &client.endpoint) ==
            NETWORK_ENDPOINT_PARSE_OK
        && !NetworkClientConfigurationIsValid(&client),
        "Client configuration приняла IPv4 endpoint в IPv6-only режиме");

    client.addressFamily = NETWORK_ADDRESS_FAMILY_IPV4;
    client.trustMode = NETWORK_TRUST_SHA256;
    client.certificateSha256[0] = 1u;
    ProtocolTestExpect(NetworkClientConfigurationIsValid(&client),
        "Client configuration отвергла непустой certificate pin");
    memset(client.certificateSha256, 0, sizeof(client.certificateSha256));
    ProtocolTestExpect(!NetworkClientConfigurationIsValid(&client),
        "Client configuration приняла пустой certificate pin");

    NetworkServerConfiguration server;
    NetworkServerConfigurationInitialize(&server);
    ProtocolTestExpect(
        server.structureSize == sizeof(server)
        && server.port == LAIUE_NETWORK_DEFAULT_PORT
        && server.addressFamily == NETWORK_ADDRESS_FAMILY_DUAL
        && !NetworkServerConfigurationIsValid(&server),
        "Server configuration должна требовать TLS credentials");
#if defined(_WIN32)
    server.certificateStoreThumbprint =
        "00112233445566778899aabbccddeeff00112233";
#else
    server.certificateFile = "server.crt";
    server.privateKeyFile = "server.key";
#endif
    ProtocolTestExpect(NetworkServerConfigurationIsValid(&server),
        "Server configuration отвергла корректные TLS credentials");
    server.addressFamily = NETWORK_ADDRESS_FAMILY_IPV4;
    server.listenAddress = "::1";
    ProtocolTestExpect(!NetworkServerConfigurationIsValid(&server),
        "Server configuration приняла IPv6 bind в IPv4-only режиме");
}

static void TestSnapshotProtocol(void)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    LaiueProtocolSnapshotBegin begin = {
        .snapshotId = 17u,
        .worldRevision = 91u,
        .serverTick = 42u,
        .chunkCount = 2u,
        .peerId = 7u,
        .worldSeed = -12345,
        .worldTime = 888u,
        .requiresReadyBarrier = true,
    };
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotBegin(payload, sizeof(payload), &begin)
            == 45u,
        "EncodeSnapshotBegin вернул неожиданный размер");
    LaiueProtocolSnapshotBegin decodedBegin;
    ProtocolTestExpect(
        LaiueProtocolDecodeSnapshotBegin(payload, 45u, &decodedBegin)
        && decodedBegin.snapshotId == 17u
        && decodedBegin.worldRevision == 91u
        && decodedBegin.serverTick == 42u
        && decodedBegin.chunkCount == 2u
        && decodedBegin.peerId == 7u
        && decodedBegin.worldSeed == -12345
        && decodedBegin.worldTime == 888u
        && decodedBegin.requiresReadyBarrier,
        "DecodeSnapshotBegin не восстановил поля");

    begin.requiresReadyBarrier = false;
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotBegin(payload, sizeof(payload), &begin)
            == 45u
        && LaiueProtocolDecodeSnapshotBegin(payload, 45u, &decodedBegin)
        && !decodedBegin.requiresReadyBarrier,
        "SnapshotBegin не сохранил live/barrier=false");
    payload[44] = 2u;
    ProtocolTestExpect(
        !LaiueProtocolDecodeSnapshotBegin(payload, 45u, &decodedBegin),
        "DecodeSnapshotBegin принял зарезервированные flags");

    LaiueProtocolChunkDelta chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.chunk[0] = -9;
    chunk.chunk[1] = 5;
    chunk.chunk[2] = 1;
    chunk.revision = 92u;
    chunk.partIndex = 0u;
    chunk.partCount = 1u;
    chunk.editCount = 3u;
    chunk.edits[0].localX = 0u;
    chunk.edits[0].localY = 0u;
    chunk.edits[0].localZ = 0u;
    chunk.edits[0].replacement = 1u;
    chunk.edits[1].localX = 0u;
    chunk.edits[1].localY = 0u;
    chunk.edits[1].localZ = 1u;
    chunk.edits[1].replacement = 2u;
    chunk.edits[2].localX = 63u;
    chunk.edits[2].localY = 63u;
    chunk.edits[2].localZ = 63u;
    chunk.edits[2].replacement = 0u;
    uint32_t chunkSize =
        LaiueProtocolEncodeSnapshotChunk(payload, sizeof(payload), &chunk);
    ProtocolTestExpect(chunkSize == 50u,
        "EncodeSnapshotChunk вернул неожиданный размер");

    LaiueProtocolChunkDelta decodedChunk;
    ProtocolTestExpect(
        LaiueProtocolDecodeSnapshotChunk(payload, chunkSize, &decodedChunk)
        && decodedChunk.chunk[0] == -9
        && decodedChunk.chunk[1] == 5
        && decodedChunk.chunk[2] == 1
        && decodedChunk.revision == 92u
        && decodedChunk.partIndex == 0u
        && decodedChunk.partCount == 1u
        && decodedChunk.editCount == 3u
        && decodedChunk.edits[2].localX == 63u
        && decodedChunk.edits[2].replacement == 0u,
        "DecodeSnapshotChunk не восстановил поля");

    chunk.edits[1] = chunk.edits[0];
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotChunk(payload, sizeof(payload), &chunk)
            == 0,
        "EncodeSnapshotChunk принял дубликат/несортированные edits");
    chunk.edits[1].localX = 64u;
    chunk.edits[1].localY = 0u;
    chunk.edits[1].localZ = 1u;
    chunk.edits[1].replacement = 2u;
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotChunk(payload, sizeof(payload), &chunk)
            == 0,
        "EncodeSnapshotChunk принял локальную координату вне чанка");
    chunk.edits[1].localX = 0u;
    chunk.partIndex = 1u;
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotChunk(payload, sizeof(payload), &chunk)
            == 0u,
        "EncodeSnapshotChunk принял partIndex вне partCount");
    chunk.partIndex = 0u;
    chunk.partCount = 2u;
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotChunk(payload, sizeof(payload), &chunk)
            == 0u,
        "EncodeSnapshotChunk принял неполную не-последнюю часть");
    chunk.partIndex = 0u;
    chunk.partCount = 1u;
    chunk.editCount = 0u;
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotChunk(payload, sizeof(payload), &chunk)
            == 38u
        && LaiueProtocolDecodeSnapshotChunk(
            payload, 38u, &decodedChunk)
        && decodedChunk.partCount == 1u
        && decodedChunk.editCount == 0u,
        "Пустой single-part chunk delta не прошёл round-trip");

    uint64_t snapshotId = 0;
    uint64_t worldRevision = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeSnapshotEnd(payload, sizeof(payload), 17u, 95u)
            == 16u
        && LaiueProtocolDecodeSnapshotEnd(payload, 16u,
            &snapshotId, &worldRevision)
        && snapshotId == 17u && worldRevision == 95u,
        "SnapshotEnd не прошёл round-trip");
    snapshotId = 0;
    worldRevision = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeSyncApplied(
            payload, sizeof(payload), 17u, 95u) == 16u
        && LaiueProtocolDecodeSyncApplied(
            payload, 16u, &snapshotId, &worldRevision)
        && snapshotId == 17u && worldRevision == 95u,
        "SyncApplied не привязан к snapshot generation");

    LaiueProtocolChunkResyncRequest request = {
        .chunk = { -1, 2, 3 },
        .expectedRevision = 94u,
    };
    LaiueProtocolChunkResyncRequest decodedRequest;
    ProtocolTestExpect(
        LaiueProtocolEncodeChunkResyncRequest(payload, sizeof(payload),
            &request) == 32u
        && LaiueProtocolDecodeChunkResyncRequest(payload, 32u,
            &decodedRequest)
        && decodedRequest.chunk[0] == -1
        && decodedRequest.expectedRevision == 94u,
        "ChunkResyncRequest не прошёл round-trip");
    memset(&decodedRequest, 0, sizeof(decodedRequest));
    ProtocolTestExpect(
        LaiueProtocolEncodeChunkResyncCancelled(
            payload, sizeof(payload), &request) == 32u
        && LaiueProtocolDecodeChunkResyncCancelled(
            payload, 32u, &decodedRequest)
        && decodedRequest.chunk[0] == -1
        && decodedRequest.chunk[1] == 2
        && decodedRequest.chunk[2] == 3
        && decodedRequest.expectedRevision == 94u,
        "ChunkResyncCancelled не сохранил correlation");
    ProtocolTestExpect(
        !LaiueProtocolDecodeChunkResyncCancelled(
            payload, 31u, &decodedRequest)
        && !LaiueProtocolDecodeChunkResyncCancelled(
            payload, 33u, &decodedRequest),
        "ChunkResyncCancelled принял неточный размер");

    uint32_t peerId = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodePeerId(payload, sizeof(payload), 7u) == 4u
        && LaiueProtocolDecodePeerId(payload, 4u, &peerId)
        && peerId == 7u,
        "PeerId не прошёл round-trip");
    uint64_t worldTime = 0;
    ProtocolTestExpect(
        LaiueProtocolEncodeWorldTime(payload, sizeof(payload),
            123456789u) == 8u
        && LaiueProtocolDecodeWorldTime(payload, 8u, &worldTime)
        && worldTime == 123456789u,
        "WorldTime не прошёл round-trip");
}

static bool ConstructVectorsEqual(
    const double left[3], const double right[3])
{
    for (uint32_t axis = 0; axis < 3U; ++axis)
    {
        if (!ExactlyEqualDouble(left[axis], right[axis]))
        {
            return false;
        }
    }
    return true;
}

static void TestConstructResetProtocol(void)
{
    uint8_t payload[8];
    LaiueProtocolConstructReset reset = {
        .bodyCount = LAIUE_PROTOCOL_MAX_CONSTRUCT_BODIES,
    };
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructReset(payload, sizeof(payload), &reset)
                == 4U &&
            payload[0] == 16U && payload[1] == 0U &&
            payload[2] == 0U && payload[3] == 0U,
        "ConstructReset не записан в canonical little-endian виде");

    LaiueProtocolConstructReset decoded;
    ProtocolTestExpect(
        LaiueProtocolDecodeConstructReset(payload, 4U, &decoded) &&
            decoded.bodyCount == LAIUE_PROTOCOL_MAX_CONSTRUCT_BODIES,
        "ConstructReset не прошёл round-trip");
    reset.bodyCount = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructReset(payload, sizeof(payload), &reset)
                == 4U &&
            LaiueProtocolDecodeConstructReset(payload, 4U, &decoded) &&
            decoded.bodyCount == 0U,
        "ConstructReset не принял пустой authoritative set");
    reset.bodyCount = LAIUE_PROTOCOL_MAX_CONSTRUCT_BODIES + 1U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructReset(payload, sizeof(payload), &reset)
            == 0U,
        "ConstructReset принял bodyCount сверх предела");

    reset.bodyCount = 1U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructReset(payload, 3U, &reset) == 0U &&
            LaiueProtocolEncodeConstructReset(NULL, sizeof(payload), &reset)
                == 0U &&
            LaiueProtocolEncodeConstructReset(payload, sizeof(payload), NULL)
                == 0U,
        "ConstructReset не проверил указатели/ёмкость");
    LaiueProtocolEncodeConstructReset(payload, sizeof(payload), &reset);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructReset(payload, 3U, &decoded) &&
            !LaiueProtocolDecodeConstructReset(payload, 5U, &decoded) &&
            !LaiueProtocolDecodeConstructReset(NULL, 4U, &decoded) &&
            !LaiueProtocolDecodeConstructReset(payload, 4U, NULL),
        "ConstructReset decoder принял неточный размер/NULL");
    payload[2] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructReset(payload, 4U, &decoded),
        "ConstructReset принял ненулевой reserved");
    payload[2] = 0U;
    payload[0] = (uint8_t)(LAIUE_PROTOCOL_MAX_CONSTRUCT_BODIES + 1U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructReset(payload, 4U, &decoded),
        "ConstructReset decoder принял bodyCount сверх предела");
}

static LaiueProtocolConstructBody MakeConstructBody(void)
{
    LaiueProtocolConstructBody body;
    memset(&body, 0, sizeof(body));
    body.id = UINT64_C(0x1122334455667788);
    body.revision = UINT64_C(0x0102030405060708);
    body.origin[0] = -12.5;
    body.origin[1] = 2.25;
    body.origin[2] = 99.0;
    body.velocity[0] = 0.5;
    body.velocity[1] = -1.25;
    body.velocity[2] = 0.0;
    body.blockCount = LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BODY;
    return body;
}

static void TestConstructBeginProtocol(void)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    LaiueProtocolConstructBody body = MakeConstructBody();
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
                == 68U &&
            payload[0] == 0x88U && payload[7] == 0x11U &&
            payload[8] == 0x08U && payload[15] == 0x01U &&
            payload[64] == 0x00U && payload[65] == 0x01U &&
            payload[66] == 0U && payload[67] == 0U,
        "ConstructBegin имеет неверную exact little-endian раскладку");

    LaiueProtocolConstructBody decoded;
    ProtocolTestExpect(
        LaiueProtocolDecodeConstructBegin(payload, 68U, &decoded) &&
            decoded.id == body.id && decoded.revision == body.revision &&
            decoded.blockCount == body.blockCount &&
            ConstructVectorsEqual(decoded.origin, body.origin) &&
            ConstructVectorsEqual(decoded.velocity, body.velocity),
        "ConstructBegin не прошёл round-trip");
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, 67U, &body) == 0U &&
            LaiueProtocolEncodeConstructBegin(NULL, sizeof(payload), &body)
                == 0U &&
            LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), NULL)
                == 0U &&
            !LaiueProtocolDecodeConstructBegin(payload, 67U, &decoded) &&
            !LaiueProtocolDecodeConstructBegin(payload, 69U, &decoded) &&
            !LaiueProtocolDecodeConstructBegin(NULL, 68U, &decoded) &&
            !LaiueProtocolDecodeConstructBegin(payload, 68U, NULL),
        "ConstructBegin не проверил указатели/точный размер");

    body = MakeConstructBody();
    body.id = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
            == 0U,
        "ConstructBegin принял нулевой id");
    body = MakeConstructBody();
    body.revision = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
            == 0U,
        "ConstructBegin принял нулевую revision");
    body = MakeConstructBody();
    body.blockCount = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
            == 0U,
        "ConstructBegin принял пустое тело");
    body.blockCount = LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BODY + 1U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
            == 0U,
        "ConstructBegin принял блоки сверх предела");

    body = MakeConstructBody();
    LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body);
    payload[66] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBegin(payload, 68U, &decoded),
        "ConstructBegin принял ненулевой reserved");
    payload[66] = 0U;
    TestWriteU64(payload, 0U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBegin(payload, 68U, &decoded),
        "ConstructBegin decoder принял нулевой id");
    TestWriteU64(payload, body.id);
    TestWriteU64(payload + 8U, 0U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBegin(payload, 68U, &decoded),
        "ConstructBegin decoder принял нулевую revision");
    TestWriteU64(payload + 8U, body.revision);
    TestWriteU16(payload + 64U,
        LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BODY + 1U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBegin(payload, 68U, &decoded),
        "ConstructBegin decoder принял blockCount сверх предела");

    static const uint64_t nonFinite[3] = {
        UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000000),
    };
    for (uint32_t pattern = 0; pattern < 3U; ++pattern)
    {
        for (uint32_t field = 0; field < 6U; ++field)
        {
            body = MakeConstructBody();
            double poison = ProtocolTestBitsToDouble(nonFinite[pattern]);
            if (field < 3U)
                body.origin[field] = poison;
            else
                body.velocity[field - 3U] = poison;
            ProtocolTestExpect(
                LaiueProtocolEncodeConstructBegin(
                    payload, sizeof(payload), &body) == 0U,
                "ConstructBegin encoder принял non-finite vector field");

            body = MakeConstructBody();
            LaiueProtocolEncodeConstructBegin(
                payload, sizeof(payload), &body);
            uint32_t offset = field < 3U
                ? 16U + field * 8U
                : 40U + (field - 3U) * 8U;
            TestWriteU64(payload + offset, nonFinite[pattern]);
            ProtocolTestExpect(
                !LaiueProtocolDecodeConstructBegin(payload, 68U, &decoded),
                "ConstructBegin decoder принял non-finite vector field");
        }
    }

    body = MakeConstructBody();
    body.origin[0] = 1099511627777.0;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
            == 0U,
        "ConstructBegin принял origin за world bound");
    body = MakeConstructBody();
    body.velocity[0] = 1048577.0;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBegin(payload, sizeof(payload), &body)
            == 0U,
        "ConstructBegin принял velocity за dynamic bound");
}

static void MakeConstructBlockBatch(
    LaiueProtocolConstructBlockBatch* batch)
{
    memset(batch, 0, sizeof(*batch));
    batch->id = UINT64_C(0x1122334455667788);
    batch->revision = UINT64_C(0x1020304050607080);
    batch->firstBlock = 253U;
    batch->blockCount = 3U;
    batch->blocks[0].local[0] = INT16_MIN;
    batch->blocks[0].local[1] = -2;
    batch->blocks[0].local[2] = 5;
    batch->blocks[0].material = 1U;
    batch->blocks[0].kind = 0U;
    batch->blocks[1].local[0] = INT16_MIN;
    batch->blocks[1].local[1] = -2;
    batch->blocks[1].local[2] = 6;
    batch->blocks[1].material = 2U;
    batch->blocks[1].kind = 0U;
    batch->blocks[2].local[0] = 0;
    batch->blocks[2].local[1] = 0;
    batch->blocks[2].local[2] = 0;
    batch->blocks[2].mountNormal[2] = 1;
    batch->blocks[2].material = 0U;
    batch->blocks[2].kind = 1U;
}

static void TestConstructBlocksProtocol(void)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    LaiueProtocolConstructBlockBatch batch;
    MakeConstructBlockBatch(&batch);
    uint32_t size = LaiueProtocolEncodeConstructBlocks(
        payload, sizeof(payload), &batch);
    ProtocolTestExpect(
        size == 53U && payload[0] == 0x88U && payload[7] == 0x11U &&
            payload[8] == 0x80U && payload[15] == 0x10U &&
            payload[16] == 0xfdU && payload[17] == 0x00U &&
            payload[18] == 3U && payload[19] == 0U &&
            payload[20] == 0x00U && payload[21] == 0x80U &&
            payload[29] == 1U && payload[30] == 0U &&
            payload[50] == 1U && payload[51] == 0U && payload[52] == 1U,
        "ConstructBlocks имеет неверную exact little-endian раскладку");

    LaiueProtocolConstructBlockBatch decoded;
    ProtocolTestExpect(
        LaiueProtocolDecodeConstructBlocks(payload, size, &decoded) &&
            decoded.id == batch.id && decoded.revision == batch.revision &&
            decoded.firstBlock == 253U && decoded.blockCount == 3U &&
            decoded.blocks[0].local[0] == INT16_MIN &&
            decoded.blocks[1].local[2] == 6 &&
            decoded.blocks[1].material == 2U &&
            decoded.blocks[2].kind == 1U &&
            decoded.blocks[2].mountNormal[2] == 1,
        "ConstructBlocks не прошёл round-trip");
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, size - 1U, &batch)
                == 0U &&
            LaiueProtocolEncodeConstructBlocks(NULL, sizeof(payload), &batch)
                == 0U &&
            LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), NULL)
                == 0U &&
            !LaiueProtocolDecodeConstructBlocks(payload, size - 1U, &decoded) &&
            !LaiueProtocolDecodeConstructBlocks(payload, size + 1U, &decoded) &&
            !LaiueProtocolDecodeConstructBlocks(NULL, size, &decoded) &&
            !LaiueProtocolDecodeConstructBlocks(payload, size, NULL),
        "ConstructBlocks не проверил указатели/точный размер");

    MakeConstructBlockBatch(&batch);
    batch.id = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял нулевой id");
    MakeConstructBlockBatch(&batch);
    batch.revision = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял нулевую revision");
    MakeConstructBlockBatch(&batch);
    batch.blockCount = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял пустой batch");
    batch.blockCount = LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BATCH + 1U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял batch сверх предела");
    MakeConstructBlockBatch(&batch);
    batch.firstBlock = 254U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял диапазон за пределами body");
    MakeConstructBlockBatch(&batch);
    batch.blocks[1] = batch.blocks[0];
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял duplicate coordinates");
    MakeConstructBlockBatch(&batch);
    batch.blocks[1].local[2] = 4;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял non-canonical coordinate order");
    MakeConstructBlockBatch(&batch);
    batch.blocks[1].material = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял voxel с нулевым material");
    batch.blocks[1].material = 3U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял неизвестный voxel material");
    MakeConstructBlockBatch(&batch);
    batch.blocks[1].mountNormal[0] = 1;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял mountNormal у voxel");
    MakeConstructBlockBatch(&batch);
    batch.blocks[2].material = 1U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял material у lever");
    MakeConstructBlockBatch(&batch);
    batch.blocks[2].local[0] = 1;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял lever вне local origin");
    MakeConstructBlockBatch(&batch);
    memset(batch.blocks[2].mountNormal, 0,
        sizeof(batch.blocks[2].mountNormal));
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял нулевой lever mountNormal");
    MakeConstructBlockBatch(&batch);
    batch.blocks[2].mountNormal[0] = 1;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял diagonal lever mountNormal");
    MakeConstructBlockBatch(&batch);
    batch.blocks[2].mountNormal[2] = 2;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял lever mountNormal вне unit axis");
    MakeConstructBlockBatch(&batch);
    batch.blocks[2].kind = 2U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch)
            == 0U,
        "ConstructBlocks принял неизвестный block kind");

    MakeConstructBlockBatch(&batch);
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[19] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks принял ненулевой reserved");
    payload[19] = 0U;
    TestWriteU64(payload, 0U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял нулевой id");
    TestWriteU64(payload, batch.id);
    TestWriteU64(payload + 8U, 0U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял нулевую revision");
    TestWriteU64(payload + 8U, batch.revision);
    payload[18] = 0U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, 20U, &decoded),
        "ConstructBlocks decoder принял нулевой count");
    payload[18] = 3U;
    TestWriteU16(payload + 16U, 254U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял диапазон за пределами body");

    MakeConstructBlockBatch(&batch);
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[29] = 0U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял voxel с нулевым material");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[29] = 3U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял неизвестный voxel material");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[26] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял mountNormal у voxel");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[51] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял material у lever");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[42] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял lever вне local origin");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[50] = 0U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял нулевой lever mountNormal");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[48] = 1U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял diagonal lever mountNormal");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[50] = 2U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял lever mountNormal вне unit axis");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    payload[52] = 2U;
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял неизвестный block kind");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    memcpy(payload + 31U, payload + 20U, 11U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял duplicate canonical record");
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    TestWriteU16(payload + 35U, 4U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder принял non-canonical record order");

    memset(payload, 0, sizeof(payload));
    TestWriteU64(payload, batch.id);
    TestWriteU64(payload + 8U, batch.revision);
    payload[18] =
        (uint8_t)(LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BATCH + 1U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructBlocks(payload,
            20U + (LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BATCH + 1U) * 11U,
            &decoded),
        "ConstructBlocks decoder принял batch сверх предела");

    MakeConstructBlockBatch(&batch);
    batch.firstBlock = 0U;
    batch.blockCount = LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BATCH;
    memset(batch.blocks, 0, sizeof(batch.blocks));
    for (uint32_t index = 0; index < batch.blockCount; ++index)
    {
        batch.blocks[index].local[0] = (int16_t)index;
        batch.blocks[index].local[1] = 0;
        batch.blocks[index].local[2] = 0;
        batch.blocks[index].material = (uint8_t)(1U + index % 2U);
        batch.blocks[index].kind = 0U;
    }
    size = LaiueProtocolEncodeConstructBlocks(payload, sizeof(payload), &batch);
    ProtocolTestExpect(
        size == 900U,
        "ConstructBlocks encoder не принял максимальный bounded batch");
    ProtocolTestExpect(
        LaiueProtocolDecodeConstructBlocks(payload, size, &decoded),
        "ConstructBlocks decoder не принял максимальный bounded batch");
    ProtocolTestExpect(
        decoded.blockCount == LAIUE_PROTOCOL_MAX_CONSTRUCT_BLOCKS_PER_BATCH &&
            decoded.blocks[79].local[0] == 79,
        "ConstructBlocks повредил максимальный bounded batch");
}

static LaiueProtocolConstructState MakeConstructState(void)
{
    LaiueProtocolConstructState state;
    memset(&state, 0, sizeof(state));
    state.serverTick = UINT32_MAX;
    state.id = UINT64_C(0xa1b2c3d4e5f60718);
    state.revision = UINT64_C(0x8877665544332211);
    state.origin[0] = -1.5;
    state.origin[1] = 2.0;
    state.origin[2] = 3.25;
    state.velocity[0] = -4.0;
    state.velocity[1] = 5.5;
    state.velocity[2] = 0.125;
    state.heldBy = UINT64_C(0x0102030405060708);
    return state;
}

static void TestConstructStateProtocol(void)
{
    uint8_t payload[LAIUE_PROTOCOL_MAX_PAYLOAD_SIZE];
    LaiueProtocolConstructState state = MakeConstructState();
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state)
                == 80U &&
            payload[0] == 0xffU && payload[3] == 0xffU &&
            payload[4] == 0U && payload[7] == 0U &&
            payload[8] == 0x18U && payload[15] == 0xa1U &&
            payload[16] == 0x11U && payload[23] == 0x88U &&
            payload[72] == 0x08U && payload[79] == 0x01U,
        "ConstructState имеет неверную exact little-endian раскладку");

    LaiueProtocolConstructState decoded;
    ProtocolTestExpect(
        LaiueProtocolDecodeConstructState(payload, 80U, &decoded) &&
            decoded.serverTick == state.serverTick && decoded.id == state.id &&
            decoded.revision == state.revision &&
            decoded.heldBy == state.heldBy &&
            ConstructVectorsEqual(decoded.origin, state.origin) &&
            ConstructVectorsEqual(decoded.velocity, state.velocity),
        "ConstructState не прошёл round-trip");
    state.serverTick = 0U;
    state.heldBy = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state)
                == 80U &&
            LaiueProtocolDecodeConstructState(payload, 80U, &decoded) &&
            decoded.serverTick == 0U && decoded.heldBy == 0U,
        "ConstructState не принял tick wrap/unheld state");
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, 79U, &state) == 0U &&
            LaiueProtocolEncodeConstructState(NULL, sizeof(payload), &state)
                == 0U &&
            LaiueProtocolEncodeConstructState(payload, sizeof(payload), NULL)
                == 0U &&
            !LaiueProtocolDecodeConstructState(payload, 79U, &decoded) &&
            !LaiueProtocolDecodeConstructState(payload, 81U, &decoded) &&
            !LaiueProtocolDecodeConstructState(NULL, 80U, &decoded) &&
            !LaiueProtocolDecodeConstructState(payload, 80U, NULL),
        "ConstructState не проверил указатели/точный размер");

    state = MakeConstructState();
    state.id = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state)
            == 0U,
        "ConstructState принял нулевой id");
    state = MakeConstructState();
    state.revision = 0U;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state)
            == 0U,
        "ConstructState принял нулевую revision");

    static const uint64_t nonFinite[3] = {
        UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000000),
    };
    for (uint32_t pattern = 0; pattern < 3U; ++pattern)
    {
        for (uint32_t field = 0; field < 6U; ++field)
        {
            state = MakeConstructState();
            double poison = ProtocolTestBitsToDouble(nonFinite[pattern]);
            if (field < 3U)
                state.origin[field] = poison;
            else
                state.velocity[field - 3U] = poison;
            ProtocolTestExpect(
                LaiueProtocolEncodeConstructState(
                    payload, sizeof(payload), &state) == 0U,
                "ConstructState encoder принял non-finite vector field");

            state = MakeConstructState();
            LaiueProtocolEncodeConstructState(
                payload, sizeof(payload), &state);
            uint32_t offset = field < 3U
                ? 24U + field * 8U
                : 48U + (field - 3U) * 8U;
            TestWriteU64(payload + offset, nonFinite[pattern]);
            ProtocolTestExpect(
                !LaiueProtocolDecodeConstructState(payload, 80U, &decoded),
                "ConstructState decoder принял non-finite vector field");
        }
    }

    state = MakeConstructState();
    LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state);
    TestWriteU32(payload + 4U, 1U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructState(payload, 80U, &decoded),
        "ConstructState decoder принял ненулевой reserved");
    TestWriteU32(payload + 4U, 0U);
    TestWriteU64(payload + 8U, 0U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructState(payload, 80U, &decoded),
        "ConstructState decoder принял нулевой id");
    TestWriteU64(payload + 8U, state.id);
    TestWriteU64(payload + 16U, 0U);
    ProtocolTestExpect(
        !LaiueProtocolDecodeConstructState(payload, 80U, &decoded),
        "ConstructState decoder принял нулевую revision");
    state = MakeConstructState();
    state.origin[0] = -1099511627777.0;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state)
            == 0U,
        "ConstructState принял origin за world bound");
    state = MakeConstructState();
    state.velocity[0] = -1048577.0;
    ProtocolTestExpect(
        LaiueProtocolEncodeConstructState(payload, sizeof(payload), &state)
            == 0U,
        "ConstructState принял velocity за dynamic bound");
}

static void TestConstructProtocol(void)
{
    TestConstructResetProtocol();
    TestConstructBeginProtocol();
    TestConstructBlocksProtocol();
    TestConstructStateProtocol();
}

LAIUE_TEST_ENTRY(ProtocolTestEntryPoint)
{
    TestHeaderAcceptsEveryDeclaredType();
    TestHeaderRoundTripAndRejections();
    TestWriteFrame();
    TestHelloAndWelcome();
    TestModNegotiation();
    TestContentBegin();
    TestInput();
    TestEditIntent();
    TestNonFiniteRejected();
    TestPlayerStateAndBlockDelta();
    TestDropsAndInventory();
    TestEndpointParser();
    TestTrustParser();
    TestNetworkConfigurations();
    TestSnapshotProtocol();
    TestConstructProtocol();

    ProtocolTestWrite("Проверок пройдено: ");
    ProtocolTestWriteNumber(protocolTestChecks);
    ProtocolTestWrite("\r\n");
    LAIUE_TEST_SUCCESS();
}
