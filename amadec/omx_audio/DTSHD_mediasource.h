#ifndef MEDIA_DTSHD_MEDIASOURCE_H_
#define MEDIA_DTSHD_MEDIASOURCE_H_

#include  "MediaSource.h"
#include  "DataSource.h"
#include  "MediaBufferGroup.h"
#include  "MetaData.h"
#include  "audio_mediasource.h"

namespace android {

#define DTSHD_INPUT_DATA_LEN_PTIME               (32*1024)
#define DTSHD_OMX_DECODER_NUMBUF    2
#define DTSDEMUX_SYNCWORD_CORE_16M          0x7ffe8001
#define DTSDEMUX_SYNCWORD_CORE_14M          0x1fffe800
#define DTSDEMUX_SYNCWORD_CORE_24M          0xfe80007f    
#define DTSDEMUX_SYNCWORD_CORE_16             0xfe7f0180
#define DTSDEMUX_SYNCWORD_CORE_14             0xff1f00e8
#define DTSDEMUX_SYNCWORD_CORE_24             0x80fe7f01
#define DTSDEMUX_SYNCWORD_SUBSTREAM_M    0x64582025
#define DTSDEMUX_SYNCWORD_SUBSTREAM         0x58642520

typedef int (*fp_read_buffer)(unsigned char *,int);
class Dtshd_MediaSource : public AudioMediaSource 
{
public:
	Dtshd_MediaSource(void *read_buffer);
	
    status_t start(MetaData *params = NULL);
    status_t stop();
    sp<MetaData> getFormat();
    status_t read(MediaBuffer **buffer, const ReadOptions *options = NULL);
	
	int  GetReadedBytes();
	int GetSampleRate();
	int GetChNum();
	int* Get_pStop_ReadBuf_Flag();
	int Set_pStop_ReadBuf_Flag(int *pStop);
	int MediaSourceRead_buffer(unsigned char *buffer,int size);
	
	fp_read_buffer fpread_buffer;
	int sample_rate;
	int ChNum;
	int frame_size;
	int FrameSizeDetectFlag;
	unsigned char * FirFraBuf;
	int FirFraBuf_Len;
	int FirFraBuf_Offset;
	int FrameNumReaded;
        int *pStop_ReadBuf_Flag;
	int64_t bytes_readed_sum_pre;
	int64_t bytes_readed_sum;
protected:
    virtual ~Dtshd_MediaSource();

private:
	bool mStarted;
    sp<DataSource> mDataSource;
    sp<MetaData>   mMeta;
    MediaBufferGroup *mGroup;	
    int64_t mCurrentTimeUs;	
    int     mBytesReaded;
    int     block_align;
    Dtshd_MediaSource(const Dtshd_MediaSource &);
    Dtshd_MediaSource &operator=(const Dtshd_MediaSource &);
};


}

#endif

