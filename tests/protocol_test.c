#include <windows.h>
#include <string.h>

#include "network/protocol.h"

// Кодек протокола — чистая логика без сокетов и Windows-состояния, поэтому
// protocol.c компилируется прямо в тест. Экспортировать его из laiue_network
// ради проверок нельзя: это расширило бы ABI DLL без второго потребителя.

static uint32_t protocolTestChecks;

// Тест собирается без CRT, поэтому вывод — прямая запись в stdout.
static void ProtocolTestWrite(const char* text)
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
    ExitProcess(1);
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
        .movementX = 0.5f,
        .movementY = -0.5f,
        .yaw = 1.0f,
        .pitch = 0.5f,
        .jumpPressed = true,
        .jumpHeld = false,
        .sprintHeld = true,
        .crouchHeld = false,
    };

    ProtocolTestExpect(
        LaiueProtocolEncodeInput(payload, sizeof(payload), &input) == 9u,
        "EncodeInput вернул неожиданный размер");

    LaiueProtocolInput decoded;
    ProtocolTestExpect(LaiueProtocolDecodeInput(payload, 9u, &decoded),
        "DecodeInput отверг собственную запись");
    // Допуск — шаг квантования: движение 1/32767, углы — предел/32767.
    ProtocolTestExpect(NearlyEqual(decoded.movementX, 0.5f, 1.0e-4f)
        && NearlyEqual(decoded.movementY, -0.5f, 1.0e-4f)
        && NearlyEqual(decoded.yaw, 1.0f, 2.0e-4f)
        && NearlyEqual(decoded.pitch, 0.5f, 2.0e-4f),
        "DecodeInput потерял точность сверх шага квантования");
    ProtocolTestExpect(decoded.jumpPressed && !decoded.jumpHeld
        && decoded.sprintHeld && !decoded.crouchHeld,
        "DecodeInput перепутал флаги");

    ProtocolTestExpect(!LaiueProtocolDecodeInput(payload, 8u, &decoded)
        && !LaiueProtocolDecodeInput(payload, 10u, &decoded),
        "DecodeInput принял неточный размер");

    // Старший полубайт зарезервирован и обязан быть нулевым.
    uint8_t reserved[9];
    memcpy(reserved, payload, sizeof(reserved));
    reserved[8] |= 0x10u;
    ProtocolTestExpect(!LaiueProtocolDecodeInput(reserved, 9u, &decoded),
        "DecodeInput принял установленные резервные биты");

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
    uint8_t forged[9];
    memset(forged, 0, sizeof(forged));
    TestWriteU16(forged, 32767u);
    TestWriteU16(forged + 2, 32767u);
    ProtocolTestExpect(!LaiueProtocolDecodeInput(forged, 9u, &decoded),
        "DecodeInput принял вектор движения длиннее единицы");
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
            2u, east) == 8u,
        "EncodeEditIntent вернул неожиданный размер для установки");
    ProtocolTestExpect(
        LaiueProtocolDecodeEditIntent(payload, 8u, &breakBlock, &placeBlock,
            &placementBlock, direction)
        && !breakBlock && placeBlock && placementBlock == 2u
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
            3u, down) == 0,
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
}

static void TestPlayerStateAndBlockDelta(void)
{
    uint8_t payload[64];
    LaiueProtocolPlayerState state = {
        .serverTick = 42u,
        .peerId = 3u,
        .position = { 1.5, -2.25, 64.0 },
        .yaw = 0.25f,
        .pitch = -0.5f,
        .grounded = true,
    };

    ProtocolTestExpect(
        LaiueProtocolEncodePlayerState(payload, sizeof(payload), &state) == 37u,
        "EncodePlayerState вернул неожиданный размер");

    LaiueProtocolPlayerState decodedState;
    ProtocolTestExpect(
        LaiueProtocolDecodePlayerState(payload, 37u, &decodedState),
        "DecodePlayerState отверг собственную запись");
    ProtocolTestExpect(decodedState.serverTick == 42u
        && decodedState.peerId == 3u && decodedState.grounded
        && ExactlyEqualDouble(decodedState.position[0], 1.5)
        && ExactlyEqualDouble(decodedState.position[1], -2.25)
        && ExactlyEqualDouble(decodedState.position[2], 64.0)
        && NearlyEqual(decodedState.yaw, 0.25f, 2.0e-4f)
        && NearlyEqual(decodedState.pitch, -0.5f, 2.0e-4f),
        "DecodePlayerState не восстановил поля");
    ProtocolTestExpect(
        !LaiueProtocolDecodePlayerState(payload, 36u, &decodedState)
        && !LaiueProtocolDecodePlayerState(payload, 38u, &decodedState),
        "DecodePlayerState принял неточный размер");

    payload[36] = 2u;
    ProtocolTestExpect(
        !LaiueProtocolDecodePlayerState(payload, 37u, &decodedState),
        "DecodePlayerState принял небулево значение grounded");

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

    LaiueProtocolBlockDelta delta = {
        .serverTick = 7u,
        .block = { -100000LL, 5LL, 63LL },
        .replacement = 2u,
    };
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDelta(payload, sizeof(payload), &delta) == 29u,
        "EncodeBlockDelta вернул неожиданный размер");

    LaiueProtocolBlockDelta decodedDelta;
    ProtocolTestExpect(
        LaiueProtocolDecodeBlockDelta(payload, 29u, &decodedDelta)
        && decodedDelta.serverTick == 7u
        && decodedDelta.block[0] == -100000LL && decodedDelta.block[1] == 5LL
        && decodedDelta.block[2] == 63LL && decodedDelta.replacement == 2u,
        "DecodeBlockDelta не восстановил поля");
    ProtocolTestExpect(
        !LaiueProtocolDecodeBlockDelta(payload, 28u, &decodedDelta)
        && !LaiueProtocolDecodeBlockDelta(payload, 30u, &decodedDelta),
        "DecodeBlockDelta принял неточный размер");

    payload[28] = 3u;
    ProtocolTestExpect(!LaiueProtocolDecodeBlockDelta(payload, 29u, &decodedDelta),
        "DecodeBlockDelta принял неизвестный блок");

    LaiueProtocolBlockDelta invalidDelta = delta;
    invalidDelta.replacement = 3u;
    ProtocolTestExpect(
        LaiueProtocolEncodeBlockDelta(payload, sizeof(payload),
            &invalidDelta) == 0,
        "EncodeBlockDelta принял неизвестный блок");
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
    invalidInventory.slots[1].item = 3u;
    invalidInventory.slots[1].count = 1u;
    ProtocolTestExpect(
        LaiueProtocolEncodeInventory(payload, sizeof(payload),
            &invalidInventory) == 0,
        "EncodeInventory принял неизвестный предмет");
}

void ProtocolTestEntryPoint(void)
{
    TestHeaderAcceptsEveryDeclaredType();
    TestHeaderRoundTripAndRejections();
    TestWriteFrame();
    TestHelloAndWelcome();
    TestModNegotiation();
    TestContentBegin();
    TestInput();
    TestEditIntent();
    TestPlayerStateAndBlockDelta();
    TestDropsAndInventory();

    ProtocolTestWrite("Проверок пройдено: ");
    ProtocolTestWriteNumber(protocolTestChecks);
    ProtocolTestWrite("\r\n");
    ExitProcess(0);
}
