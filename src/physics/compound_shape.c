#include "physics/compound_shape.h"

#include <string.h>

// === Скаляры ===

static bool CompoundShapeIsFinite(double value)
{
    union
    {
        double scalar;
        uint64_t bits;
    } representation = {value};
    return ((representation.bits >> 52) & 0x7ffu) != 0x7ffu;
}

// Границы коробки одновременно проверяют конечность центра и полуребра,
// строгую положительность полуребра и невырожденность интервала: NaN/Inf
// центра или полуребра, нулевое/отрицательное полуребро, переполнение
// center + halfExtent и схлопывание в точку дают false.
static bool CompoundShapeBounds(const VoxelRigidCompoundBox *box, double outMin[3],
                                double outMax[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        double low = box->center[axis] - box->halfExtent[axis];
        double high = box->center[axis] + box->halfExtent[axis];
        if (!CompoundShapeIsFinite(low) || !CompoundShapeIsFinite(high) || !(low < high))
        {
            return false;
        }
        outMin[axis] = low;
        outMax[axis] = high;
    }
    return true;
}

// Строгое пересечение интервалов: касание гранями перекрытием не считается.
static bool CompoundShapeOverlap(const double firstMin[3], const double firstMax[3],
                                 const double secondMin[3], const double secondMax[3])
{
    for (int32_t axis = 0; axis < 3; ++axis)
    {
        if (!(firstMin[axis] < secondMax[axis]) || !(secondMin[axis] < firstMax[axis]))
        {
            return false;
        }
    }
    return true;
}

// Слияние lower и upper вдоль axis. Сливаются только коробки с точно
// совпадающими границами по двум другим осям и точно сомкнутыми гранями по
// axis. Результат обязан точно восстановить объединённые концы, иначе
// слияние пропускается: терять представимость на огромных координатах
// нельзя. lower и upper не перекрываются (это гарантирует превалидация).
static bool CompoundShapeMergePair(const VoxelRigidCompoundBox *lower,
                                   const VoxelRigidCompoundBox *upper, int32_t axis,
                                   VoxelRigidCompoundBox *outMerged)
{
    for (int32_t other = 0; other < 3; ++other)
    {
        if (other == axis)
        {
            continue;
        }
        double lowerMin = lower->center[other] - lower->halfExtent[other];
        double lowerMax = lower->center[other] + lower->halfExtent[other];
        double upperMin = upper->center[other] - upper->halfExtent[other];
        double upperMax = upper->center[other] + upper->halfExtent[other];
        if (lowerMin != upperMin || lowerMax != upperMax)
        {
            return false;
        }
    }

    double lowerMin = lower->center[axis] - lower->halfExtent[axis];
    double lowerMax = lower->center[axis] + lower->halfExtent[axis];
    double upperMin = upper->center[axis] - upper->halfExtent[axis];
    double upperMax = upper->center[axis] + upper->halfExtent[axis];
    if (lowerMax != upperMin && upperMax != lowerMin)
    {
        return false;
    }

    double low = lowerMin < upperMin ? lowerMin : upperMin;
    double high = lowerMax > upperMax ? lowerMax : upperMax;
    double half = (high - low) / 2.0;
    double center = low + half;
    if (!(half > 0.0) || !CompoundShapeIsFinite(half) || !CompoundShapeIsFinite(center))
    {
        return false;
    }
    if (center - half != low || center + half != high)
    {
        return false;
    }

    *outMerged = *lower;
    outMerged->center[axis] = center;
    outMerged->halfExtent[axis] = half;
    return true;
}

static bool CompoundShapeRangesOverlap(const void *first, uint64_t firstBytes, const void *second,
                                       uint64_t secondBytes)
{
    uintptr_t firstAddress = (uintptr_t)first;
    uintptr_t secondAddress = (uintptr_t)second;
    // Вычитание адресов не переполняется даже на заведомо неверном диапазоне.
    return firstBytes != 0u && secondBytes != 0u &&
           (firstAddress <= secondAddress ? secondAddress - firstAddress < firstBytes
                                          : firstAddress - secondAddress < secondBytes);
}

bool VoxelRigidCompoundMergeBoxes(const VoxelRigidCompoundBox *boxes, uint32_t count,
                                  VoxelRigidCompoundBox *outBoxes, uint32_t capacity,
                                  uint32_t *outCount)
{
    if (boxes == NULL || outBoxes == NULL || outCount == NULL || count == 0u ||
        capacity < count)
    {
        return false;
    }

    uint64_t inputBytes = (uint64_t)count * sizeof(*boxes);
    uint64_t outputBytes = (uint64_t)capacity * sizeof(*outBoxes);
    if (inputBytes > SIZE_MAX || outputBytes > SIZE_MAX ||
        inputBytes > UINTPTR_MAX - (uintptr_t)boxes ||
        outputBytes > UINTPTR_MAX - (uintptr_t)outBoxes)
    {
        return false;
    }
    // outCount не может указывать ни во вход, ни в выход: иначе запись
    // результата затрёт счётчик или наоборот.
    if (CompoundShapeRangesOverlap(outCount, sizeof(*outCount), boxes, inputBytes) ||
        CompoundShapeRangesOverlap(outCount, sizeof(*outCount), outBoxes, outputBytes))
    {
        return false;
    }
    // Полное совмещение (in-place) разрешено, частичное — нет.
    if (boxes != outBoxes &&
        CompoundShapeRangesOverlap(boxes, inputBytes, outBoxes, outputBytes))
    {
        return false;
    }

    // Как и весь solver, геометрия считается при фиксированном FP-режиме.
    VoxelPhysicsConfigureThread();

    for (uint32_t index = 0u; index < count; ++index)
    {
        double low[3];
        double high[3];
        if (!CompoundShapeBounds(&boxes[index], low, high))
        {
            return false;
        }
    }
    for (uint32_t first = 0u; first + 1u < count; ++first)
    {
        double firstMin[3];
        double firstMax[3];
        CompoundShapeBounds(&boxes[first], firstMin, firstMax);
        for (uint32_t second = first + 1u; second < count; ++second)
        {
            double secondMin[3];
            double secondMax[3];
            CompoundShapeBounds(&boxes[second], secondMin, secondMax);
            if (CompoundShapeOverlap(firstMin, firstMax, secondMin, secondMax))
            {
                return false;
            }
        }
    }

    if (outBoxes != boxes)
    {
        memcpy(outBoxes, boxes, (size_t)inputBytes);
    }

    uint32_t live = count;
    bool mergedAny = true;
    while (mergedAny)
    {
        mergedAny = false;
        for (int32_t axis = 0; axis < 3; ++axis)
        {
            uint32_t index = 0u;
            while (index < live)
            {
                uint32_t candidate = index + 1u;
                while (candidate < live)
                {
                    VoxelRigidCompoundBox merged;
                    if (CompoundShapeMergePair(&outBoxes[index], &outBoxes[candidate], axis,
                                               &merged))
                    {
                        outBoxes[index] = merged;
                        for (uint32_t move = candidate; move + 1u < live; ++move)
                        {
                            outBoxes[move] = outBoxes[move + 1u];
                        }
                        --live;
                        mergedAny = true;
                        // candidate теперь указывает на сдвинутый элемент:
                        // продолжаем с него, не увеличивая индекс.
                    }
                    else
                    {
                        ++candidate;
                    }
                }
                ++index;
            }
        }
    }

    *outCount = live;
    return true;
}
