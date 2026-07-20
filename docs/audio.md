# Аудио

`laiue_audio.dll` — независимый C17-модуль потокового воспроизведения для
клиента движка и приложений на laiue. Windows-бэкенд использует Media
Foundation в audio-only режиме; декодирование, HTTPS-буферизация и вывод на
выбранное системой устройство не попадают в игровой main loop.

## Возможности

- локальные файлы через `AudioPlayerLoadFile`;
- `http://`, `https://` и `file://` через `AudioPlayerLoadUri`;
- асинхронная загрузка и буферизация;
- play, pause, stop, seek и loop;
- независимые volume/mute для каждого экземпляра;
- snapshot позиции, длительности и seekability;
- неблокирующая очередь из 32 последних событий;
- диагностические категории ошибок и исходный Windows HRESULT.

Фактические контейнеры и кодеки определяются установленными системными
Media Foundation transforms. На обычных редакциях Windows доступны как
минимум стандартные музыкальные форматы Microsoft, включая MP3 и AAC.

## Минимальный цикл

```c
AudioPlayer* player = NULL;
if (AudioPlayerCreate(NULL, &player) != AUDIO_RESULT_OK)
{
    return;
}

if (AudioPlayerLoadUri(player, L"https://example.test/music.mp3")
    == AUDIO_RESULT_OK)
{
    AudioEvent event;
    while (AudioPlayerPollEvent(player, &event))
    {
        if (event.type == AUDIO_EVENT_READY)
        {
            AudioPlayerPlay(player);
        }
    }
}

AudioPlayerDestroy(player);
```

Успех `LoadUri`/`LoadFile` означает только принятие асинхронного запроса.
Источник готов после `AUDIO_EVENT_READY`; сетевой, decoder и format error
приходят как `AUDIO_EVENT_ERROR`. UI может каждый кадр читать
`AudioPlayerGetSnapshot`, не блокируя callback Media Foundation.

Проигрыватель следует создавать и уничтожать на одном клиентском потоке.
Callbacks могут выполняться на рабочих потоках Windows, но модуль сам
синхронизирует snapshot и очередь событий. Одновременный вызов `Destroy` с
публичными методами из другого клиентского потока не поддерживается.

