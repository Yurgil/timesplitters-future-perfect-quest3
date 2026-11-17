#ifndef TSFP_STEREO_SHADER_H
#define TSFP_STEREO_SHADER_H

/* The stereo shader variant receives four clip-transform rows for each eye.
 * Keep the fixed-function and NV2A vertex-program variants on the same ABI. */
#define TSFP_XR_EYE_UBO_SLOT 4
#define TSFP_XR_STRINGIFY_INNER(value) #value
#define TSFP_XR_STRINGIFY(value) TSFP_XR_STRINGIFY_INNER(value)

#define TSFP_XR_EYE_UNIFORMS \
    "#ifdef TSFP_STEREO_GEOMETRY\n" \
    "cbuffer StereoView : register(b" TSFP_XR_STRINGIFY(TSFP_XR_EYE_UBO_SLOT) ") { float4 stereoRows[8]; };\n" \
    "float4 tsfpEyeClip(float4 pos, uint eye) {\n" \
    "    uint row = (eye & 1u) * 4u;\n" \
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

#endif
