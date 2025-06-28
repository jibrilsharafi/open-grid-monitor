#pragma once

#include <stdint.h>

typedef struct {
    int64_t timestamp_us;
    float frequency;
    float voltage;
} measurement_t;