#include "core/block_effects.h"

#include "core/math.h"
#include "render/chunk_geometry.h"

#include <stddef.h>
#include <string.h>

static RendererMesh* CreateBlockMesh(Renderer* renderer, BlockType block)
{
    ChunkQuad quads[6];
    for (uint32_t face = 0; face < 6; ++face)
    {
        quads[face] = PackChunkQuad(0, 0, 0, face, block, 1, 1, 1);
    }
    return RendererCreateMesh(renderer, quads, 6);
}

bool BlockEffectsInit(BlockEffects* effects, Renderer* renderer)
{
    if (effects == NULL || renderer == NULL) return false;
    memset(effects, 0, sizeof(*effects));
    effects->randomState = 0x6d2b79f5U;
    effects->blockMeshes[BLOCK_EARTH] =
        CreateBlockMesh(renderer, BLOCK_EARTH);
    effects->blockMeshes[BLOCK_GRASS] =
        CreateBlockMesh(renderer, BLOCK_GRASS);
    if (effects->blockMeshes[BLOCK_EARTH] == NULL
        || effects->blockMeshes[BLOCK_GRASS] == NULL)
    {
        BlockEffectsShutdown(effects, renderer);
        return false;
    }
    return true;
}

void BlockEffectsShutdown(BlockEffects* effects, Renderer* renderer)
{
    if (effects == NULL) return;
    if (renderer != NULL)
    {
        for (uint32_t i = 0; i < 3; ++i)
            RendererDestroyMesh(renderer, effects->blockMeshes[i]);
    }
    memset(effects, 0, sizeof(*effects));
}

void BlockEffectsClear(BlockEffects* effects)
{
    if (effects == NULL) return;
    memset(effects->drops, 0, sizeof(effects->drops));
    memset(effects->particles, 0, sizeof(effects->particles));
    effects->nextDrop = 0;
    effects->nextParticle = 0;
}

static uint32_t NextRandom(BlockEffects* effects)
{
    uint32_t value = effects->randomState;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    effects->randomState = value;
    return value;
}

static float RandomSigned(BlockEffects* effects)
{
    uint32_t value = NextRandom(effects) & 0xffffU;
    return (float)value * (2.0f / 65535.0f) - 1.0f;
}

void BlockEffectsSpawnDestroyed(BlockEffects* effects, BlockType block,
    const int64_t position[3])
{
    if (effects == NULL || position == NULL || block == BLOCK_AIR
        || block > BLOCK_GRASS)
        return;

    BlockDrop* drop = &effects->drops[
        effects->nextDrop++ % BLOCK_EFFECT_DROP_CAPACITY];
    memset(drop, 0, sizeof(*drop));
    drop->active = true;
    drop->block = block;
    drop->position[0] = (double)position[0] + 0.5;
    drop->position[1] = (double)position[1] + 0.5;
    drop->position[2] = (double)position[2] + 0.65;
    drop->velocity[0] = RandomSigned(effects) * 1.1f;
    drop->velocity[1] = RandomSigned(effects) * 1.1f;
    drop->velocity[2] = 2.8f;

    for (uint32_t i = 0; i < 14U; ++i)
    {
        BlockParticle* particle = &effects->particles[
            effects->nextParticle++ % BLOCK_EFFECT_PARTICLE_CAPACITY];
        memset(particle, 0, sizeof(*particle));
        particle->active = true;
        particle->block = block;
        particle->position[0] = (double)position[0] + 0.5;
        particle->position[1] = (double)position[1] + 0.5;
        particle->position[2] = (double)position[2] + 0.5;
        particle->velocity[0] = RandomSigned(effects) * 3.3f;
        particle->velocity[1] = RandomSigned(effects) * 3.3f;
        particle->velocity[2] = 1.3f
            + (RandomSigned(effects) + 1.0f) * 2.1f;
        particle->lifetime = 0.45f
            + (float)(NextRandom(effects) & 255U) * (0.35f / 255.0f);
    }
}

void BlockEffectsSpawnNetworkDrop(BlockEffects* effects, uint32_t id,
    BlockType block, const double position[3])
{
    if (effects == NULL || position == NULL || id == 0
        || block == BLOCK_AIR || block > BLOCK_GRASS) return;
    BlockDrop* drop = &effects->drops[
        effects->nextDrop++ % BLOCK_EFFECT_DROP_CAPACITY];
    memset(drop, 0, sizeof(*drop));
    drop->active = true;
    drop->networkId = id;
    drop->block = block;
    drop->position[0] = position[0];
    drop->position[1] = position[1];
    drop->position[2] = position[2];
    drop->velocity[0] = RandomSigned(effects) * 1.1f;
    drop->velocity[1] = RandomSigned(effects) * 1.1f;
    drop->velocity[2] = 2.8f;

    for (uint32_t i = 0; i < 14U; ++i)
    {
        BlockParticle* particle = &effects->particles[
            effects->nextParticle++ % BLOCK_EFFECT_PARTICLE_CAPACITY];
        memset(particle, 0, sizeof(*particle));
        particle->active = true;
        particle->block = block;
        particle->position[0] = position[0];
        particle->position[1] = position[1];
        particle->position[2] = position[2] - 0.15;
        particle->velocity[0] = RandomSigned(effects) * 3.3f;
        particle->velocity[1] = RandomSigned(effects) * 3.3f;
        particle->velocity[2] = 1.3f
            + (RandomSigned(effects) + 1.0f) * 2.1f;
        particle->lifetime = 0.45f
            + (float)(NextRandom(effects) & 255U) * (0.35f / 255.0f);
    }
}

void BlockEffectsRemoveNetworkDrop(BlockEffects* effects, uint32_t id)
{
    if (effects == NULL || id == 0) return;
    for (uint32_t i = 0; i < BLOCK_EFFECT_DROP_CAPACITY; ++i)
    {
        if (effects->drops[i].active
            && effects->drops[i].networkId == id)
        {
            effects->drops[i].active = false;
            return;
        }
    }
}

static void IntegratePoint(World* world, double position[3],
    float velocity[3], float deltaSeconds, float radius)
{
    velocity[2] -= 15.0f * deltaSeconds;
    position[0] += (double)velocity[0] * deltaSeconds;
    position[1] += (double)velocity[1] * deltaSeconds;
    position[2] += (double)velocity[2] * deltaSeconds;

    int64_t blockX = (int64_t)position[0];
    int64_t blockY = (int64_t)position[1];
    int64_t blockZ = (int64_t)(position[2] - radius);
    if (position[0] < 0.0 && (double)blockX != position[0]) --blockX;
    if (position[1] < 0.0 && (double)blockY != position[1]) --blockY;
    if (position[2] - radius < 0.0
        && (double)blockZ != position[2] - radius) --blockZ;
    if (velocity[2] < 0.0f
        && WorldGetBlock(world, blockX, blockY, blockZ) != BLOCK_AIR)
    {
        position[2] = (double)blockZ + 1.0 + radius;
        velocity[2] *= -0.22f;
        velocity[0] *= 0.72f;
        velocity[1] *= 0.72f;
        if (velocity[2] < 0.35f) velocity[2] = 0.0f;
    }
}

void BlockEffectsUpdate(BlockEffects* effects, World* world,
    Inventory* inventory, const double playerPosition[3],
    float deltaSeconds, bool allowLocalPickup)
{
    if (effects == NULL || world == NULL || playerPosition == NULL) return;
    for (uint32_t i = 0; i < BLOCK_EFFECT_DROP_CAPACITY; ++i)
    {
        BlockDrop* drop = &effects->drops[i];
        if (!drop->active) continue;
        drop->age += deltaSeconds;
        if (drop->networkId == 0)
        {
            IntegratePoint(world, drop->position, drop->velocity,
                deltaSeconds, 0.14f);
        }

        double delta[3] = {
            playerPosition[0] - drop->position[0],
            playerPosition[1] - drop->position[1],
            playerPosition[2] - 0.8 - drop->position[2],
        };
        double distanceSquared = delta[0] * delta[0]
            + delta[1] * delta[1] + delta[2] * delta[2];
        if (allowLocalPickup && drop->age >= 0.30f
            && distanceSquared < 1.7 * 1.7 && inventory != NULL
            && InventoryAdd(inventory, drop->block, 1U) == 0)
        {
            drop->active = false;
            continue;
        }
        if (allowLocalPickup && drop->age >= 0.30f
            && distanceSquared < 3.2 * 3.2 && distanceSquared > 0.01)
        {
            float inverseDistance = 1.0f
                / ScalarSqrt((float)distanceSquared);
            drop->velocity[0] += (float)delta[0]
                * inverseDistance * 18.0f * deltaSeconds;
            drop->velocity[1] += (float)delta[1]
                * inverseDistance * 18.0f * deltaSeconds;
            drop->velocity[2] += (float)delta[2]
                * inverseDistance * 18.0f * deltaSeconds;
        }
    }

    for (uint32_t i = 0; i < BLOCK_EFFECT_PARTICLE_CAPACITY; ++i)
    {
        BlockParticle* particle = &effects->particles[i];
        if (!particle->active) continue;
        particle->age += deltaSeconds;
        if (particle->age >= particle->lifetime)
        {
            particle->active = false;
            continue;
        }
        IntegratePoint(world, particle->position, particle->velocity,
            deltaSeconds, 0.04f);
    }
}

void BlockEffectsDraw(BlockEffects* effects, Renderer* renderer,
    const int64_t cameraBlockPosition[3])
{
    if (effects == NULL || renderer == NULL || cameraBlockPosition == NULL)
        return;
    for (BlockType block = BLOCK_EARTH; block <= BLOCK_GRASS; ++block)
    {
        uint32_t count = 0;
        for (uint32_t i = 0; i < BLOCK_EFFECT_DROP_CAPACITY; ++i)
        {
            const BlockDrop* drop = &effects->drops[i];
            if (!drop->active || drop->block != block) continue;
            float scale = 0.28f;
            float bob = ScalarSin(drop->age * 4.5f) * 0.035f;
            effects->renderInstances[count].originRelative[0] = (float)(drop->position[0]
                - (double)cameraBlockPosition[0]) - scale * 0.5f;
            effects->renderInstances[count].originRelative[1] = (float)(drop->position[1]
                - (double)cameraBlockPosition[1]) - scale * 0.5f;
            effects->renderInstances[count].originRelative[2] = (float)(drop->position[2]
                - (double)cameraBlockPosition[2]) - scale * 0.5f + bob;
            effects->renderInstances[count].scale = scale;
            ++count;
        }
        for (uint32_t i = 0; i < BLOCK_EFFECT_PARTICLE_CAPACITY; ++i)
        {
            const BlockParticle* particle = &effects->particles[i];
            if (!particle->active || particle->block != block) continue;
            float scale = 0.075f;
            effects->renderInstances[count].originRelative[0] = (float)(particle->position[0]
                - (double)cameraBlockPosition[0]) - scale * 0.5f;
            effects->renderInstances[count].originRelative[1] = (float)(particle->position[1]
                - (double)cameraBlockPosition[1]) - scale * 0.5f;
            effects->renderInstances[count].originRelative[2] = (float)(particle->position[2]
                - (double)cameraBlockPosition[2]) - scale * 0.5f;
            effects->renderInstances[count].scale = scale;
            ++count;
        }
        if (count != 0)
        {
            RendererDrawMeshInstances(renderer, effects->blockMeshes[block],
                effects->renderInstances, count);
        }
    }
}

void BlockEffectsRebase(BlockEffects* effects,
    int64_t shiftX, int64_t shiftY, int64_t shiftZ)
{
    if (effects == NULL) return;
    for (uint32_t i = 0; i < BLOCK_EFFECT_DROP_CAPACITY; ++i)
    {
        if (!effects->drops[i].active) continue;
        effects->drops[i].position[0] -= (double)shiftX;
        effects->drops[i].position[1] -= (double)shiftY;
        effects->drops[i].position[2] -= (double)shiftZ;
    }
    for (uint32_t i = 0; i < BLOCK_EFFECT_PARTICLE_CAPACITY; ++i)
    {
        if (!effects->particles[i].active) continue;
        effects->particles[i].position[0] -= (double)shiftX;
        effects->particles[i].position[1] -= (double)shiftY;
        effects->particles[i].position[2] -= (double)shiftZ;
    }
}
