laiue_add_module(network
    SOURCES
        network_windows.c
        network.h
        protocol.c
        protocol.h
        ../api.h
    LINK
        ws2_32
        bcrypt
)

# Кодек протокола валидирует недоверенный сетевой ввод и обязан отвергать
# NaN/Infinity. Глобальный /fp:fast (на clang-cl это -ffast-math
# -ffinite-math-only) делает проверки NaN недействительными — доказано
# тестом на битовых представлениях: под clang-cl EncodeInput принимал NaN
# во все поля. Точная модель FP включается только для этого холодного файла;
# флаги оптимизации (/O2, /GL и прочие) остаются на месте, скорость кадра не
# затрагивается. Тот же флаг продублирован в tests/CMakeLists.txt, где
# protocol.c собирается второй раз.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/protocol.c"
    PROPERTIES COMPILE_OPTIONS "/fp:precise")
