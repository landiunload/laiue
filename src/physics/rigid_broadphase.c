#include "physics/rigid_broadphase.h"

#include "physics/rigid_body.h"

#include <float.h>
#include <stddef.h>

// The pooled-index/fat-AABB design and the need for active rotations are
// described by Box2D's primary documentation:
// https://box2d.org/documentation/group__tree.html
// https://box2d.org/posts/2014/08/balancing-dynamic-trees/
// This implementation uses a 3D saturating perimeter heuristic, strict AVL
// heights, and parent-link traversal with no allocation or recursive calls.
#define RIGID_TREE_EMPTY UINT32_MAX
#define RIGID_TREE_MARGIN 0.05

typedef struct RigidTreeNode
{
    double minimum[3];
    double maximum[3];
    uint32_t parent;
    uint32_t left;
    uint32_t right;
    uint32_t slot;
    int32_t height;
} RigidTreeNode;

static uint32_t NodeCapacity(uint32_t bodyCapacity)
{
    return bodyCapacity * 2u - 1u;
}

uint32_t VoxelRigidBroadphaseBytes(uint32_t bodyCapacity)
{
    if (bodyCapacity == 0u || bodyCapacity > VOXEL_RIGID_MAX_BODIES)
    {
        return 0u;
    }
    uint64_t total = ((uint64_t)bodyCapacity * 2u - 1u) * sizeof(RigidTreeNode) +
                     (uint64_t)bodyCapacity * sizeof(uint32_t) + 63u;
    return total > UINT32_MAX ? 0u : (uint32_t)total;
}

static bool ValidSpan(const void *pointer, size_t bytes)
{
    return pointer != NULL && bytes <= UINTPTR_MAX - (uintptr_t)pointer;
}

static bool SpansOverlap(const void *first, size_t firstBytes,
                          const void *second, size_t secondBytes)
{
    uintptr_t firstAddress = (uintptr_t)first;
    uintptr_t secondAddress = (uintptr_t)second;
    if (firstAddress <= secondAddress)
    {
        return secondAddress - firstAddress < firstBytes;
    }
    return firstAddress - secondAddress < secondBytes;
}

static bool AllocationValid(const VoxelRigidBroadphase *broadphase)
{
    if (broadphase == NULL)
    {
        return false;
    }
    uint32_t required = VoxelRigidBroadphaseBytes(broadphase->bodyCapacity);
    return required != 0u && broadphase->storageBytes >= required &&
           ValidSpan(broadphase->storage, broadphase->storageBytes) &&
           !SpansOverlap(broadphase, sizeof(*broadphase), broadphase->storage,
                           broadphase->storageBytes);
}

bool RigidBroadphaseValid(const VoxelRigidBroadphase *broadphase, uint32_t bodyCount)
{
    if (!AllocationValid(broadphase) || broadphase->bodyCapacity < bodyCount ||
        broadphase->proxyCount > broadphase->bodyCapacity ||
        broadphase->indexedBodyCount > broadphase->bodyCapacity)
    {
        return false;
    }
    uint32_t capacity = NodeCapacity(broadphase->bodyCapacity);
    return (broadphase->root == RIGID_TREE_EMPTY || broadphase->root < capacity) &&
           (broadphase->freeList == RIGID_TREE_EMPTY || broadphase->freeList < capacity) &&
           (broadphase->root == RIGID_TREE_EMPTY) == (broadphase->proxyCount == 0u);
}

static RigidTreeNode *TreeNodes(const VoxelRigidBroadphase *broadphase)
{
    uintptr_t padding = (0u - (uintptr_t)broadphase->storage) & 63u;
    return (RigidTreeNode *)((uint8_t *)broadphase->storage + padding);
}

static uint32_t *TreeSlots(const VoxelRigidBroadphase *broadphase)
{
    return (uint32_t *)(TreeNodes(broadphase) + NodeCapacity(broadphase->bodyCapacity));
}

void VoxelRigidBroadphaseReset(VoxelRigidBroadphase *broadphase)
{
    if (!AllocationValid(broadphase))
    {
        return;
    }
    broadphase->root = RIGID_TREE_EMPTY;
    broadphase->freeList = 0u;
    broadphase->proxyCount = 0u;
    broadphase->updatedProxyCount = 0u;
    broadphase->visitedNodeCount = 0u;
    broadphase->indexedBodyCount = 0u;
    RigidTreeNode *nodes = TreeNodes(broadphase);
    uint32_t capacity = NodeCapacity(broadphase->bodyCapacity);
    for (uint32_t index = 0u; index < capacity; ++index)
    {
        nodes[index].parent = index + 1u < capacity ? index + 1u : RIGID_TREE_EMPTY;
        nodes[index].left = RIGID_TREE_EMPTY;
        nodes[index].right = RIGID_TREE_EMPTY;
        nodes[index].slot = RIGID_TREE_EMPTY;
        nodes[index].height = -1;
    }
    uint32_t *slots = TreeSlots(broadphase);
    for (uint32_t index = 0u; index < broadphase->bodyCapacity; ++index)
    {
        slots[index] = RIGID_TREE_EMPTY;
    }
}

bool VoxelRigidBroadphaseInitialize(VoxelRigidBroadphase *broadphase, void *storage,
                                    uint32_t bodyCapacity, uint32_t storageBytes)
{
    uint32_t required = VoxelRigidBroadphaseBytes(bodyCapacity);
    if (broadphase == NULL || required == 0u || storageBytes < required ||
        !ValidSpan(storage, storageBytes) ||
        SpansOverlap(broadphase, sizeof(*broadphase), storage, storageBytes))
    {
        return false;
    }
    broadphase->storage = storage;
    broadphase->storageBytes = storageBytes;
    broadphase->bodyCapacity = bodyCapacity;
    VoxelRigidBroadphaseReset(broadphase);
    return true;
}

static bool BoundsValid(const double minimum[3], const double maximum[3])
{
    if (minimum == NULL || maximum == NULL)
    {
        return false;
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (!(minimum[axis] >= -DBL_MAX && maximum[axis] <= DBL_MAX &&
              minimum[axis] <= maximum[axis]))
        {
            return false;
        }
    }
    return true;
}

static void CountEvent(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
    {
        ++*counter;
    }
}

static bool BoundsContain(const RigidTreeNode *node,
                           const double minimum[3], const double maximum[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (minimum[axis] < node->minimum[axis] || maximum[axis] > node->maximum[axis])
        {
            return false;
        }
    }
    return true;
}

static bool BoundsOverlap(const RigidTreeNode *node,
                           const double minimum[3], const double maximum[3])
{
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        if (node->minimum[axis] > maximum[axis] || node->maximum[axis] < minimum[axis])
        {
            return false;
        }
    }
    return true;
}

static double BoundsMetric(const RigidTreeNode *first, const RigidTreeNode *second)
{
    double total = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        double minimum = first->minimum[axis];
        double maximum = first->maximum[axis];
        if (second != NULL)
        {
            if (second->minimum[axis] < minimum) minimum = second->minimum[axis];
            if (second->maximum[axis] > maximum) maximum = second->maximum[axis];
        }
        double extent = maximum - minimum;
        if (!(extent < DBL_MAX - total))
        {
            return DBL_MAX;
        }
        total += extent;
    }
    return total;
}

static void RefreshNode(RigidTreeNode *nodes, uint32_t index)
{
    RigidTreeNode *node = &nodes[index];
    const RigidTreeNode *left = &nodes[node->left];
    const RigidTreeNode *right = &nodes[node->right];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        node->minimum[axis] = left->minimum[axis] < right->minimum[axis] ?
                              left->minimum[axis] : right->minimum[axis];
        node->maximum[axis] = left->maximum[axis] > right->maximum[axis] ?
                              left->maximum[axis] : right->maximum[axis];
    }
    node->height = 1 + (left->height > right->height ? left->height : right->height);
}

static void ReplaceChild(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                          uint32_t parent, uint32_t oldChild, uint32_t newChild)
{
    if (parent == RIGID_TREE_EMPTY)
    {
        broadphase->root = newChild;
    }
    else if (nodes[parent].left == oldChild)
    {
        nodes[parent].left = newChild;
    }
    else
    {
        nodes[parent].right = newChild;
    }
    nodes[newChild].parent = parent;
}

static bool KeepFirstGrandchild(const RigidTreeNode *nodes, uint32_t first,
                                uint32_t second, uint32_t fixed)
{
    if (nodes[first].height != nodes[second].height)
    {
        return nodes[first].height > nodes[second].height;
    }
    // Either choice preserves AVL heights. Minimize the new lower node's box;
    // the upper node contains the same subtree, so its bounds cannot change.
    double firstCost = BoundsMetric(&nodes[fixed], &nodes[first]);
    double secondCost = BoundsMetric(&nodes[fixed], &nodes[second]);
    return secondCost < firstCost || (secondCost == firstCost && first < second);
}

static uint32_t BalanceNode(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                             uint32_t index)
{
    RigidTreeNode *node = &nodes[index];
    if (node->height < 2)
    {
        return index;
    }
    uint32_t left = node->left;
    uint32_t right = node->right;
    int32_t difference = nodes[right].height - nodes[left].height;
    if (difference > 1)
    {
        uint32_t first = nodes[right].left;
        uint32_t second = nodes[right].right;
        ReplaceChild(broadphase, nodes, node->parent, index, right);
        nodes[right].left = index;
        node->parent = right;
        bool keepFirst = KeepFirstGrandchild(nodes, first, second, left);
        uint32_t kept = keepFirst ? first : second;
        uint32_t moved = keepFirst ? second : first;
        nodes[right].right = kept;
        node->right = moved;
        nodes[kept].parent = right;
        nodes[moved].parent = index;
        RefreshNode(nodes, index);
        RefreshNode(nodes, right);
        return right;
    }
    if (difference < -1)
    {
        uint32_t first = nodes[left].left;
        uint32_t second = nodes[left].right;
        ReplaceChild(broadphase, nodes, node->parent, index, left);
        nodes[left].right = index;
        node->parent = left;
        bool keepFirst = KeepFirstGrandchild(nodes, first, second, right);
        uint32_t kept = keepFirst ? first : second;
        uint32_t moved = keepFirst ? second : first;
        nodes[left].left = kept;
        node->left = moved;
        nodes[kept].parent = left;
        nodes[moved].parent = index;
        RefreshNode(nodes, index);
        RefreshNode(nodes, left);
        return left;
    }
    return index;
}

static void ImproveNodeQuality(RigidTreeNode *nodes, uint32_t index)
{
    RigidTreeNode *node = &nodes[index];
    if (node->height < 2) return;
    uint32_t branch = nodes[node->left].height > nodes[node->right].height
                          ? node->left : node->right;
    uint32_t outside = branch == node->left ? node->right : node->left;
    // A single child/grandchild swap can preserve AVL balance only when the
    // branch is one level taller than the outer child. Equal-height children
    // would leave the new inner branch two levels taller than the moved child.
    if (nodes[branch].height != nodes[outside].height + 1) return;
    double bestCost = BoundsMetric(&nodes[branch], NULL);
    uint32_t bestMoved = RIGID_TREE_EMPTY;
    // Height balance alone cannot repair spatially overlapping branches after
    // objects move. Try swapping one outer child with an inner grandchild.
    // Both affected nodes must remain AVL-balanced. For an already balanced
    // parent this also preserves its height, so no extra ancestor pass is needed.
    // Only the inner branch changes bounds: decreasing its saturating perimeter
    // decreases the tree's total metric. The outer and moved nodes retain their
    // own bounds, while the union at this parent is unchanged.
    for (uint32_t child = 0u; child < 2u; ++child)
    {
        uint32_t moved = child == 0u ? nodes[branch].left : nodes[branch].right;
        uint32_t retained = child == 0u ? nodes[branch].right : nodes[branch].left;
        int32_t outsideHeight = nodes[outside].height;
        int32_t retainedHeight = nodes[retained].height;
        int32_t difference = outsideHeight - retainedHeight;
        if (difference < -1 || difference > 1) continue;
        int32_t branchHeight = 1 + (outsideHeight > retainedHeight
                                       ? outsideHeight : retainedHeight);
        difference = branchHeight - nodes[moved].height;
        if (difference < -1 || difference > 1) continue;
        double cost = BoundsMetric(&nodes[outside], &nodes[retained]);
        if (cost < bestCost)
        {
            bestCost = cost;
            bestMoved = moved;
        }
    }
    if (bestMoved == RIGID_TREE_EMPTY) return;
    if (node->left == branch) node->right = bestMoved;
    else node->left = bestMoved;
    if (nodes[branch].left == bestMoved) nodes[branch].left = outside;
    else nodes[branch].right = outside;
    nodes[bestMoved].parent = index;
    nodes[outside].parent = branch;
    RefreshNode(nodes, branch);
    RefreshNode(nodes, index);
}

static void RefreshAncestors(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes,
                              uint32_t index)
{
    while (index != RIGID_TREE_EMPTY)
    {
        RefreshNode(nodes, index);
        uint32_t balanced = BalanceNode(broadphase, nodes, index);
        // At most one spatial-quality swap per refitted node; never recurse or
        // iterate until convergence during a simulation tick.
        ImproveNodeQuality(nodes, balanced);
        index = nodes[balanced].parent;
    }
}

static uint32_t TakeNode(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes)
{
    uint32_t result = broadphase->freeList;
    if (result != RIGID_TREE_EMPTY)
    {
        broadphase->freeList = nodes[result].parent;
        nodes[result].parent = RIGID_TREE_EMPTY;
        nodes[result].left = RIGID_TREE_EMPTY;
        nodes[result].right = RIGID_TREE_EMPTY;
        nodes[result].slot = RIGID_TREE_EMPTY;
        nodes[result].height = 0;
    }
    return result;
}

static void ReturnNode(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t index)
{
    nodes[index].parent = broadphase->freeList;
    nodes[index].height = -1;
    broadphase->freeList = index;
}

static bool InsertLeaf(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t leaf)
{
    if (broadphase->root == RIGID_TREE_EMPTY)
    {
        broadphase->root = leaf;
        nodes[leaf].parent = RIGID_TREE_EMPTY;
        return true;
    }
    uint32_t sibling = broadphase->root;
    // Descending to a leaf keeps each insertion's height change <= 1,
    // allowing strict AVL rebalancing on the single ancestor path.
    while (nodes[sibling].left != RIGID_TREE_EMPTY)
    {
        uint32_t left = nodes[sibling].left;
        uint32_t right = nodes[sibling].right;
        double leftSize = BoundsMetric(&nodes[left], NULL);
        double rightSize = BoundsMetric(&nodes[right], NULL);
        double leftCost = BoundsMetric(&nodes[left], &nodes[leaf]) - leftSize;
        double rightCost = BoundsMetric(&nodes[right], &nodes[leaf]) - rightSize;
        bool chooseLeft = leftCost < rightCost ||
            (leftCost == rightCost && (leftSize < rightSize ||
                                       (leftSize == rightSize && left < right)));
        sibling = chooseLeft ? left : right;
    }
    uint32_t parent = TakeNode(broadphase, nodes);
    if (parent == RIGID_TREE_EMPTY)
    {
        return false;
    }
    uint32_t previousParent = nodes[sibling].parent;
    nodes[parent].left = sibling;
    nodes[parent].right = leaf;
    nodes[leaf].parent = parent;
    ReplaceChild(broadphase, nodes, previousParent, sibling, parent);
    nodes[sibling].parent = parent;
    RefreshAncestors(broadphase, nodes, parent);
    return true;
}

static void RemoveLeaf(VoxelRigidBroadphase *broadphase, RigidTreeNode *nodes, uint32_t leaf)
{
    if (broadphase->root == leaf)
    {
        broadphase->root = RIGID_TREE_EMPTY;
        nodes[leaf].parent = RIGID_TREE_EMPTY;
        return;
    }
    uint32_t parent = nodes[leaf].parent;
    uint32_t sibling = nodes[parent].left == leaf ? nodes[parent].right : nodes[parent].left;
    uint32_t grandparent = nodes[parent].parent;
    ReplaceChild(broadphase, nodes, grandparent, parent, sibling);
    ReturnNode(broadphase, nodes, parent);
    nodes[leaf].parent = RIGID_TREE_EMPTY;
    RefreshAncestors(broadphase, nodes, grandparent);
}

bool RigidBroadphaseSetProxy(VoxelRigidBroadphase *broadphase, uint32_t slot,
                              const double minimum[3], const double maximum[3])
{
    if (!RigidBroadphaseValid(broadphase, 0u) || slot >= broadphase->bodyCapacity ||
        !BoundsValid(minimum, maximum))
    {
        return false;
    }
    RigidTreeNode *nodes = TreeNodes(broadphase);
    uint32_t *slots = TreeSlots(broadphase);
    uint32_t leaf = slots[slot];
    bool existing = leaf != RIGID_TREE_EMPTY;
    if (existing && BoundsContain(&nodes[leaf], minimum, maximum))
    {
        bool oversized = false;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            // Containment alone retains giant proxies after a shape shrinks or
            // a slot is reused. A wider hysteresis envelope avoids per-tick churn.
            if (nodes[leaf].minimum[axis] < minimum[axis] - 4.0 * RIGID_TREE_MARGIN ||
                nodes[leaf].maximum[axis] > maximum[axis] + 4.0 * RIGID_TREE_MARGIN)
            {
                oversized = true;
            }
        }
        if (!oversized) return true;
    }
    if (existing)
    {
        RemoveLeaf(broadphase, nodes, leaf);
    }
    else
    {
        leaf = TakeNode(broadphase, nodes);
        if (leaf == RIGID_TREE_EMPTY)
        {
            return false;
        }
    }
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        nodes[leaf].minimum[axis] = minimum[axis] - RIGID_TREE_MARGIN;
        nodes[leaf].maximum[axis] = maximum[axis] + RIGID_TREE_MARGIN;
    }
    nodes[leaf].slot = slot;
    if (!InsertLeaf(broadphase, nodes, leaf))
    {
        if (!existing) ReturnNode(broadphase, nodes, leaf);
        return false;
    }
    slots[slot] = leaf;
    if (!existing) ++broadphase->proxyCount;
    CountEvent(&broadphase->updatedProxyCount);
    return true;
}

void RigidBroadphaseRemoveProxy(VoxelRigidBroadphase *broadphase, uint32_t slot)
{
    if (!RigidBroadphaseValid(broadphase, 0u) || slot >= broadphase->bodyCapacity)
    {
        return;
    }
    uint32_t *slots = TreeSlots(broadphase);
    uint32_t leaf = slots[slot];
    if (leaf == RIGID_TREE_EMPTY)
    {
        return;
    }
    RigidTreeNode *nodes = TreeNodes(broadphase);
    RemoveLeaf(broadphase, nodes, leaf);
    ReturnNode(broadphase, nodes, leaf);
    slots[slot] = RIGID_TREE_EMPTY;
    --broadphase->proxyCount;
    CountEvent(&broadphase->updatedProxyCount);
}

bool RigidBroadphaseQuery(VoxelRigidBroadphase *broadphase, const double minimum[3],
                           const double maximum[3], uint32_t *outSlots,
                           uint32_t capacity, uint32_t *outCount)
{
    if (!RigidBroadphaseValid(broadphase, 0u) || !BoundsValid(minimum, maximum) ||
        outCount == NULL || (capacity != 0u && outSlots == NULL))
    {
        return false;
    }
    *outCount = 0u;
    const RigidTreeNode *nodes = TreeNodes(broadphase);
    uint32_t current = broadphase->root;
    uint32_t previous = RIGID_TREE_EMPTY;
    while (current != RIGID_TREE_EMPTY)
    {
        const RigidTreeNode *node = &nodes[current];
        uint32_t next = node->parent;
        if (previous == node->parent)
        {
            CountEvent(&broadphase->visitedNodeCount);
            if (BoundsOverlap(node, minimum, maximum))
            {
                if (node->left == RIGID_TREE_EMPTY)
                {
                    if (*outCount == capacity)
                    {
                        return false;
                    }
                    outSlots[(*outCount)++] = node->slot;
                }
                else
                {
                    next = node->left;
                }
            }
        }
        else if (previous == node->left)
        {
            next = node->right;
        }
        previous = current;
        current = next;
    }
    return true;
}
