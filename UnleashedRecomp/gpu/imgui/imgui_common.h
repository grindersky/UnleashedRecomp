#pragma once

#define IMGUI_SHADER_MODIFIER_NONE                     0
#define IMGUI_SHADER_MODIFIER_SCANLINE                 1
#define IMGUI_SHADER_MODIFIER_CHECKERBOARD             2
#define IMGUI_SHADER_MODIFIER_SCANLINE_BUTTON          3
#define IMGUI_SHADER_MODIFIER_TEXT_SKEW                4
#define IMGUI_SHADER_MODIFIER_HORIZONTAL_MARQUEE_FADE  5
#define IMGUI_SHADER_MODIFIER_VERTICAL_MARQUEE_FADE    6
#define IMGUI_SHADER_MODIFIER_GRAYSCALE                7
#define IMGUI_SHADER_MODIFIER_TITLE_BEVEL              8
#define IMGUI_SHADER_MODIFIER_CATEGORY_BEVEL           9
#define IMGUI_SHADER_MODIFIER_RECTANGLE_BEVEL          10
#define IMGUI_SHADER_MODIFIER_LOW_QUALITY_TEXT         11
// The vertices' depth (as the game's depth buffer has it: reversed, 1 at the near plane) in the texture coordinates' x,
// and the texture is the game's depth buffer: pixels behind what the game drew are left out.
#define IMGUI_SHADER_MODIFIER_DEPTH_TEST               12
// Drawn by imgui_multisampled_ps into a multisampled target like the game's scene (ImGuiCallback::SetMultisampled), so
// the edges are smoothed like the scene's; the depth test one does DEPTH_TEST's test per sample, with the game's
// multisampled depth buffer as the texture. RESOLVE averages that target's samples onto the backbuffer.
#define IMGUI_SHADER_MODIFIER_MULTISAMPLED             13
#define IMGUI_SHADER_MODIFIER_MULTISAMPLED_DEPTH_TEST  14
#define IMGUI_SHADER_MODIFIER_MULTISAMPLED_RESOLVE     15

#ifdef __cplusplus

enum class ImGuiCallback : int32_t
{
    SetGradient = -1,
    SetShaderModifier = -2,
    SetOrigin = -3,
    SetScale = -4,
    SetMarqueeFade = -5,
    SetOutline = -6,
    SetProceduralOrigin = -7,
    // -8 is ImDrawCallback_ResetRenderState, don't use!
    SetAdditive = -9,
    SetMultisampled = -10
};

union ImGuiCallbackData
{
    struct
    {
        float boundsMin[2];
        float boundsMax[2];
        uint32_t gradientTopLeft;
        uint32_t gradientTopRight;
        uint32_t gradientBottomRight;
        uint32_t gradientBottomLeft;
    } setGradient;

    struct
    {
        uint32_t shaderModifier;
    } setShaderModifier;

    struct
    {
        float origin[2];
    } setOrigin;

    struct
    {
        float scale[2];
    } setScale;

    struct
    {
        float boundsMin[2];
        float boundsMax[2];
    } setMarqueeFade;

    struct
    {
        float outline;
    } setOutline;

    struct
    {
        float proceduralOrigin[2];
    } setProceduralOrigin;

    struct
    {
        bool enabled;
    } setAdditive;

    // A sample count starts drawing into the multisampled target with that shader modifier; 0 puts what it holds onto
    // the backbuffer.
    struct
    {
        uint32_t sampleCount;
        uint32_t shaderModifier;
    } setMultisampled;
};

extern ImGuiCallbackData* AddImGuiCallback(ImGuiCallback callback);

extern void ResetImGuiCallbacks();

#endif
