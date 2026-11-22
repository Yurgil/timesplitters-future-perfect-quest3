#ifndef TSFP_STEREO_SHADER_H
#define TSFP_STEREO_SHADER_H

/* Two eye transforms and an optional third transform for the sniper scope. */
#define TSFP_XR_EYE_UBO_SLOT 4
#define TSFP_XR_SCREEN_PROJECTION 48
#define TSFP_XR_CLIP_FLOATS (TSFP_XR_SCREEN_PROJECTION + 4)
#define TSFP_XR_STRINGIFY_INNER(value) #value
#define TSFP_XR_STRINGIFY(value) TSFP_XR_STRINGIFY_INNER(value)

#define TSFP_XR_EYE_UNIFORMS \
    "#ifdef TSFP_STEREO_GEOMETRY\n" \
    "cbuffer StereoView : register(b" TSFP_XR_STRINGIFY(TSFP_XR_EYE_UBO_SLOT) ") {\n" \
    "    float4 stereoRows[12];\n" \
    "    float4 stereoDrawOptions;\n" \
    "};\n" \
    "float4 tsfpEyeClip(float4 pos, uint eye) {\n" \
    "    if (eye == 2u && !any(stereoRows[11])) return float4(0,0,0,-1);\n" \
    "    uint row = min(eye, 2u) * 4u;\n" \
    "    return float4(dot(stereoRows[row], pos), dot(stereoRows[row+1], pos),\n" \
    "                  dot(stereoRows[row+2], pos), dot(stereoRows[row+3], pos));\n" \
    "}\n" \
    "#endif\n"

#define TSFP_XR_EYE_INPUT \
    "#ifdef TSFP_STEREO_GEOMETRY\n" \
    "    uint tsfpEye : TSFPVIEWINDEX0;\n" \
    "#endif\n"

#define TSFP_XR_EYE_TRANSFORM(position) \
    "#ifdef TSFP_STEREO_GEOMETRY\n" \
    "    " position " = tsfpEyeClip(" position ", input.tsfpEye);\n" \
    "#endif\n"

#define TSFP_XR_SCREEN_TRANSFORM(position) \
    "#ifdef TSFP_STEREO_GEOMETRY\n" \
    "    if (stereoDrawOptions.x != 0.0) " position " = tsfpEyeClip(" position ", input.tsfpEye);\n" \
    "#endif\n"

#endif
