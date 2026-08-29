#include "physics/voxel_body.h"
#include "test_runtime.h"

typedef struct PhysicsTestContext
{
    bool useFloor;
    bool useDynamicPlatform;
    uint32_t blockQueries;
    uint32_t dynamicQueries;
} PhysicsTestContext;

static uint32_t physicsChecks;

static void PhysicsExpect(bool condition, const char *name)
{
    ++physicsChecks;
    if (condition)
    {
        return;
    }
    LaiueTestRuntimeWrite("Physics check failed: ");
    LaiueTestRuntimeWrite(name);
    LaiueTestRuntimeWrite("\r\n");
    LaiueTestRuntimeExit(1);
}

static bool Near(double value, double expected, double tolerance)
{
    double difference = value - expected;
    if (difference < 0.0)
    {
        difference = -difference;
    }
    return difference <= tolerance;
}

static void QueryBlockPhysics(void *rawContext, int64_t x, int64_t y, int64_t z,
                              VoxelBlockPhysics *outBlock)
{
    PhysicsTestContext *context = (PhysicsTestContext *)rawContext;
    (void)x;
    (void)y;
    ++context->blockQueries;
    outBlock->flags = context->useFloor && z == 0 ? VOXEL_BLOCK_PHYSICS_SOLID : 0U;
    outBlock->friction = context->useFloor && z == 0 ? 0.75f : 0.0f;
}

static bool QueryDynamicPlatform(void *rawContext, const VoxelBodyBounds *queryBounds,
                                 VoxelDynamicCollider *outColliders, uint32_t colliderCapacity,
                                 uint32_t *outColliderCount)
{
    PhysicsTestContext *context = (PhysicsTestContext *)rawContext;
    (void)queryBounds;
    ++context->dynamicQueries;
    if (!context->useDynamicPlatform)
    {
        *outColliderCount = 0U;
        return true;
    }
    if (colliderCapacity < 1U)
    {
        *outColliderCount = 0U;
        return false;
    }
    outColliders[0] = (VoxelDynamicCollider){
        .bounds =
            {
                .minimum = {-1.0, -1.0, 0.0},
                .maximum = {1.0, 1.0, 0.5},
            },
        .velocity = {0.25, -0.5, 0.0},
        .friction = 0.5f,
        .stableId = 17U,
    };
    *outColliderCount = 1U;
    return true;
}

static VoxelBodyShape TestShape(void)
{
    VoxelBodyShape shape = {
        .radius = 0.3,
        .height = 1.8,
        .eyeHeight = 1.6,
        .collisionEpsilon = 0.001,
    };
    return shape;
}

static void TestStaticVoxelCollision(void)
{
    PhysicsTestContext context = {
        .useFloor = true,
    };
    VoxelCollisionSource source = {
        .context = &context,
        .queryBlockPhysics = QueryBlockPhysics,
        .queryDynamicColliders = NULL,
    };
    VoxelBodyShape shape = TestShape();
    const double clearPosition[3] = {0.5, 0.5, 2.6};
    VoxelBodyBounds bounds;
    VoxelBodyCalculateBounds(clearPosition, &shape, &bounds);
    PhysicsExpect(
        Near(bounds.minimum[0], 0.2, 0.000001) && Near(bounds.maximum[0], 0.8, 0.000001) &&
            Near(bounds.minimum[2], 1.0, 0.000001) && Near(bounds.maximum[2], 2.8, 0.000001),
        "body bounds are wrong");
    PhysicsExpect(!VoxelBodyCollides(&source, clearPosition, &shape),
                  "clear body collides with the floor");

    const double penetratingPosition[3] = {0.5, 0.5, 1.5};
    PhysicsExpect(VoxelBodyCollides(&source, penetratingPosition, &shape),
                  "solid floor overlap was missed");

    double fallingPosition[3] = {0.5, 0.5, 3.0};
    PhysicsExpect(VoxelBodyMoveAxis(&source, fallingPosition, &shape, 2, -1.0) &&
                      Near(fallingPosition[2], 2.601, 0.000001),
                  "downward movement was not clipped at the floor");
    VoxelGroundContact contact;
    VoxelBodyQueryGroundContact(&source, fallingPosition, &shape, 0.01, &contact);
    PhysicsExpect(contact.supported && Near((double)contact.friction, 0.75, 0.000001) &&
                      contact.surfaceStableId == 0U,
                  "static ground contact properties are wrong");
    PhysicsExpect(VoxelBodyHasGroundContact(&source, fallingPosition, &shape, 0.01) &&
                      VoxelBodyHasStableGround(&source, fallingPosition, &shape, 0.01, 0.2),
                  "floor support query failed");

    const int64_t overlappingBlock[3] = {0, 0, 1};
    const int64_t separateBlock[3] = {0, 0, 3};
    PhysicsExpect(VoxelBodyOverlapsBlock(clearPosition, &shape, overlappingBlock) &&
                      !VoxelBodyOverlapsBlock(clearPosition, &shape, separateBlock),
                  "block overlap query is wrong");

    double unchanged[3] = {4.0, 5.0, 6.0};
    PhysicsExpect(VoxelBodyMoveAxis(&source, unchanged, &shape, 3, 1.0) && unchanged[0] == 4.0 &&
                      unchanged[1] == 5.0 && unchanged[2] == 6.0,
                  "invalid axis did not fail closed");
    VoxelBodyShape invalidShape = shape;
    invalidShape.radius = 0.0;
    PhysicsExpect(VoxelBodyCollides(&source, clearPosition, &invalidShape),
                  "invalid shape did not fail closed");
}

static void TestDynamicColliderSource(void)
{
    PhysicsTestContext context = {
        .useFloor = false,
        .useDynamicPlatform = true,
    };
    VoxelCollisionSource source = {
        .context = &context,
        .queryBlockPhysics = QueryBlockPhysics,
        .queryDynamicColliders = QueryDynamicPlatform,
    };
    VoxelBodyShape shape = TestShape();
    const double supportedPosition[3] = {0.0, 0.0, 2.101};
    VoxelGroundContact contact;
    VoxelBodyQueryGroundContact(&source, supportedPosition, &shape, 0.01, &contact);
    PhysicsExpect(contact.supported && contact.surfaceStableId == 17U &&
                      Near((double)contact.friction, 0.5, 0.000001) &&
                      Near(contact.surfaceVelocity[0], 0.25, 0.000001) &&
                      Near(contact.surfaceVelocity[1], -0.5, 0.000001),
                  "dynamic platform contact is wrong");
    PhysicsExpect(context.dynamicQueries != 0U, "dynamic broadphase callback was not used");

    const double penetratingPosition[3] = {0.0, 0.0, 1.0};
    PhysicsExpect(VoxelBodyCollides(&source, penetratingPosition, &shape),
                  "dynamic collider overlap was missed");
}

LAIUE_TEST_ENTRY(PhysicsTestEntryPoint)
{
    TestStaticVoxelCollision();
    TestDynamicColliderSource();
    LaiueTestRuntimeWrite("Physics tests passed.\r\n");
    LAIUE_TEST_SUCCESS();
}
