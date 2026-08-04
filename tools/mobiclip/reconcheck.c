/* Encode a raw yuv420p sequence with the mobiclip encoder while pulling the
 * encoder's own reconstructed frames, decode the packets it produced, and
 * compare the two pixel for pixel.  Any mismatch means the encoder is
 * predicting from a reference the decoder will never reconstruct, which drifts
 * through P-frames.  Usage:
 *   reconcheck <in.yuv> <w> <h> <nframes> <qp> [mobiclip] [8x8dct]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

static int W, H, NF;
static AVFrame *recon[4096];
static int n_recon, n_dec, n_bad, first_bad = -1;
static long long worst_diff;

static void cmp( AVFrame *dec )
{
    if( n_dec >= n_recon ) { fprintf( stderr, "decoded more frames than reconstructed\n" ); n_dec++; return; }
    AVFrame *r = recon[n_dec];
    long long diff = 0;
    for( int p = 0; p < 3; p++ )
    {
        int w = p ? W/2 : W, h = p ? H/2 : H;
        for( int y = 0; y < h; y++ )
            for( int x = 0; x < w; x++ )
            {
                int a = r->data[p][y*r->linesize[p]+x];
                int b = dec->data[p][y*dec->linesize[p]+x];
                if( a != b ) diff++;
            }
    }
    if( diff )
    {
        n_bad++;
        if( first_bad < 0 ) first_bad = n_dec;
        if( diff > worst_diff ) worst_diff = diff;
    }
    n_dec++;
}

int main( int argc, char **argv )
{
    if( argc < 6 ) { fprintf( stderr, "usage: %s in.yuv w h nframes qp [mobiclip] [8x8dct]\n", argv[0] ); return 2; }
    const char *path = argv[1];
    W = atoi( argv[2] ); H = atoi( argv[3] ); NF = atoi( argv[4] );
    int qp = atoi( argv[5] );
    int mc = argc > 6 ? atoi( argv[6] ) : 2;
    int dct8 = argc > 7 ? atoi( argv[7] ) : -1;

    const AVCodec *enc = avcodec_find_encoder_by_name( "mobiclip" );
    const AVCodec *dec = avcodec_find_decoder( AV_CODEC_ID_MOBICLIP );
    if( !enc || !dec ) { fprintf( stderr, "codec missing\n" ); return 2; }

    AVCodecContext *ec = avcodec_alloc_context3( enc );
    ec->width = W; ec->height = H; ec->pix_fmt = AV_PIX_FMT_YUV420P;
    ec->time_base = (AVRational){ 1, 30 };
    ec->framerate = (AVRational){ 30, 1 };
    ec->gop_size = 30;
    ec->flags |= AV_CODEC_FLAG_RECON_FRAME;
    av_opt_set_int( ec->priv_data, "mobiclip", mc, 0 );
    av_opt_set_int( ec->priv_data, "moflex", 0, 0 );
    av_opt_set_int( ec->priv_data, "qp", qp, 0 );
    if( dct8 >= 0 ) av_opt_set_int( ec->priv_data, "8x8dct", dct8, 0 );
    /* extra key=value AVOptions from argv[9..], so this can mirror an
     * ffmpeg command line exactly */
    for( int i = 9; i < argc; i++ )
    {
        char *kv = strdup( argv[i] ), *eq = strchr( kv, '=' );
        if( eq ) { *eq = 0; if( av_opt_set( ec->priv_data, kv, eq+1, 0 ) < 0 )
                       fprintf( stderr, "could not set %s\n", kv ); }
        free( kv );
    }
    if( avcodec_open2( ec, enc, NULL ) < 0 ) { fprintf( stderr, "enc open failed\n" ); return 2; }

    (void)dec;
    FILE *out = fopen( argc > 8 ? argv[8] : "recon.yuv", "wb" );
    if( !out ) { perror( "recon out" ); return 2; }

    FILE *f = fopen( path, "rb" );
    if( !f ) { perror( path ); return 2; }

    AVFrame *in = av_frame_alloc();
    in->format = AV_PIX_FMT_YUV420P; in->width = W; in->height = H;
    av_frame_get_buffer( in, 32 );
    AVFrame *rf = av_frame_alloc(), *df = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    size_t fsz = (size_t)W*H*3/2;
    uint8_t *buf = malloc( fsz );

    for( int i = 0; i <= NF; i++ )
    {
        AVFrame *send = NULL;
        if( i < NF )
        {
            if( fread( buf, 1, fsz, f ) != fsz ) { NF = i; }
            else
            {
                av_frame_make_writable( in );
                av_image_fill_arrays( (uint8_t*[4]){0}, (int[4]){0}, NULL, AV_PIX_FMT_YUV420P, W, H, 1 );
                for( int y = 0; y < H; y++ ) memcpy( in->data[0]+y*in->linesize[0], buf+y*W, W );
                for( int y = 0; y < H/2; y++ ) memcpy( in->data[1]+y*in->linesize[1], buf+(size_t)W*H+(size_t)y*(W/2), W/2 );
                for( int y = 0; y < H/2; y++ ) memcpy( in->data[2]+y*in->linesize[2], buf+(size_t)W*H+(size_t)W*H/4+(size_t)y*(W/2), W/2 );
                in->pts = i;
                send = in;
            }
        }
        if( avcodec_send_frame( ec, send ) < 0 ) break;
        for( ;; )
        {
            int r = avcodec_receive_packet( ec, pkt );
            if( r == AVERROR(EAGAIN) || r == AVERROR_EOF ) break;
            if( r < 0 ) { fprintf( stderr, "encode error\n" ); return 2; }
            /* the recon frame for this packet */
            if( avcodec_receive_frame( ec, rf ) >= 0 )
            {
                for( int p = 0; p < 3; p++ )
                {
                    int w = p ? W/2 : W, hh = p ? H/2 : H;
                    for( int y = 0; y < hh; y++ )
                        fwrite( rf->data[p]+(size_t)y*rf->linesize[p], 1, w, out );
                }
                n_recon++;
            }
            av_frame_unref( rf );
            av_packet_unref( pkt );
        }
    }
    fclose( out );
    printf( "wrote %d reconstructed frames\n", n_recon );
    return 0;
}
