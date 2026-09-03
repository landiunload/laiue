// Заглушка системного вывода для сборок без звуковой подсистемы:
// на Linux — когда ALSA не найдена, во внешних профилях — пока адаптер
// платформы не предоставил свой вывод.
//
// Отказ здесь громкий и предсказуемый: AudioDeviceCreate возвращает
// AUDIO_RESULT_BACKEND_INITIALIZATION_FAILED, а не делает вид, что звук
// играет. Микшер при этом полностью работоспособен через
// AUDIO_BACKEND_OFFSCREEN, поэтому его тесты остаются осмысленными.

#include "audio/audio_backend.h"

bool AudioSystemBackendCreate(const AudioBackendDescription *description,
                              AudioBackend **outBackend)
{
    (void)description;
    if (outBackend != NULL) *outBackend = NULL;
    return false;
}
