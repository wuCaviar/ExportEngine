#include "NearestResampler.h"

#include <vips/vips.h>

#include <algorithm>
#include <cstring>

using namespace ATHC::EE;

// ============================================================================
//  NearestResampler — vips KERNEL_NEAREST + 精确尺寸修正
// ============================================================================
//  原 ImageRenderer.cpp 的 resizeExact 原样迁移（字节级一致）。vips_resize
//  输出是 ROUND(scale × input)，浮点误差可能差 1px；策略：resize → verify →
//  re-scale from actual size → crop/embed 兜底（≤1px 边缘修正，NN 语义下不可见）。
//  返回新引用（调用方拥有），失败返回 nullptr（输入引用不动）。
static VipsImage *resizeExactVips(VipsImage *in, int targetW, int targetH)
{
    int curW = vips_image_get_width(in);
    int curH = vips_image_get_height(in);
    VipsImage *out = in;
    bool owned = false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        VipsImage *next = nullptr;
        if (vips_resize(out, &next, static_cast<double>(targetW) / curW, "vscale",
                static_cast<double>(targetH) / curH, "kernel", VIPS_KERNEL_NEAREST, nullptr)) {
            if (owned)
                g_object_unref(out);
            return nullptr;
        }
        if (owned)
            g_object_unref(out);
        out = next;
        owned = true;

        int ow = vips_image_get_width(out);
        int oh = vips_image_get_height(out);
        if (ow == targetW && oh == targetH)
            return out;
        curW = ow;
        curH = oh;
    }

    // Fallback: crop/embed to force exact dimensions (NN semantics — only
    // the edge row/column is affected at most).
    int ow = vips_image_get_width(out);
    int oh = vips_image_get_height(out);
    if (ow > targetW || oh > targetH) {
        VipsImage *cropped = nullptr;
        if (vips_crop(out, &cropped, 0, 0, targetW, targetH, nullptr)) {
            g_object_unref(out);
            return nullptr;
        }
        g_object_unref(out);
        return cropped;
    }
    if (ow < targetW || oh < targetH) {
        VipsImage *embedded = nullptr;
        if (vips_embed(
                out, &embedded, 0, 0, targetW, targetH, "extend", VIPS_EXTEND_COPY, nullptr)) {
            g_object_unref(out);
            return nullptr;
        }
        g_object_unref(out);
        return embedded;
    }
    return out;
}

void NearestResampler::resize(
    const uint8_t *src, int srcW, int srcH, uint8_t *dst, int dstW, int dstH, int channels) const
{
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || channels <= 0)
        return;

    if (srcW == dstW && srcH == dstH) {
        std::memcpy(dst, src, static_cast<size_t>(srcW) * srcH * channels);
        return;
    }

    // Wrap the raw buffer into a vips memory image (no copy), resize, unwrap.
    VipsImage *in = vips_image_new_from_memory(
        src, static_cast<size_t>(srcW) * srcH * channels, srcW, srcH, channels, VIPS_FORMAT_UCHAR);
    if (!in) {
        vips_error_clear();
        return;
    }

    VipsImage *out = resizeExactVips(in, dstW, dstH);
    g_object_unref(in);
    if (!out) {
        vips_error_clear();
        return;
    }

    size_t outSize = 0;
    void *data = vips_image_write_to_memory(out, &outSize);
    g_object_unref(out);
    if (!data) {
        vips_error_clear();
        return;
    }

    std::memcpy(dst, data, std::min(outSize, static_cast<size_t>(dstW) * dstH * channels));
    g_free(data);
}
