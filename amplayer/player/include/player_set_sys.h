#ifndef _PLAYER_SET_DISP_H_
#define _PLAYER_SET_DISP_H_

int set_black_policy(int blackout);
int get_black_policy();
int set_tsync_enable(int enable);
int set_tsync_discontinue(int enable);
int get_pts_discontinue();
int set_fb0_blank(int blank);
int set_fb1_blank(int blank);
int set_subtitle_num(int num);
int av_get_subtitle_curr();
int set_subtitle_startpts(int pts);
int set_subtitle_fps(int fps);
int set_subtitle_subtype(int subtype);
char *get_display_mode();
int set_fb0_freescale(int freescale);
int set_fb1_freescale(int freescale);
int set_display_axis(int *coordinate);
int set_video_axis(int *coordinate);
int set_fb0_scale_width(int width);
int set_fb0_scale_height(int height);
int set_fb1_scale_width(int width);
int set_fb1_scale_height(int height);
#endif
