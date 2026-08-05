/* mods_ref_enc — drive the reference MODS encoder (Helwettpackardenterprise's
 * MODS_Encoder_v43_2, a verified translation of the retail Mobiclip VfW codec)
 * over a raw yuv420p sequence.
 *
 * Two host constraints shape this program:
 *
 *   - The encoder core needs x87 (80-bit) arithmetic and x86 inline asm for its
 *     rate-control model, so it only builds for i386/x86_64.  On Apple Silicon
 *     that means an x86_64 binary under Rosetta.
 *
 *   - Every pointer the encoder stores into its context is truncated to 32 bits
 *     (mods_load_abi32_pointer), so all of its memory must live below 4 GB.
 *     macOS reserves the low 4 GB of an x86_64 process as __PAGEZERO, so this
 *     must be linked with -Wl,-pagezero_size,0x1000 and serve every allocation
 *     from one low arena.
 *
 * Output is a flat stream of frames: u32 size (LE), u8 keyframe, payload.
 *
 * usage: mods_ref_enc <in.yuv> <w> <h> <frames> <qp> <keyint> <out.bin>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#define DLL_BASE    ((void *)0x10000000UL)   /* the image's own base address */
#define ARENA_ADDR  ((void *)0x20000000UL)
#define ARENA_SIZE  (192u * 1024u * 1024u)
#define INSTANCE_SZ 0xB100u
#define FCC_MODS    0x53444F4Du

typedef struct {
    int32_t biSize; int32_t biWidth; int32_t biHeight;
    int16_t biPlanes; int16_t biBitCount;
    uint32_t biCompression; uint32_t biSizeImage;
    int32_t biXPelsPerMeter; int32_t biYPelsPerMeter;
    uint32_t biClrUsed; uint32_t biClrImportant;
} BIH;

typedef struct { uint32_t settings[21]; uint32_t feature_flags; int32_t initial_qp; } BeginOptions;

typedef struct {
    void (*pre_encode)(void *, uint8_t *, void *);
    int  (*force_iframe)(void *, uint8_t *, void *);
    int  (*output_ready)(void *, uint8_t *, void *);
    int  (*reencode)(void *, int32_t *, int32_t, uint8_t *, void *);
} PolicyCallbacks;

/* encoder.cq policy: Quantizer is carried as QP*100, exactly as the retail
 * configuration model stores it (MODS_READER_INT("Quantizer", 1200..4800)). */
typedef struct {
    int32_t interval_frames; int32_t last_keyframe;
    float adjustment; float target_qp; float base_qp;
    int32_t boost_percent; int32_t threshold_percent;
    int32_t timing_num; int32_t timing_den;
} CqPolicy;

extern int32_t mods_vfw_compress_begin_portable(
    uint8_t *instance, const BIH *in, const BIH *out, const BeginOptions *opt,
    void *(*alloc_fn)(void *, uint32_t), void (*free_fn)(void *, void *),
    uint32_t (*resolver)(void *, uint32_t),
    int (*host_create)(void *, uint8_t *, void *),
    void (*host_cleanup)(void *, uint8_t *, void *), void *opaque);
/* mods_vfw_compress_query_input (the desktop VfW DriverProc gate that Begin
 * calls before this) rejects any biWidth > 256 -- that cap is specific to
 * that wrapper's own DriverProc contract, not the core codec: this function
 * (what Begin calls *after* query_input passes) only requires width/height
 * > 0 and a multiple of 16. Call it directly to drive resolutions the VfW
 * wrapper's width policy would otherwise block, e.g. real retail titles'
 * actual frame sizes. */
extern int mods_vfw_encode_context_init_100441d0_contract(
    uint8_t *ctx, int32_t width, int32_t height, const uint32_t settings[21],
    uint32_t feature_flags,
    void *(*alloc_fn)(void *, uint32_t),
    uint32_t (*resolver)(void *, uint32_t),
    int (*host_create)(void *, uint8_t *, const uint8_t *), void *opaque);
extern void mods_vfw_set_qp(uint8_t *ctx, int32_t qp);
extern int32_t mods_vfw_compress_end(uint8_t *instance,
    void (*free_fn)(void *, void *),
    void (*host_cleanup)(void *, uint8_t *, void *), void *opaque);
extern int mods_compress_frame_planar420_hosted(
    uint8_t *ctx, const uint8_t *planar, int is_yv12,
    int32_t *out_size, uint8_t *out_bytes,
    const PolicyCallbacks *policy, void *opaque);
extern void mods_cq_policy_init(CqPolicy *, int32_t quantizer_100,
    int32_t boost_percent, int32_t threshold_percent,
    int32_t interval_frames, int32_t timing_num, int32_t timing_den);
extern void mods_cq_pre_encode(void *, uint8_t *, void *);
extern int  mods_cq_force_iframe(void *, uint8_t *, void *);
extern int  mods_cq_output_ready(void *, uint8_t *, void *);
extern int  mods_cq_reencode(void *, int32_t *, int32_t, uint8_t *, void *);

static uint8_t *arena; static size_t arena_used;

static void *low_alloc(void *opaque, uint32_t size)
{
    (void)opaque;
    size_t p = (arena_used + 15u) & ~(size_t)15u;
    if (p + size > ARENA_SIZE) { fprintf(stderr, "arena exhausted\n"); return NULL; }
    arena_used = p + size;
    void *ptr = arena + p;
    memset(ptr, 0, size);
    return ptr;
}
static void low_free(void *opaque, void *p) { (void)opaque; (void)p; }
static uint32_t resolve(void *opaque, uint32_t addr) { (void)opaque; return addr; }
static int host_create(void *opaque, uint8_t *ctx, const uint8_t *settings)
{ (void)opaque; (void)ctx; (void)settings; return 1; }
static void host_cleanup(void *opaque, uint8_t *ctx, void *obj)
{ (void)opaque; (void)ctx; (void)obj; }

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: %s in.yuv w h frames qp keyint out.bin "
                        "[dll_image.bin] [yuvmode] [vlctable]\n", argv[0]);
        return 2;
    }
    const char *inpath = argv[1];
    int W = atoi(argv[2]), H = atoi(argv[3]);
    int NF = atoi(argv[4]), QP = atoi(argv[5]), KEYINT = atoi(argv[6]);
    int YUVMODE = (argc > 9) ? atoi(argv[9]) : 0;
    /* VlcTable selects the coefficient table set: 1 is the retail MODS/DS
     * default, 0 is the other set -- which is what the Wii .mo path uses in our
     * encoder (-mobiclip 1). Exposed so the two table sets can be compared. */
    int VLCTABLE = (argc > 10) ? atoi(argv[10]) : 1;
    const char *outpath = argv[7];
    const char *dllpath = (argc > 8) ? argv[8]
        : "ref43/reference/controlled/virtualdubmod_10000000.bin";

    arena = mmap(ARENA_ADDR, ARENA_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (arena == MAP_FAILED || (uintptr_t)arena + ARENA_SIZE > 0xFFFFFFFFUL) {
        fprintf(stderr, "could not map a sub-4GB arena (link with "
                        "-Wl,-pagezero_size,0x1000)\n");
        return 2;
    }
    memset(arena, 0, ARENA_SIZE);

    /* The encoder reads constant tables that live in the original DLL's data
     * section (ctx+0x344 -> ~0x1009E1C8, and the prediction/callback vtables).
     * Map the shipped image at its own base so those addresses are simply
     * valid and the resolver can stay an identity. */
    {
        FILE *fd = fopen(dllpath, "rb");
        if (!fd) { perror(dllpath); return 2; }
        fseek(fd, 0, SEEK_END); long dsz = ftell(fd); fseek(fd, 0, SEEK_SET);
        size_t dmap = ((size_t)dsz + 0xFFFFu) & ~(size_t)0xFFFFu;
        void *dll = mmap(DLL_BASE, dmap, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (dll != DLL_BASE) { fprintf(stderr, "could not map the DLL image at 0x10000000\n"); return 2; }
        if (fread(dll, 1, (size_t)dsz, fd) != (size_t)dsz) { fprintf(stderr, "short read of DLL image\n"); return 2; }
        fclose(fd);
    }

    uint8_t *instance = low_alloc(NULL, INSTANCE_SZ);
    uint8_t *outbuf   = low_alloc(NULL, (uint32_t)(W * H * 2 + 4096));
    uint8_t *frame    = low_alloc(NULL, (uint32_t)(W * H * 3 / 2));
    if (!instance || !outbuf || !frame) return 2;

    BIH in  = { 40, W, H, 1, 12, 0x30323449 /* I420 */, (uint32_t)(W*H*3/2), 0,0,0,0 };
    BIH out = { 40, W, H, 1, 24, FCC_MODS, (uint32_t)(W*H*2), 0,0,0,0 };

    BeginOptions opt; memset(&opt, 0, sizeof opt);
    /* encoder.general defaults from the retail configuration model:
     * MeMethod=1, RefCount=5, YuvMode=0, VlcTable=1, SliceCount=1. */
    const char *rc_env = getenv("MODS_REFCOUNT");
    opt.settings[0] = 1; opt.settings[1] = rc_env ? atoi(rc_env) : 5;
    opt.settings[2] = YUVMODE;
    opt.settings[3] = VLCTABLE; opt.settings[4] = 1;
    opt.feature_flags = 0; opt.initial_qp = QP;

    CqPolicy *cq = low_alloc(NULL, (uint32_t)sizeof(CqPolicy));
    /* IBoostPercent 40 and IThreshold 90 are the retail defaults. */
    mods_cq_policy_init(cq, QP * 100, 40, 90, KEYINT, 30000, 1001);
    PolicyCallbacks cb = { mods_cq_pre_encode, mods_cq_force_iframe,
                           mods_cq_output_ready, mods_cq_reencode };

    (void)in; (void)out; /* format-descriptor fields unused now that we skip
                             the VfW query_input width<=256 wrapper gate */
    uint8_t *ctx = instance + 0x234;
    memset(ctx, 0, 0x728);
    fprintf(stderr, "[dbg] calling context_init (bypassing VfW width<=256 gate)\n");
    if (!mods_vfw_encode_context_init_100441d0_contract(
            ctx, W, H, opt.settings, opt.feature_flags,
            low_alloc, resolve, host_create, NULL)) {
        fprintf(stderr, "context_init failed\n");
        return 1;
    }
    if (QP >= 0 && QP <= 51)
        mods_vfw_set_qp(ctx, QP);
    instance[0xB0F4] = 1;
    fprintf(stderr, "[dbg] context_init ok\n");

    FILE *fi = fopen(inpath, "rb");
    FILE *fo = fopen(outpath, "wb");
    if (!fi || !fo) { perror("open"); return 2; }

    size_t fsz = (size_t)W * H * 3 / 2;
    int n = 0, keys = 0;
    long long total = 0;
    for (; n < NF; n++) {
        if (fread(frame, 1, fsz, fi) != fsz) break;
        int32_t size = 0;
        if (n < 2) fprintf(stderr, "[dbg] encoding frame %d\n", n);
        int kf = mods_compress_frame_planar420_hosted(ctx, frame, 0, &size,
                                                      outbuf, &cb, cq);
        if (n < 2) fprintf(stderr, "[dbg] frame %d -> kf=%d size=%d\n", n, kf, size);
        if (kf < 0) { fprintf(stderr, "encode failed at frame %d\n", n); return 1; }
        uint32_t sz = (uint32_t)size;
        uint8_t k = (uint8_t)(kf ? 1 : 0);
        fwrite(&sz, 4, 1, fo); fwrite(&k, 1, 1, fo);
        fwrite(outbuf, 1, sz, fo);
        total += sz; keys += kf ? 1 : 0;
    }
    fclose(fi); fclose(fo);
    mods_vfw_compress_end(instance, low_free, host_cleanup, NULL);
    printf("frames=%d keyframes=%d bytes=%lld\n", n, keys, total);
    return 0;
}
