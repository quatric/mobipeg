/* Encode a raw yuv420p sequence with the mobiclip encoder and dump the raw
 * bitstream of every packet, with no container framing in the way, so it can be
 * diffed against the reference encoder's output byte for byte.
 *
 * The framing matches reference/mods_ref_enc.c -- `u32 size, u8 keyframe,
 * payload` -- except that the reference's payload carries a 4-byte length
 * prefix that is not part of the Mobiclip frame; strip it there, not here.
 *
 *   pktdump <in.yuv> <w> <h> <nframes> <qp> <keyint> <out.bin> [mobiclip] [key=value ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>

/* The encoder emits H.264 Annex-B NALs; the Mobiclip frame the muxers write is
 * what this strips out.  Declared here rather than including mo.h, which pulls
 * in libavformat internals. */
int ff_extract_mobiclip_payload(const uint8_t *src, int src_size,
                                uint8_t **out_data, int *out_size);

int main( int argc, char **argv )
{
    if( argc < 8 ) { fprintf( stderr, "usage: %s in.yuv w h nframes qp keyint out.bin [mobiclip] [key=value ...]\n", argv[0] ); return 2; }
    const char *path = argv[1];
    int W = atoi( argv[2] ), H = atoi( argv[3] ), NF = atoi( argv[4] );
    int qp = atoi( argv[5] ), keyint = atoi( argv[6] );
    const char *outpath = argv[7];
    int mc = argc > 8 ? atoi( argv[8] ) : 1;

    const AVCodec *enc = avcodec_find_encoder_by_name( "mobiclip" );
    if( !enc ) { fprintf( stderr, "mobiclip encoder missing\n" ); return 2; }

    AVCodecContext *ec = avcodec_alloc_context3( enc );
    ec->width = W; ec->height = H; ec->pix_fmt = AV_PIX_FMT_YUV420P;
    ec->time_base = (AVRational){ 1, 30 };
    ec->framerate = (AVRational){ 30, 1 };
    ec->gop_size = keyint;
    av_opt_set_int( ec->priv_data, "mobiclip", mc, 0 );
    av_opt_set_int( ec->priv_data, "moflex", 0, 0 );
    av_opt_set_int( ec->priv_data, "qp", qp, 0 );
    for( int i = 9; i < argc; i++ )
    {
        char *kv = strdup( argv[i] ), *eq = strchr( kv, '=' );
        if( eq ) { *eq = 0; if( av_opt_set( ec->priv_data, kv, eq+1, 0 ) < 0 )
                       fprintf( stderr, "could not set %s\n", kv ); }
        free( kv );
    }
    if( avcodec_open2( ec, enc, NULL ) < 0 ) { fprintf( stderr, "enc open failed\n" ); return 2; }

    FILE *out = fopen( outpath, "wb" );
    if( !out ) { perror( outpath ); return 2; }
    FILE *f = fopen( path, "rb" );
    if( !f ) { perror( path ); return 2; }

    AVFrame *in = av_frame_alloc();
    in->format = AV_PIX_FMT_YUV420P; in->width = W; in->height = H;
    av_frame_get_buffer( in, 32 );
    AVPacket *pkt = av_packet_alloc();
    size_t fsz = (size_t)W*H*3/2;
    uint8_t *buf = malloc( fsz );
    int n = 0;

    for( int i = 0; i <= NF; i++ )
    {
        AVFrame *send = NULL;
        if( i < NF )
        {
            if( fread( buf, 1, fsz, f ) != fsz ) NF = i;
            else
            {
                av_frame_make_writable( in );
                for( int y = 0; y < H; y++ ) memcpy( in->data[0]+(size_t)y*in->linesize[0], buf+(size_t)y*W, W );
                for( int y = 0; y < H/2; y++ ) memcpy( in->data[1]+(size_t)y*in->linesize[1], buf+(size_t)W*H+(size_t)y*(W/2), W/2 );
                for( int y = 0; y < H/2; y++ ) memcpy( in->data[2]+(size_t)y*in->linesize[2], buf+(size_t)W*H+(size_t)W*H/4+(size_t)y*(W/2), W/2 );
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
            uint8_t *vp = NULL; int vsz = 0;
            if( ff_extract_mobiclip_payload( pkt->data, pkt->size, &vp, &vsz ) < 0 )
            { fprintf( stderr, "no mobiclip payload in packet %d\n", n ); return 2; }
            uint32_t sz = vsz;
            uint8_t key = !!(pkt->flags & AV_PKT_FLAG_KEY);
            fwrite( &sz, 4, 1, out );
            fwrite( &key, 1, 1, out );
            fwrite( vp, 1, vsz, out );
            av_free( vp );
            n++;
            av_packet_unref( pkt );
        }
    }
    fclose( out ); fclose( f );
    printf( "wrote %d packets\n", n );
    return 0;
}
