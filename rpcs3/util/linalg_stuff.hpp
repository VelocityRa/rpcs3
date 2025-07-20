#pragma once

#include <util/endian.hpp>
#include <util/types.hpp>

#include <Emu/RSX/linalg.h>
using namespace linalg::aliases;

struct vec2be
{
	be_t<float> u, v;
};
struct vec3be
{
	be_t<float> x, y, z;
};
struct uvec3
{
	u32 x_u, y_u, z_u;
};
struct vec4be
{
	be_t<float> x, y, z, w;
};
using vec2 = float2;
using vec3 = float3;
using vec4 = float4;
using mat3 = float3x3;
using mat4 = float4x4;
