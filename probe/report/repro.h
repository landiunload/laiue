#pragma once
#include <stdint.h>

typedef struct Api
{
    void *context;
    void *getBlock;
    void *fillRegion;
    void *rebase;
} Api;

typedef struct Coord
{
    uint64_t *limbs;
    uint32_t count;
    int32_t sign;
} Coord;

typedef struct State
{
    int64_t origin[3];
    Api api;
    void *world;
    void *collision[3];
    Coord coords[3];
    double position[3];
    double velocity[3];
    uint64_t hash;
    uint32_t mask;
} State;

void *Create(const Api *api);
void Init(State *state);
