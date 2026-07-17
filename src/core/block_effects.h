#pragma once

#include "gameplay/inventory.h"
#include "render/renderer.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>

#define BLOCK_EFFECT_DROP_CAPACITY 128U
#define BLOCK_EFFECT_PARTICLE_CAPACITY 192U

typedef struct BlockDrop
{
    double position[3];
    float velocity[3];
    float age;
    uint32_t networkId;
    BlockType block;
    bool active;
} BlockDrop;

typedef struct BlockParticle
{
    double position[3];
    float velocity[3];
    float age;
    float lifetime;
    BlockType block;
    bool active;
} BlockParticle;

typedef struct BlockEffects
{
    BlockDrop drops[BLOCK_EFFECT_DROP_CAPACITY];
    BlockParticle particles[BLOCK_EFFECT_PARTICLE_CAPACITY];
    RendererMeshInstance renderInstances[
        BLOCK_EFFECT_DROP_CAPACITY + BLOCK_EFFECT_PARTICLE_CAPACITY];
    RendererMesh* blockMeshes[3];
    uint32_t randomState;
    uint32_t nextDrop;
    uint32_t nextParticle;
} BlockEffects;

bool BlockEffectsInit(BlockEffects* effects, Renderer* renderer);
void BlockEffectsShutdown(BlockEffects* effects, Renderer* renderer);
void BlockEffectsClear(BlockEffects* effects);
void BlockEffectsSpawnDestroyed(BlockEffects* effects, BlockType block,
    const int64_t position[3]);
void BlockEffectsSpawnNetworkDrop(BlockEffects* effects, uint32_t id,
    BlockType block, const double position[3]);
void BlockEffectsRemoveNetworkDrop(BlockEffects* effects, uint32_t id);
void BlockEffectsUpdate(BlockEffects* effects, World* world,
    Inventory* inventory, const double playerPosition[3],
    float deltaSeconds, bool allowLocalPickup);
void BlockEffectsDraw(BlockEffects* effects, Renderer* renderer,
    const int64_t cameraBlockPosition[3]);
void BlockEffectsRebase(BlockEffects* effects,
    int64_t shiftX, int64_t shiftY, int64_t shiftZ);
