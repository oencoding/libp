#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <android/log.h>
#include <cutils/properties.h>
#include "DTSHD_mediasource.h"

extern "C" int read_buffer(unsigned char *buffer,int size);

#define LOG_TAG "DTSHD_Medissource"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)


namespace android {
static int DTSDemuxMatchDTSSync2(uint32_t ui32TempSync, uint32_t ui32TempSync2)
{
      /* 16-bit bit core stream*/
      if( ui32TempSync == DTSDEMUX_SYNCWORD_CORE_16  || ui32TempSync == DTSDEMUX_SYNCWORD_CORE_14 ||
          ui32TempSync == DTSDEMUX_SYNCWORD_CORE_16M || ui32TempSync == DTSDEMUX_SYNCWORD_CORE_14M
     /*ui32TempSync == DTSDEMUX_SYNCWORD_SUBSTREAM|| ui32TempSync == DTSDEMUX_SYNCWORD_SUBSTREAM_M*/)
      {
            return 1;
      }

      if( (ui32TempSync & 0xffffff00) == (DTSDEMUX_SYNCWORD_CORE_24 & 0xffffff00)&&
          ((ui32TempSync2 >> 16) & 0xFF)== (DTSDEMUX_SYNCWORD_CORE_24 & 0xFF))
      {
          return 1;
      }
      return 0;
}
static int Dtshd_Estimate_Frame_size( unsigned char *buf,int size,int *syncpos)
{
     int i32Index=0;
     int result=0;
     unsigned int ui32Sync_word=0,ui32Sync_word2=0;
     int first_sync_Detected=0,first_sync_pos=-1;
     int frame_size=0;
     *syncpos=0;
     for(i32Index=0; i32Index+7<size;i32Index++)
     {
         ui32Sync_word    = buf[i32Index];      ui32Sync_word <<= 8;
         ui32Sync_word   |= buf[i32Index + 1];  ui32Sync_word <<= 8;
         ui32Sync_word   |= buf[i32Index + 2];  ui32Sync_word <<= 8;
         ui32Sync_word   |= buf[i32Index + 3];

         ui32Sync_word2   = buf[i32Index + 4];  ui32Sync_word2 <<= 8;
         ui32Sync_word2  |= buf[i32Index + 5];  ui32Sync_word2 <<= 8;
         ui32Sync_word2  |= buf[i32Index + 6];  ui32Sync_word2 <<= 8;
         ui32Sync_word2  |= buf[i32Index + 7];
            
         result=DTSDemuxMatchDTSSync2(ui32Sync_word,ui32Sync_word2);
        
         if(result){
            ALOGI("SyncWord detect: ui32Sync_word/0x%x ui32Sync_word2/0x%x ",ui32Sync_word,ui32Sync_word2);
            if(first_sync_Detected==0){
                first_sync_Detected=1;
                first_sync_pos=i32Index;
                ALOGI("first_sync_pos/%d ",first_sync_pos);
            }else if(first_sync_Detected==1){
                frame_size=i32Index-first_sync_pos;
                ALOGI("FrameSize detect: %d/bytes",frame_size);
                break;
            }
         }
     }
   
     if(frame_size==0)
     {
         ALOGE("[%s %d ]FrameSize detect Falied ",__FUNCTION__,__LINE__);
     }else{
         frame_size += 4;
     }
     *syncpos=first_sync_pos;
     return frame_size;
}



Dtshd_MediaSource::Dtshd_MediaSource(void *read_buffer) 
{   
    ALOGI("%s %d \n",__FUNCTION__,__LINE__);
    mStarted=false;
    mMeta=new MetaData;
    mDataSource=NULL;
    mGroup=NULL;
    mBytesReaded=0;
    mCurrentTimeUs=0;
    pStop_ReadBuf_Flag=NULL;
    fpread_buffer=(fp_read_buffer)read_buffer;
    sample_rate=0;
    ChNum=0;
    frame_size=0;
    bytes_readed_sum_pre=0;
    bytes_readed_sum=0;
    FrameSizeDetectFlag=0;
    FirFraBuf=NULL;
    FirFraBuf_Len=0;
    FirFraBuf_Offset=0;
    FrameNumReaded=0;
}


Dtshd_MediaSource::~Dtshd_MediaSource() 
{
    ALOGI("%s %d \n",__FUNCTION__,__LINE__);
    if(FirFraBuf)
        free(FirFraBuf);
    if (mStarted) {
        stop();
    }
}


int Dtshd_MediaSource::GetSampleRate()
{   
    return sample_rate;
}

int Dtshd_MediaSource::GetChNum()
{
    return ChNum;
}

int* Dtshd_MediaSource::Get_pStop_ReadBuf_Flag()
{
    return pStop_ReadBuf_Flag;
}

int Dtshd_MediaSource::Set_pStop_ReadBuf_Flag(int *pStop)
{
    pStop_ReadBuf_Flag = pStop;
    return 0;
}

int Dtshd_MediaSource::GetReadedBytes()
{   
    return frame_size;
}

sp<MetaData> Dtshd_MediaSource::getFormat() 
{
    ALOGI("%s %d \n",__FUNCTION__,__LINE__);
    return mMeta;
}

status_t  Dtshd_MediaSource::start(MetaData *params)
{
    ALOGI("%s %d \n",__FUNCTION__,__LINE__);
    mGroup = new MediaBufferGroup;
    mGroup->add_buffer(new MediaBuffer(DTSHD_INPUT_DATA_LEN_PTIME));
    mStarted = true;
    return OK; 
}

status_t Dtshd_MediaSource::stop()
{
    ALOGI("%s %d \n",__FUNCTION__,__LINE__);
    delete mGroup;
    mGroup = NULL;
    mStarted = false;
    return OK;
}


int Dtshd_MediaSource::MediaSourceRead_buffer(unsigned char *buffer,int size)
{
     int readcnt=0;
     int readsum=0;
     if(fpread_buffer!=NULL)
     {  
         int sleep_time=0;
         while((readsum<size)&& (*pStop_ReadBuf_Flag==0))
         {
             readcnt=fpread_buffer(buffer+readsum,size-readsum);
             if(readcnt<(size-readsum)){
                 sleep_time++;
                 usleep(10000);
             }
             readsum+=readcnt;
             if((sleep_time>0) && (sleep_time%100==0) ){ //wait for max 10s to get audio data
                 ALOGE("[%s] Can't get data from audiobuffer,wait for %d ms\n ", __FUNCTION__,sleep_time*10);
             }
         }
         bytes_readed_sum +=readsum;
         if(*pStop_ReadBuf_Flag==1)
         {
            ALOGI("[%s] End of Stream: *pStop_ReadBuf_Flag==1\n ", __FUNCTION__);
         }
         return readsum;
     }else{
         ALOGE("[%s]ERR: fpread_buffer=NULL\n ", __FUNCTION__);
         return 0;
     }
}


status_t Dtshd_MediaSource::read(MediaBuffer **out, const ReadOptions *options)
{
     
    *out = NULL;
     int read_bytes_per_time;
     MediaBuffer *buffer;
     int byte_readed=0;
     status_t err;
     if(!FrameSizeDetectFlag)
     {   
         FirFraBuf=(unsigned char*)malloc(DTSHD_INPUT_DATA_LEN_PTIME);
         if(FirFraBuf==NULL){    
              ALOGE("[%s %d] mallco memory for <FirFraBuf> failed!\n",__FUNCTION__,__LINE__);
              return ERROR_END_OF_STREAM;
         }
         FirFraBuf_Len=DTSHD_INPUT_DATA_LEN_PTIME;
            
         if (MediaSourceRead_buffer(FirFraBuf,FirFraBuf_Len) != FirFraBuf_Len)
         {
            ALOGE("[%s %d] Stream Lengh <%d> Err!\n",__FUNCTION__,__LINE__,FirFraBuf_Len);
            return ERROR_END_OF_STREAM;
         }
        
         frame_size= Dtshd_Estimate_Frame_size(FirFraBuf,FirFraBuf_Len,&FirFraBuf_Offset);
         if(frame_size==0)
         {
            ALOGE("[%s %d] Detect Frame_size Err!\n",__FUNCTION__,__LINE__);
            return ERROR_END_OF_STREAM;
         }
         FrameSizeDetectFlag=1;
     }
     
     err = mGroup->acquire_buffer(&buffer);
     if (err != OK) {
         ALOGE("[%s %d] mGroup->acquire_buffer ERR!\n",__FUNCTION__,__LINE__);
         return err;
     }
     
     if(FrameNumReaded==0 && FirFraBuf_Offset>0)
     {   
          int pre_suspend_bytes=0;
          int redunt=FirFraBuf_Offset%4;
          if(redunt)
              pre_suspend_bytes=4-redunt;
          unsigned char zarray[32]={0};
          memcpy((unsigned char*)(buffer->data()),FirFraBuf,FirFraBuf_Offset);
          memcpy((unsigned char*)(buffer->data())+FirFraBuf_Offset, zarray, pre_suspend_bytes);
          memcpy((unsigned char*)(buffer->data())+FirFraBuf_Offset+pre_suspend_bytes,FirFraBuf+FirFraBuf_Offset, frame_size);
           
          memcpy(zarray,FirFraBuf,32);
          ALOGI("[%s %d]  pre_suspend_bytes/%d  %s \n",__FUNCTION__,__LINE__,pre_suspend_bytes,zarray);
           
          buffer->set_range(0, pre_suspend_bytes+FirFraBuf_Offset+frame_size);
          buffer->meta_data()->setInt64(kKeyTime, mCurrentTimeUs);
          buffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
          *out = buffer;
          FirFraBuf_Offset+=frame_size;
          FrameNumReaded++;
          return OK;
     }
     
     if(FirFraBuf_Offset<FirFraBuf_Len)
     {
         if(FirFraBuf_Len-FirFraBuf_Offset>=frame_size){
            byte_readed=frame_size;
         }else{
            byte_readed=(FirFraBuf_Len-FirFraBuf_Offset);
         }
         memcpy((unsigned char*)(buffer->data()),FirFraBuf+FirFraBuf_Offset,byte_readed);
         FirFraBuf_Offset+=byte_readed;
     }
         
     if (MediaSourceRead_buffer((unsigned char*)(buffer->data())+byte_readed,frame_size-byte_readed) != (frame_size-byte_readed))
     {
         buffer->release();
         buffer = NULL;
         return ERROR_END_OF_STREAM;
     }
         
     buffer->set_range(0,frame_size);
     buffer->meta_data()->setInt64(kKeyTime, mCurrentTimeUs);
     buffer->meta_data()->setInt32(kKeyIsSyncFrame, 1);
     *out = buffer;
     FrameNumReaded++;
     return OK;
}


}  // namespace android


