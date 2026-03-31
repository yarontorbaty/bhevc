#include "bhevc.h"

/*
 * BHEVC Encoder — brain-inspired hierarchical video encoder.
 *
 * Pipeline per frame:
 *   1. Edge analysis: compute signed ON/OFF edge map (thalamocortical model)
 *   2. Region classification: segment MBs into flat/edge/texture
 *   3. Adaptive prediction: edge-guided intra modes or structure-aware ME
 *   4. Transform + contour-priority quantization (variable QP per region)
 *   5. Entropy coding to bitstream
 *   6. Reconstruction for reference
 */

int encoder_init(EncoderCtx *e, int w, int h, int qp, int gop) {
    memset(e, 0, sizeof(*e));
    e->width  = w & ~(BHEVC_MB_SIZE - 1);
    e->height = h & ~(BHEVC_MB_SIZE - 1);
    e->mb_w = e->width / BHEVC_MB_SIZE;
    e->mb_h = e->height / BHEVC_MB_SIZE;
    e->base_qp = qp;
    e->gop = gop;
    e->frame_num = 0;
    e->ref = frame_alloc(e->width, e->height);
    e->cur = frame_alloc(e->width, e->height);
    e->rec = frame_alloc(e->width, e->height);
    e->emap_cur = edgemap_alloc(e->width, e->height);
    e->emap_ref = edgemap_alloc(e->width, e->height);
    e->mb_info = calloc(e->mb_w * e->mb_h, sizeof(MBAnalysis));
    if (!e->ref || !e->cur || !e->rec || !e->emap_cur ||
        !e->emap_ref || !e->mb_info)
        return -1;
    return 0;
}

void encoder_destroy(EncoderCtx *e) {
    frame_free(e->ref);
    frame_free(e->cur);
    frame_free(e->rec);
    edgemap_free(e->emap_cur);
    edgemap_free(e->emap_ref);
    free(e->mb_info);
}

/*
 * Encode a single 4x4 residual block:
 *   - Forward DCT, quantize, write to bitstream
 *   - Dequantize, inverse DCT, reconstruct into rec_plane
 *
 * orig/rec use frame-width stride; pred is packed 4x4.
 */
static void encode_blk(EncoderCtx *e, int bx, int by,
                        const uint8_t *orig, int orig_stride,
                        const uint8_t *pred,
                        uint8_t *rec, int rec_stride, int qp) {
    int16_t res[16], coeffs[16];

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            res[r * 4 + c] = (int16_t)orig[(by + r) * orig_stride + bx + c] -
                              (int16_t)pred[r * 4 + c];

    fdct4x4(res, coeffs);
    quant4x4(coeffs, qp);

    int nonzero = 0;
    for (int i = 0; i < 16; i++)
        if (coeffs[i]) { nonzero = 1; break; }

    bs_write_bits(&e->bs, nonzero, 1);
    if (nonzero) {
        int last_sig = 0;
        for (int i = 15; i >= 0; i--)
            if (coeffs[zigzag4x4[i]]) { last_sig = i; break; }
        bs_write_ue(&e->bs, last_sig);
        for (int i = 0; i <= last_sig; i++)
            bs_write_se(&e->bs, coeffs[zigzag4x4[i]]);
    }

    /* local reconstruction */
    dequant4x4(coeffs, qp);
    int16_t recon[16];
    idct4x4(coeffs, recon);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int val = (int)pred[r * 4 + c] + (int)recon[r * 4 + c];
            rec[(by + r) * rec_stride + bx + c] = BHEVC_CLIP8(val);
        }
}

static void encode_mb_intra(EncoderCtx *e, int mbx, int mby,
                            const MBAnalysis *mba) {
    int w = e->width;
    int sx = mbx * BHEVC_MB_SIZE;
    int sy = mby * BHEVC_MB_SIZE;
    int qp = BHEVC_CLIP(e->base_qp + mba->qp_offset, 0, BHEVC_MAX_QP);

    /* select one intra mode for the whole MB using brain-inspired guidance */
    IntraMode mb_mode = predict_intra_best(
        e->cur->y, e->rec->y, w, sx, sy, mba);
    bs_write_bits(&e->bs, mb_mode, 2);

    /* luma: 16 4x4 blocks, all using the MB-level mode */
    for (int by_off = 0; by_off < BHEVC_MB_SIZE; by_off += 4) {
        for (int bx_off = 0; bx_off < BHEVC_MB_SIZE; bx_off += 4) {
            int bx = sx + bx_off, by = sy + by_off;
            uint8_t pred[16];
            predict_intra_blk(e->rec->y, w, bx, by, mb_mode, pred);
            encode_blk(e, bx, by, e->cur->y, w, pred, e->rec->y, w, qp);
        }
    }

    /* chroma */
    int cw = w / 2;
    int cqp = BHEVC_CLIP(qp + 3, 0, BHEVC_MAX_QP);
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *cur_c = (comp == 0) ? e->cur->u : e->cur->v;
        uint8_t *rec_c = (comp == 0) ? e->rec->u : e->rec->v;
        for (int by_off = 0; by_off < 8; by_off += 4) {
            for (int bx_off = 0; bx_off < 8; bx_off += 4) {
                int bx = sx / 2 + bx_off, by = sy / 2 + by_off;
                uint8_t pred[16];
                predict_intra_blk(rec_c, cw, bx, by, INTRA_DC, pred);
                encode_blk(e, bx, by, cur_c, cw, pred, rec_c, cw, cqp);
            }
        }
    }
}

static void encode_mb_inter(EncoderCtx *e, int mbx, int mby,
                            const MBAnalysis *mba) {
    int w = e->width;
    int sx = mbx * BHEVC_MB_SIZE;
    int sy = mby * BHEVC_MB_SIZE;
    int qp = BHEVC_CLIP(e->base_qp + mba->qp_offset, 0, BHEVC_MAX_QP);

    MotionVector mv = predict_inter_mb(
        e->cur->y, e->ref->y, w, mbx, mby,
        e->width, e->height, mba, e->emap_cur, e->emap_ref);

    /* skip: zero MV + flat region + low residual energy */
    int skip = 0;
    if (mv.x == 0 && mv.y == 0 && mba->type == REGION_FLAT) {
        int sad = sad16x16(e->cur->y + sy * w + sx,
                           e->ref->y + sy * w + sx, w, w);
        if (sad < BHEVC_MB_SIZE * BHEVC_MB_SIZE * 3)
            skip = 1;
    }

    bs_write_bits(&e->bs, skip, 1);
    if (skip) {
        for (int r = 0; r < BHEVC_MB_SIZE; r++)
            memcpy(e->rec->y + (sy + r) * w + sx,
                   e->ref->y + (sy + r) * w + sx, BHEVC_MB_SIZE);
        int cw = w / 2;
        for (int r = 0; r < 8; r++) {
            memcpy(e->rec->u + (sy / 2 + r) * cw + sx / 2,
                   e->ref->u + (sy / 2 + r) * cw + sx / 2, 8);
            memcpy(e->rec->v + (sy / 2 + r) * cw + sx / 2,
                   e->ref->v + (sy / 2 + r) * cw + sx / 2, 8);
        }
        return;
    }

    bs_write_se(&e->bs, mv.x);
    bs_write_se(&e->bs, mv.y);

    /* luma */
    int rx = sx + mv.x, ry = sy + mv.y;
    for (int by_off = 0; by_off < BHEVC_MB_SIZE; by_off += 4) {
        for (int bx_off = 0; bx_off < BHEVC_MB_SIZE; bx_off += 4) {
            int bx = sx + bx_off, by = sy + by_off;
            int rbx = rx + bx_off, rby = ry + by_off;
            uint8_t pred[16];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    pred[r * 4 + c] = e->ref->y[(rby + r) * w + rbx + c];
            encode_blk(e, bx, by, e->cur->y, w, pred, e->rec->y, w, qp);
        }
    }

    /* chroma */
    int cw = w / 2;
    int cmvx = mv.x / 2, cmvy = mv.y / 2;
    int cqp = BHEVC_CLIP(qp + 3, 0, BHEVC_MAX_QP);
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *cur_c = (comp == 0) ? e->cur->u : e->cur->v;
        uint8_t *ref_c = (comp == 0) ? e->ref->u : e->ref->v;
        uint8_t *rec_c = (comp == 0) ? e->rec->u : e->rec->v;
        for (int by_off = 0; by_off < 8; by_off += 4) {
            for (int bx_off = 0; bx_off < 8; bx_off += 4) {
                int bx = sx / 2 + bx_off, by = sy / 2 + by_off;
                int rbx = BHEVC_CLIP(bx + cmvx, 0, cw - 4);
                int rby = BHEVC_CLIP(by + cmvy, 0, e->height / 2 - 4);
                uint8_t pred[16];
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        pred[r * 4 + c] = ref_c[(rby + r) * cw + rbx + c];
                encode_blk(e, bx, by, cur_c, cw, pred, rec_c, cw, cqp);
            }
        }
    }
}

int encode_frame(EncoderCtx *e, const Frame *input, uint8_t *out, int *out_bytes) {
    for (int r = 0; r < e->height; r++)
        memcpy(e->cur->y + r * e->width,
               input->y + r * input->stride_y, e->width);
    for (int r = 0; r < e->height / 2; r++) {
        memcpy(e->cur->u + r * (e->width / 2),
               input->u + r * input->stride_uv, e->width / 2);
        memcpy(e->cur->v + r * (e->width / 2),
               input->v + r * input->stride_uv, e->width / 2);
    }

    FrameType ftype = (e->frame_num % e->gop == 0) ? FRAME_I : FRAME_P;

    /* === PASS 1: Brain-inspired structural analysis === */
    edge_compute(e->cur, e->emap_cur);
    edge_classify_mbs(e->emap_cur, e->mb_info, e->mb_w, e->mb_h);
    if (ftype == FRAME_P)
        edge_compute(e->ref, e->emap_ref);

    /* === PASS 2: Encode === */
    int buf_size = e->width * e->height * 4;
    bs_init_write(&e->bs, out, buf_size);

    bs_write_bits(&e->bs, ftype, 1);
    bs_write_bits(&e->bs, e->base_qp, 6);

    for (int mby = 0; mby < e->mb_h; mby++) {
        for (int mbx = 0; mbx < e->mb_w; mbx++) {
            MBAnalysis *mba = &e->mb_info[mby * e->mb_w + mbx];
            bs_write_bits(&e->bs, mba->type, 2);

            if (ftype == FRAME_I)
                encode_mb_intra(e, mbx, mby, mba);
            else
                encode_mb_inter(e, mbx, mby, mba);
        }
    }

    bs_align_byte(&e->bs);
    *out_bytes = bs_pos_bytes(&e->bs);

    frame_copy(e->ref, e->rec);
    e->frame_num++;
    return 0;
}
