#include "bhevc.h"

int decoder_init(DecoderCtx *d, int w, int h, int qp) {
    memset(d, 0, sizeof(*d));
    d->width  = w & ~(BHEVC_MB_SIZE - 1);
    d->height = h & ~(BHEVC_MB_SIZE - 1);
    d->mb_w = d->width / BHEVC_MB_SIZE;
    d->mb_h = d->height / BHEVC_MB_SIZE;
    d->base_qp = qp;
    d->ref = frame_alloc(d->width, d->height);
    d->rec = frame_alloc(d->width, d->height);
    if (!d->ref || !d->rec) return -1;
    return 0;
}

void decoder_destroy(DecoderCtx *d) {
    frame_free(d->ref);
    frame_free(d->rec);
}

static void decode_blk4x4(DecoderCtx *d, int bx, int by, int stride,
                           int qp, uint8_t *rec_plane, const uint8_t *pred) {
    int coded = bs_read_bits(&d->bs, 1);
    int16_t coeffs[16] = {0};

    if (coded) {
        int last_sig = bs_read_ue(&d->bs);
        if (last_sig > 15) last_sig = 15;
        for (int i = 0; i <= last_sig; i++)
            coeffs[zigzag4x4[i]] = (int16_t)bs_read_se(&d->bs);
    }

    dequant4x4(coeffs, qp);
    int16_t recon[16];
    idct4x4(coeffs, recon);

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int val = (int)pred[r * 4 + c] + (int)recon[r * 4 + c];
            rec_plane[(by + r) * stride + bx + c] = BHEVC_CLIP8(val);
        }
}

static void decode_mb_intra(DecoderCtx *d, int mbx, int mby,
                            RegionType region) {
    int w = d->width;
    int sx = mbx * BHEVC_MB_SIZE;
    int sy = mby * BHEVC_MB_SIZE;
    int qp_offset = (region == REGION_FLAT) ? 6 :
                    (region == REGION_EDGE) ? -6 : 0;
    int qp = BHEVC_CLIP(d->base_qp + qp_offset, 0, BHEVC_MAX_QP);
    IntraMode mb_mode = (IntraMode)bs_read_bits(&d->bs, 2);

    int first_block = 1;
    for (int by_off = 0; by_off < BHEVC_MB_SIZE; by_off += 4) {
        for (int bx_off = 0; bx_off < BHEVC_MB_SIZE; bx_off += 4) {
            int bx = sx + bx_off, by = sy + by_off;

            IntraMode mode = mb_mode;
            if (!first_block) {
                /* sub-blocks reuse MB mode */
            }
            first_block = 0;

            uint8_t pred[16];
            predict_intra_blk(d->rec->y, w, bx, by, mode, pred);
            decode_blk4x4(d, bx, by, w, qp, d->rec->y, pred);
        }
    }

    /* chroma */
    int cw = w / 2;
    int cqp = BHEVC_CLIP(qp + 3, 0, BHEVC_MAX_QP);
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *rec_c = (comp == 0) ? d->rec->u : d->rec->v;
        for (int by_off = 0; by_off < 8; by_off += 4) {
            for (int bx_off = 0; bx_off < 8; bx_off += 4) {
                int bx = sx/2 + bx_off, by = sy/2 + by_off;
                uint8_t pred[16];
                predict_intra_blk(rec_c, cw, bx, by, INTRA_DC, pred);
                decode_blk4x4(d, bx, by, cw, cqp, rec_c, pred);
            }
        }
    }
}

static void decode_mb_inter(DecoderCtx *d, int mbx, int mby,
                            RegionType region) {
    int w = d->width;
    int sx = mbx * BHEVC_MB_SIZE;
    int sy = mby * BHEVC_MB_SIZE;
    int qp_offset = (region == REGION_FLAT) ? 6 :
                    (region == REGION_EDGE) ? -6 : 0;
    int qp = BHEVC_CLIP(d->base_qp + qp_offset, 0, BHEVC_MAX_QP);

    int skip = bs_read_bits(&d->bs, 1);
    if (skip) {
        for (int r = 0; r < BHEVC_MB_SIZE; r++)
            memcpy(d->rec->y + (sy + r) * w + sx,
                   d->ref->y + (sy + r) * w + sx, BHEVC_MB_SIZE);
        int cw = w / 2;
        for (int r = 0; r < 8; r++) {
            memcpy(d->rec->u + (sy/2 + r) * cw + sx/2,
                   d->ref->u + (sy/2 + r) * cw + sx/2, 8);
            memcpy(d->rec->v + (sy/2 + r) * cw + sx/2,
                   d->ref->v + (sy/2 + r) * cw + sx/2, 8);
        }
        return;
    }

    MotionVector mv;
    mv.x = bs_read_se(&d->bs);
    mv.y = bs_read_se(&d->bs);

    int rx = sx + mv.x, ry = sy + mv.y;
    rx = BHEVC_CLIP(rx, 0, w - BHEVC_MB_SIZE);
    ry = BHEVC_CLIP(ry, 0, d->height - BHEVC_MB_SIZE);

    for (int by_off = 0; by_off < BHEVC_MB_SIZE; by_off += 4) {
        for (int bx_off = 0; bx_off < BHEVC_MB_SIZE; bx_off += 4) {
            int bx = sx + bx_off, by = sy + by_off;
            int rbx = rx + bx_off, rby = ry + by_off;

            uint8_t pred[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    pred[r * 4 + c] = d->ref->y[(rby + r) * w + rbx + c];
            decode_blk4x4(d, bx, by, w, qp, d->rec->y, pred);
        }
    }

    /* chroma */
    int cw = w / 2;
    int cmvx = mv.x / 2, cmvy = mv.y / 2;
    int cqp = BHEVC_CLIP(qp + 3, 0, BHEVC_MAX_QP);

    for (int comp = 0; comp < 2; comp++) {
        uint8_t *ref_c = (comp == 0) ? d->ref->u : d->ref->v;
        uint8_t *rec_c = (comp == 0) ? d->rec->u : d->rec->v;

        for (int by_off = 0; by_off < 8; by_off += 4) {
            for (int bx_off = 0; bx_off < 8; bx_off += 4) {
                int bx = sx/2 + bx_off, by = sy/2 + by_off;
                int rbx = bx + cmvx, rby = by + cmvy;
                rbx = BHEVC_CLIP(rbx, 0, cw - 4);
                rby = BHEVC_CLIP(rby, 0, d->height/2 - 4);

                uint8_t pred[16];
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        pred[r * 4 + c] = ref_c[(rby + r) * cw + rbx + c];
                decode_blk4x4(d, bx, by, cw, cqp, rec_c, pred);
            }
        }
    }
}

int decode_frame(DecoderCtx *d, const uint8_t *in, int in_bytes,
                 Frame *output, FrameType *ft_out) {
    bs_init_read(&d->bs, in, in_bytes);

    FrameType ftype = (FrameType)bs_read_bits(&d->bs, 1);
    int qp = bs_read_bits(&d->bs, 6);
    d->base_qp = qp;
    if (ft_out) *ft_out = ftype;

    for (int mby = 0; mby < d->mb_h; mby++) {
        for (int mbx = 0; mbx < d->mb_w; mbx++) {
            RegionType region = (RegionType)bs_read_bits(&d->bs, 2);

            if (ftype == FRAME_I)
                decode_mb_intra(d, mbx, mby, region);
            else
                decode_mb_inter(d, mbx, mby, region);
        }
    }

    /* copy to output */
    if (output) {
        for (int r = 0; r < d->height; r++)
            memcpy(output->y + r * output->stride_y,
                   d->rec->y + r * d->width, d->width);
        for (int r = 0; r < d->height / 2; r++) {
            memcpy(output->u + r * output->stride_uv,
                   d->rec->u + r * (d->width / 2), d->width / 2);
            memcpy(output->v + r * output->stride_uv,
                   d->rec->v + r * (d->width / 2), d->width / 2);
        }
    }

    frame_copy(d->ref, d->rec);
    return 0;
}
