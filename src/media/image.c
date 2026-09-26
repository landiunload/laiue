#include "media/image.h"

#include "media/gif_decode.h"
#include "media/jpeg_decode.h"
#include "media/png_decode.h"

#include <stddef.h>
#include <string.h>

// Обратные величины для деления при уменьшении ровно вдвое.
//
// Выходной пиксель такого уменьшения — блок 2×2, поэтому alphaSum лежит
// в [1, 1020], а взвешенная сумма канала не больше 255·alphaSum. Общий
// путь для непрозрачного блока делит с округлением к ближайшему,
// (w + a/2)/a, что равно floor((2w + a) / (2a)). Это частное считается
// без деления: q0 = (m · kAlphaReciprocal[a]) >> 24, где m = 2w + a.
// Здесь kAlphaReciprocal[a] = ceil(2^24 / (2a)).
//
// Округление вверх даёт переоценку меньше единицы. Обозначив
// e = M·2a − 2^24 ∈ [0, 2a), получаем
//   m·M/2^24 = m/(2a) + m·e/((2a)·2^24),
// а вторая добавка строго меньше m/2^24 ≤ 511·1020/2^24 = 0,032 < 1.
// Значит q0 не больше floor(m/(2a)) более чем на единицу, и одного
// условного декремента достаточно для точного результата.
//
// Произведение m·M тоже влезает в 32 бита: M ≤ 2^23/a + 1, m ≤ 511a,
// поэтому m·M ≤ 511·2^23 + 511a ≤ 4 286 624 772 < 2^32. Исчерпывающая
// проверка всех alphaSum ∈ [1, 1020] и всех числителей 0..255·1020
// живёт в стенде, а не в тесте дерева.
static const uint32_t kAlphaReciprocal[1021] = {
    0u,
    8388608u, 4194304u, 2796203u, 2097152u, 1677722u,
    1398102u, 1198373u, 1048576u, 932068u, 838861u,
    762601u, 699051u, 645278u, 599187u, 559241u,
    524288u, 493448u, 466034u, 441506u, 419431u,
    399458u, 381301u, 364723u, 349526u, 335545u,
    322639u, 310690u, 299594u, 289263u, 279621u,
    270601u, 262144u, 254201u, 246724u, 239675u,
    233017u, 226720u, 220753u, 215093u, 209716u,
    204601u, 199729u, 195084u, 190651u, 186414u,
    182362u, 178482u, 174763u, 171197u, 167773u,
    164483u, 161320u, 158276u, 155345u, 152521u,
    149797u, 147169u, 144632u, 142180u, 139811u,
    137519u, 135301u, 133153u, 131072u, 129056u,
    127101u, 125204u, 123362u, 121575u, 119838u,
    118150u, 116509u, 114913u, 113360u, 111849u,
    110377u, 108943u, 107547u, 106185u, 104858u,
    103564u, 102301u, 101068u, 99865u, 98690u,
    97542u, 96421u, 95326u, 94255u, 93207u,
    92183u, 91181u, 90201u, 89241u, 88302u,
    87382u, 86481u, 85599u, 84734u, 83887u,
    83056u, 82242u, 81443u, 80660u, 79892u,
    79138u, 78399u, 77673u, 76960u, 76261u,
    75574u, 74899u, 74236u, 73585u, 72945u,
    72316u, 71698u, 71090u, 70493u, 69906u,
    69328u, 68760u, 68201u, 67651u, 67109u,
    66577u, 66053u, 65536u, 65028u, 64528u,
    64036u, 63551u, 63073u, 62602u, 62138u,
    61681u, 61231u, 60788u, 60350u, 59919u,
    59494u, 59075u, 58662u, 58255u, 57853u,
    57457u, 57066u, 56680u, 56300u, 55925u,
    55554u, 55189u, 54828u, 54472u, 54121u,
    53774u, 53431u, 53093u, 52759u, 52429u,
    52104u, 51782u, 51464u, 51151u, 50841u,
    50534u, 50232u, 49933u, 49637u, 49345u,
    49057u, 48771u, 48490u, 48211u, 47935u,
    47663u, 47394u, 47128u, 46864u, 46604u,
    46346u, 46092u, 45840u, 45591u, 45344u,
    45101u, 44859u, 44621u, 44385u, 44151u,
    43920u, 43691u, 43465u, 43241u, 43019u,
    42800u, 42582u, 42367u, 42154u, 41944u,
    41735u, 41528u, 41324u, 41121u, 40921u,
    40722u, 40525u, 40330u, 40137u, 39946u,
    39757u, 39569u, 39384u, 39200u, 39017u,
    38837u, 38658u, 38480u, 38305u, 38131u,
    37958u, 37787u, 37618u, 37450u, 37283u,
    37118u, 36955u, 36793u, 36632u, 36473u,
    36315u, 36158u, 36003u, 35849u, 35697u,
    35545u, 35395u, 35247u, 35099u, 34953u,
    34808u, 34664u, 34522u, 34380u, 34240u,
    34101u, 33962u, 33826u, 33690u, 33555u,
    33421u, 33289u, 33157u, 33027u, 32897u,
    32768u, 32641u, 32514u, 32389u, 32264u,
    32141u, 32018u, 31896u, 31776u, 31656u,
    31537u, 31419u, 31301u, 31185u, 31069u,
    30955u, 30841u, 30728u, 30616u, 30505u,
    30394u, 30284u, 30175u, 30067u, 29960u,
    29853u, 29747u, 29642u, 29538u, 29434u,
    29331u, 29229u, 29128u, 29027u, 28927u,
    28827u, 28729u, 28631u, 28533u, 28436u,
    28340u, 28245u, 28150u, 28056u, 27963u,
    27870u, 27777u, 27686u, 27595u, 27504u,
    27414u, 27325u, 27236u, 27148u, 27061u,
    26974u, 26887u, 26801u, 26716u, 26631u,
    26547u, 26463u, 26380u, 26297u, 26215u,
    26133u, 26052u, 25971u, 25891u, 25812u,
    25732u, 25654u, 25576u, 25498u, 25421u,
    25344u, 25267u, 25192u, 25116u, 25041u,
    24967u, 24893u, 24819u, 24746u, 24673u,
    24601u, 24529u, 24457u, 24386u, 24315u,
    24245u, 24175u, 24106u, 24037u, 23968u,
    23900u, 23832u, 23764u, 23697u, 23630u,
    23564u, 23498u, 23432u, 23367u, 23302u,
    23238u, 23173u, 23110u, 23046u, 22983u,
    22920u, 22858u, 22796u, 22734u, 22672u,
    22611u, 22551u, 22490u, 22430u, 22370u,
    22311u, 22251u, 22193u, 22134u, 22076u,
    22018u, 21960u, 21903u, 21846u, 21789u,
    21733u, 21676u, 21621u, 21565u, 21510u,
    21455u, 21400u, 21346u, 21291u, 21237u,
    21184u, 21130u, 21077u, 21025u, 20972u,
    20920u, 20868u, 20816u, 20764u, 20713u,
    20662u, 20611u, 20561u, 20511u, 20461u,
    20411u, 20361u, 20312u, 20263u, 20214u,
    20165u, 20117u, 20069u, 20021u, 19973u,
    19926u, 19879u, 19832u, 19785u, 19738u,
    19692u, 19646u, 19600u, 19554u, 19509u,
    19464u, 19419u, 19374u, 19329u, 19285u,
    19240u, 19196u, 19153u, 19109u, 19066u,
    19022u, 18979u, 18936u, 18894u, 18851u,
    18809u, 18767u, 18725u, 18683u, 18642u,
    18601u, 18559u, 18518u, 18478u, 18437u,
    18397u, 18356u, 18316u, 18276u, 18237u,
    18197u, 18158u, 18118u, 18079u, 18041u,
    18002u, 17963u, 17925u, 17887u, 17849u,
    17811u, 17773u, 17735u, 17698u, 17661u,
    17624u, 17587u, 17550u, 17513u, 17477u,
    17440u, 17404u, 17368u, 17332u, 17297u,
    17261u, 17226u, 17190u, 17155u, 17120u,
    17085u, 17051u, 17016u, 16981u, 16947u,
    16913u, 16879u, 16845u, 16811u, 16778u,
    16744u, 16711u, 16678u, 16645u, 16612u,
    16579u, 16546u, 16514u, 16481u, 16449u,
    16417u, 16384u, 16353u, 16321u, 16289u,
    16257u, 16226u, 16195u, 16164u, 16132u,
    16101u, 16071u, 16040u, 16009u, 15979u,
    15948u, 15918u, 15888u, 15858u, 15828u,
    15798u, 15769u, 15739u, 15710u, 15680u,
    15651u, 15622u, 15593u, 15564u, 15535u,
    15506u, 15478u, 15449u, 15421u, 15392u,
    15364u, 15336u, 15308u, 15280u, 15253u,
    15225u, 15197u, 15170u, 15142u, 15115u,
    15088u, 15061u, 15034u, 15007u, 14980u,
    14953u, 14927u, 14900u, 14874u, 14848u,
    14821u, 14795u, 14769u, 14743u, 14717u,
    14692u, 14666u, 14640u, 14615u, 14589u,
    14564u, 14539u, 14514u, 14489u, 14464u,
    14439u, 14414u, 14389u, 14365u, 14340u,
    14316u, 14291u, 14267u, 14243u, 14218u,
    14194u, 14170u, 14147u, 14123u, 14099u,
    14075u, 14052u, 14028u, 14005u, 13982u,
    13958u, 13935u, 13912u, 13889u, 13866u,
    13843u, 13820u, 13798u, 13775u, 13752u,
    13730u, 13707u, 13685u, 13663u, 13641u,
    13618u, 13596u, 13574u, 13552u, 13531u,
    13509u, 13487u, 13465u, 13444u, 13422u,
    13401u, 13379u, 13358u, 13337u, 13316u,
    13295u, 13274u, 13253u, 13232u, 13211u,
    13190u, 13169u, 13149u, 13128u, 13108u,
    13087u, 13067u, 13047u, 13026u, 13006u,
    12986u, 12966u, 12946u, 12926u, 12906u,
    12886u, 12866u, 12847u, 12827u, 12808u,
    12788u, 12769u, 12749u, 12730u, 12711u,
    12691u, 12672u, 12653u, 12634u, 12615u,
    12596u, 12577u, 12558u, 12540u, 12521u,
    12502u, 12484u, 12465u, 12447u, 12428u,
    12410u, 12391u, 12373u, 12355u, 12337u,
    12319u, 12301u, 12283u, 12265u, 12247u,
    12229u, 12211u, 12193u, 12176u, 12158u,
    12140u, 12123u, 12105u, 12088u, 12070u,
    12053u, 12036u, 12019u, 12001u, 11984u,
    11967u, 11950u, 11933u, 11916u, 11899u,
    11882u, 11866u, 11849u, 11832u, 11815u,
    11799u, 11782u, 11766u, 11749u, 11733u,
    11716u, 11700u, 11684u, 11668u, 11651u,
    11635u, 11619u, 11603u, 11587u, 11571u,
    11555u, 11539u, 11523u, 11508u, 11492u,
    11476u, 11460u, 11445u, 11429u, 11414u,
    11398u, 11383u, 11367u, 11352u, 11336u,
    11321u, 11306u, 11291u, 11276u, 11260u,
    11245u, 11230u, 11215u, 11200u, 11185u,
    11170u, 11156u, 11141u, 11126u, 11111u,
    11097u, 11082u, 11067u, 11053u, 11038u,
    11024u, 11009u, 10995u, 10980u, 10966u,
    10952u, 10937u, 10923u, 10909u, 10895u,
    10881u, 10867u, 10853u, 10838u, 10825u,
    10811u, 10797u, 10783u, 10769u, 10755u,
    10741u, 10728u, 10714u, 10700u, 10687u,
    10673u, 10659u, 10646u, 10632u, 10619u,
    10606u, 10592u, 10579u, 10565u, 10552u,
    10539u, 10526u, 10513u, 10499u, 10486u,
    10473u, 10460u, 10447u, 10434u, 10421u,
    10408u, 10395u, 10382u, 10370u, 10357u,
    10344u, 10331u, 10319u, 10306u, 10293u,
    10281u, 10268u, 10256u, 10243u, 10231u,
    10218u, 10206u, 10193u, 10181u, 10169u,
    10156u, 10144u, 10132u, 10119u, 10107u,
    10095u, 10083u, 10071u, 10059u, 10047u,
    10035u, 10023u, 10011u, 9999u, 9987u,
    9975u, 9963u, 9951u, 9940u, 9928u,
    9916u, 9904u, 9893u, 9881u, 9869u,
    9858u, 9846u, 9835u, 9823u, 9812u,
    9800u, 9789u, 9777u, 9766u, 9755u,
    9743u, 9732u, 9721u, 9710u, 9698u,
    9687u, 9676u, 9665u, 9654u, 9643u,
    9632u, 9620u, 9609u, 9598u, 9587u,
    9577u, 9566u, 9555u, 9544u, 9533u,
    9522u, 9511u, 9501u, 9490u, 9479u,
    9468u, 9458u, 9447u, 9437u, 9426u,
    9415u, 9405u, 9394u, 9384u, 9373u,
    9363u, 9352u, 9342u, 9332u, 9321u,
    9311u, 9301u, 9290u, 9280u, 9270u,
    9259u, 9249u, 9239u, 9229u, 9219u,
    9209u, 9199u, 9188u, 9178u, 9168u,
    9158u, 9148u, 9138u, 9128u, 9119u,
    9109u, 9099u, 9089u, 9079u, 9069u,
    9059u, 9050u, 9040u, 9030u, 9021u,
    9011u, 9001u, 8992u, 8982u, 8972u,
    8963u, 8953u, 8944u, 8934u, 8925u,
    8915u, 8906u, 8896u, 8887u, 8877u,
    8868u, 8859u, 8849u, 8840u, 8831u,
    8821u, 8812u, 8803u, 8794u, 8784u,
    8775u, 8766u, 8757u, 8748u, 8739u,
    8730u, 8720u, 8711u, 8702u, 8693u,
    8684u, 8675u, 8666u, 8657u, 8649u,
    8640u, 8631u, 8622u, 8613u, 8604u,
    8595u, 8587u, 8578u, 8569u, 8560u,
    8552u, 8543u, 8534u, 8526u, 8517u,
    8508u, 8500u, 8491u, 8482u, 8474u,
    8465u, 8457u, 8448u, 8440u, 8431u,
    8423u, 8414u, 8406u, 8398u, 8389u,
    8381u, 8372u, 8364u, 8356u, 8347u,
    8339u, 8331u, 8323u, 8314u, 8306u,
    8298u, 8290u, 8281u, 8273u, 8265u,
    8257u, 8249u, 8241u, 8233u, 8225u,
};

ImageFormat ImageProbe(const void *bytes, uint32_t sizeBytes)
{
    if (bytes == NULL) return IMAGE_FORMAT_UNKNOWN;
    const uint8_t *file = (const uint8_t *)bytes;

    if (sizeBytes >= 8u && file[0] == 137u && file[1] == 80u && file[2] == 78u &&
        file[3] == 71u && file[4] == 13u && file[5] == 10u && file[6] == 26u && file[7] == 10u)
    {
        return IMAGE_FORMAT_PNG;
    }
    if (sizeBytes >= 6u && file[0] == 'G' && file[1] == 'I' && file[2] == 'F' && file[3] == '8' &&
        (file[4] == '7' || file[4] == '9') && file[5] == 'a')
    {
        return IMAGE_FORMAT_GIF;
    }
    // JPEG начинается с маркера SOI, за которым сразу идёт следующий
    // маркер: два байта различают его слишком слабо.
    if (sizeBytes >= 3u && file[0] == 0xFFu && file[1] == 0xD8u && file[2] == 0xFFu)
    {
        return IMAGE_FORMAT_JPEG;
    }
    return IMAGE_FORMAT_UNKNOWN;
}

ImageStatus ImageInspect(const void *bytes, uint32_t sizeBytes, ImageInfo *outInfo)
{
    if (bytes == NULL || outInfo == NULL) return IMAGE_INVALID_ARGUMENT;
    switch (ImageProbe(bytes, sizeBytes))
    {
    case IMAGE_FORMAT_PNG: return PngInspect(bytes, sizeBytes, outInfo);
    case IMAGE_FORMAT_GIF: return GifInspect(bytes, sizeBytes, outInfo);
    case IMAGE_FORMAT_JPEG: return JpegInspect(bytes, sizeBytes, outInfo);
    case IMAGE_FORMAT_UNKNOWN: break;
    }
    return IMAGE_NOT_RECOGNISED;
}

ImageStatus ImageDecode(const void *bytes, uint32_t sizeBytes, const ImageInfo *info, void *pixels,
                        uint32_t pixelBytes, void *scratch, uint32_t scratchBytes)
{
    if (bytes == NULL || info == NULL || pixels == NULL) return IMAGE_INVALID_ARGUMENT;
    switch (ImageProbe(bytes, sizeBytes))
    {
    case IMAGE_FORMAT_PNG:
        return PngDecode(bytes, sizeBytes, info, pixels, pixelBytes, scratch, scratchBytes);
    case IMAGE_FORMAT_GIF:
        return GifDecode(bytes, sizeBytes, info, pixels, pixelBytes, scratch, scratchBytes);
    case IMAGE_FORMAT_JPEG:
        return JpegDecode(bytes, sizeBytes, info, pixels, pixelBytes, scratch, scratchBytes);
    case IMAGE_FORMAT_UNKNOWN: break;
    }
    return IMAGE_NOT_RECOGNISED;
}

const char *ImageStatusText(ImageStatus status)
{
    switch (status)
    {
    case IMAGE_OK: return "ok";
    case IMAGE_INVALID_ARGUMENT: return "invalid argument";
    case IMAGE_NOT_RECOGNISED: return "the file is not an image this tool reads";
    case IMAGE_TRUNCATED: return "the file ends before the image does";
    case IMAGE_CORRUPT: return "the image data is damaged";
    case IMAGE_UNSUPPORTED_FEATURE: return "the image uses a feature this decoder does not read";
    case IMAGE_TOO_LARGE: return "the image is larger than a texture layer may be";
    case IMAGE_BUFFER_TOO_SMALL: return "the output buffer is too small";
    }
    return "unknown error";
}

const char *ImageFormatName(ImageFormat format)
{
    switch (format)
    {
    case IMAGE_FORMAT_PNG: return "PNG";
    case IMAGE_FORMAT_GIF: return "GIF";
    case IMAGE_FORMAT_JPEG: return "JPEG";
    case IMAGE_FORMAT_UNKNOWN: break;
    }
    return "unknown";
}

void ImageResample(const uint8_t *source, uint32_t sourceWidth, uint32_t sourceHeight,
                 uint8_t *destination, uint32_t destinationWidth, uint32_t destinationHeight)
{
    if (source == NULL || destination == NULL) return;
    if (sourceWidth == 0u || sourceHeight == 0u) return;
    if (destinationWidth == 0u || destinationHeight == 0u) return;

    // Копия 1:1 — это общий путь при блоке 1×1. При совпадающих размерах
    // firstRow == row, lastRow == row + 1 (то же по столбцам), значит
    // samples == 1, alphaSum == alpha, weighted == texel·alpha, plain == texel.
    // При alpha != 0 частное (texel·alpha + alpha/2)/alpha равно texel, ведь
    // 0 ≤ (alpha/2)/alpha < 1; при alpha == 0 частное (texel + 1/2)/1 равно
    // texel. Альфа даёт (alpha + 1/2)/1 == alpha. Значит результат общего
    // пути побайтово равен исходнику — это копирование.
    if (sourceWidth == destinationWidth && sourceHeight == destinationHeight)
    {
        if (source != destination)
        {
            memcpy(destination, source, (size_t)sourceWidth * sourceHeight * 4u);
        }
        return;
    }

    // Уменьшение ровно вдвое: каждый выходной пиксель — блок 2×2, границы
    // считать не нужно. Все суммы помещаются в 32 бита (alphaSum ≤ 1020,
    // weighted ≤ 255·1020), и деления с округлением заменены умножением на
    // обратную величину (см. kAlphaReciprocal).
    if (sourceWidth == destinationWidth * 2u && sourceHeight == destinationHeight * 2u)
    {
        for (uint32_t row = 0; row < destinationHeight; ++row)
        {
            const uint8_t *firstLine = source + (size_t)(row * 2u) * sourceWidth * 4u;
            const uint8_t *secondLine = firstLine + (size_t)sourceWidth * 4u;
            uint8_t *destinationLine = destination + (size_t)row * destinationWidth * 4u;

            for (uint32_t column = 0; column < destinationWidth; ++column)
            {
                const uint8_t *topLeft = firstLine + (size_t)(column * 2u) * 4u;
                const uint8_t *topRight = topLeft + 4u;
                const uint8_t *bottomLeft = secondLine + (size_t)(column * 2u) * 4u;
                const uint8_t *bottomRight = bottomLeft + 4u;

                uint32_t alphaSum = (uint32_t)topLeft[3] + topRight[3] + bottomLeft[3] +
                                    bottomRight[3];
                uint32_t weightedRed = (uint32_t)topLeft[0] * topLeft[3] +
                                       (uint32_t)topRight[0] * topRight[3] +
                                       (uint32_t)bottomLeft[0] * bottomLeft[3] +
                                       (uint32_t)bottomRight[0] * bottomRight[3];
                uint32_t weightedGreen = (uint32_t)topLeft[1] * topLeft[3] +
                                         (uint32_t)topRight[1] * topRight[3] +
                                         (uint32_t)bottomLeft[1] * bottomLeft[3] +
                                         (uint32_t)bottomRight[1] * bottomRight[3];
                uint32_t weightedBlue = (uint32_t)topLeft[2] * topLeft[3] +
                                        (uint32_t)topRight[2] * topRight[3] +
                                        (uint32_t)bottomLeft[2] * bottomLeft[3] +
                                        (uint32_t)bottomRight[2] * bottomRight[3];

                uint8_t *out = destinationLine + (size_t)column * 4u;
                if (alphaSum != 0u)
                {
                    uint32_t reciprocal = kAlphaReciprocal[alphaSum];
                    uint32_t doubled = alphaSum * 2u;

                    uint32_t numerator = 2u * weightedRed + alphaSum;
                    uint32_t red = (numerator * reciprocal) >> 24u;
                    if (red * doubled > numerator) --red;

                    numerator = 2u * weightedGreen + alphaSum;
                    uint32_t green = (numerator * reciprocal) >> 24u;
                    if (green * doubled > numerator) --green;

                    numerator = 2u * weightedBlue + alphaSum;
                    uint32_t blue = (numerator * reciprocal) >> 24u;
                    if (blue * doubled > numerator) --blue;

                    out[0] = (uint8_t)red;
                    out[1] = (uint8_t)green;
                    out[2] = (uint8_t)blue;
                }
                else
                {
                    // Полностью прозрачный блок: обычное среднее, как в
                    // общем пути при samples == 4.
                    out[0] = (uint8_t)((topLeft[0] + topRight[0] + bottomLeft[0] +
                                        bottomRight[0] + 2u) >> 2u);
                    out[1] = (uint8_t)((topLeft[1] + topRight[1] + bottomLeft[1] +
                                        bottomRight[1] + 2u) >> 2u);
                    out[2] = (uint8_t)((topLeft[2] + topRight[2] + bottomLeft[2] +
                                        bottomRight[2] + 2u) >> 2u);
                }
                out[3] = (uint8_t)((alphaSum + 2u) >> 2u);
            }
        }
        return;
    }

    // Увеличение: блок выборки — ровно один исходный пиксель. При
    // samples == 1 взвешенное частное (texel·alpha + alpha/2)/alpha равно
    // texel, ведь 0 ≤ alpha/2 < alpha; прозрачный пиксель даёт то же
    // обычным средним, а альфа-канал — сам alpha. Значит увеличение есть
    // повтор ближайшего пикселя, и его можно копировать без делений.
    if (sourceWidth <= destinationWidth && sourceHeight <= destinationHeight)
    {
        uint32_t columnStep = sourceWidth / destinationWidth;
        uint32_t columnRemainderStep = sourceWidth % destinationWidth;

        for (uint32_t row = 0; row < destinationHeight; ++row)
        {
            uint32_t sourceRow = (uint32_t)((uint64_t)row * sourceHeight / destinationHeight);
            const uint8_t *sourceLine = source + (size_t)sourceRow * sourceWidth * 4u;
            uint8_t *destinationLine = destination + (size_t)row * destinationWidth * 4u;

            // firstColumn(c) = floor(c·sourceWidth / destinationWidth)
            // считается накопительным счётчиком: на выходной пиксель не
            // приходится ни одного деления.
            uint32_t sourceColumn = 0u;
            uint32_t remainder = 0u;
            for (uint32_t column = 0; column < destinationWidth; ++column)
            {
                const uint8_t *texel = sourceLine + (size_t)sourceColumn * 4u;
                uint8_t *out = destinationLine + (size_t)column * 4u;
                out[0] = texel[0];
                out[1] = texel[1];
                out[2] = texel[2];
                out[3] = texel[3];

                sourceColumn += columnStep;
                remainder += columnRemainderStep;
                if (remainder >= destinationWidth)
                {
                    remainder -= destinationWidth;
                    ++sourceColumn;
                }
            }
        }
        return;
    }

    // Общий путь: уменьшение с произвольным коэффициентом и смешанный
    // масштаб (одна ось вниз, другая вверх). Границы исходного
    // прямоугольника считаются в целых числах: одна и та же формула даёт
    // и усреднение при уменьшении, и ближайший пиксель при увеличении.
    //
    // Границы столбцов firstColumn(c) = floor(c·sourceWidth/destinationWidth)
    // зависят только от номера столбца, поэтому ведутся накопительным
    // счётчиком, как в пути увеличения: на выходной пиксель не приходится
    // ни одного деления. Значения те же самые, что и у прямого деления.
    uint32_t columnStep = sourceWidth / destinationWidth;
    uint32_t columnRemainderStep = sourceWidth % destinationWidth;

    for (uint32_t row = 0; row < destinationHeight; ++row)
    {
        uint32_t firstRow = (uint32_t)((uint64_t)row * sourceHeight / destinationHeight);
        uint32_t lastRow = (uint32_t)(((uint64_t)row + 1u) * sourceHeight / destinationHeight);
        if (lastRow <= firstRow) lastRow = firstRow + 1u;

        uint32_t columnBegin = 0u;
        uint32_t columnRemainder = 0u;

        for (uint32_t column = 0; column < destinationWidth; ++column)
        {
            uint32_t columnEnd = columnBegin + columnStep;
            columnRemainder += columnRemainderStep;
            if (columnRemainder >= destinationWidth)
            {
                columnRemainder -= destinationWidth;
                ++columnEnd;
            }
            uint32_t firstColumn = columnBegin;
            uint32_t lastColumn = columnEnd;
            if (lastColumn <= firstColumn) lastColumn = firstColumn + 1u;
            columnBegin = columnEnd;

            uint64_t weightedRed = 0u;
            uint64_t weightedGreen = 0u;
            uint64_t weightedBlue = 0u;
            uint64_t plainRed = 0u;
            uint64_t plainGreen = 0u;
            uint64_t plainBlue = 0u;
            uint64_t alphaSum = 0u;
            uint64_t samples = 0u;

            for (uint32_t sourceRow = firstRow; sourceRow < lastRow; ++sourceRow)
            {
                const uint8_t *line = source + (size_t)sourceRow * sourceWidth * 4u;
                for (uint32_t sourceColumn = firstColumn; sourceColumn < lastColumn;
                     ++sourceColumn)
                {
                    const uint8_t *texel = line + (size_t)sourceColumn * 4u;
                    uint32_t alpha = texel[3];
                    weightedRed += (uint64_t)texel[0] * alpha;
                    weightedGreen += (uint64_t)texel[1] * alpha;
                    weightedBlue += (uint64_t)texel[2] * alpha;
                    plainRed += texel[0];
                    plainGreen += texel[1];
                    plainBlue += texel[2];
                    alphaSum += alpha;
                    ++samples;
                }
            }

            uint8_t *out = destination + ((size_t)row * destinationWidth + column) * 4u;
            if (alphaSum != 0u)
            {
                out[0] = (uint8_t)((weightedRed + alphaSum / 2u) / alphaSum);
                out[1] = (uint8_t)((weightedGreen + alphaSum / 2u) / alphaSum);
                out[2] = (uint8_t)((weightedBlue + alphaSum / 2u) / alphaSum);
            }
            else
            {
                // Полностью прозрачный блок: веса нет, поэтому цвет
                // усредняется обычным образом. Терять его нельзя —
                // фильтрация текстуры вытащит его на границе.
                out[0] = (uint8_t)((plainRed + samples / 2u) / samples);
                out[1] = (uint8_t)((plainGreen + samples / 2u) / samples);
                out[2] = (uint8_t)((plainBlue + samples / 2u) / samples);
            }
            out[3] = (uint8_t)((alphaSum + samples / 2u) / samples);
        }
    }
}
