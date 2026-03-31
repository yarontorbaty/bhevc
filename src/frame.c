#include "bhevc.h"

Frame *frame_alloc(int w, int h) {
    Frame *f = calloc(1, sizeof(Frame));
    if (!f) return NULL;
    f->width = w;
    f->height = h;
    f->stride_y = w;
    f->stride_uv = w / 2;
    f->y = calloc(w * h, 1);
    f->u = calloc((w / 2) * (h / 2), 1);
    f->v = calloc((w / 2) * (h / 2), 1);
    if (!f->y || !f->u || !f->v) { frame_free(f); return NULL; }
    return f;
}

void frame_free(Frame *f) {
    if (!f) return;
    free(f->y); free(f->u); free(f->v);
    free(f);
}

void frame_copy(Frame *dst, const Frame *src) {
    memcpy(dst->y, src->y, src->stride_y * src->height);
    memcpy(dst->u, src->u, src->stride_uv * (src->height / 2));
    memcpy(dst->v, src->v, src->stride_uv * (src->height / 2));
}
