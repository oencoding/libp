/*
 * Buffered I/O for ffmpeg system
 * Copyright (c) 2000,2001 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "avio.h"
#include <stdarg.h>

#define IO_BUFFER_SIZE 32768



static void fill_buffer(ByteIOContext *s);
#if LIBAVFORMAT_VERSION_MAJOR >= 53
static int url_resetbuf(ByteIOContext *s, int flags);
#endif

int init_put_byte(ByteIOContext *s,
                  unsigned char *buffer,
                  int buffer_size,
                  int write_flag,
                  void *opaque,
                  int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int64_t (*seek)(void *opaque, int64_t offset, int whence))
{
    s->buffer = buffer;
    s->buffer_size = buffer_size;
    s->buf_ptr = buffer;
    s->opaque = opaque;
    url_resetbuf(s, write_flag ? URL_WRONLY : URL_RDONLY);
    s->write_packet = write_packet;
    s->read_packet = read_packet;
    s->seek = seek;
    s->pos = 0;
    s->must_flush = 0;
    s->eof_reached = 0;
    s->error = 0;
    s->is_streamed = 0;
    s->max_packet_size = 0;
    s->update_checksum= NULL;
    if(!read_packet && !write_flag){
        s->pos = buffer_size;
        s->buf_end = s->buffer + buffer_size;
    }
    s->read_pause = NULL;
    s->read_seek  = NULL;
    return 0;
}

ByteIOContext *av_alloc_put_byte(
                  unsigned char *buffer,
                  int buffer_size,
                  int write_flag,
                  void *opaque,
                  int (*read_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int (*write_packet)(void *opaque, uint8_t *buf, int buf_size),
                  int64_t (*seek)(void *opaque, int64_t offset, int whence))
{
    ByteIOContext *s = av_mallocz(sizeof(ByteIOContext));
    init_put_byte(s, buffer, buffer_size, write_flag, opaque,
                  read_packet, write_packet, seek);
    return s;
}

static void flush_buffer(ByteIOContext *s)
{
    if (s->buf_ptr > s->buffer) {
        if (s->write_packet && !s->error){
            int ret= s->write_packet(s->opaque, s->buffer, s->buf_ptr - s->buffer);
            if(ret < 0){
                s->error = ret;
            }
        }
        if(s->update_checksum){
            s->checksum= s->update_checksum(s->checksum, s->checksum_ptr, s->buf_ptr - s->checksum_ptr);
            s->checksum_ptr= s->buffer;
        }
        s->pos += s->buf_ptr - s->buffer;
    }
    s->buf_ptr = s->buffer;
}

void put_byte(ByteIOContext *s, int b)
{
    *(s->buf_ptr)++ = b;
    if (s->buf_ptr >= s->buf_end)
        flush_buffer(s);
}

void put_buffer(ByteIOContext *s, const unsigned char *buf, int size)
{
    while (size > 0) {
        int len = FFMIN(s->buf_end - s->buf_ptr, size);
        memcpy(s->buf_ptr, buf, len);
        s->buf_ptr += len;

        if (s->buf_ptr >= s->buf_end)
            flush_buffer(s);

        buf += len;
        size -= len;
    }
}

void put_flush_packet(ByteIOContext *s)
{
    flush_buffer(s);
    s->must_flush = 0;
}

int64_t url_fseek(ByteIOContext *s, int64_t offset, int whence)
{
    int64_t offset1;
    int64_t pos;

    if(!s)
        return AVERROR(EINVAL);

    pos = s->pos - (s->write_flag ? 0 : (s->buf_end - s->buffer));
	if(whence == AVSEEK_TO_TIME ){
		if(s->seek){
	 		if((offset1=s->seek(s->opaque, offset, AVSEEK_TO_TIME))>=0)
	 		{
	 			if (!s->write_flag)
	            	s->buf_end = s->buffer;
		        s->buf_ptr = s->buffer;
		        s->pos = 0;/*I think it is the first,pos now*/
				s->eof_reached=0;/*clear eof error*/
				return offset1;
	 		}
	 	}
		return AVERROR(EPIPE);
	}
    if (whence != SEEK_CUR && whence != SEEK_SET)
        return AVERROR(EINVAL);

    if (whence == SEEK_CUR) {
        offset1 = pos + (s->buf_ptr - s->buffer);
        if (offset == 0)
            return offset1;
        offset += offset1;
    }
    offset1 = offset - pos;
	//av_log(NULL, AV_LOG_INFO, "url_fseek:::  offset=%lld,pos=%lld,offset1=%lld,buffer_size=%x\n",
	//		offset,pos,offset1,s->buffer_size);
    if (!s->must_flush &&
        offset1 >= 0 && offset1 <= (s->buf_end - s->buffer)) {
        /* can do the seek inside the buffer */
        s->buf_ptr = s->buffer + offset1;
	}else if((s->is_streamed) && !s->write_flag &&
              offset1 >= 0 && offset1 <(s->buf_end - s->buffer)+(1<<16)){
        while(s->pos < offset && !s->eof_reached && !s->error)
            fill_buffer(s);
        if (s->eof_reached)
        {
        	av_log(NULL, AV_LOG_INFO, "url_fseek:reached eof_reached=%d\n",s->eof_reached);
            return AVERROR(EPIPE);
        }
        s->buf_ptr = s->buf_end + offset - s->pos;
    } else {
        int64_t res = AVERROR(EPIPE);
#if CONFIG_MUXERS || CONFIG_NETWORK
        if (s->write_flag) {
            flush_buffer(s);
            s->must_flush = 1;
        }
#endif /* CONFIG_MUXERS || CONFIG_NETWORK */
        if (!s->seek || (res = s->seek(s->opaque, offset, SEEK_SET)) < 0)
            return res;
        if (!s->write_flag)
            s->buf_end = s->buffer;
        s->buf_ptr = s->buffer;
        s->pos = offset;
    }
    s->eof_reached = 0;
    return offset;
}

void url_fskip(ByteIOContext *s, int64_t offset)
{
    url_fseek(s, offset, SEEK_CUR);
}

int64_t url_ftell(ByteIOContext *s)
{
    return url_fseek(s, 0, SEEK_CUR);
}
int64_t url_ffulltime(ByteIOContext *s)
{
	int64_t size;
	if (!s->seek)
        return AVERROR(EPIPE);
	size = s->seek(s->opaque, 0, AVSEEK_FULLTIME);
	return size;
}

int64_t url_fsize(ByteIOContext *s)
{
    int64_t size;

    if(!s)
        return AVERROR(EINVAL);
	if(s->file_size > 0)
		return s->file_size;
    if (!s->seek)
        return AVERROR(EPIPE);
    size = s->seek(s->opaque, 0, AVSEEK_SIZE);
	av_log(NULL, AV_LOG_INFO, "[%s:%d]size=0x%llx (%lld)\n",__FUNCTION__,__LINE__,size,size);
    if(size<0){
        if ((size = s->seek(s->opaque, -1, SEEK_END)) < 0)
        {
        	if(s->file_size == 0)
				s->file_size = size;
        	return size;
        }
        size++;
        s->seek(s->opaque, s->pos, SEEK_SET);
    }
	if(s->file_size == 0)
		s->file_size = size;
    return size;
}

int url_feof(ByteIOContext *s)
{
    if(!s)
        return 0;
    return s->eof_reached;
}

int url_ferror(ByteIOContext *s)
{
    if(!s)
        return 0;
    return s->error;
}

void put_le32(ByteIOContext *s, unsigned int val)
{
    put_byte(s, val);
    put_byte(s, val >> 8);
    put_byte(s, val >> 16);
    put_byte(s, val >> 24);
}

void put_be32(ByteIOContext *s, unsigned int val)
{
    put_byte(s, val >> 24);
    put_byte(s, val >> 16);
    put_byte(s, val >> 8);
    put_byte(s, val);
}

void put_strz(ByteIOContext *s, const char *str)
{
    if (str)
        put_buffer(s, (const unsigned char *) str, strlen(str) + 1);
    else
        put_byte(s, 0);
}

void put_le64(ByteIOContext *s, uint64_t val)
{
    put_le32(s, (uint32_t)(val & 0xffffffff));
    put_le32(s, (uint32_t)(val >> 32));
}

void put_be64(ByteIOContext *s, uint64_t val)
{
    put_be32(s, (uint32_t)(val >> 32));
    put_be32(s, (uint32_t)(val & 0xffffffff));
}

void put_le16(ByteIOContext *s, unsigned int val)
{
    put_byte(s, val);
    put_byte(s, val >> 8);
}

void put_be16(ByteIOContext *s, unsigned int val)
{
    put_byte(s, val >> 8);
    put_byte(s, val);
}

void put_le24(ByteIOContext *s, unsigned int val)
{
    put_le16(s, val & 0xffff);
    put_byte(s, val >> 16);
}

void put_be24(ByteIOContext *s, unsigned int val)
{
    put_be16(s, val >> 8);
    put_byte(s, val);
}

void put_tag(ByteIOContext *s, const char *tag)
{
    while (*tag) {
        put_byte(s, *tag++);
    }
}

/* Input stream */

static void fill_buffer(ByteIOContext *s)
{
    uint8_t *dst=NULL;
	 int len=0; 

	 if(!s->max_packet_size && (s->buf_end - s->buffer < s->buffer_size) )
	 {
		dst=s->buf_ptr;
	 }
	 else
	 {
	 	dst=s->buffer;
	 }
    len=s->buffer_size - (dst - s->buffer);
    
    assert(s->buf_ptr == s->buf_end);    
    /* no need to do anything if EOF already reached */
    if (s->eof_reached)
        return;

    if(s->update_checksum && dst == s->buffer){
        if(s->buf_end > s->checksum_ptr)
            s->checksum= s->update_checksum(s->checksum, s->checksum_ptr, s->buf_end - s->checksum_ptr);
        s->checksum_ptr= s->buffer;
    }    
    if(s->read_packet)
        len = s->read_packet(s->opaque, dst, len);
    else
        len = 0;
    if (len <= 0) {
        /* do not modify buffer if EOF reached so that a seek back can
           be done without rereading data */
        if((len == AVERROR_EOF) || (len==0))
            s->eof_reached = 1;
        else
			{
			s->error= len;   
        	}
    } else {
        s->pos += len;
        s->buf_ptr = dst;
        s->buf_end = dst + len;
    }
}

unsigned long ff_crc04C11DB7_update(unsigned long checksum, const uint8_t *buf,
                                    unsigned int len)
{
    return av_crc(av_crc_get_table(AV_CRC_32_IEEE), checksum, buf, len);
}

unsigned long get_checksum(ByteIOContext *s)
{
    s->checksum= s->update_checksum(s->checksum, s->checksum_ptr, s->buf_ptr - s->checksum_ptr);
    s->update_checksum= NULL;
    return s->checksum;
}

void init_checksum(ByteIOContext *s,
                   unsigned long (*update_checksum)(unsigned long c, const uint8_t *p, unsigned int len),
                   unsigned long checksum)
{
    s->update_checksum= update_checksum;
    if(s->update_checksum){
        s->checksum= checksum;
        s->checksum_ptr= s->buf_ptr;
    }
}

/* XXX: put an inline version */
int get_byte(ByteIOContext *s)
{   
	int retry_fill_cnt = 0;
    if (s->buf_ptr < s->buf_end) {        
        return *s->buf_ptr++;       
    } else {
get_data:
		fill_buffer(s);
		if (s->buf_ptr < s->buf_end)                    
            return *s->buf_ptr++; 
        else
        {
			// return 0;
			if (url_interrupt_cb())       
            	return AVERROR(EINTR);
			if(s->eof_reached || s->error)
				av_log(NULL, AV_LOG_ERROR,"[%s]fill buffer: eof=%d error=%d retry_fill_cnt=%d\n",__FUNCTION__,s->eof_reached,s->error,retry_fill_cnt);
			if(retry_fill_cnt >= 1000)
			{
				av_log(NULL, AV_LOG_ERROR,"[%s]fill buffer retry for a long time,give up!\n",__FUNCTION__);
				return 0;
			}			 
			if(s->error == AVERROR(EAGAIN))
			{
				retry_fill_cnt ++;
				goto get_data;
			} 
        }
    }
	return 0;
}

int url_fgetc(ByteIOContext *s)
{
    if (s->buf_ptr < s->buf_end) {
        return *s->buf_ptr++;
    } else {
        fill_buffer(s);
        if (s->buf_ptr < s->buf_end)
            return *s->buf_ptr++;
        else
            return URL_EOF;
    }
}
int url_buffering_data(ByteIOContext *s,int size)
{
	if(s->enabled_lp_buffer)
		return url_lp_intelligent_buffering(s->opaque,size);
	else
		return -1;
}
int get_buffer(ByteIOContext *s, unsigned char *buf, int size)
{
    int len, size1;
	int read_len = 0;
    s->error = 0;   //xh,20100419
    size1 = size;    
    while (size > 0) {
        len = s->buf_end - s->buf_ptr;
        if (len > size)
            len = size;
        if (len == 0) {           
            if(size > s->buffer_size && !s->update_checksum){                  
                if(s->read_packet)
                {            
                    len = s->read_packet(s->opaque, buf, size);
                }
                if (len <= 0) {
                    /* do not modify buffer if errot getso that a seek back can
                    be done without rereading data */
                    if(len<0)
                        s->error= len; 
					else
						s->eof_reached = 1;
					av_log(NULL,AV_LOG_ERROR,"get_buffer ret len: %d\n",len);
                    break;
                } else {
                    s->pos += len;
                    size -= len;
                    buf += len;
                    s->buf_ptr = s->buffer;
                    s->buf_end = s->buffer/* + len*/;
					read_len += len;
                }
            }else{
                fill_buffer(s);
                len = s->buf_end - s->buf_ptr;  
				if(s->error)
					av_log(NULL,AV_LOG_ERROR,"fill_buffer error: %d\n",s->error);
                if (len == 0)                   
                    break;                 
            }
        } else if(len>0){
            memcpy(buf, s->buf_ptr, len);
            buf += len;
            s->buf_ptr += len;
            size -= len;
			read_len += len;
        }else{/*len <0*/
        	av_log(NULL,AV_LOG_ERROR,"get_buffer failed: buffer overflow= %d\n",len);
        	return s->error?s->error:-1; 
        	}
    }	
    #if 0
    if (size1 == size) {        
        if (url_ferror(s)) return url_ferror(s);   
        if (url_feof(s))   return AVERROR_EOF;
    }
    #else  //if (s->buf_end - s->buf_ptr)<size1, old can't return eof
	if(read_len <= 0)
	{
	    if (url_ferror(s)) return url_ferror(s);         
	    if (url_feof(s))   return AVERROR_EOF;
	}
    #endif
    
    return size1 - size;
}

int get_partial_buffer(ByteIOContext *s, unsigned char *buf, int size)
{
    int len;

    if(size<0)
        return -1;

    len = s->buf_end - s->buf_ptr;
    if (len == 0) {
        fill_buffer(s);
        len = s->buf_end - s->buf_ptr;
    }
    if (len > size)
        len = size;
    memcpy(buf, s->buf_ptr, len);
    s->buf_ptr += len;
    if (!len) {
        if (url_ferror(s)) return url_ferror(s);
        if (url_feof(s))   return AVERROR_EOF;
    }
    return len;
}

unsigned int get_le16(ByteIOContext *s)
{
    unsigned int val;
    val = get_byte(s);
    val |= get_byte(s) << 8;
    return val;
}

unsigned int get_le24(ByteIOContext *s)
{
    unsigned int val;
    val = get_le16(s);
    val |= get_byte(s) << 16;
    return val;
}

unsigned int get_le32(ByteIOContext *s)
{
    unsigned int val;
    val = get_le16(s);
    val |= get_le16(s) << 16;
    return val;
}

uint64_t get_le64(ByteIOContext *s)
{
    uint64_t val;
    val = (uint64_t)get_le32(s);
    val |= (uint64_t)get_le32(s) << 32;
    return val;
}

unsigned int get_be16(ByteIOContext *s)
{
    unsigned int val;
    val = get_byte(s) << 8;
    val |= get_byte(s);
    return val;
}

unsigned int get_be24(ByteIOContext *s)
{
    unsigned int val;
    val = get_be16(s) << 8;
    val |= get_byte(s);
    return val;
}
unsigned int get_be32(ByteIOContext *s)
{
    unsigned int val;
    val = get_be16(s) << 16;
    val |= get_be16(s);
    return val;
}

char *get_strz(ByteIOContext *s, char *buf, int maxlen)
{
    int i = 0;
    char c;

    while ((c = get_byte(s))) {
        if (i < maxlen-1)
            buf[i++] = c;
    }

    buf[i] = 0; /* Ensure null terminated, but may be truncated */

    return buf;
}

uint64_t get_be64(ByteIOContext *s)
{
    uint64_t val;
    val = (uint64_t)get_be32(s) << 32;
    val |= (uint64_t)get_be32(s);
    return val;
}

uint64_t ff_get_v(ByteIOContext *bc){
    uint64_t val = 0;
    int tmp;

    do{
        tmp = get_byte(bc);
        val= (val<<7) + (tmp&127);
    }while(tmp&128);
    return val;
}

int url_fdopen(ByteIOContext **s, URLContext *h)
{
    uint8_t *buffer;
    int buffer_size, max_packet_size;
	int lpbuffer_size=(h->flags & URL_MINI_BUFFER)?IO_LP_BUFFER_MINI_SIZE:IO_LP_BUFFER_SIZE;
	av_log(NULL, AV_LOG_INFO, "url_fdopen buffer size=%d\n",lpbuffer_size);
	if(h->flags & URL_MINI_BUFFER)
	{
		buffer_size=1024;
	}
	else
	{
	    max_packet_size = url_get_max_packet_size(h);
	    if (max_packet_size) {
	        buffer_size = max_packet_size; /* no need to bufferize more than one packet */
	    } else {
	        buffer_size = IO_BUFFER_SIZE;
	    }
	}
    buffer = av_malloc(buffer_size);
    if (!buffer)
        return AVERROR(ENOMEM);

    *s = av_mallocz(sizeof(ByteIOContext));
    if(!*s) {
        av_free(buffer);
        return AVERROR(ENOMEM);
    }
    if((h->is_slowmedia) && 
		!(h->flags &URL_NO_LP_BUFFER)	&&		/*no lp buffer*/	
		!(h->flags & URL_WRONLY || h->flags & URL_RDWR)  && /*no write support*/
		!url_lpopen(h,lpbuffer_size))
	{
		(*s)->enabled_lp_buffer=1;
		if (init_put_byte(*s, buffer, buffer_size,
			(h->flags & URL_WRONLY || h->flags & URL_RDWR), h,
			url_lpread, NULL, url_lpseek) < 0) {
			av_free(buffer);
			av_freep(s);
			return AVERROR(EIO);
		}
	}
	else
	{
		if (init_put_byte(*s, buffer, buffer_size,
			(h->flags & URL_WRONLY || h->flags & URL_RDWR), h,
			url_read, url_write, url_seek) < 0) {
			av_free(buffer);
			av_freep(s);
			return AVERROR(EIO);
		}
	}

    (*s)->is_streamed = h->is_streamed;
	(*s)->is_slowmedia = h->is_slowmedia;
    (*s)->max_packet_size = max_packet_size;
    if(h->prot) {
        (*s)->read_pause = (int (*)(void *, int))h->prot->url_read_pause;
        (*s)->read_seek  = (int64_t (*)(void *, int, int64_t, int))h->prot->url_read_seek;
    }
    return 0;
}

int url_setbufsize(ByteIOContext *s, int buf_size)
{
    uint8_t *buffer;	
    buffer = av_malloc(buf_size);
    if (!buffer)
        return AVERROR(ENOMEM);	
    av_free(s->buffer);
    s->buffer = buffer;
    s->buffer_size = buf_size;
	//s->buffer_size = 2048;
    s->buf_ptr = buffer;
    url_resetbuf(s, s->write_flag ? URL_WRONLY : URL_RDONLY);	
    return 0;
}

#if LIBAVFORMAT_VERSION_MAJOR < 53
int url_resetbuf(ByteIOContext *s, int flags)
#else
static int url_resetbuf(ByteIOContext *s, int flags)
#endif
{
#if LIBAVFORMAT_VERSION_MAJOR < 53
    URLContext *h = s->opaque;
    if ((flags & URL_RDWR) || (h && h->flags != flags && !h->flags & URL_RDWR))
        return AVERROR(EINVAL);
#else
    assert(flags == URL_WRONLY || flags == URL_RDONLY);
#endif

    if (flags & URL_WRONLY) {
        s->buf_end = s->buffer + s->buffer_size;
        s->write_flag = 1;
    } else {
        s->buf_end = s->buffer;
        s->write_flag = 0;
    }
    return 0;
}

int url_fopen(ByteIOContext **s, const char *filename, int flags)
{
    return url_fopen_h(s, filename, flags, NULL);
}

int url_fopen_h(ByteIOContext **s, const char *filename, int flags, const char *headers)
{
    URLContext *h;
    int err;

    err = url_open_h(&h, filename, flags, headers);
    if (err < 0)
        return err;
    err = url_fdopen(s, h);
    if (err < 0) {
        url_close(h);
        return err;
    }
    return 0;
}

int url_fclose(ByteIOContext *s)
{
	URLContext *h = s->opaque;

	av_free(s->buffer);
	av_free(s);
	if(h && h->lpbuf)	
	{
		url_lpfree(h);
	}
	return url_close(h);
}

URLContext *url_fileno(ByteIOContext *s)
{
    return s->opaque;
}

#if CONFIG_MUXERS
int url_fprintf(ByteIOContext *s, const char *fmt, ...)
{
    va_list ap;
    char buf[4096];
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    put_buffer(s, buf, strlen(buf));
    return ret;
}
#endif //CONFIG_MUXERS

char *url_fgets(ByteIOContext *s, char *buf, int buf_size)
{
    int c;
    char *q;

    c = url_fgetc(s);
    if (c == EOF)
        return NULL;
    q = buf;
    for(;;) {
        if (c == EOF || c == '\n')
            break;
        if ((q - buf) < buf_size - 1)
            *q++ = c;
        c = url_fgetc(s);
    }
    if (buf_size > 0)
        *q = '\0';
    return buf;
}

int url_fget_max_packet_size(ByteIOContext *s)
{
    return s->max_packet_size;
}

int av_url_read_fpause(ByteIOContext *s, int pause)
{
    if (!s->read_pause)
        return AVERROR(ENOSYS);
    return s->read_pause(s->opaque, pause);
}

int64_t av_url_read_fseek(ByteIOContext *s, int stream_index,
                          int64_t timestamp, int flags)
{
    URLContext *h = s->opaque;
    int64_t ret;
    if (!s->read_seek)
        return AVERROR(ENOSYS);
    ret = s->read_seek(h, stream_index, timestamp, flags);
    if(ret >= 0) {
        s->buf_ptr = s->buf_end; // Flush buffer
        s->pos = s->seek(h, 0, SEEK_CUR);
    }
    return ret;
}

/* url_open_dyn_buf and url_close_dyn_buf are used in rtp.c to send a response
 * back to the server even if CONFIG_MUXERS is false. */
#if CONFIG_MUXERS || CONFIG_NETWORK
/* buffer handling */
int url_open_buf(ByteIOContext **s, uint8_t *buf, int buf_size, int flags)
{
    int ret;
    *s = av_mallocz(sizeof(ByteIOContext));
    if(!*s)
        return AVERROR(ENOMEM);
    ret = init_put_byte(*s, buf, buf_size,
                        (flags & URL_WRONLY || flags & URL_RDWR),
                        NULL, NULL, NULL, NULL);
    if(ret != 0)
        av_freep(s);
    return ret;
}

int url_close_buf(ByteIOContext *s)
{
    put_flush_packet(s);
    return s->buf_ptr - s->buffer;
}

/* output in a dynamic buffer */

typedef struct DynBuffer {
    int pos, size, allocated_size;
    uint8_t *buffer;
    int io_buffer_size;
    uint8_t io_buffer[1];
} DynBuffer;

static int dyn_buf_write(void *opaque, uint8_t *buf, int buf_size)
{
    DynBuffer *d = opaque;
    unsigned new_size, new_allocated_size;

    /* reallocate buffer if needed */
    new_size = d->pos + buf_size;
    new_allocated_size = d->allocated_size;
    if(new_size < d->pos || new_size > INT_MAX/2)
        return -1;
    while (new_size > new_allocated_size) {
        if (!new_allocated_size)
            new_allocated_size = new_size;
        else
            new_allocated_size += new_allocated_size / 2 + 1;
    }

    if (new_allocated_size > d->allocated_size) {
        d->buffer = av_realloc(d->buffer, new_allocated_size);
        if(d->buffer == NULL)
             return AVERROR(ENOMEM);
        d->allocated_size = new_allocated_size;
    }
    memcpy(d->buffer + d->pos, buf, buf_size);
    d->pos = new_size;
    if (d->pos > d->size)
        d->size = d->pos;
    return buf_size;
}

static int dyn_packet_buf_write(void *opaque, uint8_t *buf, int buf_size)
{
    unsigned char buf1[4];
    int ret;

    /* packetized write: output the header */
    AV_WB32(buf1, buf_size);
    ret= dyn_buf_write(opaque, buf1, 4);
    if(ret < 0)
        return ret;

    /* then the data */
    return dyn_buf_write(opaque, buf, buf_size);
}

static int64_t dyn_buf_seek(void *opaque, int64_t offset, int whence)
{
    DynBuffer *d = opaque;

    if (whence == SEEK_CUR)
        offset += d->pos;
    else if (whence == SEEK_END)
        offset += d->size;
    if (offset < 0 || offset > 0x7fffffffLL)
        return -1;
    d->pos = offset;
    return 0;
}

static int url_open_dyn_buf_internal(ByteIOContext **s, int max_packet_size)
{
    DynBuffer *d;
    int ret;
    unsigned io_buffer_size = max_packet_size ? max_packet_size : 1024;

    if(sizeof(DynBuffer) + io_buffer_size < io_buffer_size)
        return -1;
    d = av_mallocz(sizeof(DynBuffer) + io_buffer_size);
    if (!d)
        return AVERROR(ENOMEM);
    *s = av_mallocz(sizeof(ByteIOContext));
    if(!*s) {
        av_free(d);
        return AVERROR(ENOMEM);
    }
    d->io_buffer_size = io_buffer_size;
    ret = init_put_byte(*s, d->io_buffer, io_buffer_size,
                        1, d, NULL,
                        max_packet_size ? dyn_packet_buf_write : dyn_buf_write,
                        max_packet_size ? NULL : dyn_buf_seek);
    if (ret == 0) {
        (*s)->max_packet_size = max_packet_size;
    } else {
        av_free(d);
        av_freep(s);
    }
    return ret;
}

int url_open_dyn_buf(ByteIOContext **s)
{
    return url_open_dyn_buf_internal(s, 0);
}

int url_open_dyn_packet_buf(ByteIOContext **s, int max_packet_size)
{
    if (max_packet_size <= 0)
        return -1;
    return url_open_dyn_buf_internal(s, max_packet_size);
}

int url_close_dyn_buf(ByteIOContext *s, uint8_t **pbuffer)
{
    DynBuffer *d = s->opaque;
    int size;

    put_flush_packet(s);

    *pbuffer = d->buffer;
    size = d->size;
    av_free(d);
    av_free(s);
    return size;
}
#endif /* CONFIG_MUXERS || CONFIG_NETWORK */
