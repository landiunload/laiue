# Геометрия GPU

Публичный `LaiueGraphicsDeviceV2` поддерживает независимые vertex/index
буферы и необязательный хвост `getCapabilities` / `submitInstances`.
Перед чтением callback проверяйте `structSize` по
`LAIUE_GRAPHICS_DEVICE_V2_CAPABILITIES_SIZE` и
`LAIUE_GRAPHICS_DEVICE_V2_INSTANCES_SIZE`, затем указатель и capability.
Старый V2 provider остаётся допустимым; размер `DrawItemV2` не меняется.

`VERTEX` содержит 24-байтовые `LaiueGraphicsVertexV2`, `INDEX` — uint32.
Один index buffer обслуживает несколько мешей; один меш — несколько
диапазонов и index buffers. `firstIndex` и `indexCount` задают выбранный
диапазон; нулевой count выбирает остаток. `vertexOffset` прибавляется к
индексам со знаком. Provider проверяет диапазон и разрешённые вершины до
записи команды. Индексы не раскрываются в копию вершин внутри submit.
Загрузка IB также учитывает предел сырых индексов Vulkan-устройства:
неподдерживаемые значения отвергаются до выделения GPU-памяти.
Без index buffer сохраняется прежний V2 путь от вершины zero;
`indexCount=0` выбирает весь vertex buffer. Геометрия — список треугольников;
generic-материал рисует обе стороны на D3D12 и Vulkan.

Загружайте геометрию до `beginFrame`. Частичная загрузка готовит замену
транзакционно: отказ сохраняет прежние CPU-данные и GPU-буфер. Геометрию
можно уничтожить после submit: handle сразу становится недействительным,
но записанные команды сохраняют ресурс до завершения GPU. Handle другого
устройства и повторное использование старого поколения отвергаются.

`submitInstances` отправляет один меш, материал и диапазон с несколькими
32-байтовыми `LaiueGraphicsInstanceV2`. Записи заимствуются только на время
вызова; после успеха массив можно менять. Итоговая позиция:

```
item.originRelative + instance.originRelative
    + rotate(local * item.scale * instance.scale, instance.rotation)
```

Масштаб не влияет на смещения. Нулевой `item.scale` означает один, как в
обычном submit; `instance.scale` сохраняет знак и буквальное значение zero.
Quaternion — единичный `(x,y,z,w)` либо четыре zero для identity.
Не конечные числа, неверные ресурсы и исчерпанные кадровые бюджеты дают
отказ. Нулевое количество записей — успешный no-op в активном кадре.
Внутренний `RendererGeometryDraw.scale` сохраняет буквальное значение.

Память GPU и CPU измеряется через [диагностику](graphics_diagnostics.md).
Ручной `laiue_graphics_geometry_benchmark` отдельно измеряет запись
команд CPU и проверяет изображение; это не замер FPS всей игры.
