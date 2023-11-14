#include "svg.h"

#include <stdlib.h>

#define LOG_MODULE "svg"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include <nanosvg.h>
#include <nanosvgrast.h>

pixman_image_t *
svg_load(const char *path, int size)
{
    NSVGimage *svg = nsvgParseFromFile(path, "px", 96);
    if (svg == NULL)
        return NULL;

    if (svg->width == 0 || svg->height == 0) {
        nsvgDelete(svg);
        return NULL;
    }

    struct NSVGrasterizer *rast = nsvgCreateRasterizer();

    const int w = size;
    const int h = size;
    float scale = w > h ? w / svg->width : h / svg->height;

    uint8_t *data = malloc(h * w * 4);
    nsvgRasterize(rast, svg, 0, 0, scale, data, w, h, w * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    pixman_image_t *img = pixman_image_create_bits_no_clear(PIXMAN_a8b8g8r8, w, h, (uint32_t *)data, w * 4);

    /* Nanosvg produces non-premultiplied ABGR, while pixman expects
     * premultiplied */
    for (uint32_t *abgr = (uint32_t *)data; abgr < (uint32_t *)(data + h * w * 4); abgr++) {
        uint8_t alpha = (*abgr >> 24) & 0xff;
        uint8_t blue = (*abgr >> 16) & 0xff;
        uint8_t green = (*abgr >> 8) & 0xff;
        uint8_t red = (*abgr >> 0) & 0xff;

        if (alpha == 0xff)
            continue;

        if (alpha == 0x00)
            blue = green = red = 0x00;
        else {
            blue = blue * alpha / 0xff;
            green = green * alpha / 0xff;
            red = red * alpha / 0xff;
        }

        *abgr = (uint32_t)alpha << 24 | blue << 16 | green << 8 | red;
    }

    return img;
}
