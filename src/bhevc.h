#ifndef BHEVC_H
#define BHEVC_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define BHEVC_MB_SIZE       16
#define BHEVC_BLK_SIZE      4
#define BHEVC_MAX_QP        51
#define BHEVC_DEFAULT_QP    28
#define BHEVC_GOP_SIZE      8
#define BHEVC_EDGE_THRESH   30.0f
#define BHEVC_FLAT_DENSITY  0.05f
#define BHEVC_EDGE_DENSITY  0.15f
#define BHEVC_MAGIC         "BHEVC"
#define BHEVC_VERSION       1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BHEVC_MIN(a,b) ((a)<(b)?(a):(b))
#define BHEVC_MAX(a,b) ((a)>(b)?(a):(b))
#define BHEVC_CLIP(v,lo,hi) BHEVC_MIN(BHEVC_MAX(v,lo),hi)
#define BHEVC_CLIP8(v) ((uint8_t)BHEVC_CLIP(v,0,255))

typedef enum { REGION_FLAT = 0, REGION_EDGE = 1, REGION_TEXTURE = 2 } RegionType;
typedef enum { INTRA_DC = 0, INTRA_H = 1, INTRA_V = 2, INTRA_DIAG = 3 } IntraMode;
typedef enum { FRAME_I = 0, FRAME_P = 1 } FrameType;

typedef struct {
    float gx, gy;
    float magnitude;
    float direction;    /* [0, PI) */
    int8_t polarity;    /* +1 ON, -1 OFF, 0 flat */
} EdgePixel;

typedef struct {
    EdgePixel *pixels;
    int width, height;
} EdgeMap;

typedef struct {
    RegionType type;
    float edge_density;
    float avg_magnitude;
    float dominant_orientation;
    float contour_confidence;
    int qp_offset;
    int search_range;
    IntraMode best_intra;
} MBAnalysis;

typedef struct { int16_t x, y; } MotionVector;

typedef struct {
    int width, height;
    int stride_y, stride_uv;
    uint8_t *y, *u, *v;
} Frame;

typedef struct {
    uint8_t *buf;
    int capacity;
    int byte_pos;
    int bit_pos;    /* next bit to write/read within current byte (7=MSB) */
} Bitstream;

typedef struct {
    int width, height;
    int mb_w, mb_h;
    int base_qp, gop;
    int frame_num;
    Frame *ref, *cur, *rec;
    EdgeMap *emap_cur, *emap_ref;
    MBAnalysis *mb_info;
    Bitstream bs;
} EncoderCtx;

typedef struct {
    int width, height;
    int mb_w, mb_h;
    int base_qp;
    Frame *ref, *rec;
    Bitstream bs;
} DecoderCtx;

/* ---- frame.c ---- */
Frame *frame_alloc(int w, int h);
void   frame_free(Frame *f);
void   frame_copy(Frame *dst, const Frame *src);

/* ---- edge_analysis.c ---- */
EdgeMap *edgemap_alloc(int w, int h);
void     edgemap_free(EdgeMap *m);
void     edge_compute(const Frame *f, EdgeMap *m);
void     edge_classify_mbs(const EdgeMap *m, MBAnalysis *info, int mb_w, int mb_h);
void     edge_temporal(const Frame *cur, const Frame *ref, EdgeMap *ev);

/* ---- transform.c ---- */
void fdct4x4(const int16_t *src, int16_t *dst);
void idct4x4(const int16_t *src, int16_t *dst);
void quant4x4(int16_t *c, int qp);
void dequant4x4(int16_t *c, int qp);

/* ---- bitstream.c ---- */
void     bs_init_write(Bitstream *bs, uint8_t *buf, int cap);
void     bs_init_read(Bitstream *bs, const uint8_t *buf, int len);
void     bs_write_bits(Bitstream *bs, uint32_t val, int n);
void     bs_write_ue(Bitstream *bs, uint32_t v);
void     bs_write_se(Bitstream *bs, int32_t v);
uint32_t bs_read_bits(Bitstream *bs, int n);
uint32_t bs_read_ue(Bitstream *bs);
int32_t  bs_read_se(Bitstream *bs);
void     bs_align_byte(Bitstream *bs);
int      bs_pos_bytes(const Bitstream *bs);

/* ---- prediction.c ---- */
void     predict_intra_blk(const uint8_t *rec, int stride,
                           int bx, int by, IntraMode mode, uint8_t pred[16]);
IntraMode predict_intra_best(const uint8_t *orig, const uint8_t *rec,
                             int stride, int bx, int by, const MBAnalysis *mba);
MotionVector predict_inter_mb(const uint8_t *orig, const uint8_t *ref,
                              int stride, int mb_x, int mb_y,
                              int frame_w, int frame_h,
                              const MBAnalysis *mba,
                              const EdgeMap *emap_orig, const EdgeMap *emap_ref);
int sad16x16(const uint8_t *a, const uint8_t *b, int stride_a, int stride_b);

/* ---- encoder.c ---- */
int  encoder_init(EncoderCtx *e, int w, int h, int qp, int gop);
void encoder_destroy(EncoderCtx *e);
int  encode_frame(EncoderCtx *e, const Frame *input, uint8_t *out, int *out_bytes);

/* ---- decoder.c ---- */
int  decoder_init(DecoderCtx *d, int w, int h, int qp);
void decoder_destroy(DecoderCtx *d);
int  decode_frame(DecoderCtx *d, const uint8_t *in, int in_bytes,
                  Frame *output, FrameType *ft_out);

/* ---- metrics.c ---- */
double calc_psnr(const Frame *a, const Frame *b);
double calc_ssim_frame(const Frame *a, const Frame *b);

/* zigzag scan order for 4x4 */
static const int zigzag4x4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* quantization tables (H.264-style scaling) */
static const int quant_mul[6] = {13107, 11916, 10082, 9362, 8192, 7282};
static const int dequant_scale[6] = {10, 11, 13, 14, 16, 18};

#endif /* BHEVC_H */
