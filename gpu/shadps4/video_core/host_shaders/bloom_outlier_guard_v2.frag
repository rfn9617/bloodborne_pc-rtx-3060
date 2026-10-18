#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_16bit_storage : require
#if defined(GL_ARB_gpu_shader_int64)
#extension GL_ARB_gpu_shader_int64 : require
#else
#error No extension available for 64-bit integers.
#endif
#extension GL_EXT_fragment_shader_barycentric : require

struct full_result_i32x2
{
    int _m0;
    int _m1;
};

struct full_result_u32x2
{
    uint _m0;
    uint _m1;
};

struct frexp_result_f32
{
    float _m0;
    int _m1;
};

layout(set = 0, binding = 0, std430) readonly buffer ssbo_1
{
    uint data[];
} ssbo_1_1;

layout(push_constant, std430) uniform AuxData
{
    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    uvec4 ud_regs0;
    uvec4 ud_regs1;
    uvec4 ud_regs2;
    uvec4 ud_regs3;
    uvec4 buf_offsets0;
    uvec4 buf_offsets1;
    uvec2 buf_offsets2;
    uint motion_param;
    uint scene_size;
} push_data;

layout(set = 0, binding = 1) uniform texture2D fs_img0;
layout(set = 0, binding = 2) uniform sampler fs_samp0;

layout(location = 0) pervertexEXT in vec4 fs_in_attr0_p[3];
layout(location = 0) out vec4 frag_color0;


// Only the first HDR bloom reduction uses this guard. Scene color, alpha and
// later small bloom mips retain the original sample. No history or extra target.
vec4 GuardBloom(vec2 uv) {
    vec4 original = textureLod(sampler2D(fs_img0, fs_samp0), uv, 0.0);
    ivec2 size = textureSize(sampler2D(fs_img0, fs_samp0), 0);
    if (size.x < 960 || size.y < 540)
        return original;
    vec2 p = uv * vec2(size) - vec2(0.5);
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p);
    vec4 t11 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(0, 0), ivec2(0), size-1), 0);
    vec4 t21 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(1, 0), ivec2(0), size-1), 0);
    vec4 t12 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(0, 1), ivec2(0), size-1), 0);
    vec4 t22 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(1, 1), ivec2(0), size-1), 0);
    float centralPeak = max(max(max(max(t11.r,t11.g),t11.b), max(max(t21.r,t21.g),t21.b)),
                            max(max(max(t12.r,t12.g),t12.b), max(max(t22.r,t22.g),t22.b)));
    if (centralPeak <= 32.0) return original;
    vec4 t00 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(-1, -1), ivec2(0), size-1), 0);
    vec4 t10 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(0, -1), ivec2(0), size-1), 0);
    vec4 t20 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(1, -1), ivec2(0), size-1), 0);
    vec4 t30 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(2, -1), ivec2(0), size-1), 0);
    vec4 t01 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(-1, 0), ivec2(0), size-1), 0);
    vec4 t31 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(2, 0), ivec2(0), size-1), 0);
    vec4 t02 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(-1, 1), ivec2(0), size-1), 0);
    vec4 t32 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(2, 1), ivec2(0), size-1), 0);
    vec4 t03 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(-1, 2), ivec2(0), size-1), 0);
    vec4 t13 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(0, 2), ivec2(0), size-1), 0);
    vec4 t23 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(1, 2), ivec2(0), size-1), 0);
    vec4 t33 = texelFetch(sampler2D(fs_img0, fs_samp0), clamp(base + ivec2(2, 2), ivec2(0), size-1), 0);
    float bound11 = max(32.0, 8.0 * max(max(max(max(max(max(max(max(max(t00.r, t00.g), t00.b), max(max(t10.r, t10.g), t10.b)), max(max(t20.r, t20.g), t20.b)), max(max(t01.r, t01.g), t01.b)), max(max(t21.r, t21.g), t21.b)), max(max(t02.r, t02.g), t02.b)), max(max(t12.r, t12.g), t12.b)), max(max(t22.r, t22.g), t22.b)));
    float peak11 = max(max(t11.r, t11.g), t11.b);
    t11.rgb *= min(1.0, bound11 / max(peak11, 0.000001));
    float bound21 = max(32.0, 8.0 * max(max(max(max(max(max(max(max(max(t10.r, t10.g), t10.b), max(max(t20.r, t20.g), t20.b)), max(max(t30.r, t30.g), t30.b)), max(max(t11.r, t11.g), t11.b)), max(max(t31.r, t31.g), t31.b)), max(max(t12.r, t12.g), t12.b)), max(max(t22.r, t22.g), t22.b)), max(max(t32.r, t32.g), t32.b)));
    float peak21 = max(max(t21.r, t21.g), t21.b);
    t21.rgb *= min(1.0, bound21 / max(peak21, 0.000001));
    float bound12 = max(32.0, 8.0 * max(max(max(max(max(max(max(max(max(t01.r, t01.g), t01.b), max(max(t11.r, t11.g), t11.b)), max(max(t21.r, t21.g), t21.b)), max(max(t02.r, t02.g), t02.b)), max(max(t22.r, t22.g), t22.b)), max(max(t03.r, t03.g), t03.b)), max(max(t13.r, t13.g), t13.b)), max(max(t23.r, t23.g), t23.b)));
    float peak12 = max(max(t12.r, t12.g), t12.b);
    t12.rgb *= min(1.0, bound12 / max(peak12, 0.000001));
    float bound22 = max(32.0, 8.0 * max(max(max(max(max(max(max(max(max(t11.r, t11.g), t11.b), max(max(t21.r, t21.g), t21.b)), max(max(t31.r, t31.g), t31.b)), max(max(t12.r, t12.g), t12.b)), max(max(t32.r, t32.g), t32.b)), max(max(t13.r, t13.g), t13.b)), max(max(t23.r, t23.g), t23.b)), max(max(t33.r, t33.g), t33.b)));
    float peak22 = max(max(t22.r, t22.g), t22.b);
    t22.rgb *= min(1.0, bound22 / max(peak22, 0.000001));
    if (peak11 <= bound11 && peak21 <= bound21 && peak12 <= bound12 && peak22 <= bound22)
        return original;
    vec3 filtered = mix(mix(t11.rgb, t21.rgb, f.x), mix(t12.rgb, t22.rgb, f.x), f.y);
    return vec4(filtered, original.a);
}

void main()
{
    uint buf0_dword_off = bitfieldExtract(push_data.buf_offsets0.x, int(0u), int(8u)) >> 2u;
    precise float _86 = fs_in_attr0_p[1u].x - fs_in_attr0_p[0u].x;
    precise float _93 = fs_in_attr0_p[1u].y - fs_in_attr0_p[0u].y;
    precise float _99 = fs_in_attr0_p[2u].x - fs_in_attr0_p[0u].x;
    precise float _105 = fs_in_attr0_p[2u].y - fs_in_attr0_p[0u].y;
    vec4 _111 = GuardBloom(vec2(fma(_99, gl_BaryCoordEXT.z, fma(_86, gl_BaryCoordEXT.y, fs_in_attr0_p[0u].x)), fma(_105, gl_BaryCoordEXT.z, fma(_93, gl_BaryCoordEXT.y, fs_in_attr0_p[0u].y))));
    precise float _133 = uintBitsToFloat(ssbo_1_1.data[616u + buf0_dword_off]) * _111.x;
    precise float _135 = uintBitsToFloat(ssbo_1_1.data[617u + buf0_dword_off]) * _111.y;
    precise float _137 = uintBitsToFloat(ssbo_1_1.data[618u + buf0_dword_off]) * _111.z;
    precise float _139 = uintBitsToFloat(ssbo_1_1.data[619u + buf0_dword_off]) * _111.w;
    frag_color0.x = _133;
    frag_color0.y = _135;
    frag_color0.z = _137;
    frag_color0.w = _139;
}
