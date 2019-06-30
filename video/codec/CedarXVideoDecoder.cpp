
/*
 * Copyright (c) 2018-2019 WangBin <wbsecg1 at gmail.com>
 * Original code is from QtAV project
 */
#include "mdk/VideoDecoder.h"
#include "mdk/MediaInfo.h"
#include "mdk/Packet.h"
#include "mdk/VideoFrame.h"
#include <iostream>
extern "C" {
#include <libcedarv/libcedarv.h>
}
// TODO: libcedarv allocate memory by ump, and add picture ump flag
MDK_NS_BEGIN
using namespace std;
class CedarXVideoDecoder final : public VideoDecoder
{
public:
    const char* name() const override {return "CedarX";}
    bool open() override;
    bool close() override;
    bool flush() override {
        onFlush();
        return true;
    }
    bool decode(const Packet& pkt) override;
private:
    shared_ptr<CEDARV_DECODER> dec_;
    NativeVideoBufferPoolRef pool_ = NativeVideoBufferPool::create("CedarV"); // GLVA.CedarV
};

#define CEDARX_ENSURE(f, ...) CEDARX_CHECK(f, return __VA_ARGS__;)
#define CEDARX_WARN(f) CEDARX_CHECK(f)
#define CEDARX_CHECK(f, ...)  do { \
        int __cedar_ret__ = f; \
        if (__cedar_ret__ < 0) { \
            std::clog << #f "  ERROR@" << __LINE__ << __FUNCTION__ << ": (" << __cedar_ret__ << ") " << std::endl; \
            __VA_ARGS__ \
        } \
    } while (false)

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
        int ret = 0;
        auto dec = libcedarv_init(&ret);
        if (ret < 0 || !dec)
            return false;
        dec_.reset(dec, [](CEDARV_DECODER* dec){
            dec->close(dec); // does not affect picture release?
            libcedarv_exit(dec);
        });
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
    CEDARX_ENSURE(dec_->set_vstream_info(dec_.get(), &info), false);
    CEDARX_ENSURE(dec_->open(dec_.get()), false);
    dec_->ioctrl(dec_.get(), CEDARV_COMMAND_RESET, 0);
    dec_->ioctrl(dec_.get(), CEDARV_COMMAND_PLAY, 0);
    onOpen();
    return true;
}

bool CedarXVideoDecoder::close()
{
    if (!dec_)
        return true;
    dec_->ioctrl(dec_.get(), CEDARV_COMMAND_STOP, 0);
    dec_.reset();
    onClose();
    return true;
}

bool CedarXVideoDecoder::decode(const Packet& pkt)
{
    if (pkt.isEnd())
        return false;
    if (pkt.buffer->size() <= 0) // TODO: EOS
        return true;
    //dec_->ioctrl(dec_.get(), CEDARV_COMMAND_JUMP, 0);
    u32 bufsize0, bufsize1;
    u8 *buf0, *buf1;
    CEDARX_ENSURE(dec_->request_write(dec_.get(), pkt.buffer->size(), &buf0, &bufsize0, &buf1, &bufsize1), false);
    memcpy(buf0, pkt.buffer->constData(), bufsize0);
    if ((u32)pkt.buffer->size() > bufsize0)
        memcpy(buf1, pkt.buffer->constData() + bufsize0, bufsize1);
    cedarv_stream_data_info_t info;
    info.type = 0; // TODO
    info.lengh = pkt.buffer->size();
    info.pts = pkt.pts * TimeScaleForInt;
    info.flags = CEDARV_FLAG_FIRST_PART | CEDARV_FLAG_LAST_PART | CEDARV_FLAG_PTS_VALID;
    CEDARX_ENSURE(dec_->update_data(dec_.get(), &info), false);
    CEDARX_ENSURE(dec_->decode(dec_.get()), false);
    cedarv_picture_t *pic = new cedarv_picture_t();
    auto ret = dec_->display_request(dec_.get(), pic);
    if (ret > 3 || ret < 0) {
        std::clog << "CedarV: display_request failed: " <<  ret << ", picture id: " << pic->id << std::endl;
        delete pic;
        return false;
    }
    //std::clog << "cedarv_picture_t.id: " << pic->id<< std::endl;
    auto buf = pool_->getBuffer(pic, [pic, this]{ // TODO: shared_ptr<dec_>
        dec_->display_release(dec_.get(), pic->id);
        delete pic;
    });
    VideoFrame frame(pic->display_width, pic->display_height, PixelFormat::NV12, buf); // TODO: can be mapped as yuv420p, rgb24
    frame.setTimestamp(double(pic->pts)/TimeScaleForInt);
    frameDecoded(frame);
    return !pkt.isEnd();
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