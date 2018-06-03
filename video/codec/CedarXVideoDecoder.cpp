
/*
 * Copyright (c) 2018 WangBin <wbsecg1 at gmail.com>
 */
#include "mdk/VideoDecoder.h"
#include "mdk/MediaInfo.h"
#include "mdk/Packet.h"
#include "mdk/VideoFrame.h"
#include <iostream>
extern "C" {
#include <libcedarv/libcedarv.h>
}
MDK_NS_BEGIN

// source data is colum major. every block is 32x32
static void map32x32_to_yuv_Y(void* srcY, void* tarY, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height)
{
    unsigned long offset;
    unsigned char *ptr = (unsigned char *)srcY;
    const unsigned int mb_width = (coded_width+15) >> 4;
    const unsigned int mb_height = (coded_height+15) >> 4;
    const unsigned int twomb_line = (mb_height+1) >> 1;
    const unsigned int recon_width = (mb_width+1) & 0xfffffffe;

    for (unsigned int i = 0; i < twomb_line; i++) {
        const unsigned int M = 32*i;
        for (unsigned int j = 0; j < recon_width; j+=2) {
            const unsigned int n = j*16;
            offset = M*dst_pitch + n;
            for (unsigned int l = 0; l < 32; l++) {
                if (M+l < coded_height) {
                    if (n+16 < coded_width) {
                        //1st & 2nd mb
                        memcpy((unsigned char *)tarY+offset, ptr, 32);
                    } else if (n<coded_width) {
                        // 1st mb
                        memcpy((unsigned char *)tarY+offset, ptr, 16);
                    }
                    offset += dst_pitch;
                }
                ptr += 32;
            }
        }
    }
}

static void map32x32_to_yuv_C(void* srcC, void* tarCb, void* tarCr, unsigned int dst_pitch, unsigned int coded_width, unsigned int coded_height)
{
    coded_width /= 2; // libvdpau-sunxi compatible
    unsigned char line[32];
    unsigned long offset;
    unsigned char *ptr = (unsigned char *)srcC;
    const unsigned int mb_width = (coded_width+7) >> 3;
    const unsigned int mb_height = (coded_height+7) >> 3;
    const unsigned int fourmb_line = (mb_height+3) >> 2;
    const unsigned int recon_width = (mb_width+1) & 0xfffffffe;

    for (unsigned int i = 0; i < fourmb_line; i++) {
        const int M = i*32;
        for (unsigned int j = 0; j < recon_width; j+=2) {
            const unsigned int n = j*8;
            offset = M*dst_pitch + n;
            for (unsigned int l = 0; l < 32; l++) {
                if (M+l < coded_height) {
                    if (n+8 < coded_width) {
                        // 1st & 2nd mb
                        memcpy(line, ptr, 32);
                        //unsigned char *line = ptr;
                        for (int k = 0; k < 16; k++) {
                            *((unsigned char *)tarCb + offset + k) = line[2*k];
                            *((unsigned char *)tarCr + offset + k) = line[2*k+1];
                        }
                    } else if (n < coded_width) {
                        // 1st mb
                        memcpy(line, ptr, 16);
                        //unsigned char *line = ptr;
                        for (int k = 0; k < 8; k++) {
                            *((unsigned char *)tarCb + offset + k) = line[2*k];
                            *((unsigned char *)tarCr + offset + k) = line[2*k+1];
                        }
                    }
                    offset += dst_pitch;
                }
                ptr += 32;
            }
        }
    }
}

class CedarXVideoDecoder final : public VideoDecoder
{
public:
    ~CedarXVideoDecoder() override;
    const char* name() const override {return "CedarX";}
    bool open() override;
    bool close() override;
    bool flush() override {return true;}
    bool decode(const Packet& pkt) override;
private:
    CEDARV_DECODER *dec_ = nullptr;
    typedef void (*map_y_t)(void* src, void* dst, unsigned int dst_pitch, unsigned int w, unsigned int h);
    map_y_t map_y_ = map32x32_to_yuv_Y;
    typedef void (*map_c_t)(void* src, void* dst1, void* dst2, unsigned int dst_pitch, unsigned int w, unsigned int h);
    map_c_t map_c_ = map32x32_to_yuv_C;
};

#define CEDARX_ENSURE(f, ...) CEDARX_CHECK(f, return __VA_ARGS__;)
#define CEDARX_WARN(f) CEDARX_CHECK(f)
#define CEDARX_CHECK(f, ...)  do { \
        int ret = f; \
        if (ret < 0) { \
            std::clog << #f "  ERROR@" << __LINE__ << __FUNCTION__ << ": (" << ret << ") " << std::endl; \
            __VA_ARGS__ \
        } \
    } while (false)

CedarXVideoDecoder::~CedarXVideoDecoder()
{
    //libcedarv_exit(dec_);
}

static const struct {
    const char* name;
    cedarv_stream_format_e format;
    cedarv_sub_format_e sub_format;
} cedarv_codecs[] = {
    { "mpeg1", CEDARV_STREAM_FORMAT_MPEG2, CEDARV_MPEG2_SUB_FORMAT_MPEG1 },
    { "mpeg2", CEDARV_STREAM_FORMAT_MPEG2, CEDARV_MPEG2_SUB_FORMAT_MPEG2 },
    { "h263", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_H263 },
    { "h264", CEDARV_STREAM_FORMAT_H264, CEDARV_SUB_FORMAT_UNKNOW }, //CEDARV_CONTAINER_FORMAT_TS
    { "vc1", CEDARV_STREAM_FORMAT_VC1, CEDARV_SUB_FORMAT_UNKNOW },
    { "vp6", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_VP6 },
    { "vp8", CEDARV_STREAM_FORMAT_VP8, CEDARV_SUB_FORMAT_UNKNOW },
    { "wmv1", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_WMV1 },
    { "wmv2", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_WMV2 },
    { "wmv3", CEDARV_STREAM_FORMAT_VC1, CEDARV_SUB_FORMAT_UNKNOW },
    { "mjpeg", CEDARV_STREAM_FORMAT_MJPEG, CEDARV_SUB_FORMAT_UNKNOW },
    { "msmpeg4v1", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_DIVX1 },
    { "msmpeg4v2", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_DIVX2 },
    { "msmpeg4v3", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_DIVX3 },
    { "mpeg4", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_XVID },
    { "flv", CEDARV_STREAM_FORMAT_MPEG4, CEDARV_MPEG4_SUB_FORMAT_SORENSSON_H263 },
    { "rv10", CEDARV_STREAM_FORMAT_REALVIDEO, CEDARV_SUB_FORMAT_UNKNOW },
    { "rv20", CEDARV_STREAM_FORMAT_REALVIDEO, CEDARV_SUB_FORMAT_UNKNOW },
    { "rv30", CEDARV_STREAM_FORMAT_REALVIDEO, CEDARV_SUB_FORMAT_UNKNOW },
    { "rv40", CEDARV_STREAM_FORMAT_REALVIDEO, CEDARV_SUB_FORMAT_UNKNOW },
    // TODO: h265
};

bool to_cedarv(const char* name, cedarv_stream_format_e* format, cedarv_sub_format_e *sub_format)
{
    for (const auto& c : cedarv_codecs) {
        if (strstr(name, c.name)) {
            *format = c.format;
            *sub_format = c.sub_format;
            return true;
        }
    }
    return false;
}

bool CedarXVideoDecoder::open()
{
    const VideoCodecParameters& par = parameters();
    cedarv_stream_format_e format = CEDARV_STREAM_FORMAT_UNKNOW;
    cedarv_sub_format_e sub_format = CEDARV_SUB_FORMAT_UNKNOW;
    if (!to_cedarv(par.codec.data(), &format, &sub_format)) {
        std::clog << par.codec << " is not supported by CedarV" << std::endl;
        return false;
    }
    if (!dec_) {
        int ret;
        dec_ = libcedarv_init(&ret);
        if (ret < 0 || !dec_)
            return false;
    }

    cedarv_stream_info_t info{};
    info.format = format;
    info.sub_format = sub_format;
    info.video_width = par.width; //coded_width?
    info.video_height = par.height;
    if (par.extra.size() > 0) {
        info.init_data = (u8*)par.extra.data();
        info.init_data_len = par.extra.size();
    }
    CEDARX_ENSURE(dec_->set_vstream_info(dec_, &info), false);
    CEDARX_ENSURE(dec_->open(dec_), false);
    dec_->ioctrl(dec_, CEDARV_COMMAND_RESET, 0);
    dec_->ioctrl(dec_, CEDARV_COMMAND_PLAY, 0);
    return true;
}

bool CedarXVideoDecoder::close()
{
    if (!dec_)
        return true;
    dec_->ioctrl(dec_, CEDARV_COMMAND_STOP, 0);
    // FIXME: why crash?
    //dec_->close(dec_);
    return true;
}

bool CedarXVideoDecoder::decode(const Packet& pkt)
{
    if (pkt.buffer->size() <= 0) // TODO: EOS
        return true;
    //dec_->ioctrl(dec_, CEDARV_COMMAND_JUMP, 0);
    u32 bufsize0, bufsize1;
    u8 *buf0, *buf1;
    CEDARX_ENSURE(dec_->request_write(dec_, pkt.buffer->size(), &buf0, &bufsize0, &buf1, &bufsize1), false);
    memcpy(buf0, pkt.buffer->constData(), bufsize0);
    if ((u32)pkt.buffer->size() > bufsize0)
        memcpy(buf1, pkt.buffer->constData() + bufsize0, bufsize1);
    cedarv_stream_data_info_t info;
    info.type = 0; // TODO
    info.lengh = pkt.buffer->size();
    info.pts = pkt.pts * 1000.0;
    info.flags = CEDARV_FLAG_FIRST_PART | CEDARV_FLAG_LAST_PART | CEDARV_FLAG_PTS_VALID;
    CEDARX_ENSURE(dec_->update_data(dec_, &info), false);
    CEDARX_ENSURE(dec_->decode(dec_), false);
    cedarv_picture_t pic{};
    auto ret = dec_->display_request(dec_, &pic);
    if (ret > 3 || ret < 0) {
       std::clog << "CedarV: display_request failed: " <<  ret << std::endl;
       if (pic.id) {
           dec_->display_release(dec_, pic.id);
           pic.id = 0;
       }
       return false;
    }

#define FFALIGN(x, a) (((x)+(a)-1)&~((a)-1))
    pic.display_height = FFALIGN(pic.display_height, 8);
    const int display_h_align = FFALIGN(pic.display_height, 2); // already aligned to 8!
    const int display_w_align = FFALIGN(pic.display_width, 16);
    const int dst_y_stride = display_w_align;
    const int dst_c_stride = FFALIGN(pic.display_width/2, 16);

    const uint8_t *plane[2]{};
    int pitch[] = {
        dst_y_stride,
        dst_y_stride, // uv plane
    };
    VideoFrame frame(display_w_align, display_h_align, PixelFormat::NV12, pitch, plane);
    frame.setTimestamp(double(pic.pts)/1000.0);
    if (map_y_) {
        map_y_(pic.y, (void*)plane[0], display_w_align, pitch[0], display_h_align);
        map_y_(pic.u, (void*)plane[1], pitch[1], display_w_align, display_h_align/2);
    } else {
        memcpy((void*)plane[0], pic.y, pitch[0]*display_h_align);
        memcpy((void*)plane[1], pic.u, pitch[1]*display_h_align/2);
    }
    frameDecoded(frame);
    dec_->display_release(dec_, pic.id);

    /*
    const EGLint renderImageAttrs[] = { 
      EGL_IMAGE_PRESERVED_KHR, EGL_FALSE, 
      EGL_NONE
    };
    */
    return true;
}

void register_video_decoders_cedarx() {
    VideoDecoder::registerOnce("CedarX", []{return new CedarXVideoDecoder();});
}
namespace { // DCE
static const struct register_at_load_time_if_no_dce {
    inline register_at_load_time_if_no_dce() { register_video_decoders_cedarx();}
} s;
}
MDK_NS_END