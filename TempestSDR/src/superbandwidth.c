/*
#-------------------------------------------------------------------------------
# Copyright (c) 2014 Martin Marinov.
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the GNU Public License v3.0
# which accompanies this distribution, and is available at
# http://www.gnu.org/licenses/gpl.html
#
# Contributors:
#     Martin Marinov - initial API and implementation
#-------------------------------------------------------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "superbandwidth.h"
#include "internaldefinitions.h"
#include "fft.h"
#include "threading.h"
#include <math.h>

#define SUPER_HOPS_TO_MAKE (4)

#define SUPER_STATE_STOPPED (0)
#define SUPER_STATE_STARTING (1)
#define SUPER_STATE_GATHERING (2)
#define SUPER_STATE_PAUSE (3)
#define SUPER_STATE_DATA_READY (4)
#define SUPER_STATE_OUTPUT_DATA_READY (5)

#define SUPER_SAMPLES_TO_RECORD (10)

#define SUPER_SECS_TO_PAUSE (0.5)

/*
 * 初始化superbandwidth结构体，分配必要资源。
 */
void superb_init(superbandwidth_t * bw) {
	bw->state = SUPER_STATE_STOPPED;
	bw->buffscount = 0;
	bw->buffsbuffcount = 0;
	bw->samplerate = 0;
	bw->buffs = NULL;

	extbuffer_init(&bw->extb);
	extbuffer_init(&bw->extb_out);
	extbuffer_init(&bw->extb_temp);
	mutex_init(&bw->thread_unlock);
}

/*
 * 释放superbandwidth结构体中的多帧缓冲区。
 */
void super_freebuff(superbandwidth_t * bw) {
	if (bw->buffs) {
		int i;
		for (i = 0; i < bw->buffscount; i++) free(bw->buffs[i]); // 释放每一帧的缓冲区
		free (bw->buffs); // 释放指针数组
		bw->buffs = NULL;
	}
}

/*
 * 释放superbandwidth结构体的所有资源。
 */
void superb_free(superbandwidth_t * bw) {
	super_stopthread(bw);
	super_freebuff(bw);

	extbuffer_free(&bw->extb);
	extbuffer_free(&bw->extb_out);
	extbuffer_free(&bw->extb_temp);
	mutex_free(&bw->thread_unlock);
}

/*
 * 将复数数组转为幅值差分，虚部清零。
 */
void complex_to_abs_diff(float * data, int size) {

	int i;
	float prev = data[0]*data[0] + data[1]*data[1];
	for (i = 0; i < size; i+=2) {
		const int i1 = i+1;
		const float I = data[i];
		const float Q = data[i1];
		const float curr = sqrtf(I*I+Q*Q); // 计算当前幅值
		const float diff = curr - prev;    // 计算幅值差分
		prev = curr;
		data[i] = diff;
		data[i1] = 0;
	}
}

/*
 * 计算两帧数据的最佳对齐偏移（互相关最大值位置），用于帧拼接。
 */
inline static int superb_bestfit(superbandwidth_t * bw, float * data1, float * data2, int size) {
	size = (size / bw->samples_in_frame) * bw->samples_in_frame;
	size = fft_getrealsize(size);
	const int samples = size/2;

	extbuffer_preparetohandle(&bw->extb_out, size);
	extbuffer_preparetohandle(&bw->extb_temp, size);

	// 拷贝数据，准备FFT
	memcpy(bw->extb_out.buffer, data1, size * sizeof(float));
	memcpy(bw->extb_temp.buffer, data2, size * sizeof(float));

	complex_to_abs_diff(bw->extb_out.buffer, size);
	complex_to_abs_diff(bw->extb_temp.buffer, size);

	fft_crosscorrelation(bw->extb_out.buffer, bw->extb_temp.buffer, samples);

	int maxlength = 0;
	float maxval;

	int i;
	float * out = bw->extb_out.buffer;
	for (i = 0; i < samples; i++) {
		const float I = *(out++);
		const float Q = *(out++);
		const float val = sqrtf(I*I+Q*Q);

		if (i == 0)
			maxval = val;
		else if (val > maxval) {
			maxval = val;
			maxlength = i;
		}
	}

	return 2*maxlength;
}

/*
 * 当数据收集完成后，对所有帧进行对齐、拼接并做FFT，输出结果。
 */
void superb_ondataready(superbandwidth_t * bw, float ** outbuff, int * outbufsize, tsdr_lib_t * tsdr) {
	//printf("Data ready. Gathered %d frames\n", bw->buffsbuffcount / bw->samples_in_frame);fflush(stdout);

	bw->buffsbuffcount = fft_getrealsize(bw->buffsbuffcount);
	const uint32_t totalsamples = bw->buffscount * bw->buffsbuffcount;

	extbuffer_preparetohandle(&bw->extb, totalsamples * 2);

	// 帧对齐
	int i;
	const int bufsize = bw->buffsbuffcount * 2;
	for (i = 1; i < bw->buffscount; i++) {
		const int best_offset = superb_bestfit(bw, bw->buffs[0], bw->buffs[i], bufsize);
		if (!bw->alive) return;
		memcpy(bw->extb.buffer, &bw->buffs[i][best_offset], (bufsize - best_offset) * sizeof(float));
		memcpy(&bw->buffs[i][bufsize -best_offset], bw->buffs[i], best_offset * sizeof(float));
		memcpy(bw->buffs[i], bw->extb.buffer, (bufsize - best_offset) * sizeof(float));
		fft_perform(bw->buffs[i], bw->buffsbuffcount, 0);
	}
	fft_perform(bw->buffs[0], bw->buffsbuffcount, 0);

	// 拼接所有帧的FFT结果
	for (i = 0; i < bw->buffscount; i++)
		memcpy(&bw->extb.buffer[i*bw->buffsbuffcount*2], bw->buffs[i], bw->buffsbuffcount * 2 * sizeof(float));

	fft_perform(bw->extb.buffer, totalsamples, 1);

	*outbuff = bw->extb.buffer;
	*outbufsize = totalsamples;

	set_internal_samplerate(tsdr, bw->buffscount*bw->samplerate);
}

/*
 * 超带宽处理线程，等待数据收集完成后进行处理。
 */
void super_thread(void * ctx) {
	superbandwidth_t * bw = (superbandwidth_t *) ctx;

	while (bw->alive) {

		while (bw->state != SUPER_STATE_DATA_READY)
			mutex_wait(&bw->thread_unlock);

		bw->outbuf = NULL;
		superb_ondataready(bw, &bw->outbuf, &bw->outbufsize, bw->tsdr);
		bw->state = SUPER_STATE_OUTPUT_DATA_READY;
	}
}

/*
 * 启动超带宽处理线程。
 */
void super_startthread(superbandwidth_t * bw) {

	bw->alive = 1;

	thread_start(super_thread, bw);
}

/*
 * 停止超带宽处理线程。
 */
void super_stopthread(superbandwidth_t * bw) {
	bw->alive = 0;
}

/*
 * 超带宽主处理流程，负责数据采集、状态切换、帧管理等。
 * iq为输入数据，size为长度，outbuff/outbufsize为输出。
 */
void superb_run(superbandwidth_t * bw, float * iq, int size, tsdr_lib_t * tsdr, int dropped, float ** outbuff, int * outbufsize) {
	*outbuff = NULL;
	if (bw->tsdr != tsdr) bw->tsdr = tsdr;

	if (bw->state == SUPER_STATE_STOPPED) {
		bw->state = SUPER_STATE_STARTING;
		super_startthread(bw);
	}

	if (bw->state == SUPER_STATE_STARTING) {
		bw->buffid_current = 0;
		bw->samples_gathered = 0;
		bw->buffsbuffcount = 0;

		if (tsdr->samplerate_real != bw->samplerate) {
			bw->samplerate = tsdr->samplerate_real;

			bw->samples_in_frame =  tsdr->samplerate_real / tsdr->refreshrate;
			bw->samples_to_gather = SUPER_SAMPLES_TO_RECORD * bw->samples_in_frame;
			bw->samples_to_pause = SUPER_SECS_TO_PAUSE * tsdr->samplerate_real;

			super_freebuff(bw);

			bw->buffscount = SUPER_HOPS_TO_MAKE;

			bw->buffs = malloc(sizeof(float *) * bw->buffscount); // 分配帧指针数组
			int i;
			for (i = 0; i < bw->buffscount; i++) bw->buffs[i] = malloc(sizeof(float) * bw->samples_to_gather * 2); // 分配每帧缓冲区
		}

		bw->state = SUPER_STATE_GATHERING;
	}

	if (bw->state == SUPER_STATE_PAUSE) {
		bw->samples_gathered+=size / 2;
		if (bw->samples_gathered > bw->samples_to_pause) {
			bw->samples_gathered = 0;
			bw->state = SUPER_STATE_GATHERING;
		}
	}

	if (bw->state == SUPER_STATE_GATHERING) {
		if (dropped) {bw->samples_gathered = 0; return;}

		const int samples_now = size / 2;
		if (bw->samples_gathered + samples_now < bw->samples_to_gather) {
			memcpy(&bw->buffs[bw->buffid_current][bw->samples_gathered*2], iq, size * sizeof(float)); // 采集数据
			bw->samples_gathered+=samples_now;
		} else {
			const int samples_remain = bw->samples_to_gather - bw->samples_gathered;
			memcpy(&bw->buffs[bw->buffid_current][bw->samples_gathered*2], iq, samples_remain * 2 * sizeof(float)); // 补齐最后一段
			bw->samples_gathered+=samples_remain;

			//printf("samples_gathered %d + samples_now %d >= samples_to_gather %d\n", bw->samples_gathered, samples_now, bw->samples_to_gather); fflush(stdout);

			bw->buffid_current++;
			bw->buffsbuffcount = bw->samples_gathered;
			bw->samples_gathered = 0;

			if (bw->buffid_current >= bw->buffscount) {
				bw->state = SUPER_STATE_DATA_READY;
			} else {
				shiftfreq(tsdr, (bw->buffid_current-bw->buffscount/2)*bw->samplerate); // 跳频
				bw->state = SUPER_STATE_PAUSE;
			}

		}

	}

	if (bw->state == SUPER_STATE_OUTPUT_DATA_READY) {
		bw->state = SUPER_STATE_STARTING;
		*outbuff = bw->outbuf;
		*outbufsize = bw->outbufsize;
	}
}

/*
 * 停止超带宽处理，释放线程和采样率设置。
 */
void superb_stop(superbandwidth_t * bw, tsdr_lib_t * tsdr) {
	if (bw->state != SUPER_STATE_STOPPED) {
		bw->state = SUPER_STATE_STOPPED;
		shiftfreq(tsdr, 0);
		set_internal_samplerate(tsdr, tsdr->samplerate_real);
		super_stopthread(bw);
	}
}


