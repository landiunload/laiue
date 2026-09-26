#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Внутренний AABB-BVH над детьми одной составной коробки.
 *
 * Дерево ускоряет узкую фазу compound-тел: для AABB одного ребёнка оно
 * возвращает только тех детей второго тела, которые реально могут касаться.
 * Отбор обязан совпадать с первым тестом BuildBoxManifold (строгое касание
 * исключено), иначе изменится порядок контактов и replay-hash. Поэтому Query
 * отдаёт исходные индексы детей строго по возрастанию.
 *
 * Структура и обе функции — внутренние для модуля physics, а не часть
 * экспортируемого ABI (как RigidBroadphaseQuery). Память не выделяется ни
 * разу: узлы и workspace принадлежат вызывающему. Глобального состояния нет,
 * поэтому Build можно звать из параллельных диапазонов для разных тел, если
 * их nodes/workspace/outRoot не пересекаются. Query после барьера серийна.
 *
 * Раскладка узлов фиксирована: лист с исходным индексом i лежит в nodes[i]
 * (first == i, second == UINT32_MAX), а внутренние узлы дописываются с
 * индекса count. Внутренний узел хранит номера детей в first/second, а его
 * AABB — точное объединение AABB детей. Для count >= 1 строится ровно
 * 2 * count - 1 узлов (count листьев и count - 1 внутренних).
 */

typedef struct RigidCompoundBvhNode
{
    double minimum[3];
    double maximum[3];
    // Лист: second == UINT32_MAX, first — исходный индекс ребёнка.
    // Внутренний узел: first/second — индексы детей в этом же массиве.
    uint32_t first;
    uint32_t second;
} RigidCompoundBvhNode;

/*
 * Build.
 *
 * bounds указывает на первый входной AABB: шесть подряд идущих double
 * (minimum[0..2], затем maximum[0..2]); следующий AABB начинается через
 * stride байт. stride обязан быть не меньше 6 * sizeof(double) и кратен
 * sizeof(double), а bounds — выровнен по double; nodes выровнен по
 * RigidCompoundBvhNode. Диапазон [bounds, bounds + (count - 1) * stride + 48)
 * целиком адресуем.
 *
 * count >= 1. nodeCapacity >= 2 * count - 1 (переполнение проверяется в
 * uint64 до индексации). workspace — scratch вызывающего ровно на count
 * элементов uint32; после успеха его содержимое не определено. nodes,
 * workspace, bounds и outRoot не должны пересекаться ни с чем другим.
 *
 * Каждый AABB обязан быть конечным и упорядоченным: minimum[axis] <=
 * maximum[axis]. Вырожденный AABB (minimum == maximum) допустим. NaN и
 * ±Inf отклоняются.
 *
 * При любом неверном аргументе возвращается false до первой записи: ни
 * nodes, ни workspace, ни *outRoot не меняются. На успехе *outRoot получает
 * индекс корня, и дерево строится сбалансированным детерминированным
 * делением по медиане: ось — самая длинная у совокупного AABB диапазона
 * (ничья у младшей оси), ключ — безопасно посчитанный центр (ничья — по
 * исходному индексу). Диапазон делится по рангу, а не полной сортировкой:
 * собственная quickselect-разбиение с медианой трёх без выделения памяти,
 * с heapsort-фолбэком при исчерпании лимита глубины, поэтому асимптотика
 * не хуже прежней, а линейный по уровню типичный путь дешевле. Глубина
 * рекурсии Build не превышает log2(count), то есть 31.
 */
bool RigidCompoundBvhBuild(const void *bounds, size_t stride, uint32_t count,
                           RigidCompoundBvhNode *nodes, uint32_t nodeCapacity,
                           uint32_t *workspace, uint32_t *outRoot);

/*
 * Query.
 *
 * nodes и root — из успешного Build. Входной AABB [minimum, maximum] обязан
 * быть конечным и упорядоченным. Узел отвергается ровно как в
 * BuildBoxManifold: если по любой оси node.maximum <= query.minimum или
 * query.maximum <= node.minimum, касание не считается пересечением. Иначе
 * спуск продолжается; на листе исходный индекс пишется в outIndices.
 *
 * Попадания возвращаются по возрастанию исходного индекса. Если их больше
 * capacity, функция возвращает false, не переполняя буфер и не трогая
 * *outCount. На успехе *outCount получает число попаданий (<= capacity).
 *
 * Обход идёт явным стеком на 32 кадра: сбалансированное дерево из Build не
 * глубже 31. Вызывающий передаёт корректно построенное дерево; nodes,
 * outIndices, minimum и maximum не должны пересекаться. Неверные аргументы,
 * нефинитный запрос и root == UINT32_MAX дают false без записи *outCount.
 */
bool RigidCompoundBvhQuery(const RigidCompoundBvhNode *nodes, uint32_t root,
                           const double minimum[3], const double maximum[3],
                           uint32_t *outIndices, uint32_t capacity, uint32_t *outCount);
