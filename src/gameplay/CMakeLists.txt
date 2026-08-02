laiue_add_module(gameplay
    RUNTIME_ROLE HEADLESS
    SOURCES
        player_controller.c
        player_controller.h
        player_replication.c
        player_replication.h
        player_locomotion.c
        player_locomotion.h
        player_stance.c
        player_stance.h
        player_jump.c
        player_jump.h
        game_mode.h
        game_events.h
        inventory.c
        inventory.h
        view_direction.c
        view_direction.h
        ../game/camera.h
        ../api.h
    LINK
        laiue_physics
)

# Детерминизм симуляции движения: те же правила FP, что и в physics (см.
# src/physics/CMakeLists.txt). Пинятся только модули шага симуляции;
# inventory к детерминированной физике не относится. Правило: в этих файлах
# никаких libm-трансцендент (sin/cos/exp/pow) — они разнятся между
# платформами; sqrt только аппаратный (_mm_sqrt_sd). Страж —
# tests/determinism_test.c.
set_source_files_properties(
    player_controller.c
    player_replication.c
    player_locomotion.c
    player_stance.c
    player_jump.c
    view_direction.c
    PROPERTIES COMPILE_OPTIONS "${LAIUE_DETERMINISTIC_FP_OPTIONS}")
