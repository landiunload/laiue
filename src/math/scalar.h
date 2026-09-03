#pragma once

// Скалярная математика без CRT: полиномиальные аппроксимации вместо
// библиотечных sinf/cosf и аппаратный sqrt интринсиком. Windows собирает
// движок с /NODEFAULTLIB, поэтому libm недоступна ни там, ни в модулях,
// которые обязаны вести себя одинаково на всех платформах.
//
// Это внутренняя граница движка, а не часть SDK: заголовок не
// устанавливается, символы не экспортируются. Библиотека статическая и
// линкуется в те модули, которым нужна, — по образцу platform_support.

float ScalarSin(float radians);
float ScalarCos(float radians);
float ScalarTan(float radians);
float ScalarClamp(float value, float minimum, float maximum);
float ScalarWrap(float radians);
float ScalarSqrt(float value);

// Арктангенсы — минимаксный полином, точность ~1e-6 рад.
// ScalarAtan2 повторяет соглашения atan2f: результат в (-pi, pi],
// учитывает знаки обоих аргументов.
float ScalarAtan(float value);
float ScalarAtan2(float y, float x);
float ScalarAcos(float value);
