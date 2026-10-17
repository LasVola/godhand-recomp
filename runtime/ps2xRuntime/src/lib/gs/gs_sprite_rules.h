#pragma once

#include <algorithm>
#include <cmath>

struct GSSpriteAxis
{
    int first = 0;
    int last = -1;
    float origin = 0.0f;
    float scale = 1.0f;
};

inline GSSpriteAxis gsSpriteAxis(float p0, float p1, float offset)
{
    GSSpriteAxis axis;
    axis.origin = p0 - offset;
    const float end = p1 - offset;
    const float extent = end - axis.origin;
    axis.first = static_cast<int>(std::ceil(std::min(axis.origin, end)));
    axis.last = static_cast<int>(std::ceil(std::max(axis.origin, end))) - 1;
    axis.scale = extent != 0.0f ? 1.0f / extent : 0.0f;
    return axis;
}

inline float gsSpriteParameter(const GSSpriteAxis &axis, int pixel)
{
    return (static_cast<float>(pixel) - axis.origin) * axis.scale;
}
