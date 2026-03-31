#include "bhevc.h"

/*
 * Properly normalized 4x4 DCT-II / IDCT (Type III).
 * Uses float internally for correctness; the quantization step
 * absorbs the dynamic range so integer overflow is not a concern.
 *
 * DCT-II basis: C[k][n] = alpha(k) * cos(pi*k*(2n+1) / (2*N))
 *   alpha(0) = sqrt(1/N), alpha(k>0) = sqrt(2/N)
 * With N=4, orthonormal: C^T * C = I
 */

static const float dct_matrix[4][4] = {
    { 0.5f,        0.5f,        0.5f,        0.5f       },
    { 0.653281f,   0.270598f,  -0.270598f,  -0.653281f  },
    { 0.5f,       -0.5f,       -0.5f,        0.5f       },
    { 0.270598f,  -0.653281f,   0.653281f,  -0.270598f  }
};

void fdct4x4(const int16_t *src, int16_t *dst) {
    float tmp[16], out[16];

    /* rows: tmp = X * C^T */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += (float)src[i * 4 + k] * dct_matrix[j][k];
            tmp[i * 4 + j] = sum;
        }
    }
    /* columns: out = C * tmp */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += dct_matrix[i][k] * tmp[k * 4 + j];
            out[i * 4 + j] = sum;
        }
    }

    for (int i = 0; i < 16; i++)
        dst[i] = (int16_t)roundf(out[i]);
}

void idct4x4(const int16_t *src, int16_t *dst) {
    float tmp[16], out[16];

    /* rows: tmp = Y * C  (C^T transposed = C, since orthonormal: inverse = C^T) */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += (float)src[i * 4 + k] * dct_matrix[k][j];
            tmp[i * 4 + j] = sum;
        }
    }
    /* columns: out = C^T * tmp */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += dct_matrix[k][i] * tmp[k * 4 + j];
            out[i * 4 + j] = sum;
        }
    }

    for (int i = 0; i < 16; i++)
        dst[i] = (int16_t)roundf(out[i]);
}

/*
 * Quantization: qstep = 2^((QP-4)/6), doubling every 6 QP.
 * Dead-zone quantizer: small coefficients near zero are discarded.
 */
static float get_qstep(int qp) {
    return powf(2.0f, (qp - 4.0f) / 6.0f);
}

void quant4x4(int16_t *c, int qp) {
    float qstep = get_qstep(qp);
    float inv_qstep = 1.0f / qstep;
    float dead_zone = qstep * 0.4f;

    for (int i = 0; i < 16; i++) {
        float val = (float)c[i];
        int sign = (val >= 0) ? 1 : -1;
        float absval = fabsf(val);
        if (absval < dead_zone) {
            c[i] = 0;
        } else {
            c[i] = (int16_t)(sign * (int)(absval * inv_qstep + 0.5f));
        }
    }
}

void dequant4x4(int16_t *c, int qp) {
    float qstep = get_qstep(qp);
    for (int i = 0; i < 16; i++)
        c[i] = (int16_t)roundf((float)c[i] * qstep);
}
