#pragma once

typedef enum GameMode
{
    GAME_MODE_CREATIVE = 0,
    GAME_MODE_SURVIVAL = 1,
} GameMode;

// Имена сохраняются как ABI/source-совместимые псевдонимы: режим полёта
// относится к креативу, контроллер ходьбы — к выживанию.
#define GAME_MODE_FLY GAME_MODE_CREATIVE
#define GAME_MODE_WALK GAME_MODE_SURVIVAL
