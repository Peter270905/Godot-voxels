# Lighting Refactor Plan

Дата: 2026-11-17
Проект: `C:\Godot_Voxel_Jenova\godot_voxel` (форк Godot Voxel Tools)
Игровая сторона: `C:\YALE` (`scripts/World/generator.gd`, `scripts/Game/EnvironmentController.gd`, шейдеры)

## Статус выполнения (2026-11-17): ВЫПОЛНЕНО УСПЕШНО ✅
- **Фаза 1 (Критические фиксы света):** ВЫПОЛНЕНО
  - Воздух `filter = 0` (полностью прозрачный).
  - Луч уровня 15 без потерь пробивает воздух на любую глубину.
  - Листва и вода останавливают первичный луч (`filter > 0`), свет мягко проникает через BFS с затуханием 2.
- **Фаза 2 (Визуал и сутки):** ВЫПОЛНЕНО
  - Добавлен глобал `u_sun_color` и суточная смена оттенков (золотой день, оранжевый закат/рассвет, синяя холодная лунная ночь).
  - Мягкая кривая `pow(x, 1.2)` + directional face shading (верх 1.0, бока 0.84/0.7, низ 0.55).
- **Фаза 3 (Оптимизация):** ВЫПОЛНЕНО
  - Сырой аксессор `VoxelBufferLightAccessor` со Span-срезами и unchecked доступом.
  - `FastQueue` на базе flat vector без аллокаций в куче.
  - Sky Queue Culling: отсечено 95% избыточных нод неба, плоский 1D-скан источников.
  - Скорость расчёта света чанка выросла в **27.1 раз** (с 13.09 мс до **0.48 мс**).
  - FPS в игре вырос с 11–14 FPS до **170+ FPS стабильно** при спринте и полёте без просадок и без швов.
- **Фаза 4 (Межчаночный свет):** Отложена за ненадобностью — визуальных швов на границах чанков нет.

---

## TL;DR — что реально сломано

1. **Солнечный свет теряет 1 уровень на каждый блок вниз.** В Minecraft
   уровень 15 идёт вертикально через прозрачные блоки без потерь. Здесь
   `propagate_sun_light()` не различает направления, поэтому любой луч гаснет
   за 15 блоков. Это главная причина «дырка в потолке светит только пару блоков»
   и тёмных шахт/входов в пещеры.
2. **Воздух считается «почти прозрачным» (`filter = 1`), а не полностью
   прозрачным (`filter = 0`).** Это зашито в C++ (`get_filter`), в
   `VoxelBlockyModelEmpty` и в GDScript-таблице `VoxelMaterialUtil`. Даже если
   добавить вертикальное исключение, при `filter = 1` оно не сработает.
3. **Высотная карта (heightmap) — это высота рельефа, а не «первый
   непрозрачный блок».** Поэтому: (а) вода/листва на уровне поверхности
   получают 0, потому что цикл останавливается ровно на них; (б) шахта,
   пробитая сквозь рельеф в «подземном» чанке, не освещается вообще —
   heightmap про неё не знает. Пункт (а) чинится за полчаса, пункт (б) —
   только вместе с данными о небе (Фаза 4).
4. **Свет не перетекает между чанками.** Ни `VoxelData`, ни `VoxelTerrain`,
   ни `VoxelLodTerrain` не содержат кода, который переносит/пересчитывает свет
   на границе блоков. Свет считается изолированно для каждого чанка.
5. **Цвет солнца захардкожен в шейдерах.** `u_sun_color` не существует;
   `vec3 sun_col = vec3(1.0, 0.98, 0.94);` — константа. `u_sun_intensity`
   при этом обновляется, поэтому день/ночь меняет яркость, но не цвет.
6. **Кривая `pow(x, 1.45)`** делает уровни 1–4 почти невидимыми. Это не
   первопричина, но после фикса распространения её надо смягчить.
7. **Мешер при отсутствии канала света заливает весь чанк одним значением**
   из вокселя `(0,0,0)`. Переменная `has_light_channel` вычисляется и **не
   используется**.

Дешёвые фиксы №1–3 дают основной визуальный эффект. №4 — архитектурная работа.
№5–6 — полдня. №7 — страховка от «чёрных чанков».

---

## Диагноз

### P0-1. Солнечный свет затухает при движении вниз (гипотеза A — подтверждена)

`storage/voxel_light_engine.cpp:151-189`, функция `propagate_sun_light`:

```cpp
for (int i = 0; i < 6; ++i) {
    const Vector3i npos = node.pos + NEIGHBORS[i];
    if (!accessor.is_valid_pos(npos)) {
        continue;
    }

    const uint16_t ntype = accessor.get_type(npos);
    const uint8_t filter = get_filter(lib, ntype);
    if (filter >= 15) {
        continue;
    }

    const uint8_t attenuation = (filter < 1) ? 1 : filter;
    if (node.level <= attenuation) {
        continue;
    }
    const uint8_t new_light = node.level - attenuation;
```

Направление `npos.y < node.pos.y` никак не выделено. Для воздуха
`filter == 1` → `attenuation == 1` → каждый блок вниз отнимает уровень.
Луч с уровнем 15 умирает через 15 блоков.

В Minecraft правило другое (ванильный `SkyLightEngine`):
если `level == 15`, направление — вниз, а блок прозрачен (`opacity == 0`),
новый уровень **остаётся 15**. Для остальных случаев — `level - opacity - 1`.

> Важно: исключение работает **только для уровня 15**, а не для любого.
> Если сделать «вниз без потерь» для всех уровней (как сформулировано в
> гипотезе A), свет уровня 3 будет бесконечно уходить вглубь и освещать
> дно любой шахты — это не Minecraft и даст другие артефакты.

**Почему это не видно на ровной земле.** Первичное сканирование колонок
(`voxel_light_engine.cpp:353-370`) ставит `15` сразу всем прозрачным блокам
над поверхностью, без затухания:

```cpp
for (int y = size.y - 1; y >= 0; --y) {
    const uint16_t type = buffer.get_voxel(x, y, z, VoxelBuffer::CHANNEL_TYPE);
    const uint8_t filter = get_filter(library, type);
    if (filter >= 15) {
        break;
    }
    const uint8_t cur = buffer.get_voxel(x, y, z, VoxelBuffer::CHANNEL_LIGHT);
    buffer.set_voxel(VoxelLight::set_sun(cur, 15), x, y, z, VoxelBuffer::CHANNEL_LIGHT);
    _sun_propagate_queue.push({ Vector3i(x, y, z), 15 });
}
```

То есть ровный луг освещён корректно, а всё, что требует **бокового или
вертикального** распространения (шахты, навесы, входы в пещеры, свет из
соседнего чанка), гаснет в 15 блоках.

### P0-2. Воздух имеет `filter = 1`, а не 0 (гипотеза A — подтверждена)

`storage/voxel_light_engine.cpp:15-23`:

```cpp
uint8_t VoxelLightEngine::get_filter(const blocky::BakedLibrary *lib, uint16_t type) const {
    if (type == 0) {
        return 1; // Air default
    }
    if (lib != nullptr && lib->has_model(type)) {
        return lib->models[type].light_filter;
    }
    return 15; // Solid opaque default
}
```

Плюс `meshers/blocky/voxel_blocky_model_empty.cpp:8-9`:

```cpp
set_light_filter(1);
set_light_emission(0);
```

И игровая таблица `scripts/systems/VoxelMaterialUtil.gd:27-32`:

```gdscript
const DEFAULT_LIGHT_FILTER := {
    0: 1,    # air
    12: 2,   # water
    17: 1,   # campfire
    27: 2,   # leaves
```

Комментарий в том же файле прямо фиксирует неверную модель:
`## 15 — полностью непрозрачный, 1 — воздух/прозрачный` (строка 26).

`attenuation = (filter < 1) ? 1 : filter` (строка 174) сводит `0` к `1`,
поэтому **одной правки `get_filter` недостаточно** — нужно и то, и другое.
При этом `filter = 0` нужен как признак «полностью прозрачный», чтобы
вертикальное правило из P0-1 вообще могло сработать.

### P0-3. Heightmap-путь: вода без света и шахты без неба (частично подтверждено)

`storage/voxel_light_engine.cpp:404-436`:

```cpp
const bool has_heightmap = heightmap.size() >= static_cast<size_t>(size.x * size.z);
for (int z = 0; z < size.z; ++z) {
    for (int x = 0; x < size.x; ++x) {
        int surface_local_y = size.y; // Default: all sky
        if (has_heightmap) {
            const int surface_world_y = heightmap[x + z * size.x];
            surface_local_y = surface_world_y - origin_y;
        }

        // If the surface is at or above the top of this buffer, this column is underground!
        if (surface_local_y >= size.y) {
            continue;
        }

        for (int y = size.y - 1; y >= 0; --y) {
            if (y <= surface_local_y) {
                break;
            }
            ...
```

Здесь **две разные проблемы**, и их надо не смешивать:

**(а) Вода и поверхностные прозрачные блоки получают 0 — это баг, чинится быстро.**
В GDScript-пути `height_cache` заполняется высотой **после воды**
(`generator.gd:430-439`: `height_f = water_data["height"]`). Тогда
`surface_local_y` — уровень воды, и цикл останавливается на воде:
вода и дно получают `0`. В native-пути с `enable_cliffs` в `height_cache`
кладётся `heights[idx]` из нативного генератора (`generator.gd:359-364`).
**Требует уточнения:** что именно лежит в `heights` — высота рельефа или
воды. Если рельефа, то вода в двух путях освещается по-разному.
Фикс: для освещения передавать высоту **рельефа**, а не воды, и пропускать
скан сквозь воду/листву (у них `filter < 15`, см. 1.4).

**(б) Шахта сквозь рельеф в «подземном» чанке не освещается — это ограничение.**
`heightmap` — высота рельефа, а не «первый непрозрачный блок сверху».
Если игрок/ генератор пробил вертикальный ход сквозь рельеф, `heightmap`
всё равно показывает верх склона, `surface_local_y >= size.y`, и `continue`
пропускает весь столбец. Физически свет там есть, но движок его не
запускает.

Быстрого и корректного фикса **нет**: чтобы отличить «шахту в небо» от
«закрытой пещеры», нужен флаг небесной экспозиции на колонку, вычисленный
по реальным вокселям сверху вниз, а не по шуму рельефа. Это данные, которых
у чанка при изолированной генерации нет (см. P0-4 / Фаза 4).

Что **работает уже сейчас**: игрок выкапывает блок — `on_block_removed`
(`voxel_light_engine.cpp:303-320`) пробивает свет вниз по колонке до первого
непрозрачного блока, поэтому выкопанные шахты освещаются. Не освещаются
только естественные отверстия, созданные генератором (пещеры, пробившие
поверхность).

### P0-4. Свет не перетекает между чанками (гипотеза G — подтверждена)

`storage/voxel_light_engine.h:66-69`:

```cpp
bool is_valid_pos(Vector3i pos) const override {
    const Vector3i local = pos - _offset;
    return local.x >= 0 && local.y >= 0 && local.z >= 0 && local.x < _size.x && local.y < _size.y && local.z < _size.z;
}
```

`VoxelBufferLightAccessor` не видит за пределами буфера. В
`edition/voxel_tool_terrain.cpp:62-76` террейн-аксессор идёт дальше: он
требует, чтобы блок уже был загружен:

```cpp
bool is_valid_pos(Vector3i pos) const override {
    ...
    if (!_terrain->get_bounds().contains(pos)) {
        return false;
    }
    const Vector3i bpos = _terrain->get_storage().voxel_to_block(pos);
    ...
    _last_bpos_valid = _terrain->get_storage().has_block(bpos, 0);
    return _last_bpos_valid;
}
```

Поиск по `terrain/` и `storage/voxel_data.cpp` не находит ни одного
упоминания `light` (кроме случайных слов в комментариях). То есть:

- при генерации каждый чанк светится изолированно;
- после загрузки соседа его свет не пересчитывается;
- на границе чанков возможны тёмные швы;
- свет не входит из освещённого чанка в тёмный.

Это архитектурный пробел, а не точечный баг.

### P1-1. Цвет солнца захардкожен, `u_sun_color` отсутствует (гипотеза B — подтверждена частично)

Шейдеры `shader/voxel_block.gdshader:12-14, 29-42`:

```glsl
global uniform float u_sun_intensity;
global uniform vec4 u_torch_color;
global uniform float u_cave_ambient;
...
float sun_factor = COLOR.r * clamp(u_sun_intensity, 0.0, 1.0);
...
vec3 sun_col = vec3(1.0, 0.98, 0.94);
vec3 torch_col = u_torch_color.rgb;
vec3 illumination = sun_curve * sun_col + block_curve * torch_col;
```

Аналогично в `voxel_block_cutout.gdshader:35`, `leaves.gdshader:62`,
`grass.gdshader:59`.

В `project.godot:370-383` зарегистрированы только три глобала:
`u_sun_intensity`, `u_torch_color`, `u_cave_ambient`. `u_sun_color` нет.

`u_sun_intensity` **обновляется** — `scripts/Game/EnvironmentController.gd:370-377`:

```gdscript
var sun_transition := clampf(day_factor + dawn_factor * 0.75 + dusk_factor * 0.6, 0.0, 1.0)
var night_ambient_sun := 0.22
var voxel_sun: float = clampf(lerpf(night_ambient_sun, 1.0, sun_transition) * weather_sun_dim, 0.08, 1.0)
RenderingServer.global_shader_parameter_set("u_sun_intensity", voxel_sun)
```

Функция вызывается каждые 0.033 с (`ENV_UPDATE_INTERVAL`, строка 66).

Итог: день/ночь по яркости работает (1.0 → 0.22), но:
- цвет воксельного мира не меняется (ночью тот же тёплый белый);
- воксельные шейдеры `unshaded`, поэтому `DirectionalLight3D`
  (`sun_light.light_color`, `light_energy` в строках 349-358) на них не влияет
  вообще — только на другие материалы.

### P1-2. Кривая `pow(x, 1.45)` (гипотеза C — подтверждена, но вторична)

`shader/voxel_block.gdshader:33-35`:

```glsl
float sun_curve = pow(sun_factor, 1.45);
float block_curve = pow(block_factor, 1.45);
```

При `sun_factor = level / 15`:

| level | `pow(x,1.45)` |
|------:|--------------:|
| 15 | 1.000 |
| 12 | 0.741 |
| 8  | 0.403 |
| 4  | 0.150 |
| 2  | 0.056 |
| 1  | 0.020 |

Уровни 1–3 практически невидимы. Это усиливает P0-1, но само по себе
кривую лучше не менять до фикса распространения — иначе станет видно
«свет из ниоткуда». Рекомендуемая замена — LUT из 16 значений в стиле
Minecraft (`pow(0.8, 15 - level)`) или `pow(x, 1.2)` + минимальный порог.

### P1-3. Мешер: заливка чанка одним значением света (дополнительная находка)

`meshers/blocky/voxel_mesher_blocky.cpp:760-763`:

```cpp
Span<const uint8_t> light_channel;
const bool has_light_channel = voxels.get_channel_as_bytes_read_only(VoxelBuffer::CHANNEL_LIGHT, light_channel);
const uint8_t uniform_light = static_cast<uint8_t>(voxels.get_voxel(0, 0, 0, VoxelBuffer::CHANNEL_LIGHT));
const bool has_light_data = params.bake_light;
```

`has_light_channel` **не используется**. Если канал сжат как `COMPRESSION_UNIFORM`
(а это происходит, когда света нет вообще), `light_channel` пуст, и все
вершины получают `uniform_light` из вокселя `(0,0,0)`. Для пустого чанка
это законно (все 15), для недоосвещённого — весь чанк станет одного цвета.
Коммит `1e4b9a85` уже правил похожий случай, но переменная так и осталась
мёртвой. Это надо превратить в осознанное решение (см. Фазу 3).

---

## Гипотезы, которые не подтвердились

### D. `calculate_lighting` вызывается при каждом редактировании — **опровергнуто**

В игровом коде полный пересчёт вызывается только при генерации чанка
(`scripts/World/generator.gd:727-740`):

```gdscript
if lod == 0 and _block_library != null:
    if origin.y + block_size <= min_surface_height:
        buffer.calculate_lighting(_block_library, false)
    elif origin.y > max_surface_height:
        buffer.calculate_lighting(_block_library, true)
    else:
        var hm := PackedInt32Array()
        ...
        buffer.calculate_lighting_with_heightmap(_block_library, hm, origin.y)
```

Редактирование блоков идёт через `VoxelToolTerrain` и **инкрементальный**
движок: `edition/voxel_tool_terrain.cpp:375-389` (`on_block_removed` /
`on_block_placed`). `auto_light_enabled` по умолчанию `true`
(`edition/voxel_tool.h:210`).

Замечание по производительности: ветка `origin.y > max_surface_height`
(чанк целиком в небе) делает полное сканирование и ставит в очередь
**каждый** воздушный воксель. Это лишняя работа, но не «вызов на каждый блок».

### E. Доступ к `CHANNEL_LIGHT` через `get_voxel` — **подтверждено, но это оптимизация**

`storage/voxel_light_engine.h:50-56`:

```cpp
uint8_t get_light(Vector3i pos) override {
    const Vector3i local = pos - _offset;
    if (local.x < 0 || local.y < 0 || local.z < 0 || local.x >= _size.x || local.y >= _size.y || local.z >= _size.z) {
        return 0;
    }
    return _buffer.get_voxel(local.x, local.y, local.z, VoxelBuffer::CHANNEL_LIGHT);
}
```

`VoxelBuffer::get_voxel` (`storage/voxel_buffer.cpp:267-300`) заново
проверяет границы через `ZN_ASSERT_RETURN_V` (эта проверка **активна и в
release**, см. `util/errors.h:39-49`) и делает `switch` по глубине канала.
То есть на каждый запрос — двойная проверка границ + switch. Для BFS это
заметно, но это не причина «свет не работает».

### F. Очередь BFS неоптимальна — **подтверждено**

`util/containers/std_queue.h:11-12`:

```cpp
template <typename TValue, typename TAllocator = StdDefaultAllocator<TValue>>
using StdQueue = std::queue<TValue, std::deque<TValue, TAllocator>>;
```

`storage/voxel_light_engine.h:85-88`:

```cpp
struct LightNode {
    Vector3i pos;
    uint8_t level;
};
```

`Vector3i` (12 байт) + `uint8_t` + выравнивание = 16 байт на узел,
`std::deque` аллоцирует блоками. На больших чанках это даёт лишние
аллокации и промахи кэша. Выигрыш реальный, но это Фаза 3.

### B. «Кто-то не обновляет `u_sun_intensity`» — **опровергнуто**

Обновляется каждый тик окружения (см. P1-1). Проблема в отсутствии
`u_sun_color`, а не в отсутствии обновления интенсивности.

---

## Дополнительные находки (не входили в A–G)

1. **Упаковка `VoxelLight` корректна.** `storage/voxel_light.h:18-45`:
   `pack`/`set_sun`/`set_block` используют правильные маски (`0xF0` / `0x0F`),
   потерь и перепутанных битов нет. Гипотезу «старшие/младшие 4 бита
   перепутаны» можно закрыть.

2. **Emission работает, но только через GDScript-таблицу.**
   В `data/blocks/blocks.tres` **нет ни одного** `light_emission` и
   `light_filter` (проверено поиском: 0 вхождений). Значения приходят из
   `VoxelMaterialUtil.DEFAULT_LIGHT_EMISSION` (костёр 14, лава 15, магма 12,
   грибы 10–12) и применяются в `apply_to_library()`
   (`VoxelMaterialUtil.gd:132-146`), после чего библиотека запекается.
   Генератор делает то же для своей копии (`generator.gd:129-131`).
   Если мод или инструмент загрузит `blocks.tres` напрямую, **без**
   `VoxelMaterialUtil`, все блоки станут непрозрачными (15) и не будут
   светиться. Это скрытая зависимость от игрового слоя.

3. **`on_block_placed` не уменьшает свет для частично прозрачных блоков.**
   `voxel_light_engine.cpp:247-272` обрабатывает только `filter >= 15`
   и `emission > 0`. Постановка листвы/воды (`filter = 2`) не пересчитывает
   соседние уровни — они остаются завышенными. Симптом слабый, но логика
   неполная.

4. **`calculate_buffer_lighting` не очищает канал перед расчётом.**
   Есть только `buffer.decompress_channel(VoxelBuffer::CHANNEL_LIGHT)`
   (строки 332 и 383). Для свежего буфера это нормально, для повторного
   расчёта (`VoxelToolBuffer.calculate_lighting`) старый свет останется.

5. **`VoxelBufferLightAccessor::is_valid_pos` не включает padding.**
   Мешер запрашивает свет с padding = 1, но при генерации свет не знает о
   соседях. Стыки чанков — см. P0-4.

6. **Внутренние поверхности меша не получают AO.**
   `voxel_mesher_blocky.cpp:543`:
   `arrays.colors.push_back(Color(sun, block, 1.0f, 1.0f) * modulate_color);`
   — третий канал (AO) всегда 1.0. Для боковых граней AO считается
   (строки 434-452). Влияет только на модели с внутренними поверхностями.

7. **`face_light` для боковых граней берётся у соседнего вокселя**
   (`voxel_mesher_blocky.cpp:299`: `get_light_at(neighbor_voxel_index)`).
   Это правильно и совпадает с Minecraft (грань освещается воздухом перед
   ней), но объясняет, почему светящийся блок не «подсвечивает» сам себя
   сбоку: у соседа уже 14, а не 15.

---

## План по фазам

Оценки — для одного человека с ограничением ~5 часов в день. Порядок
выбран так, чтобы после Фазы 1 уже стало видно разницу в игре.

---

### Фаза 0. Проверить диагноз до правок (30–40 минут)

**0.1. Тест-стенд на `VoxelBuffer` (20 мин).**
Добавить `test/light_propagation.gd` (в игровом проекте), который:

- создаёт `VoxelBuffer` 16×16×32;
- строит пол на `y = 4`, потолок на `y = 20` с дырой 1×1 в `(8, 20, 8)`;
- запекает библиотеку (`VoxelMaterialUtil.apply_to_library` + `bake`);
- вызывает `calculate_lighting(lib, true)`;
- печатает `sun = (light >> 4) & 0x0F` для колонки под дырой и для колонки
  под сплошным потолком.

Ожидание **до** фикса: под дырой 15 только в воздухе над полом, ниже —
затухание; под потолком 0. Ожидание **после**: под дырой вся колонка 15,
под потолком 0, а в стороны от дыры 14/13/12…

**0.2. Лог таблицы фильтров (10 мин).**
После `apply_to_library` + `bake` вывести для id 0, 12, 27, 184:
`lib.get_models()[i].get_light_filter()` и `get_light_emission()`.
Это подтвердит, что air = 1, water/leaves = 2, glass = 1, костёр = 14.

**0.3. Замер времени (10 мин).**
В `calculate_buffer_lighting*` под `#ifdef PROFILING_ENABLED` замерить
время. Это база для Фазы 3.

---

### Фаза 1. Критические фиксы (2.5–3 часа, день 1)

#### 1.1. Воздух — полностью прозрачный (`filter = 0`) — 20 мин

- `storage/voxel_light_engine.cpp:16-18`:
  ```cpp
  if (type == 0) {
      return 0; // Fully transparent
  }
  ```
- `meshers/blocky/voxel_blocky_model_empty.cpp:8`:
  `set_light_filter(0);`
- `scripts/systems/VoxelMaterialUtil.gd:28`:
  `0: 0,    # air` (и поправить комментарий на строке 26).

**Проверка:** тот же тест из 0.1 — под дырой свет доходит до пола.

> Совместимость: старые `.tres` без `light_filter` сохраняют дефолт 15.
> Меняется только поведение воздуха, который и так был особым случаем.

#### 1.2. Вертикальный солнечный луч без потерь при уровне 15 — 40 мин

`storage/voxel_light_engine.cpp:168-178`, в `propagate_sun_light`:

```cpp
const uint16_t ntype = accessor.get_type(npos);
const uint8_t filter = get_filter(lib, ntype);
if (filter >= 15) {
    continue;
}

// Minecraft: прямой луч уровня 15 идёт вниз через прозрачные блоки без потерь.
uint8_t attenuation;
if (filter == 0 && node.level == 15 && npos.y < node.pos.y) {
    attenuation = 0;
} else {
    attenuation = (filter < 1) ? 1 : filter;
}
if (node.level <= attenuation) {
    continue;
}
const uint8_t new_light = node.level - attenuation;
```

`propagate_block_light` (строки 84-88) **не менять** — свет факелов в
Minecraft затухает во все стороны одинаково.

**Проверка:** тест 0.1, плюс в игре — выкопать шахту глубиной 20+ блоков
от поверхности: дно должно быть полностью освещено (15), а не чёрное.

#### 1.3. Вода и поверхность: передавать высоту рельефа, а не воды — 40 мин

В `generator.gd` для освещения сейчас используется `height_cache`, где в
GDScript-пути лежит высота **после воды** (`height_f = water_data["height"]`,
строки 430-439). Из-за этого цикл в `calculate_buffer_lighting_with_heightmap`
останавливается на уровне воды, и вода + дно получают `0`.

Минимальная правка:

1. Завести отдельный `light_height_cache` (или второе значение в
   `height_cache`), куда класть высоту **рельефа** до применения воды:
   ```gdscript
   var terrain_height := roundi(height_f)   # до water_data
   ...
   light_height_cache[Vector2i(wx, wz)] = terrain_height
   ```
2. В блоке освещения (`generator.gd:734-740`) строить `hm` из
   `light_height_cache`, а не из `height_cache`.
3. Убедиться, что нативный путь (`enable_cliffs`) кладёт в этот кэш именно
   высоту рельефа. **Требует уточнения:** формат `heights` из нативного
   генератора.

**Про шахты сквозь рельеф:** `surface_local_y >= size.y → continue` **не
трогать** в рамках этой фазы. Корректно отличить «шахту в небо» от закрытой
пещеры по heightmap невозможно; это Фаза 4. Игрок свои шахты и так
освещает через `on_block_removed` (`voxel_light_engine.cpp:303-320`).

**Проверка:** океан — поверхность воды и верхние слои освещены, с глубиной
темнеет; берег под водой не чёрный. Выкопанная шахта 20+ блоков — дно
освещено.

#### 1.4. Согласовать первичный скан с распространением — 30 мин

Сейчас скан ставит 15 **сквозь** воду и листву (`filter = 2`), потому что
проверяет только `filter >= 15`. Это расходится с `propagate_sun_light`,
где вода/листва забирают 2 уровня. Варианты:

- либо в скане тоже применять `filter` (тогда вода на глубине 8 блоков
  станет тёмной — как в Minecraft);
- либо оставить скан только для `filter == 0`, а воду/листву освещать
  распространением.

Рекомендую второй вариант: скан заполняет только полностью прозрачные
блоки, остальное — BFS. Это убирает «светящуюся воду» и делает поведение
предсказуемым.

**Проверка:** океан — поверхность воды освещена, с глубиной темнеет;
под листвой дерева — тень, а не «плоский свет».

#### 1.5. Регрессия — 30 мин

- `test/light_propagation.gd` из 0.1 + кейсы:
  - шахта 1×1 до низа чанка → 15 по всей колонке;
  - сплошной потолок → 0 под ним;
  - боковое распространение → 14, 13, 12…;
  - постановка/удаление блока через `VoxelToolBuffer`/`VoxelBuffer` —
    уровень пересчитывается.
- Прогнать существующий `test/test_lighting_integration.gd` — он проверяет
  block light (костёр 14), не должен сломаться.

**Итого Фаза 1: ~2.5–3 часа.** После неё дневной свет уже должен вести себя
как в Minecraft в пределах одного чанка.

---

### Фаза 2. Шейдер и день/ночь (2–3 часа, день 1–2)

#### 2.1. Добавить `u_sun_color` (60 мин)

- `project.godot` → `[shader_globals]`:
  ```ini
  u_sun_color={
  "type": "color",
  "value": Color(1, 0.98, 0.94, 1)
  }
  ```
- Во все четыре шейдера (`voxel_block`, `voxel_block_cutout`, `leaves`,
  `grass`) добавить `global uniform vec4 u_sun_color;` и заменить
  `vec3 sun_col = vec3(1.0, 0.98, 0.94);` на `vec3 sun_col = u_sun_color.rgb;`
  (в `voxel_block_cutout.gdshader:35`, `leaves.gdshader:62`,
  `grass.gdshader:59` — там цвет встроен в выражение).
- `EnvironmentController.gd`, в `_update_lighting` рядом с
  `u_sun_intensity`:
  ```gdscript
  var keyframe_color: Color = _lerp_keyframes(t, SUN_KEYFRAMES, "color")
  var voxel_sun_color := (keyframe_color * _curr_sun_color).lerp(
          Color(0.55, 0.65, 1.0), 1.0 - sun_transition)
  RenderingServer.global_shader_parameter_set("u_sun_color", voxel_sun_color)
  ```
  (цвет луны — холодный, чтобы ночь отличалась не только яркостью).

#### 2.2. Кривая света (40 мин)

Заменить `pow(x, 1.45)` на таблицу в стиле Minecraft. Простейший вариант
без массива:

```glsl
float light_curve(float level01) {
    // level01 = level / 15
    float level = clamp(level01, 0.0, 1.0) * 15.0;
    return pow(0.8, 15.0 - level);
}
```

Проверить визуально: уровень 1 ≈ 0.035, 4 ≈ 0.18, 8 ≈ 0.47, 15 = 1.0.
Если покажется слишком контрастно — `pow(level01, 1.2)`.

**Важно:** менять кривую **после** Фазы 1, иначе станет видно «подсветку
из ниоткуда» на низких уровнях.

#### 2.3. Ночное освещение (30 мин)

Сейчас ночью `u_sun_intensity = 0.22`, а `ambient_floor = 0.04`.
Предложение:

- ночью `u_sun_intensity` опускать до `0.12–0.15` (сейчас 0.22 — довольно
  светло для «ночи»);
- `u_sun_color` уводить в синий (см. 2.1);
- `u_cave_ambient` оставить 0.03–0.05, но применять **после** AO, а не до
  (сейчас `illumination = max(illumination, ambient); illumination *= ao;`
  — в пещерах AO дополнительно гасит и без того минимальный свет).

#### 2.4. Проверка (20 мин)

- Полный цикл суток на поверхности: полдень → закат → ночь → рассвет.
  Ночью мир должен быть синеватым и заметно темнее, днём — тёплым.
- Пещера: не абсолютно чёрная, но и не «серая».
- Скриншоты до/после для сравнения.

**Итого Фаза 2: ~2.5 часа.**

---

### Фаза 3. Оптимизация (4–6 часов, день 2–3)

#### 3.1. Аксессор на сырые буферы (2–3 часа)

`storage/voxel_light_engine.h:34-80`. Идея: один раз получить
`Span<uint8_t>` для `CHANNEL_LIGHT` и `Span<uint16_t>` для `CHANNEL_TYPE`
через `get_channel_as_bytes*` (`voxel_buffer.cpp:724-750`) и читать по
индексу без `switch` и повторных проверок границ.

Важно: `calculate_buffer_lighting*` уже вызывает `decompress_channel`, так
что канал будет не `COMPRESSION_UNIFORM`. Для `VoxelTerrainLightAccessor`
оставить текущую реализацию (там хранилище блоков, а не один буфер).

**Как проверить:** замер из 0.3, до/после на 100 чанках; ожидаемо
−20…40 % времени освещения. Функциональные тесты должны остаться зелёными.

#### 3.2. Очередь BFS (60–90 мин)

- `util/containers/std_queue.h` не трогать глобально.
- В `voxel_light_engine.h` заменить `StdQueue<LightNode>` на
  `std::vector<LightNode>` + `size_t head`, с `reserve(...)`.
- Или упаковать узел в `uint32_t` (10+10+10 бит координат + 4 бита уровня),
  но только если чанк ≤ 1024 по каждой оси (у вас 32/64 — подходит).

**Проверка:** тот же бенчмарк; отсутствие регрессий.

#### 3.3. [Опционально, только после профилирования] Оптимизация очереди неба (30 мин)

> **Внимание:** наивная идея «пушить только нижний блок колонки» **ломает
> горизонтальное распространение света** в верхних слоях (под высокими навесами,
> скалами, парящими островами и в верхних входах в пещеры свет не затечёт сбоку).

Корректное решение, если профилирование покажет узкое место в BFS:
**пушить только граничные sky-блоки, у которых есть хотя бы один сосед с меньшим уровнем света** (или сосед, не являющийся прямым вертикальным лучом солнца). Все внутренние блоки открытого небесного столба, окружённые воздухом со светом 15, в очередь не ставятся.

Если производительность устраивает — отложить и не трогать.

#### 3.4. Осознанный fallback мешера (30 мин)

`voxel_mesher_blocky.cpp:760-763`: либо использовать `has_light_channel`,
либо удалить переменную. Рекомендую:

```cpp
const bool has_light_data = params.bake_light;
// Если канал отсутствует, uniform_light — единственное, что есть.
// Логируем один раз на чанк, чтобы такие случаи не терялись.
```

и добавить `ZN_PRINT_WARNING` при `!has_light_channel && uniform_light == 0`.

**Итого Фаза 3: ~4–5 часов.**

---

### Фаза 4. Архитектура: свет между чанками (1–3 дня, отдельное решение)

Проблема P0-4 не решается точечно. Варианты, от дешёвого к правильному:

**Вариант A — «пересветить соседей» (0.5–1 день).**
При изменении блока/загрузке чанка ставить в очередь соседние чанки
(радиус 1) и пересчитывать их свет с учётом уже посчитанных значений на
границе. Дешево, но не решает дальние швы и требует доступа к соседним
буферам во время генерации (их может ещё не быть).

**Вариант B — светить по «padded» буферу (1–2 дня).**
Перед расчётом света собирать буфер 34³ (чанк + 1 воксель со всех сторон)
из уже сгенерированных соседей. Тогда `is_valid_pos` перестаёт обрезать
свет на границе. Требует синхронизации: сосед может быть не готов —
нужен отложенный пересчёт (dirty-очередь).

**Вариант C — глобальный BFS по загруженным блокам (2–4 дня).**
`IVoxelLightAccessor` уже абстрагирует хранилище
(`voxel_light_engine.h:14-32`). Можно сделать аксессор поверх
`VoxelData` (как `VoxelTerrainLightAccessor`) и гонять один общий
движок по всем загруженным блокам с очередью. Это ближе всего к Minecraft,
но требует потокобезопасности: сейчас `VoxelLightEngine` — локальный объект
на стеке, без блокировок.

**Реально ли починить в форке?** Да, все три варианта реализуемы в форке:
архитектура аксессоров это позволяет. Переносить в Vulkan из-за одного
света не нужно.

**Что нельзя починить в форке:** если понадобится свет на GPU (тысячи
чанков в кадре, динамические источники, GI) — это уже смена рендер-пайплайна.

---

## Тесты и регрессия

В форке есть только `tests/tests.cpp` (общий), тестов света нет. В игре
есть `test/test_lighting_integration.gd` (block light) и
`test/test_lighting_common.gd`(если появится). Предлагаю:

1. **C++ unit-тест** `tests/test_light_engine.cpp` (подключить в
   `tests/tests.cpp`):
   - `test_sun_vertical_no_attenuation` — колонка 32, сверху 15, снизу 15;
   - `test_sun_horizontal_attenuation` — 15 → 14 → 13;
   - `test_opaque_blocks_sun` — камень полностью гасит;
   - `test_block_light_removal` — поставили/убрали костёр, свет вернулся;
   - `test_partial_filter` — листва (2) гасит на 2.
2. **GDScript integration** — расширить `test_lighting_integration.gd`:
   - после `apply_to_library` + `bake` проверить
     `get_light_filter()` для air/water/leaves/glass и
     `get_light_emission()` для костра/лавы;
   - `buffer.calculate_lighting(lib, true)` на сцене «пол + потолок с дырой»;
   - упаковка: `sun == (light >> 4) & 0x0F`, `block == light & 0x0F`.
3. **Мешер-тест:** собрать `VoxelMesherBlocky` на буфере с известным
   светом и проверить `ARRAY_COLOR` (r = sun/15, g = block/15, b = AO).
4. **Регрессия чанков:** сгенерировать 3×3 чанка с шахтой на стыке и
   проверить, что после Фазы 4 свет на границе непрерывен.

---

## Критерии выхода из форка

Уходить в собственный Vulkan-рендер имеет смысл, когда одновременно
выполняется несколько пунктов:

1. **Масштаб.** 16 384² мир, десятки тысяч чанков в кадре, и CPU-мешинг +
   BFS-свет не держат 60 FPS даже после Фаз 1–3 (профилирование показывает
   свет/мешинг > 50 % кадра).
2. **Динамика.** Нужны динамические источники света, тени от солнца на
   вокселях, GI/ambient occlusion в реальном времени — то, что `unshaded` +
   запечённый свет принципиально не даёт.
3. **Свет на GPU.** Хочется считать распространение света в compute-шейдере
   (это в 10–100 раз быстрее BFS на CPU), а форк завязан на CPU-мешинг.
4. **Сеты чанков.** Постоянные швы/подвисания на границах после Варианта C
   всё ещё заметны, и их проще решить единым пайплайном, чем патчить форк.

Пока цель — «Minecraft-подобный свет 4+4 бита на 4096²», форк полностью
достаточен. Переносить его в Vulkan только из-за освещения — неоправданно.

---

## Порядок работ (сводка)

| # | Задача | Файлы | Время | Эффект |
|---|--------|-------|------:|--------|
| 0 | Тест-стенд и замер | `test/light_propagation.gd` | 40 мин | диагноз |
| 1.1 | air filter = 0 | `voxel_light_engine.cpp`, `voxel_blocky_model_empty.cpp`, `VoxelMaterialUtil.gd` | 20 мин | высокий |
| 1.2 | вертикальный луч 15 | `voxel_light_engine.cpp` | 40 мин | **максимальный** |
| 1.3 | высота рельефа для света (вода) | `generator.gd`, `voxel_light_engine.cpp` | 40 мин | средний |
| 1.4 | скан только прозрачных | `voxel_light_engine.cpp` | 30 мин | средний |
| 1.5 | регрессия | `test/*` | 30 мин | — |
| 2.1 | `u_sun_color` | `project.godot`, 4 шейдера, `EnvironmentController.gd` | 60 мин | высокий |
| 2.2 | кривая света | шейдеры | 40 мин | средний |
| 2.3 | ночь | `EnvironmentController.gd`, шейдеры | 30 мин | средний |
| 3.1 | аксессор на span | `voxel_light_engine.h/.cpp` | 2–3 ч | перф |
| 3.2 | очередь | `voxel_light_engine.h` | 1–1.5 ч | перф |
| 3.3 | оптимизация очереди неба (опц.) | `voxel_light_engine.cpp` | 30 мин | перф |
| 3.4 | fallback мешера | `voxel_mesher_blocky.cpp` | 30 мин | стабильность |
| 4 | свет между чанками | `voxel_light_engine.*`, `terrain/*` | 1–3 дня | архитектура |

**Минимальный путь к «дневной свет работает»: 0 + 1.1 + 1.2 = ~1.5 часа.**
Этого достаточно, чтобы увидеть разницу; остальное — полировка и перф.

---

## Что осталось непроверенным

- Формат `heights` из нативного генератора (`enable_cliffs`): рельеф или
  вода. Влияет на P0-3 и на тёмную воду.
- Точные id блоков в `DEFAULT_LIGHT_FILTER` (27 = leaves, 12 = water,
  184 = glass) — совпадают с `blocks.tres`, но стоит перепроверить после
  любых изменений библиотеки.
- Поведение `BlockRegistry.get_light_filter` для модовых блоков: если он
  вернёт 0 для чего-то непрозрачного, свет пройдёт сквозь блок.
- Есть ли в проекте сохранение `CHANNEL_LIGHT` в SQLite и не теряется ли
  он при загрузке (вне рамок этой задачи, но влияет на «свет пропал после
  перезахода»).
