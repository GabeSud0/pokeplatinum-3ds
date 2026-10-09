/*
 * hello3d: toolchain test for pokeplatinum-3ds.
 *
 * Draws a spinning, vertex-coloured cube on the top screen at the native
 * 400x240 resolution, in stereoscopic 3D (move the 3D slider), and shows
 * per-frame CPU/GPU timings on the bottom screen.
 *
 * If this runs smoothly on your console, devkitARM, libctru, citro3d and
 * picasso are all working, and you know how to get code onto the 3DS.
 *
 * START exits. A pauses / resumes the rotation.
 */

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "vshader_shbin.h" /* generated at build time from vshader.v.pica */

#define CLEAR_COLOR 0x1A2B3CFF /* RGBA */

/* Copy the rendered image to the screen: RGBA8 render target -> RGB8 framebuffer. */
#define DISPLAY_TRANSFER_FLAGS                                                         \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |  \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                                   \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                                   \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    float position[3];
    float color[3];
} Vertex;

/* A cube from -0.5 to 0.5: 6 faces x 2 triangles x 3 vertices. */
static const Vertex cube[] = {
    /* +Z (front), red */
    {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.3f, 0.3f}}, {{0.5f, -0.5f, 0.5f}, {1.0f, 0.3f, 0.3f}},
    {{0.5f, 0.5f, 0.5f}, {1.0f, 0.3f, 0.3f}},   {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.3f, 0.3f}},
    {{0.5f, 0.5f, 0.5f}, {1.0f, 0.3f, 0.3f}},   {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.3f, 0.3f}},
    /* -Z (back), green */
    {{0.5f, -0.5f, -0.5f}, {0.3f, 1.0f, 0.3f}}, {{-0.5f, -0.5f, -0.5f}, {0.3f, 1.0f, 0.3f}},
    {{-0.5f, 0.5f, -0.5f}, {0.3f, 1.0f, 0.3f}}, {{0.5f, -0.5f, -0.5f}, {0.3f, 1.0f, 0.3f}},
    {{-0.5f, 0.5f, -0.5f}, {0.3f, 1.0f, 0.3f}}, {{0.5f, 0.5f, -0.5f}, {0.3f, 1.0f, 0.3f}},
    /* +X (right), blue */
    {{0.5f, -0.5f, 0.5f}, {0.3f, 0.5f, 1.0f}},  {{0.5f, -0.5f, -0.5f}, {0.3f, 0.5f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.3f, 0.5f, 1.0f}},  {{0.5f, -0.5f, 0.5f}, {0.3f, 0.5f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.3f, 0.5f, 1.0f}},  {{0.5f, 0.5f, 0.5f}, {0.3f, 0.5f, 1.0f}},
    /* -X (left), yellow */
    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.9f, 0.3f}}, {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.9f, 0.3f}},
    {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.9f, 0.3f}},   {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.9f, 0.3f}},
    {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.9f, 0.3f}},   {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.9f, 0.3f}},
    /* +Y (top), magenta */
    {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.3f, 1.0f}},  {{0.5f, 0.5f, 0.5f}, {1.0f, 0.3f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, {1.0f, 0.3f, 1.0f}},  {{-0.5f, 0.5f, 0.5f}, {1.0f, 0.3f, 1.0f}},
    {{0.5f, 0.5f, -0.5f}, {1.0f, 0.3f, 1.0f}},  {{-0.5f, 0.5f, -0.5f}, {1.0f, 0.3f, 1.0f}},
    /* -Y (bottom), cyan */
    {{-0.5f, -0.5f, -0.5f}, {0.3f, 1.0f, 1.0f}}, {{0.5f, -0.5f, -0.5f}, {0.3f, 1.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, {0.3f, 1.0f, 1.0f}},   {{-0.5f, -0.5f, -0.5f}, {0.3f, 1.0f, 1.0f}},
    {{0.5f, -0.5f, 0.5f}, {0.3f, 1.0f, 1.0f}},   {{-0.5f, -0.5f, 0.5f}, {0.3f, 1.0f, 1.0f}},
};
#define CUBE_VERTEX_COUNT (sizeof(cube) / sizeof(cube[0]))

static DVLB_s *vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_modelView;
static void *vbo_data;

static void scene_init(void)
{
    /* Load the vertex shader. */
    vshader_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&program);

    uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
    uLoc_modelView = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");

    /* Vertex layout: v0 = position (3 floats), v1 = color (3 floats). */
    C3D_AttrInfo *attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 3);

    /* The GPU can only read from linear memory. */
    vbo_data = linearAlloc(sizeof(cube));
    memcpy(vbo_data, cube, sizeof(cube));

    C3D_BufInfo *bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, vbo_data, sizeof(Vertex), 2, 0x10);

    /* Output the interpolated vertex color directly. */
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    /* citro3d's projection matrices use reversed depth, hence GREATER. */
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
}

static void scene_draw(float iod, float angle)
{
    C3D_Mtx projection, modelView;

    /* Per-eye projection. iod (inter-ocular distance) comes from the 3D slider;
     * 2.0 is the distance at which objects appear level with the screen. */
    Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(40.0f), C3D_AspectRatioTop,
                        0.01f, 100.0f, iod, 2.0f, false);

    Mtx_Identity(&modelView);
    Mtx_Translate(&modelView, 0.0f, 0.0f, -2.5f, true);
    Mtx_RotateX(&modelView, angle * 0.7f, true);
    Mtx_RotateY(&modelView, angle, true);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_modelView, &modelView);

    C3D_DrawArrays(GPU_TRIANGLES, 0, CUBE_VERTEX_COUNT);
}

static void scene_exit(void)
{
    linearFree(vbo_data);
    shaderProgramFree(&program);
    DVLB_Free(vshader_dvlb);
}

int main(void)
{
    /* On New 3DS: 804 MHz CPU + L2 cache. Does nothing on Old 3DS. */
    osSetSpeedupEnable(true);

    gfxInitDefault();
    gfxSet3D(true); /* stereoscopic top screen */
    consoleInit(GFX_BOTTOM, NULL);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    /* One render target per eye. The screens are mounted sideways,
     * so a 400x240 screen is a 240x400 target. */
    C3D_RenderTarget *left = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTarget *right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(left, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(right, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

    scene_init();

    bool is_new3ds = false;
    APT_CheckNew3DS(&is_new3ds);

    printf("\x1b[1;1Hpokeplatinum-3ds :: hello3d\n");
    printf("Console:  %s\n", is_new3ds ? "New 3DS (804 MHz)" : "Old 3DS (268 MHz)");
    printf("Top:      400x240, stereoscopic\n\n");
    printf("START: exit    A: pause rotation\n");

    float angle = 0.0f;
    bool paused = false;
    u32 frame = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START)
            break;
        if (kDown & KEY_A)
            paused = !paused;

        if (!paused)
            angle += C3D_AngleFromDegrees(1.0f);

        float slider = osGet3DSliderState(); /* 0.0 .. 1.0 */
        float iod = slider / 3.0f;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C3D_RenderTargetClear(left, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(left);
        scene_draw(-iod, angle);

        /* With the slider at 0 the right eye isn't shown, so skip it. */
        if (iod > 0.0f) {
            C3D_RenderTargetClear(right, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
            C3D_FrameDrawOn(right);
            scene_draw(iod, angle);
        }

        C3D_FrameEnd(0);

        /* Refresh the stats a few times per second (printing is slow). */
        if ((frame++ % 15) == 0) {
            printf("\x1b[8;1H3D slider: %3d%%  \n", (int)(slider * 100.0f));
            printf("CPU time:  %6.2f ms  \n", C3D_GetProcessingTime());
            printf("GPU time:  %6.2f ms  \n", C3D_GetDrawingTime());
            printf("Cmd buf:   %6.2f %%  \n", C3D_GetCmdBufUsage() * 100.0f);
            printf("\nBudget at 60 fps: 16.67 ms\n");
        }
    }

    scene_exit();
    C3D_Fini();
    gfxExit();
    return 0;
}
