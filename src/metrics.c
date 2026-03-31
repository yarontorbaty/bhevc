#include "bhevc.h"

double calc_psnr(const Frame *a, const Frame *b) {
    int n = a->width * a->height;
    double sse = 0;
    for (int i = 0; i < n; i++) {
        int d = (int)a->y[i] - (int)b->y[i];
        sse += d * d;
    }
    if (sse == 0) return 99.0;
    double mse = sse / n;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

static double ssim_window(const uint8_t *a, const uint8_t *b,
                           int stride, int wx, int wy, int wsz) {
    const double C1 = 6.5025, C2 = 58.5225;
    double sum_a = 0, sum_b = 0, sum_a2 = 0, sum_b2 = 0, sum_ab = 0;
    int n = 0;
    for (int y = 0; y < wsz; y++) {
        for (int x = 0; x < wsz; x++) {
            double va = a[(wy + y) * stride + wx + x];
            double vb = b[(wy + y) * stride + wx + x];
            sum_a += va;
            sum_b += vb;
            sum_a2 += va * va;
            sum_b2 += vb * vb;
            sum_ab += va * vb;
            n++;
        }
    }
    double mu_a = sum_a / n, mu_b = sum_b / n;
    double sig_a2 = sum_a2 / n - mu_a * mu_a;
    double sig_b2 = sum_b2 / n - mu_b * mu_b;
    double sig_ab = sum_ab / n - mu_a * mu_b;
    double num = (2 * mu_a * mu_b + C1) * (2 * sig_ab + C2);
    double den = (mu_a * mu_a + mu_b * mu_b + C1) * (sig_a2 + sig_b2 + C2);
    return num / den;
}

double calc_ssim_frame(const Frame *a, const Frame *b) {
    int wsz = 8;
    int w = a->width, h = a->height;
    double total = 0;
    int count = 0;
    for (int y = 0; y + wsz <= h; y += wsz) {
        for (int x = 0; x + wsz <= w; x += wsz) {
            total += ssim_window(a->y, b->y, a->stride_y, x, y, wsz);
            count++;
        }
    }
    return count > 0 ? total / count : 1.0;
}
