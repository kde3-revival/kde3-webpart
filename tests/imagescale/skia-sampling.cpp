// skia-sampling: draw a raw RGBA image downscaled with WebKit's Skia sampling options
// (current and proposed), on the CPU and on the GPU (Ganesh GL, EGL surfaceless).
//   skia-sampling in.rgba W H dstW dstH outprefix
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSamplingOptions.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void save(const char* prefix, const char* name, SkSurface* s, GrDirectContext* ctx)
{
    SkImageInfo info = SkImageInfo::Make(s->width(), s->height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    std::vector<uint8_t> px(info.computeMinByteSize());
    if (!s->readPixels(info, px.data(), info.minRowBytes(), 0, 0)) {
        fprintf(stderr, "readPixels failed\n");
        return;
    }
    char fn[512];
    snprintf(fn, sizeof fn, "%s-%s.rgba", prefix, name);
    FILE* f = fopen(fn, "wb");
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    (void)ctx;
}

struct Mode { const char* name; SkSamplingOptions s; };

static double draw(SkSurface* surf, const sk_sp<SkImage>& img, const SkSamplingOptions& s, GrDirectContext* ctx)
{
    auto t0 = std::chrono::steady_clock::now();
    SkCanvas* c = surf->getCanvas();
    c->clear(SK_ColorWHITE);
    c->drawImageRect(img, SkRect::MakeIWH(img->width(), img->height()),
                     SkRect::MakeIWH(surf->width(), surf->height()), s, nullptr,
                     SkCanvas::kFast_SrcRectConstraint);
    if (ctx)
        ctx->flushAndSubmit(GrSyncCpu::kYes);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv)
{
    if (argc < 7)
        return 2;
    int w = atoi(argv[2]), h = atoi(argv[3]), dw = atoi(argv[4]), dh = atoi(argv[5]);
    const char* prefix = argv[6];
    std::vector<uint8_t> raw(size_t(w) * h * 4);
    FILE* f = fopen(argv[1], "rb");
    if (!f || fread(raw.data(), 1, raw.size(), f) != raw.size())
        return 1;
    fclose(f);
    SkImageInfo ii = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    sk_sp<SkImage> img = SkImages::RasterFromPixmapCopy(SkPixmap(ii, raw.data(), ii.minRowBytes()));

    Mode modes[] = {
        { "cur", SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone) },
        { "mipnear", SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest) },
        { "miplin", SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear) },
        { "cubic", SkSamplingOptions(SkCubicResampler::CatmullRom()) },
    };

    SkImageInfo di = SkImageInfo::MakeN32Premul(dw, dh);
    sk_sp<SkSurface> cpu = SkSurfaces::Raster(di);
    for (auto& m : modes) {
        // first draw builds the mipmaps (cached per image), second is the steady state
        double t1 = draw(cpu.get(), img, m.s, nullptr);
        double t2 = draw(cpu.get(), img, m.s, nullptr);
        printf("cpu %-8s first %.2f ms, again %.2f ms\n", m.name, t1, t2);
        char n[64];
        snprintf(n, sizeof n, "cpu-%s", m.name);
        save(prefix, n, cpu.get(), nullptr);
    }

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        printf("no EGL surfaceless display, GPU skipped\n");
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ectx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    if (ectx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ectx)) {
        printf("no EGL context, GPU skipped\n");
        return 0;
    }
    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeGL(GrGLInterfaces::MakeEGL());
    if (!ctx) {
        printf("no Ganesh context\n");
        return 0;
    }
    sk_sp<SkSurface> gpu = SkSurfaces::RenderTarget(ctx.get(), skgpu::Budgeted::kNo, di);
    // WebKit draws both raster images (uploaded by Skia) and texture-backed
    // images (decoded/uploaded by WebKit without mipmaps); try both.
    sk_sp<SkImage> tex = SkImages::TextureFromImage(ctx.get(), img, skgpu::Mipmapped::kNo);
    struct { const char* name; sk_sp<SkImage> im; } srcs[] = { { "gpuraster", img }, { "gputex", tex } };
    for (auto& src : srcs) {
        for (auto& m : modes) {
            double t1 = draw(gpu.get(), src.im, m.s, ctx.get());
            double t2 = draw(gpu.get(), src.im, m.s, ctx.get());
            printf("%s %-8s first %.2f ms, again %.2f ms\n", src.name, m.name, t1, t2);
            char n[64];
            snprintf(n, sizeof n, "%s-%s", src.name, m.name);
            save(prefix, n, gpu.get(), ctx.get());
        }
    }
    return 0;
}
