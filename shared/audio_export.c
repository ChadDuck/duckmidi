#include "audio_export.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

struct DmAudioEncoder {
    DmOutput *output;
    AVFormatContext *format;
    AVCodecContext *codec;
    AVAudioFifo *fifo;
    AVIOContext *io;
    SwrContext *convert;
    AVStream *stream;
    int64_t samples;
    char *error;
    size_t cap;
};
DmAudioOptions dm_audio_options_default(void) { return (DmAudioOptions){DM_AUDIO_WAV,192,3,16,5,false}; }
const char *dm_audio_codec_name(DmAudioCodec codec) {
    static const char *names[]={"WAV / PCM","MP3","AAC (M4A)","Ogg Vorbis","Opus","FLAC"};
    return codec>=0&&codec<DM_AUDIO_CODECS?names[codec]:"Unknown codec";
}
const char *dm_audio_codec_extension(DmAudioCodec codec) {
    static const char *names[]={"wav","mp3","m4a","ogg","opus","flac"};
    return codec>=0&&codec<DM_AUDIO_CODECS?names[codec]:"";
}
bool dm_audio_options_valid(const DmAudioOptions *o) {
    if(!o||o->codec<0||o->codec>=DM_AUDIO_CODECS)return false;
    if(o->bits!=16&&o->bits!=24)return false;
    if(o->codec==DM_AUDIO_FLAC)return o->compression>=0&&o->compression<=12;
    if(o->codec==DM_AUDIO_WAV)return true;
    if(o->bitrate<32||o->bitrate>320)return false;
    if(!o->variable||o->codec==DM_AUDIO_OPUS)return true;
    int limit=o->codec==DM_AUDIO_MP3?9:o->codec==DM_AUDIO_AAC?5:10;
    return o->quality>=(o->codec==DM_AUDIO_AAC?1:0)&&o->quality<=limit;
}
static bool failure(DmAudioEncoder *e,int code,const char *operation) {
    if(!*e->error){char detail[AV_ERROR_MAX_STRING_SIZE];av_strerror(code,detail,sizeof(detail));snprintf(e->error,e->cap,"%s: %s",operation,detail);}
    return false;
}
static int write_bytes(void *user,const uint8_t *bytes,int size) {
    DmAudioEncoder *e=user;
    return dm_output_write(e->output,bytes,(size_t)size,e->error,e->cap)?size:AVERROR(EIO);
}
static int64_t seek_bytes(void *user,int64_t offset,int whence) {
    DmAudioEncoder *e=user;FILE *file=e->output->file;
#ifdef _WIN32
#define DM_SEEK _fseeki64
#define DM_TELL _ftelli64
#else
#define DM_SEEK fseeko
#define DM_TELL ftello
#endif
    if(whence==AVSEEK_SIZE) {
        int64_t old=DM_TELL(file);
        if(old<0||DM_SEEK(file,0,SEEK_END))return AVERROR(EIO);
        int64_t size=DM_TELL(file);
        if(DM_SEEK(file,old,SEEK_SET))return AVERROR(EIO);
        return size;
    }
    if(DM_SEEK(file,offset,whence&~AVSEEK_FORCE))return AVERROR(EIO);
    return DM_TELL(file);
}
static bool packets(DmAudioEncoder *e) {
    AVPacket *packet=av_packet_alloc();if(!packet)return failure(e,AVERROR(ENOMEM),"Allocate audio packet");
    bool ok=true;int code;
    while((code=avcodec_receive_packet(e->codec,packet))>=0) {
        av_packet_rescale_ts(packet,e->codec->time_base,e->stream->time_base);packet->stream_index=e->stream->index;
        code=av_interleaved_write_frame(e->format,packet);av_packet_unref(packet);
        if(code<0){ok=failure(e,code,"Write encoded audio");break;}
    }
    if(ok&&code!=AVERROR(EAGAIN)&&code!=AVERROR_EOF)ok=failure(e,code,"Encode audio packet");
    av_packet_free(&packet);return ok;
}
static bool frame(DmAudioEncoder *e,int count) {
    AVFrame *f=av_frame_alloc();if(!f)return failure(e,AVERROR(ENOMEM),"Allocate audio frame");
    int padded=count;
    if(e->codec->frame_size&&!(e->codec->codec->capabilities&AV_CODEC_CAP_SMALL_LAST_FRAME))padded=e->codec->frame_size;
    f->format=e->codec->sample_fmt;f->sample_rate=48000;f->nb_samples=padded;
    int code=av_channel_layout_copy(&f->ch_layout,&e->codec->ch_layout);
    if(code>=0)code=av_frame_get_buffer(f,0);
    if(code>=0&&av_audio_fifo_read(e->fifo,(void**)f->extended_data,count)!=count)code=AVERROR(EIO);
    if(code>=0&&padded>count)code=av_samples_set_silence(f->extended_data,count,padded-count,2,e->codec->sample_fmt);
    f->pts=e->samples;e->samples+=padded;
    if(code>=0)code=avcodec_send_frame(e->codec,f);
    av_frame_free(&f);
    return code<0?failure(e,code,"Encode audio frame"):packets(e);
}
DmAudioEncoder *dm_audio_encoder_begin(DmOutput *output,const DmAudioOptions *o,char *error,size_t cap) {
    if(!dm_audio_options_valid(o)){snprintf(error,cap,"Invalid audio export settings.");return NULL;}
    DmAudioEncoder *e=calloc(1,sizeof(*e));if(!e){snprintf(error,cap,"Cannot allocate audio encoder.");return NULL;}
    e->output=output;e->error=error;e->cap=cap;
    const char *encoders[]={o->bits==24?"pcm_s24le":"pcm_s16le","libmp3lame","aac","libvorbis","libopus","flac"};
    const char *containers[]={"wav","mp3","ipod","ogg","opus","flac"};
    const AVCodec *codec=avcodec_find_encoder_by_name(encoders[o->codec]);
    if(!codec){snprintf(error,cap,"The %s encoder is unavailable in the installed audio libraries.",dm_audio_codec_name(o->codec));goto fail;}
    int code=avformat_alloc_output_context2(&e->format,NULL,containers[o->codec],NULL);
    if(code<0){failure(e,code,"Create audio container");goto fail;}
    e->codec=avcodec_alloc_context3(codec);if(!e->codec){failure(e,AVERROR(ENOMEM),"Allocate audio encoder");goto fail;}
    e->codec->sample_rate=48000;e->codec->time_base=(AVRational){1,48000};
    av_channel_layout_default(&e->codec->ch_layout,2);
    const enum AVSampleFormat *formats=NULL;int format_count=0;
    code=avcodec_get_supported_config(e->codec,codec,AV_CODEC_CONFIG_SAMPLE_FORMAT,0,(const void**)&formats,&format_count);
    if(code<0||!format_count){failure(e,code<0?code:AVERROR(EINVAL),"Read encoder sample format");goto fail;}
    enum AVSampleFormat preferred=(o->codec==DM_AUDIO_WAV||o->codec==DM_AUDIO_FLAC)?(o->bits==24?AV_SAMPLE_FMT_S32:AV_SAMPLE_FMT_S16):AV_SAMPLE_FMT_FLTP;
    e->codec->sample_fmt=formats[0];for(int i=0;i<format_count;i++)if(formats[i]==preferred)e->codec->sample_fmt=preferred;
    e->codec->bits_per_raw_sample=o->bits;
    if(e->format->oformat->flags&AVFMT_GLOBALHEADER)e->codec->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary *settings=NULL;
    if(o->codec==DM_AUDIO_FLAC)e->codec->compression_level=o->compression;
    else if(o->codec!=DM_AUDIO_WAV) {
        e->codec->bit_rate=o->bitrate*1000;
        if(o->codec==DM_AUDIO_OPUS)av_dict_set(&settings,"vbr",o->variable?"on":"off",0);
        else if(o->variable){e->codec->flags|=AV_CODEC_FLAG_QSCALE;e->codec->global_quality=o->quality*FF_QP2LAMBDA;}
        else if(o->codec==DM_AUDIO_VORBIS)e->codec->rc_min_rate=e->codec->rc_max_rate=e->codec->bit_rate;
    }
    code=avcodec_open2(e->codec,codec,&settings);av_dict_free(&settings);
    if(code<0){failure(e,code,"Open audio encoder");goto fail;}
    e->stream=avformat_new_stream(e->format,NULL);
    if(!e->stream){failure(e,AVERROR(ENOMEM),"Allocate audio stream");goto fail;}
    e->stream->time_base=e->codec->time_base;
    code=avcodec_parameters_from_context(e->stream->codecpar,e->codec);
    if(code<0){failure(e,code,"Set audio stream parameters");goto fail;}
    unsigned char *buffer=av_malloc(32768);
    if(!buffer){failure(e,AVERROR(ENOMEM),"Allocate audio output buffer");goto fail;}
    e->io=avio_alloc_context(buffer,32768,1,e,NULL,write_bytes,seek_bytes);
    if(!e->io){av_free(buffer);failure(e,AVERROR(ENOMEM),"Allocate audio writer");goto fail;}
    e->format->pb=e->io;e->format->flags|=AVFMT_FLAG_CUSTOM_IO;
    av_dict_set(&e->format->metadata,"encoder","duckmidi",0);
    code=avformat_write_header(e->format,NULL);
    if(code<0){failure(e,code,"Write audio header");goto fail;}
    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
    code=swr_alloc_set_opts2(&e->convert,&e->codec->ch_layout,e->codec->sample_fmt,48000,&stereo,AV_SAMPLE_FMT_FLT,48000,0,NULL);
    if(code>=0)code=swr_init(e->convert);
    if(code<0){failure(e,code,"Prepare audio conversion");goto fail;}
    e->fifo=av_audio_fifo_alloc(e->codec->sample_fmt,2,2048);
    if(!e->fifo){failure(e,AVERROR(ENOMEM),"Allocate audio queue");goto fail;}
    return e;
fail:
    dm_audio_encoder_finish(e,false);return NULL;
}
bool dm_audio_encoder_write(DmAudioEncoder *e,const float *stereo,int count) {
    AVFrame *f=av_frame_alloc();if(!f)return failure(e,AVERROR(ENOMEM),"Allocate conversion frame");
    f->format=e->codec->sample_fmt;f->sample_rate=48000;f->nb_samples=count;
    int code=av_channel_layout_copy(&f->ch_layout,&e->codec->ch_layout);
    if(code>=0)code=av_frame_get_buffer(f,0);
    const uint8_t *input=(const uint8_t*)stereo;
    if(code>=0)code=swr_convert(e->convert,f->extended_data,count,&input,count);
    if(code>=0&&av_audio_fifo_write(e->fifo,(void**)f->extended_data,code)!=code)code=AVERROR(ENOMEM);
    av_frame_free(&f);
    if(code<0)return failure(e,code,"Convert audio samples");
    int block=e->codec->frame_size?e->codec->frame_size:2048;
    while(av_audio_fifo_size(e->fifo)>=block)if(!frame(e,block))return false;
    return true;
}
bool dm_audio_encoder_finish(DmAudioEncoder *e,bool commit) {
    if(!e)return !commit;
    bool ok=commit;
    if(ok&&av_audio_fifo_size(e->fifo))ok=frame(e,av_audio_fifo_size(e->fifo));
    if(ok){int code=avcodec_send_frame(e->codec,NULL);ok=code<0?failure(e,code,"Flush audio encoder"):packets(e);}
    if(ok){int code=av_write_trailer(e->format);if(code<0)ok=failure(e,code,"Finish audio container");}
    if(e->io){avio_flush(e->io);if(ok&&e->io->error<0)ok=failure(e,e->io->error,"Flush audio output");av_freep(&e->io->buffer);avio_context_free(&e->io);}
    if(e->fifo)av_audio_fifo_free(e->fifo);
    swr_free(&e->convert);avcodec_free_context(&e->codec);avformat_free_context(e->format);free(e);return ok;
}
