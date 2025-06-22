/*******************************************************************************
 * Copyright (c) 2014 Martin Marinov.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Public License v3.0
 * which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/gpl.html
 *
 * Contributors:
 *     Martin Marinov - initial API and implementation
 ******************************************************************************/

#include "frameratedetector.h"
#include "internaldefinitions.h"
#include <assert.h>
#include "threading.h"
#include <math.h>
#include "extbuffer.h"
#include "fft.h"

#define MIN_FRAMERATE (55)
#define MIN_HEIGHT (590)
#define MAX_FRAMERATE (87)
#define MAX_HEIGHT (1500)
#define FRAMES_TO_CAPTURE (3.1)

/*
 * 对输入缓冲区进行自相关处理，结果写入extbuffer。
 */
void autocorrelate(extbuffer_t * buff, float * data, int size) {
	// 为extbuffer分配空间，准备处理2倍size长度的数据
	extbuffer_preparetohandle(buff, 2*size);

	// 调用FFT自相关函数，计算自相关结果
	fft_autocorrelation(buff->buffer, data, size);
	if (!buff->valid) return; // 如果结果无效则直接返回
}

/*
 * 对输入缓冲区in的复数幅值进行累加平均，结果写入out。
 * startid为起始索引，length为处理长度。
 */
void accummulate(extbuffer_t * out, extbuffer_t * in, int startid, int length) {
	const uint64_t calls = in->calls;
	const uint64_t currcalls = calls - 1;

	// 为输出缓冲区分配空间
	extbuffer_preparetohandle(out, length);
	uint32_t i;

	float * in_buff = in->buffer + startid*2;
	double * out_buff = out->dbuffer;

	if (calls == 0) {
		// 第一次调用，直接赋值
		for (i = 0; i < length; i++) {
			const double I = *(in_buff++); // 取实部
			const double Q = *(in_buff++); // 取虚部
			const double now_avg = sqrt(I*I + Q*Q); // 计算幅值
			*(out_buff++) = now_avg;
		}
	} else {
		// 后续调用，做累加平均
		const double callsd = calls;
		const double currcallsd = currcalls;
		for (i = 0; i < length; i++) {
			const double I = *(in_buff++);
			const double Q = *(in_buff++);
			const double now_avg = sqrt(I*I + Q*Q);
			const double prev_avg = *out_buff;
			*(out_buff++) = (prev_avg*currcallsd + now_avg)/callsd; // 累加平均
		}
	}
}

/*
 * 将自相关结果以CSV格式导出到文件，便于分析。
 */
void dump_autocorrect(extbuffer_t * rawiq, double samplerate) {
	assert (rawiq->valid); // 确保输入数据有效

	FILE *f = NULL;

	f = fopen("autocorr.csv", "w"); // 打开文件用于写入

	fprintf(f, "%s, %s\n", "ms", "dB"); // 写入表头

	int i;
	// 计算最大元素数（每两个为一组IQ）
	const int maxels = fft_getrealsize(rawiq->size_valid_elements)/2;
	for (i = 0; i < maxels; i+=2) {
		// 取出I、Q分量
		const double I = (rawiq->type == EXTBUFFER_TYPE_DOUBLE) ? (rawiq->dbuffer[i]) : (rawiq->buffer[i]);
		const double Q = (rawiq->type == EXTBUFFER_TYPE_DOUBLE) ? (rawiq->dbuffer[i+1]) : (rawiq->buffer[i+1]);
		// 计算dB值
		const double db = 10.0*log10(sqrt(I*I+Q*Q));
		// 计算时间（毫秒）
		const double t = 1000.0 * (i / 2) / (double) samplerate;

		fprintf(f, "%f, %f\n", t, db); // 写入一行数据
	}

	fclose(f); // 关闭文件
}

/*
 * 在一帧数据上运行帧率检测的主要处理流程，包括自相关、累加、通知回调等。
 */
void frameratedetector_runontodata(frameratedetector_t * frameratedetector, float * data, int size, extbuffer_t * extbuff, extbuffer_t * extbuff_small1, extbuffer_t * extbuff_small2) {
	// 如果关闭了自相关绘图，直接返回
	if (frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_OFF]) return;

	// 计算最大、最小长度
	const int maxlength = frameratedetector->samplerate / (double) (MIN_FRAMERATE);
	const int minlength = frameratedetector->samplerate / (double) (MAX_FRAMERATE);

	const int height_maxlength = frameratedetector->samplerate / (double) (MIN_HEIGHT * MIN_FRAMERATE);
	const int height_minlength = frameratedetector->samplerate / (double) (MAX_HEIGHT * MAX_FRAMERATE);

	// 检查是否需要重置缓存
	if (frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_RESET]) {
		const int origval = frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_RESET];
		frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_RESET] = 0;
		extbuffer_cleartozero(extbuff);
		extbuffer_cleartozero(extbuff_small1);
		extbuffer_cleartozero(extbuff_small2);
		if (origval == 1) announce_callback_changed(frameratedetector->tsdr, VALUE_ID_AUTOCORRECT_RESET, 0, 0);
	}

	if (frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_OFF]) return;

	// 进行自相关处理
	autocorrelate(extbuff, data, size);

	// 检查是否需要导出自相关结果
	if (frameratedetector->tsdr->params_int[PARAM_AUTOCORR_DUMP]) {
		frameratedetector->tsdr->params_int[PARAM_AUTOCORR_DUMP] = 0;

		dump_autocorrect(extbuff, frameratedetector->samplerate);

		announce_callback_changed(frameratedetector->tsdr, VALUE_ID_AUTOCORRECT_DUMPED, 0, 0);
	}

	// 对自相关结果做累加平均，分别用于帧和行
	accummulate(extbuff_small1, extbuff, minlength, maxlength-minlength);
	accummulate(extbuff_small2, extbuff, height_minlength, height_maxlength-height_minlength);

	// 通知上层绘图数据已准备好
	announce_plotready(frameratedetector->tsdr, PLOT_ID_FRAME, extbuff_small1, maxlength-minlength, minlength, frameratedetector->samplerate);
	announce_plotready(frameratedetector->tsdr, PLOT_ID_LINE, extbuff_small2, height_maxlength-height_minlength, height_minlength, frameratedetector->samplerate);

	// 通知上层自相关帧数已更新
	announce_callback_changed(frameratedetector->tsdr, VALUE_ID_AUTOCORRECT_FRAMES_COUNT, 0, extbuff->calls);
}

/*
 * 帧率检测线程主循环，不断从环形缓冲区取数据并处理。
 */
void frameratedetector_thread(void * ctx) {
	/*
	* 检测输入采样的帧率
	* 通过寻找自相关峰值来估算帧率，利用FFT加速自相关计算
	*/

	frameratedetector_t * frameratedetector = (frameratedetector_t *) ctx;

	extbuffer_t extbuff;
	extbuffer_t extbuff_small1;
	extbuffer_t extbuff_small2;

	extbuffer_init(&extbuff);
	extbuffer_init_double(&extbuff_small1);
	extbuffer_init_double(&extbuff_small2);

	float * buf = NULL;
	uint32_t bufsize = 0;

	while (frameratedetector->alive) {
		// 计算需要采集的样本数
		const uint32_t desiredsize = FRAMES_TO_CAPTURE * frameratedetector->samplerate / (double) (MIN_FRAMERATE);
		if (desiredsize == 0) {
			thread_sleep(10); // 采样率为0时休眠
			continue;
		}

		// 动态分配或扩展缓冲区
		if (desiredsize > bufsize) {
			bufsize = desiredsize;
			if (buf == NULL) buf = malloc(sizeof(float) * bufsize); else buf = realloc(buf, sizeof(float) * bufsize);
		}

		// 检查是否需要清空缓存
		if (frameratedetector->purge_buffers) {
			extbuffer_cleartozero(&extbuff);
			extbuffer_cleartozero(&extbuff_small1);
			extbuffer_cleartozero(&extbuff_small2);
			frameratedetector->purge_buffers = 0;
		}

		// 从环形缓冲区获取数据并处理
		if (cb_rem_blocking(&frameratedetector->circbuff, buf, desiredsize) == CB_OK)
			frameratedetector_runontodata(frameratedetector, buf, desiredsize, &extbuff, &extbuff_small1, &extbuff_small2);
	}

	free (buf); // 释放缓冲区

	extbuffer_free(&extbuff);
	extbuffer_free(&extbuff_small1);
	extbuffer_free(&extbuff_small2);
}

/*
 * 初始化帧率检测器结构体。
 */
void frameratedetector_init(frameratedetector_t * frameratedetector, tsdr_lib_t * tsdr) {
	frameratedetector->tsdr = tsdr;
	frameratedetector->samplerate = 0;
	frameratedetector->alive = 0;

	cb_init(&frameratedetector->circbuff, CB_SIZE_MAX_COEFF_HIGH_LATENCY); // 初始化环形缓冲区
}

/*
 * 清空帧率检测器的缓存和环形缓冲区。
 */
void frameratedetector_flushcachedestimation(frameratedetector_t * frameratedetector) {
	frameratedetector->purge_buffers = 1; // 标记需要清空缓存
	frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_RESET] = 2;
	cb_purge(&frameratedetector->circbuff); // 清空环形缓冲区
}

/*
 * 启动帧率检测线程。
 */
void frameratedetector_startthread(frameratedetector_t * frameratedetector) {
	frameratedetector_flushcachedestimation(frameratedetector);

	frameratedetector->alive = 1;

	thread_start(frameratedetector_thread, frameratedetector); // 启动线程
}

/*
 * 停止帧率检测线程。
 */
void frameratedetector_stopthread(frameratedetector_t * frameratedetector) {
	frameratedetector->alive = 0;
}

/*
 * 向帧率检测器添加数据，或根据参数丢弃数据。
 */
void frameratedetector_run(frameratedetector_t * frameratedetector, float * data, int size, uint32_t samplerate, int drop) {
	// 如果关闭了自相关绘图，直接返回
	if (frameratedetector->tsdr->params_int[PARAM_AUTOCORR_PLOTS_OFF])
		return;

	if (drop) {
		cb_purge(&frameratedetector->circbuff); // 丢弃数据时清空环形缓冲区
		return;
	}

	frameratedetector->samplerate = samplerate;
	// 添加数据到环形缓冲区，失败则清空
	if (cb_add(&frameratedetector->circbuff, data, size) != CB_OK)
		cb_purge(&frameratedetector->circbuff);
}

/*
 * 释放帧率检测器相关资源。
 */
void frameratedetector_free(frameratedetector_t * frameratedetector) {
	frameratedetector_stopthread(frameratedetector);
	cb_free(&frameratedetector->circbuff); // 释放环形缓冲区
}
