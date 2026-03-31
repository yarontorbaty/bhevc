#include "bhevc.h"
#include <time.h>

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "BHEVC — Brain-inspired Hierarchical Efficient Video Codec\n\n"
        "Usage:\n"
        "  %s encode  -i input.yuv -o output.bhevc -w WIDTH -h HEIGHT [-q QP] [-g GOP] [-n FRAMES]\n"
        "  %s decode  -i input.bhevc -o output.yuv -w WIDTH -h HEIGHT [-n FRAMES]\n"
        "  %s metrics -a original.yuv -b decoded.yuv -w WIDTH -h HEIGHT [-n FRAMES]\n\n"
        "Options:\n"
        "  -w WIDTH    Frame width\n"
        "  -h HEIGHT   Frame height\n"
        "  -q QP       Base quantization parameter (0-51, default 28)\n"
        "  -g GOP      GOP size (default 8)\n"
        "  -n FRAMES   Number of frames to process (default: all)\n",
        argv0, argv0, argv0);
}

static int read_yuv_frame(FILE *fp, Frame *f, int w, int h) {
    size_t luma = w * h;
    size_t chroma = (w / 2) * (h / 2);
    if (fread(f->y, 1, luma, fp) != luma) return -1;
    if (fread(f->u, 1, chroma, fp) != chroma) return -1;
    if (fread(f->v, 1, chroma, fp) != chroma) return -1;
    return 0;
}

static int write_yuv_frame(FILE *fp, const Frame *f, int w, int h) {
    fwrite(f->y, 1, w * h, fp);
    fwrite(f->u, 1, (w / 2) * (h / 2), fp);
    fwrite(f->v, 1, (w / 2) * (h / 2), fp);
    return 0;
}

static int cmd_encode(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    int w = 0, h = 0, qp = BHEVC_DEFAULT_QP, gop = BHEVC_GOP_SIZE, nframes = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q") && i + 1 < argc) qp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gop = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nframes = atoi(argv[++i]);
    }
    if (!input || !output || w <= 0 || h <= 0) {
        fprintf(stderr, "Error: encode requires -i, -o, -w, -h\n");
        return 1;
    }

    FILE *fin = fopen(input, "rb");
    FILE *fout = fopen(output, "wb");
    if (!fin || !fout) { perror("fopen"); return 1; }

    EncoderCtx enc;
    if (encoder_init(&enc, w, h, qp, gop) < 0) {
        fprintf(stderr, "encoder_init failed\n"); return 1;
    }

    Frame *frame = frame_alloc(w, h);
    int out_cap = w * h * 4;
    uint8_t *out_buf = malloc(out_cap);

    /* write file header */
    fwrite(BHEVC_MAGIC, 1, 5, fout);
    uint8_t hdr[7];
    hdr[0] = BHEVC_VERSION;
    hdr[1] = (w >> 8) & 0xff; hdr[2] = w & 0xff;
    hdr[3] = (h >> 8) & 0xff; hdr[4] = h & 0xff;
    hdr[5] = qp;
    hdr[6] = gop;
    fwrite(hdr, 1, 7, fout);

    int frame_count = 0;
    double total_time = 0;
    long total_bytes = 12; /* header */

    fprintf(stderr, "BHEVC Encoder: %dx%d QP=%d GOP=%d\n", enc.width, enc.height, qp, gop);
    fprintf(stderr, "%-6s %-6s %-10s %-12s\n", "Frame", "Type", "Bytes", "Time(ms)");

    while ((nframes == 0 || frame_count < nframes) &&
           read_yuv_frame(fin, frame, w, h) == 0) {

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int out_bytes = 0;
        encode_frame(&enc, frame, out_buf, &out_bytes);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
        total_time += ms;

        /* write frame: 4-byte size + data */
        uint8_t szb[4];
        szb[0] = (out_bytes >> 24) & 0xff;
        szb[1] = (out_bytes >> 16) & 0xff;
        szb[2] = (out_bytes >> 8) & 0xff;
        szb[3] = out_bytes & 0xff;
        fwrite(szb, 1, 4, fout);
        fwrite(out_buf, 1, out_bytes, fout);
        total_bytes += 4 + out_bytes;

        FrameType ft = (frame_count % gop == 0) ? FRAME_I : FRAME_P;
        fprintf(stderr, "%-6d %-6s %-10d %-12.1f\n",
                frame_count, ft == FRAME_I ? "I" : "P", out_bytes, ms);
        frame_count++;
    }

    fprintf(stderr, "\nEncoded %d frames, %ld bytes total, %.1f ms avg/frame\n",
            frame_count, total_bytes, frame_count > 0 ? total_time / frame_count : 0);

    frame_free(frame);
    free(out_buf);
    encoder_destroy(&enc);
    fclose(fin);
    fclose(fout);
    return 0;
}

static int cmd_decode(int argc, char **argv) {
    const char *input = NULL, *output = NULL;
    int w = 0, h = 0, nframes = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nframes = atoi(argv[++i]);
    }
    if (!input || !output || w <= 0 || h <= 0) {
        fprintf(stderr, "Error: decode requires -i, -o, -w, -h\n");
        return 1;
    }

    FILE *fin = fopen(input, "rb");
    FILE *fout = fopen(output, "wb");
    if (!fin || !fout) { perror("fopen"); return 1; }

    /* read file header */
    char magic[5];
    fread(magic, 1, 5, fin);
    if (memcmp(magic, BHEVC_MAGIC, 5) != 0) {
        fprintf(stderr, "Not a BHEVC file\n"); return 1;
    }
    uint8_t hdr[7];
    fread(hdr, 1, 7, fin);
    int file_w = (hdr[1] << 8) | hdr[2];
    int file_h = (hdr[3] << 8) | hdr[4];
    int qp = hdr[5];
    (void)file_w; (void)file_h;

    DecoderCtx dec;
    if (decoder_init(&dec, w, h, qp) < 0) {
        fprintf(stderr, "decoder_init failed\n"); return 1;
    }

    Frame *frame = frame_alloc(w, h);
    int frame_count = 0;
    double total_time = 0;

    fprintf(stderr, "BHEVC Decoder: %dx%d\n", dec.width, dec.height);

    while (nframes == 0 || frame_count < nframes) {
        uint8_t szb[4];
        if (fread(szb, 1, 4, fin) != 4) break;
        int frame_bytes = (szb[0] << 24) | (szb[1] << 16) | (szb[2] << 8) | szb[3];
        if (frame_bytes <= 0 || frame_bytes > w * h * 4) break;

        uint8_t *data = malloc(frame_bytes);
        if (fread(data, 1, frame_bytes, fin) != (size_t)frame_bytes) {
            free(data); break;
        }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        FrameType ft;
        decode_frame(&dec, data, frame_bytes, frame, &ft);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_nsec - t0.tv_nsec) / 1e6;
        total_time += ms;

        write_yuv_frame(fout, frame, w, h);
        free(data);
        frame_count++;
    }

    fprintf(stderr, "Decoded %d frames, %.1f ms avg/frame\n",
            frame_count, frame_count > 0 ? total_time / frame_count : 0);

    frame_free(frame);
    decoder_destroy(&dec);
    fclose(fin);
    fclose(fout);
    return 0;
}

static int cmd_metrics(int argc, char **argv) {
    const char *file_a = NULL, *file_b = NULL;
    int w = 0, h = 0, nframes = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) file_a = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) file_b = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nframes = atoi(argv[++i]);
    }
    if (!file_a || !file_b || w <= 0 || h <= 0) {
        fprintf(stderr, "Error: metrics requires -a, -b, -w, -h\n");
        return 1;
    }

    FILE *fa = fopen(file_a, "rb");
    FILE *fb = fopen(file_b, "rb");
    if (!fa || !fb) { perror("fopen"); return 1; }

    Frame *a = frame_alloc(w, h);
    Frame *b = frame_alloc(w, h);
    int count = 0;
    double sum_psnr = 0, sum_ssim = 0;

    printf("%-6s %-10s %-10s\n", "Frame", "PSNR", "SSIM");
    while ((nframes == 0 || count < nframes) &&
           read_yuv_frame(fa, a, w, h) == 0 &&
           read_yuv_frame(fb, b, w, h) == 0) {
        double psnr = calc_psnr(a, b);
        double ssim = calc_ssim_frame(a, b);
        printf("%-6d %-10.2f %-10.6f\n", count, psnr, ssim);
        sum_psnr += psnr;
        sum_ssim += ssim;
        count++;
    }
    printf("\nAverage over %d frames: PSNR=%.2f dB  SSIM=%.6f\n",
           count, count > 0 ? sum_psnr / count : 0,
           count > 0 ? sum_ssim / count : 0);

    frame_free(a);
    frame_free(b);
    fclose(fa);
    fclose(fb);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "encode"))
        return cmd_encode(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "decode"))
        return cmd_decode(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "metrics"))
        return cmd_metrics(argc - 2, argv + 2);
    else {
        print_usage(argv[0]);
        return 1;
    }
}
