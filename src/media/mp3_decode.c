#include "media/mp3_decode.h"

#include "media/mp3_tables.h"

#include <stddef.h>

// Резервуар бит: main_data_begin занимает девять бит, то есть кадр
// вправе сослаться на 511 байт до себя. Плюс собственные данные самого
// длинного кадра — этого хватает с запасом.
#define MP3_RESERVOIR_BYTES 3072u
#define MP3_RESERVOIR_HISTORY 511u
// Наибольшее значение после linbits 13: 15 + 8191.
#define MP3_POW43_COUNT 8207u
// Собственная задержка декодера: столько отсчётов банк фильтров выдаёт
// до того, как появится первый настоящий.
#define MP3_DECODER_DELAY 529u
#define MP3_GRANULE_SAMPLES 576u
#define MP3_FRAME_SAMPLES 1152u
#define MP3_SUBBANDS 32u
#define MP3_SUBBAND_SAMPLES 18u

typedef struct Mp3Header
{
    uint32_t bitrate;
    uint32_t sampleRate;
    uint32_t sampleRateIndex;
    uint32_t mode;            // 0 стерео, 1 joint, 2 двойной канал, 3 моно
    uint32_t modeExtension;
    uint32_t channelCount;
    uint32_t frameBytes;
    uint32_t sideInfoBytes;
    bool crcPresent;
} Mp3Header;

typedef struct Mp3Granule
{
    uint32_t part2And3Length;
    uint32_t bigValues;
    uint32_t globalGain;
    uint32_t scalefacCompress;
    uint32_t blockType;
    uint32_t tableSelect[3];
    uint32_t subblockGain[3];
    uint32_t region0Count;
    uint32_t region1Count;
    uint32_t nonZeroCount;   // первая позиция области нулей
    bool windowSwitching;
    bool mixedBlock;
    bool preflag;
    bool scalefacScale;
    bool count1TableSelect;
} Mp3Granule;

typedef struct Mp3SideInfo
{
    uint32_t mainDataBegin;
    uint32_t scfsi[2];
    Mp3Granule granule[2][2];
} Mp3SideInfo;

typedef struct Mp3Bits
{
    const uint8_t *bytes;
    uint32_t bitPosition;
    uint32_t bitLimit;
    bool overrun;
} Mp3Bits;

typedef struct Mp3State
{
    uint8_t reservoir[MP3_RESERVOIR_BYTES];
    uint32_t reservoirFill;

    float pow43[MP3_POW43_COUNT];
    // Хвост предыдущего блока для перекрытия и кольцевой буфер банка
    // фильтров — то, что обязано пережить кадр.
    float overlap[2][MP3_SUBBANDS * MP3_SUBBAND_SAMPLES];
    float synthesis[2][1024];
    uint32_t synthesisOffset[2];

    int32_t scalefacLong[2][23];
    int32_t scalefacShort[2][13][3];

    // Целые значения после Хаффмана и они же после требантования:
    // знак и величина нужны раздельно, поэтому не одно поле, а два.
    int16_t quantized[2][MP3_GRANULE_SAMPLES];
    float spectrum[2][MP3_GRANULE_SAMPLES];
    float reorderScratch[MP3_GRANULE_SAMPLES];
    float blockScratch[36];
    float synthesisSlot[MP3_SUBBANDS];
    float synthesisWindowed[512];

    // Один кадр PCM перед обрезкой задержки.
    int16_t frameOutput[MP3_FRAME_SAMPLES * 2u];

    Mp3SideInfo side;
    Mp3Header header;
    Mp3Bits bits;
} Mp3State;

// === Чтение бит ===

static uint32_t Mp3ReadBits(Mp3Bits *bits, uint32_t count)
{
    uint32_t value = 0u;
    for (uint32_t index = 0; index < count; ++index)
    {
        if (bits->bitPosition >= bits->bitLimit)
        {
            bits->overrun = true;
            value <<= 1;
            continue;
        }
        uint32_t byte = bits->bytes[bits->bitPosition >> 3];
        uint32_t bit = (byte >> (7u - (bits->bitPosition & 7u))) & 1u;
        bits->bitPosition += 1u;
        value = (value << 1) | bit;
    }
    return value;
}

static int32_t Mp3DecodeHuffman(Mp3Bits *bits, uint32_t root)
{
    uint32_t point = root;
    for (uint32_t depth = 0; depth <= 19u; ++depth)
    {
        const Mp3HuffmanNode *node = &MP3_HUFFMAN_NODES[point];
        if (node->child[0] < 0) return node->child[1];
        point = (uint32_t)node->child[Mp3ReadBits(bits, 1u)];
    }
    bits->overrun = true;
    return 0;
}

// === Заголовок кадра ===

static uint32_t Mp3ReadU32Be(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static Mp3Status Mp3ParseHeader(const uint8_t *bytes, uint32_t available, Mp3Header *outHeader)
{
    if (available < 4u) return MP3_TRUNCATED;
    if (bytes[0] != 0xFFu || (bytes[1] & 0xE0u) != 0xE0u) return MP3_NOT_RECOGNISED;

    uint32_t version = (uint32_t)(bytes[1] >> 3) & 3u;
    uint32_t layer = (uint32_t)(bytes[1] >> 1) & 3u;
    if (version != 3u)
    {
        // 2 и 0 — MPEG-2 и MPEG-2.5 с половинными частотами и своим
        // набором таблиц; 1 зарезервировано стандартом.
        return version == 1u ? MP3_CORRUPT : MP3_UNSUPPORTED_FEATURE;
    }
    if (layer != 1u) return MP3_UNSUPPORTED_FEATURE;   // 3 — Layer I, 2 — Layer II

    uint32_t bitrateIndex = (uint32_t)(bytes[2] >> 4) & 15u;
    uint32_t sampleRateIndex = (uint32_t)(bytes[2] >> 2) & 3u;
    if (bitrateIndex == 0u)
    {
        // Свободный формат: длина кадра не следует из заголовка, её
        // пришлось бы искать по следующему синхрослову.
        return MP3_UNSUPPORTED_FEATURE;
    }
    if (bitrateIndex == 15u || sampleRateIndex == 3u) return MP3_CORRUPT;

    uint32_t padding = (uint32_t)(bytes[2] >> 1) & 1u;
    uint32_t mode = (uint32_t)(bytes[3] >> 6) & 3u;

    outHeader->bitrate = MP3_BITRATES[bitrateIndex];
    outHeader->sampleRate = MP3_SAMPLE_RATES[sampleRateIndex];
    outHeader->sampleRateIndex = sampleRateIndex;
    outHeader->mode = mode;
    outHeader->modeExtension = (uint32_t)(bytes[3] >> 4) & 3u;
    outHeader->channelCount = mode == 3u ? 1u : 2u;
    outHeader->crcPresent = (bytes[1] & 1u) == 0u;
    outHeader->sideInfoBytes = outHeader->channelCount == 1u ? 17u : 32u;
    outHeader->frameBytes = 144u * outHeader->bitrate / outHeader->sampleRate + padding;
    if (outHeader->frameBytes < 4u + (outHeader->crcPresent ? 2u : 0u) + outHeader->sideInfoBytes)
    {
        return MP3_CORRUPT;
    }
    return MP3_OK;
}

// === Теги ===

static uint32_t Mp3SkipId3(const uint8_t *bytes, uint32_t sizeBytes)
{
    uint32_t offset = 0u;
    while (offset + 10u <= sizeBytes && bytes[offset] == 'I' && bytes[offset + 1u] == 'D' &&
           bytes[offset + 2u] == '3')
    {
        // Размер записан семью битами в байте, чтобы не столкнуться с
        // синхрословом кадра.
        uint32_t size = ((uint32_t)(bytes[offset + 6u] & 0x7Fu) << 21) |
                        ((uint32_t)(bytes[offset + 7u] & 0x7Fu) << 14) |
                        ((uint32_t)(bytes[offset + 8u] & 0x7Fu) << 7) |
                        (uint32_t)(bytes[offset + 9u] & 0x7Fu);
        uint32_t total = 10u + size + (((bytes[offset + 5u] & 0x10u) != 0u) ? 10u : 0u);
        if (total > sizeBytes - offset) return sizeBytes;
        offset += total;
    }
    return offset;
}

// Кадр Xing или Info не содержит звука: он несёт длину потока и, если
// файл писал LAME, задержку кодировщика. Возвращает true, если кадр
// служебный, и заполняет задержку с хвостом.
static bool Mp3ParseXing(const uint8_t *frame, uint32_t frameBytes, const Mp3Header *header,
                         uint32_t *outDelay, uint32_t *outPadding)
{
    uint32_t offset = 4u + (header->crcPresent ? 2u : 0u) + header->sideInfoBytes;
    if (offset + 8u > frameBytes) return false;
    const uint8_t *tag = frame + offset;
    bool xing = tag[0] == 'X' && tag[1] == 'i' && tag[2] == 'n' && tag[3] == 'g';
    bool info = tag[0] == 'I' && tag[1] == 'n' && tag[2] == 'f' && tag[3] == 'o';
    if (!xing && !info) return false;

    uint32_t flags = Mp3ReadU32Be(tag + 4);
    uint32_t cursor = offset + 8u;
    if ((flags & 1u) != 0u) cursor += 4u;
    if ((flags & 2u) != 0u) cursor += 4u;
    if ((flags & 4u) != 0u) cursor += 100u;
    if ((flags & 8u) != 0u) cursor += 4u;

    // Расширение LAME начинается сразу за полями Xing; задержка и хвост
    // лежат в трёх байтах по смещению 21 от его начала.
    if (cursor + 24u <= frameBytes)
    {
        const uint8_t *lame = frame + cursor;
        uint32_t delay = ((uint32_t)lame[21] << 4) | ((uint32_t)lame[22] >> 4);
        uint32_t padding = (((uint32_t)lame[22] & 15u) << 8) | (uint32_t)lame[23];
        if (delay < MP3_FRAME_SAMPLES && padding < 4u * MP3_FRAME_SAMPLES)
        {
            *outDelay = delay;
            *outPadding = padding;
        }
    }
    return true;
}

bool Mp3Matches(const void *bytes, uint32_t sizeBytes)
{
    if (bytes == NULL || sizeBytes < 4u) return false;
    const uint8_t *file = (const uint8_t *)bytes;
    if (file[0] == 'I' && file[1] == 'D' && file[2] == '3') return true;

    // Инициализация для анализатора: он не видит, что заполнение
    // и успешный возврат разбора связаны.
    Mp3Header header = {0};
    return Mp3ParseHeader(file, sizeBytes, &header) == MP3_OK;
}

const char *Mp3StatusText(Mp3Status status)
{
    switch (status)
    {
    case MP3_OK: return "ok";
    case MP3_INVALID_ARGUMENT: return "invalid argument";
    case MP3_NOT_RECOGNISED: return "the file does not start with an MPEG audio frame";
    case MP3_TRUNCATED: return "the file ends before the stream does";
    case MP3_CORRUPT: return "the stream is damaged";
    case MP3_UNSUPPORTED_FEATURE: return "the stream uses a layer or a mode this decoder does not read";
    case MP3_TOO_LARGE: return "the stream is longer than one sound may be";
    case MP3_BUFFER_TOO_SMALL: return "the output buffer is too small";
    }
    return "unknown error";
}

Mp3Status Mp3Inspect(const void *bytes, uint32_t sizeBytes, Mp3Info *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return MP3_INVALID_ARGUMENT;
    const uint8_t *file = (const uint8_t *)bytes;

    uint32_t offset = Mp3SkipId3(file, sizeBytes);
    // Инициализация для анализатора: он не видит, что заполнение
    // и успешный возврат разбора связаны.
    Mp3Header header = {0};
    Mp3Status status = MP3_NOT_RECOGNISED;
    // Между тегом и первым кадром попадается мусор; ищем синхрослово,
    // но недалеко, чтобы не принять за MP3 чужой файл.
    uint32_t searchLimit = offset + 4096u;
    if (searchLimit > sizeBytes) searchLimit = sizeBytes;
    while (offset + 4u <= searchLimit)
    {
        status = Mp3ParseHeader(file + offset, sizeBytes - offset, &header);
        if (status == MP3_OK || status == MP3_UNSUPPORTED_FEATURE) break;
        offset += 1u;
        status = MP3_NOT_RECOGNISED;
    }
    if (status != MP3_OK) return status;

    uint32_t delay = 0u;
    uint32_t padding = 0u;
    uint32_t audioFrames = 0u;
    uint32_t cursor = offset;
    uint32_t firstAudio = offset;
    bool first = true;

    while (cursor + 4u <= sizeBytes)
    {
        Mp3Header current = {0};
        if (Mp3ParseHeader(file + cursor, sizeBytes - cursor, &current) != MP3_OK) break;
        if (current.sampleRate != header.sampleRate ||
            current.channelCount != header.channelCount)
        {
            // Смена частоты или числа каналов посреди файла: клип
            // такого не переживёт, и молча взять первую половину хуже.
            return MP3_UNSUPPORTED_FEATURE;
        }
        if (cursor + current.frameBytes > sizeBytes) break;   // обрезанный последний кадр

        if (first)
        {
            first = false;
            if (Mp3ParseXing(file + cursor, current.frameBytes, &current, &delay, &padding))
            {
                cursor += current.frameBytes;
                firstAudio = cursor;
                continue;
            }
        }
        audioFrames += 1u;
        cursor += current.frameBytes;
    }
    if (audioFrames == 0u) return MP3_TRUNCATED;

    // Декодер выдаёт ровно frames * 1152 отсчётов. Первые 529 — его
    // собственная задержка, следом идёт задержка кодировщика; в конце
    // остаётся хвост, который тег LAME называет padding. Полезная
    // длина, стало быть, decoded - delay - padding, а не
    // decoded - skip - padding: собственная задержка декодера
    // вычитается один раз, а не дважды.
    uint64_t decoded = (uint64_t)audioFrames * MP3_FRAME_SAMPLES;
    uint64_t skip = (uint64_t)MP3_DECODER_DELAY + delay;
    if (decoded <= skip) return MP3_CORRUPT;
    uint64_t audible = 0u;
    if (delay != 0u || padding != 0u)
    {
        uint64_t trim = (uint64_t)delay + padding;
        audible = decoded > trim ? decoded - trim : 0u;
    }
    else
    {
        // Тега нет: о задержке кодировщика ничего не известно, и
        // снимается только своя.
        audible = decoded > skip ? decoded - skip : 0u;
    }
    if (audible > decoded - skip) audible = decoded - skip;
    if (audible == 0u) return MP3_CORRUPT;
    if (audible > MP3_MAX_FRAMES) return MP3_TOO_LARGE;
    if (audible * header.channelCount > 0xFFFFFFFFu / 2u) return MP3_TOO_LARGE;

    outInfo->frameCount = (uint32_t)audible;
    outInfo->channelCount = header.channelCount;
    outInfo->sampleRate = header.sampleRate;
    outInfo->scratchBytes = (uint32_t)sizeof(Mp3State);
    outInfo->firstFrameOffset = firstAudio;
    outInfo->decodedFrames = (uint32_t)decoded;
    outInfo->skipFrames = (uint32_t)skip;
    return MP3_OK;
}

// === Побочные сведения ===

static void Mp3ReadSideInfo(Mp3Bits *bits, const Mp3Header *header, Mp3SideInfo *side)
{
    side->mainDataBegin = Mp3ReadBits(bits, 9u);
    Mp3ReadBits(bits, header->channelCount == 1u ? 5u : 3u);   // приватные биты

    for (uint32_t channel = 0; channel < header->channelCount; ++channel)
    {
        side->scfsi[channel] = Mp3ReadBits(bits, 4u);
    }
    for (uint32_t granule = 0; granule < 2u; ++granule)
    {
        for (uint32_t channel = 0; channel < header->channelCount; ++channel)
        {
            Mp3Granule *info = &side->granule[granule][channel];
            info->part2And3Length = Mp3ReadBits(bits, 12u);
            info->bigValues = Mp3ReadBits(bits, 9u);
            info->globalGain = Mp3ReadBits(bits, 8u);
            info->scalefacCompress = Mp3ReadBits(bits, 4u);
            info->windowSwitching = Mp3ReadBits(bits, 1u) != 0u;
            info->subblockGain[0] = 0u;
            info->subblockGain[1] = 0u;
            info->subblockGain[2] = 0u;

            if (info->windowSwitching)
            {
                info->blockType = Mp3ReadBits(bits, 2u);
                info->mixedBlock = Mp3ReadBits(bits, 1u) != 0u;
                info->tableSelect[0] = Mp3ReadBits(bits, 5u);
                info->tableSelect[1] = Mp3ReadBits(bits, 5u);
                info->tableSelect[2] = 0u;
                for (uint32_t window = 0; window < 3u; ++window)
                {
                    info->subblockGain[window] = Mp3ReadBits(bits, 3u);
                }
                // Границы областей при переключении окна не передаются.
                info->region0Count = (info->blockType == 2u && !info->mixedBlock) ? 8u : 7u;
                info->region1Count = 20u - info->region0Count;
            }
            else
            {
                info->tableSelect[0] = Mp3ReadBits(bits, 5u);
                info->tableSelect[1] = Mp3ReadBits(bits, 5u);
                info->tableSelect[2] = Mp3ReadBits(bits, 5u);
                info->region0Count = Mp3ReadBits(bits, 4u);
                info->region1Count = Mp3ReadBits(bits, 3u);
                info->blockType = 0u;
                info->mixedBlock = false;
            }
            info->preflag = Mp3ReadBits(bits, 1u) != 0u;
            info->scalefacScale = Mp3ReadBits(bits, 1u) != 0u;
            info->count1TableSelect = Mp3ReadBits(bits, 1u) != 0u;
            info->nonZeroCount = 0u;
        }
    }
}

// === Коэффициенты масштаба ===

static void Mp3ReadScalefactors(Mp3State *state, uint32_t granule, uint32_t channel)
{
    const Mp3Granule *info = &state->side.granule[granule][channel];
    uint32_t slen1 = MP3_SCALEFACTOR_SIZES[info->scalefacCompress][0];
    uint32_t slen2 = MP3_SCALEFACTOR_SIZES[info->scalefacCompress][1];
    Mp3Bits *bits = &state->bits;

    if (info->windowSwitching && info->blockType == 2u)
    {
        uint32_t firstShort = 0u;
        if (info->mixedBlock)
        {
            // Смешанный блок: две нижние полосы остаются длинными.
            for (uint32_t sfb = 0; sfb < 8u; ++sfb)
            {
                state->scalefacLong[channel][sfb] = (int32_t)Mp3ReadBits(bits, slen1);
            }
            firstShort = 3u;
        }
        for (uint32_t sfb = firstShort; sfb < 6u; ++sfb)
        {
            for (uint32_t window = 0; window < 3u; ++window)
            {
                state->scalefacShort[channel][sfb][window] = (int32_t)Mp3ReadBits(bits, slen1);
            }
        }
        for (uint32_t sfb = 6u; sfb < 12u; ++sfb)
        {
            for (uint32_t window = 0; window < 3u; ++window)
            {
                state->scalefacShort[channel][sfb][window] = (int32_t)Mp3ReadBits(bits, slen2);
            }
        }
        for (uint32_t window = 0; window < 3u; ++window)
        {
            state->scalefacShort[channel][12][window] = 0;
        }
        return;
    }

    // Длинные блоки. Вторая гранула вправе не передавать группу
    // заново: бит scfsi означает «те же значения, что и в первой».
    static const uint32_t GROUP[4][2] = {{0u, 6u}, {6u, 11u}, {11u, 16u}, {16u, 21u}};
    for (uint32_t group = 0; group < 4u; ++group)
    {
        if (granule == 1u && (state->side.scfsi[channel] & (8u >> group)) != 0u) continue;
        uint32_t length = group < 2u ? slen1 : slen2;
        for (uint32_t sfb = GROUP[group][0]; sfb < GROUP[group][1]; ++sfb)
        {
            state->scalefacLong[channel][sfb] = (int32_t)Mp3ReadBits(bits, length);
        }
    }
    // Верхняя полоса своего коэффициента не имеет.
    state->scalefacLong[channel][21] = 0;
    state->scalefacLong[channel][22] = 0;
}

// === Кодированные значения ===

static void Mp3ReadHuffman(Mp3State *state, uint32_t granule, uint32_t channel,
                           uint32_t part2Start)
{
    Mp3Granule *info = &state->side.granule[granule][channel];
    int16_t *quantized = state->quantized[channel];
    for (uint32_t index = 0; index < MP3_GRANULE_SAMPLES; ++index) quantized[index] = 0;
    info->nonZeroCount = 0u;
    if (info->part2And3Length == 0u) return;

    uint32_t endBit = part2Start + info->part2And3Length;
    const uint16_t *bandLong = MP3_BAND_LONG[state->header.sampleRateIndex];
    uint32_t region1 = 0u;
    uint32_t region2 = 0u;
    if (info->windowSwitching && info->blockType == 2u)
    {
        // У короткого блока областей две, и граница у них своя.
        region1 = 36u;
        region2 = MP3_GRANULE_SAMPLES;
    }
    else
    {
        uint32_t first = info->region0Count + 1u;
        uint32_t second = info->region0Count + info->region1Count + 2u;
        if (first > 22u) first = 22u;
        if (second > 22u) second = 22u;
        region1 = bandLong[first];
        region2 = bandLong[second];
    }

    uint32_t position = 0u;
    uint32_t bigValues = info->bigValues * 2u;
    if (bigValues > MP3_GRANULE_SAMPLES) bigValues = MP3_GRANULE_SAMPLES;
    while (position + 1u < bigValues)
    {
        uint32_t table = position < region1
                             ? info->tableSelect[0]
                             : (position < region2 ? info->tableSelect[1] : info->tableSelect[2]);
        int32_t x = 0;
        int32_t y = 0;
        uint32_t root = table < 32u ? MP3_HUFFMAN_ROOT[table] : MP3_HUFFMAN_NONE;
        if (root != MP3_HUFFMAN_NONE)
        {
            int32_t packed = Mp3DecodeHuffman(&state->bits, root);
            x = (packed >> 4) & 15;
            y = packed & 15;
            uint32_t linbits = MP3_HUFFMAN_LINBITS[table];
            if (linbits != 0u && x == 15) x += (int32_t)Mp3ReadBits(&state->bits, linbits);
            if (x != 0 && Mp3ReadBits(&state->bits, 1u) != 0u) x = -x;
            if (linbits != 0u && y == 15) y += (int32_t)Mp3ReadBits(&state->bits, linbits);
            if (y != 0 && Mp3ReadBits(&state->bits, 1u) != 0u) y = -y;
        }
        quantized[position] = (int16_t)x;
        quantized[position + 1u] = (int16_t)y;
        position += 2u;
        if (state->bits.overrun) break;
    }

    // Область четвёрок читается, пока не кончились биты гранулы.
    uint32_t root = MP3_COUNT1_ROOT[info->count1TableSelect ? 1u : 0u];
    while (position + 4u <= MP3_GRANULE_SAMPLES && state->bits.bitPosition < endBit &&
           !state->bits.overrun)
    {
        int32_t packed = Mp3DecodeHuffman(&state->bits, root);
        for (uint32_t index = 0; index < 4u; ++index)
        {
            int32_t value = (packed >> (3u - index)) & 1;
            if (value != 0 && Mp3ReadBits(&state->bits, 1u) != 0u) value = -value;
            quantized[position + index] = (int16_t)value;
        }
        position += 4u;
    }
    // Последняя четвёрка могла быть прочитана за границей гранулы: она
    // принадлежит уже следующей и здесь ничего не значит.
    if (state->bits.bitPosition > endBit && position >= 4u)
    {
        position -= 4u;
        for (uint32_t index = 0; index < 4u; ++index) quantized[position + index] = 0;
    }
    info->nonZeroCount = position;
    // Следующая гранула начинается на своей границе, а не там, где
    // остановилось чтение: лишние или недочитанные биты не сдвигают её.
    state->bits.bitPosition = endBit;
    state->bits.overrun = false;
}

// === Требантование ===

// Все показатели степени в стандарте кратны четверти: и общее усиление,
// и коэффициент масштаба, и усиление подблока.
static float Mp3PowerOfTwoQuarters(int32_t quarters)
{
    float value = MP3_QUARTER_POWER[(uint32_t)(quarters & 3)];
    int32_t whole = quarters >> 2;
    while (whole > 0)
    {
        value *= 2.0f;
        whole -= 1;
    }
    while (whole < 0)
    {
        value *= 0.5f;
        whole += 1;
    }
    return value;
}

static void Mp3ScaleRange(Mp3State *state, uint32_t channel, uint32_t start, uint32_t stop,
                          int32_t quarters)
{
    if (stop > MP3_GRANULE_SAMPLES) stop = MP3_GRANULE_SAMPLES;
    float scale = Mp3PowerOfTwoQuarters(quarters);
    const int16_t *quantized = state->quantized[channel];
    float *spectrum = state->spectrum[channel];
    for (uint32_t index = start; index < stop; ++index)
    {
        int32_t value = quantized[index];
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if (magnitude >= MP3_POW43_COUNT) magnitude = MP3_POW43_COUNT - 1u;
        float amplitude = state->pow43[magnitude] * scale;
        spectrum[index] = value < 0 ? -amplitude : amplitude;
    }
}

static void Mp3Requantize(Mp3State *state, uint32_t granule, uint32_t channel)
{
    const Mp3Granule *info = &state->side.granule[granule][channel];
    const uint16_t *bandLong = MP3_BAND_LONG[state->header.sampleRateIndex];
    const uint16_t *bandShort = MP3_BAND_SHORT[state->header.sampleRateIndex];
    int32_t gain = (int32_t)info->globalGain - 210;
    int32_t step = info->scalefacScale ? 4 : 2;

    // Нули остаются нулями сами собой: pow43[0] равен нулю, поэтому
    // область за последним значением отдельной обработки не требует.
    if (!(info->windowSwitching && info->blockType == 2u))
    {
        for (uint32_t sfb = 0; sfb < 22u; ++sfb)
        {
            int32_t preemphasis = info->preflag && sfb < 21u ? (int32_t)MP3_PRETAB[sfb] : 0;
            int32_t quarters = gain - step * (state->scalefacLong[channel][sfb] + preemphasis);
            Mp3ScaleRange(state, channel, bandLong[sfb], bandLong[sfb + 1u], quarters);
        }
        return;
    }

    uint32_t index = 0u;
    uint32_t firstShort = 0u;
    if (info->mixedBlock)
    {
        for (uint32_t sfb = 0; sfb < 8u; ++sfb)
        {
            uint32_t stop = bandLong[sfb + 1u] > 36u ? 36u : bandLong[sfb + 1u];
            int32_t quarters = gain - step * state->scalefacLong[channel][sfb];
            Mp3ScaleRange(state, channel, bandLong[sfb], stop, quarters);
            if (stop == 36u) break;
        }
        index = 36u;
        firstShort = 3u;
    }
    for (uint32_t sfb = firstShort; sfb < 13u && index < MP3_GRANULE_SAMPLES; ++sfb)
    {
        uint32_t width = (uint32_t)(bandShort[sfb + 1u] - bandShort[sfb]);
        for (uint32_t window = 0; window < 3u; ++window)
        {
            int32_t quarters = gain - 8 * (int32_t)info->subblockGain[window] -
                               step * state->scalefacShort[channel][sfb][window];
            Mp3ScaleRange(state, channel, index, index + width, quarters);
            index += width;
            if (index >= MP3_GRANULE_SAMPLES) break;
        }
    }
}

// === Перестановка коротких блоков ===

static void Mp3Reorder(Mp3State *state, uint32_t granule, uint32_t channel)
{
    const Mp3Granule *info = &state->side.granule[granule][channel];
    if (!info->windowSwitching || info->blockType != 2u) return;

    const uint16_t *bandShort = MP3_BAND_SHORT[state->header.sampleRateIndex];
    float *spectrum = state->spectrum[channel];
    float *scratch = state->reorderScratch;
    for (uint32_t index = 0; index < MP3_GRANULE_SAMPLES; ++index) scratch[index] = spectrum[index];

    // Кодировщик пишет три окна подряд внутри полосы, а банк фильтров
    // ждёт их вперемежку по частоте.
    // Полос здесь тринадцать, а не двенадцать: у последней нет своего
    // коэффициента масштаба, но переставить её нужно наравне с
    // остальными — иначе верхняя треть спектра останется в порядке окон.
    uint32_t first = info->mixedBlock ? 3u : 0u;
    uint32_t source = info->mixedBlock ? 36u : 0u;
    for (uint32_t sfb = first; sfb < 13u; ++sfb)
    {
        uint32_t start = 3u * (uint32_t)bandShort[sfb];
        uint32_t width = (uint32_t)(bandShort[sfb + 1u] - bandShort[sfb]);
        for (uint32_t window = 0; window < 3u; ++window)
        {
            for (uint32_t offset = 0; offset < width; ++offset)
            {
                spectrum[start + offset * 3u + window] = scratch[source];
                source += 1u;
            }
        }
    }
}

// === Стерео ===

static void Mp3IntensityLong(Mp3State *state, uint32_t sfb)
{
    int32_t position = state->scalefacLong[1][sfb];
    // Семёрка означает «полоса не кодирована интенсивностью».
    if (position < 0 || position > 6) return;
    const uint16_t *band = MP3_BAND_LONG[state->header.sampleRateIndex];
    float left = MP3_INTENSITY_LEFT[position];
    float right = MP3_INTENSITY_RIGHT[position];
    for (uint32_t index = band[sfb]; index < band[sfb + 1u]; ++index)
    {
        float value = state->spectrum[0][index];
        state->spectrum[0][index] = value * left;
        state->spectrum[1][index] = value * right;
    }
}

static void Mp3IntensityShort(Mp3State *state, uint32_t sfb)
{
    const uint16_t *band = MP3_BAND_SHORT[state->header.sampleRateIndex];
    uint32_t start = 3u * (uint32_t)band[sfb];
    uint32_t width = (uint32_t)(band[sfb + 1u] - band[sfb]);
    for (uint32_t window = 0; window < 3u; ++window)
    {
        int32_t position = state->scalefacShort[1][sfb][window];
        if (position < 0 || position > 6) continue;
        float left = MP3_INTENSITY_LEFT[position];
        float right = MP3_INTENSITY_RIGHT[position];
        for (uint32_t offset = 0; offset < width; ++offset)
        {
            uint32_t index = start + offset * 3u + window;
            if (index >= MP3_GRANULE_SAMPLES) break;
            float value = state->spectrum[0][index];
            state->spectrum[0][index] = value * left;
            state->spectrum[1][index] = value * right;
        }
    }
}

static void Mp3Stereo(Mp3State *state, uint32_t granule)
{
    if (state->header.channelCount != 2u) return;
    if (state->header.mode != 1u || state->header.modeExtension == 0u) return;

    const Mp3Granule *left = &state->side.granule[granule][0];
    const Mp3Granule *right = &state->side.granule[granule][1];
    const uint16_t *bandLongTable = MP3_BAND_LONG[state->header.sampleRateIndex];
    const uint16_t *bandShortTable = MP3_BAND_SHORT[state->header.sampleRateIndex];
    bool shortBlocks = left->windowSwitching && left->blockType == 2u;

    // Граница интенсивности: выше неё спектра правого канала нет, и
    // там работает не среднее с разностью, а доля от левого. Если
    // включены оба режима, они делят гранулу, а не накладываются:
    // применить средний и разностный каналы к области интенсивности —
    // значит испортить её до того, как её прочитают.
    uint32_t boundary = MP3_GRANULE_SAMPLES;
    if ((state->header.modeExtension & 1u) != 0u)
    {
        if (shortBlocks)
        {
            for (uint32_t sfb = left->mixedBlock ? 3u : 0u; sfb < 12u; ++sfb)
            {
                uint32_t start = 3u * (uint32_t)bandShortTable[sfb];
                if (start >= right->nonZeroCount)
                {
                    boundary = start;
                    break;
                }
            }
            if (left->mixedBlock)
            {
                for (uint32_t sfb = 0; sfb < 8u; ++sfb)
                {
                    if (bandLongTable[sfb] >= right->nonZeroCount)
                    {
                        if (bandLongTable[sfb] < boundary) boundary = bandLongTable[sfb];
                        break;
                    }
                }
            }
        }
        else
        {
            for (uint32_t sfb = 0; sfb < 21u; ++sfb)
            {
                if (bandLongTable[sfb] >= right->nonZeroCount)
                {
                    boundary = bandLongTable[sfb];
                    break;
                }
            }
        }
    }

    if ((state->header.modeExtension & 2u) != 0u)
    {
        // Средний и разностный каналы: сумма и разность, делённые на
        // корень из двух.
        //
        // Обрабатывается вся гранула до границы интенсивности, а не
        // область до последнего декодированного значения: у короткого
        // блока перестановка разносит значения по всей грануле, и часть
        // их оказывается за таким ограничением.
        static const float INVERSE_ROOT_TWO = 0.707106781f;
        for (uint32_t index = 0; index < boundary; ++index)
        {
            float middle = state->spectrum[0][index];
            float side = state->spectrum[1][index];
            state->spectrum[0][index] = (middle + side) * INVERSE_ROOT_TWO;
            state->spectrum[1][index] = (middle - side) * INVERSE_ROOT_TWO;
        }
    }

    if ((state->header.modeExtension & 1u) == 0u) return;

    // Интенсивностью кодируются полосы выше последнего значения
    // правого канала: там его спектра просто нет.
    if (shortBlocks)
    {
        if (left->mixedBlock)
        {
            for (uint32_t sfb = 0; sfb < 8u; ++sfb)
            {
                if (bandLongTable[sfb] >= right->nonZeroCount) Mp3IntensityLong(state, sfb);
            }
        }
        for (uint32_t sfb = left->mixedBlock ? 3u : 0u; sfb < 12u; ++sfb)
        {
            if (3u * (uint32_t)bandShortTable[sfb] >= right->nonZeroCount)
            {
                Mp3IntensityShort(state, sfb);
            }
        }
        return;
    }
    for (uint32_t sfb = 0; sfb < 21u; ++sfb)
    {
        if (bandLongTable[sfb] >= right->nonZeroCount) Mp3IntensityLong(state, sfb);
    }
}

// === Снятие наложения спектров ===

static void Mp3Antialias(Mp3State *state, uint32_t granule, uint32_t channel)
{
    const Mp3Granule *info = &state->side.granule[granule][channel];
    bool shortBlocks = info->windowSwitching && info->blockType == 2u;
    if (shortBlocks && !info->mixedBlock) return;

    // У смешанного блока длинными остаются только два нижних
    // поддиапазона, и бабочки ставятся лишь на их границе.
    uint32_t limit = shortBlocks ? 2u : MP3_SUBBANDS;
    float *spectrum = state->spectrum[channel];
    for (uint32_t subband = 1u; subband < limit; ++subband)
    {
        for (uint32_t index = 0; index < 8u; ++index)
        {
            uint32_t lower = MP3_SUBBAND_SAMPLES * subband - 1u - index;
            uint32_t upper = MP3_SUBBAND_SAMPLES * subband + index;
            float low = spectrum[lower];
            float high = spectrum[upper];
            spectrum[lower] = low * MP3_ALIAS_CS[index] - high * MP3_ALIAS_CA[index];
            spectrum[upper] = high * MP3_ALIAS_CS[index] + low * MP3_ALIAS_CA[index];
        }
    }
}

// === Обратное МДКП и перекрытие ===

static void Mp3InverseTransform(const float *input, float *output, uint32_t blockType)
{
    for (uint32_t index = 0; index < 36u; ++index) output[index] = 0.0f;

    if (blockType == 2u)
    {
        // Короткий блок — три преобразования по шесть входов; окна
        // складываются со сдвигом в шесть отсчётов.
        for (uint32_t window = 0; window < 3u; ++window)
        {
            for (uint32_t position = 0; position < 12u; ++position)
            {
                float sum = 0.0f;
                for (uint32_t index = 0; index < 6u; ++index)
                {
                    sum += input[window + 3u * index] * MP3_IMDCT_COS_SHORT[position][index];
                }
                output[6u * window + 6u + position] += sum * MP3_IMDCT_WINDOW[2][position];
            }
        }
        return;
    }

    for (uint32_t position = 0; position < 36u; ++position)
    {
        float sum = 0.0f;
        for (uint32_t index = 0; index < 18u; ++index)
        {
            sum += input[index] * MP3_IMDCT_COS_LONG[position][index];
        }
        output[position] = sum * MP3_IMDCT_WINDOW[blockType][position];
    }
}

static void Mp3HybridSynthesis(Mp3State *state, uint32_t granule, uint32_t channel)
{
    const Mp3Granule *info = &state->side.granule[granule][channel];
    float *spectrum = state->spectrum[channel];
    float *overlap = state->overlap[channel];

    for (uint32_t subband = 0; subband < MP3_SUBBANDS; ++subband)
    {
        uint32_t blockType = info->blockType;
        if (info->windowSwitching && info->mixedBlock && subband < 2u) blockType = 0u;
        Mp3InverseTransform(spectrum + subband * MP3_SUBBAND_SAMPLES, state->blockScratch,
                            blockType);
        for (uint32_t index = 0; index < MP3_SUBBAND_SAMPLES; ++index)
        {
            uint32_t position = subband * MP3_SUBBAND_SAMPLES + index;
            spectrum[position] = state->blockScratch[index] + overlap[position];
            overlap[position] = state->blockScratch[index + MP3_SUBBAND_SAMPLES];
        }
    }
}

static void Mp3FrequencyInversion(Mp3State *state, uint32_t channel)
{
    float *spectrum = state->spectrum[channel];
    for (uint32_t subband = 1u; subband < MP3_SUBBANDS; subband += 2u)
    {
        for (uint32_t index = 1u; index < MP3_SUBBAND_SAMPLES; index += 2u)
        {
            spectrum[subband * MP3_SUBBAND_SAMPLES + index] =
                -spectrum[subband * MP3_SUBBAND_SAMPLES + index];
        }
    }
}

// === Банк фильтров синтеза ===

static void Mp3SubbandSynthesis(Mp3State *state, uint32_t channel, const float *subband,
                                int16_t *output, uint32_t stride)
{
    float *buffer = state->synthesis[channel];
    // Кольцевой буфер вместо сдвига на 64 значения: сдвигать 960 чисел
    // восемнадцать раз на гранулу — это мегабайты копирования в секунду
    // ради того же результата.
    uint32_t offset = (state->synthesisOffset[channel] + 1024u - 64u) & 1023u;
    state->synthesisOffset[channel] = offset;

    for (uint32_t index = 0; index < 64u; ++index)
    {
        float sum = 0.0f;
        for (uint32_t band = 0; band < MP3_SUBBANDS; ++band)
        {
            sum += MP3_SYNTH_MATRIX[index][band] * subband[band];
        }
        buffer[(offset + index) & 1023u] = sum;
    }

    float *windowed = state->synthesisWindowed;
    for (uint32_t group = 0; group < 8u; ++group)
    {
        for (uint32_t index = 0; index < 32u; ++index)
        {
            uint32_t low = group * 64u + index;
            uint32_t high = low + 32u;
            windowed[low] =
                buffer[(offset + group * 128u + index) & 1023u] * MP3_SYNTH_WINDOW[low];
            windowed[high] =
                buffer[(offset + group * 128u + 96u + index) & 1023u] * MP3_SYNTH_WINDOW[high];
        }
    }

    for (uint32_t index = 0; index < 32u; ++index)
    {
        float sum = 0.0f;
        for (uint32_t group = 0; group < 16u; ++group) sum += windowed[index + 32u * group];
        float scaled = sum * 32768.0f;
        int32_t value = (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        output[index * stride] = (int16_t)value;
    }
}

// === Кадр ===

static void Mp3BuildPow43(float *table)
{
    table[0] = 0.0f;
    double root = 1.0;
    for (uint32_t index = 1u; index < MP3_POW43_COUNT; ++index)
    {
        double value = (double)index;
        // Кубический корень методом Ньютона: без CRT нет ни pow, ни
        // cbrt, а корень предыдущего числа — отличное начальное
        // приближение, потому что аргумент растёт на единицу.
        for (uint32_t step = 0; step < 8u; ++step)
        {
            root = (2.0 * root + value / (root * root)) / 3.0;
        }
        table[index] = (float)(value * root);
    }
}

static Mp3Status Mp3DecodeFrame(Mp3State *state, const uint8_t *frame, const Mp3Header *header,
                                int16_t *output)
{
    uint32_t channels = header->channelCount;
    uint32_t headerBytes = 4u + (header->crcPresent ? 2u : 0u);
    uint32_t mainDataBytes = header->frameBytes - headerBytes - header->sideInfoBytes;
    if (mainDataBytes > MP3_RESERVOIR_BYTES) return MP3_CORRUPT;

    state->header = *header;

    Mp3Bits sideBits;
    sideBits.bytes = frame + headerBytes;
    sideBits.bitPosition = 0u;
    sideBits.bitLimit = header->sideInfoBytes * 8u;
    sideBits.overrun = false;
    Mp3ReadSideInfo(&sideBits, header, &state->side);

    // Резервуар: кадр вправе сослаться на 511 байт до себя, поэтому
    // хвост предыдущих кадров переезжает в начало, а не теряется.
    uint32_t fill = state->reservoirFill;
    if (fill + mainDataBytes > MP3_RESERVOIR_BYTES)
    {
        uint32_t keep = fill < MP3_RESERVOIR_HISTORY ? fill : MP3_RESERVOIR_HISTORY;
        for (uint32_t index = 0; index < keep; ++index)
        {
            state->reservoir[index] = state->reservoir[fill - keep + index];
        }
        fill = keep;
    }
    const uint8_t *mainData = frame + headerBytes + header->sideInfoBytes;
    for (uint32_t index = 0; index < mainDataBytes; ++index)
    {
        state->reservoir[fill + index] = mainData[index];
    }
    state->reservoirFill = fill + mainDataBytes;

    uint32_t total = MP3_FRAME_SAMPLES * channels;
    for (uint32_t index = 0; index < total; ++index) output[index] = 0;

    if (state->side.mainDataBegin > fill)
    {
        // Начало потока: кадр ссылается на данные, которых ещё не было.
        // Это не повреждение — так выглядит первый кадр после склейки.
        return MP3_OK;
    }

    state->bits.bytes = state->reservoir;
    state->bits.bitPosition = (fill - state->side.mainDataBegin) * 8u;
    state->bits.bitLimit = state->reservoirFill * 8u;
    state->bits.overrun = false;

    for (uint32_t granule = 0; granule < 2u; ++granule)
    {
        for (uint32_t channel = 0; channel < channels; ++channel)
        {
            uint32_t part2Start = state->bits.bitPosition;
            Mp3ReadScalefactors(state, granule, channel);
            Mp3ReadHuffman(state, granule, channel, part2Start);
        }
        for (uint32_t channel = 0; channel < channels; ++channel)
        {
            Mp3Requantize(state, granule, channel);
            Mp3Reorder(state, granule, channel);
        }
        Mp3Stereo(state, granule);
        for (uint32_t channel = 0; channel < channels; ++channel)
        {
            Mp3Antialias(state, granule, channel);
            Mp3HybridSynthesis(state, granule, channel);
            Mp3FrequencyInversion(state, channel);
            for (uint32_t slot = 0; slot < MP3_SUBBAND_SAMPLES; ++slot)
            {
                for (uint32_t band = 0; band < MP3_SUBBANDS; ++band)
                {
                    state->synthesisSlot[band] =
                        state->spectrum[channel][band * MP3_SUBBAND_SAMPLES + slot];
                }
                uint32_t first = (granule * MP3_GRANULE_SAMPLES + slot * MP3_SUBBANDS) * channels;
                Mp3SubbandSynthesis(state, channel, state->synthesisSlot,
                                    output + first + channel, channels);
            }
        }
    }
    return MP3_OK;
}

Mp3Status Mp3DecodeSamples(const void *bytes, uint32_t sizeBytes, const Mp3Info *info,
                           int16_t *outSamples, uint32_t sampleCapacity, void *scratch,
                           uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || outSamples == NULL) return MP3_INVALID_ARGUMENT;
    if (scratch == NULL || scratchBytes < sizeof(Mp3State)) return MP3_BUFFER_TOO_SMALL;
    if (info->channelCount == 0u || info->channelCount > 2u) return MP3_INVALID_ARGUMENT;

    uint64_t needed = (uint64_t)info->frameCount * info->channelCount;
    if (needed > sampleCapacity) return MP3_BUFFER_TOO_SMALL;

    Mp3State *state = (Mp3State *)scratch;
    uint8_t *raw = (uint8_t *)scratch;
    for (uint32_t index = 0; index < (uint32_t)sizeof(Mp3State); ++index) raw[index] = 0u;
    Mp3BuildPow43(state->pow43);

    for (uint32_t index = 0; index < (uint32_t)needed; ++index) outSamples[index] = 0;

    const uint8_t *file = (const uint8_t *)bytes;
    uint32_t cursor = info->firstFrameOffset;
    uint32_t produced = 0u;

    while (cursor + 4u <= sizeBytes && produced < info->decodedFrames)
    {
        // Инициализация для анализатора: он не видит, что заполнение
    // и успешный возврат разбора связаны.
    Mp3Header header = {0};
        if (Mp3ParseHeader(file + cursor, sizeBytes - cursor, &header) != MP3_OK) break;
        if (header.channelCount != info->channelCount || header.sampleRate != info->sampleRate)
        {
            break;
        }
        if (cursor + header.frameBytes > sizeBytes) break;

        Mp3Status status = Mp3DecodeFrame(state, file + cursor, &header, state->frameOutput);
        if (status != MP3_OK) return status;

        for (uint32_t index = 0; index < MP3_FRAME_SAMPLES; ++index)
        {
            uint32_t absolute = produced + index;
            if (absolute < info->skipFrames) continue;
            uint32_t target = absolute - info->skipFrames;
            if (target >= info->frameCount) break;
            for (uint32_t channel = 0; channel < info->channelCount; ++channel)
            {
                outSamples[target * info->channelCount + channel] =
                    state->frameOutput[index * info->channelCount + channel];
            }
        }
        produced += MP3_FRAME_SAMPLES;
        cursor += header.frameBytes;
    }

    if (produced == 0u) return MP3_TRUNCATED;
    return MP3_OK;
}
