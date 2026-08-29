#pragma once

#include "mod/mod_types.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

void LaiueModDiagnosticClear(LaiueModDiagnostic *diagnostic);
LaiueModStatus LaiueModDiagnosticSet(LaiueModDiagnostic *diagnostic, LaiueModStatus status,
                                     int32_t modResult, const char *message);

bool LaiueModAsciiEquals(const char *first, const char *second);
bool LaiueModServiceNameIsSafe(const char *name);
bool LaiueModWideCopy(wchar_t *destination, uint32_t capacity, const wchar_t *source);
bool LaiueModPathJoin(wchar_t *destination, uint32_t capacity, const wchar_t *first,
                      const wchar_t *second, const wchar_t *third);
