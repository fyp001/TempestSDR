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

#include "dsp.h"
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <string.h>
#include "syncdetector.h"

#define AUTOGAIN_REPORT_EVERY_FRAMES (5)

/*
 * 对图像进行时域低通滤波，实现运动模糊效果，提升弱信号质量。
 */
void dsp_timelowpass_run(const float lowpassvalue, int sizetopoll, float * buffer, float * screenbuffer) {
	// antilowpassvalue为1-lowpassvalue
	const double antilowpassvalue = 1.0 - lowpassvalue;
	int i;
	for (i = 0; i < sizetopoll; i++)
		// 对每个像素做加权平均，实现运动模糊
		screenbuffer[i] = screenbuffer[i] * lowpassvalue + buffer[i] * antilowpassvalue; // 低通滤波
}

/*
 * 初始化自动增益结构体。
 */
void dsp_autogain_init(dsp_autogain_t * autogain) {
	autogain->lastmax = 0;
	autogain->lastmin = 0;
	autogain->snr = 1.0;
}

/*
 * 对图像进行自动增益处理，将像素值映射到全动态范围，增强对比度。
 */
void dsp_autogain_run(dsp_autogain_t * autogain, int sizetopoll, float * screenbuffer, float * sendbuffer, float norm) {
	int i;

	float min = screenbuffer[0];
	float max = min;
	double sum = 0.0;

	// 计算最大、最小值和均值
	for (i = 0; i < sizetopoll; i++) {
		const float val = screenbuffer[i];
#if PIXEL_SPECIAL_COLOURS_ENABLED
		if (val > 250.0 || val < -250) continue; // 跳过特殊像素
#endif
		if (val > max) max = val; else if (val < min) min = val;
		sum+=val;
	}

	const float oneminusnorm = 1.0f-norm;
	// 指数平滑更新最大最小值
	autogain->lastmax = oneminusnorm*autogain->lastmax + norm*max;
	autogain->lastmin = oneminusnorm*autogain->lastmin + norm*min;
	const float span = (autogain->lastmax == autogain->lastmin) ? (1.0f) : (autogain->lastmax - autogain->lastmin);

	const double mean = sum / (double) sizetopoll;
	double sum2 = 0.0;
	double sum3 = 0.0;
#if PIXEL_SPECIAL_COLOURS_ENABLED
	for (i = 0; i < sizetopoll; i++) {
		const float val = screenbuffer[i];
		sendbuffer[i] = (val > 250.0 || val < -250) ? (val) : ((screenbuffer[i] - autogain->lastmin) / span);

		const double valmeandiff = val - mean;
		sum2 += valmeandiff*valmeandiff;
		sum3 += valmeandiff;
	}
#else
	for (i = 0; i < sizetopoll; i++) {
		const float val = screenbuffer[i];
		// 归一化到[0,1]区间
		sendbuffer[i] = (val - autogain->lastmin) / span;

		const float valmeandiff = val - mean;
		sum2 += valmeandiff*valmeandiff;
		sum3 += valmeandiff;
	}
#endif

	// 计算标准差
	const double stdev = sqrt((sum2 - sum3*sum3/(double) sizetopoll) / (double) (sizetopoll - 1));

	// 计算信噪比
	autogain->snr = mean / stdev; // 信噪比估算
}

/*
 * 计算图像的水平和垂直方向均值，用于检测消隐区。
 */
void dsp_average_v_h(int width, int height, float * sendbuffer, float * widthcollapsebuffer, float * heightcollapsebuffer) {
	const int totalpixels = width*height;
	int i;
	// 初始化均值缓冲区
	for (i = 0; i < width; i++) widthcollapsebuffer[i] = 0.0f;
	for (i = 0; i < height; i++) heightcollapsebuffer[i] = 0.0f;
	for (i = 0; i < totalpixels; i++) {
		const float val = sendbuffer[i];
		// 按列累加
		widthcollapsebuffer[i % width] += val;
		// 按行累加
		heightcollapsebuffer[i / width] += val;
	}
}

/*
 * 初始化后处理结构体。
 */
void dsp_post_process_init(dsp_postprocess_t * pp) {
	dsp_autogain_init(&pp->dsp_autogain);

	pp->screenbuffer = NULL;
	pp->sendbuffer = NULL;
	pp->corrected_sendbuffer = NULL;

	pp->widthcollapsebuffer = NULL;
	pp->heightcollapsebuffer = NULL;

	pp->bufsize = 0;
	pp->sizetopoll = 0;
	pp->width = 0;
	pp->height = 0;

	pp->runs = 0;

	pp->lowpass_before_sync = 0;

	syncdetector_init(&pp->sync);
}

/*
 * 图像后处理主流程，包括同步检测、低通滤波、自动增益等。
 * 返回处理后的图像缓冲区。
 */
float * dsp_post_process(tsdr_lib_t * tsdr, dsp_postprocess_t * pp, float * buffer, int nowwidth, int nowheight, float motionblur, float lowpasscoeff, const int lowpass_before_sync, const int autogain_after_proc) {
	// 检查尺寸变化，必要时重新分配缓冲区
	if (nowheight != pp->height || nowwidth != pp->width) {
		const int oldheight = pp->height;
		const int oldwidth = pp->width;

		pp->height = nowheight;
		pp->width = nowwidth;
		pp->sizetopoll = pp->height * pp->width;
		assert(pp->sizetopoll > 0);

		// 动态分配/扩容所有相关缓冲区
		if (pp->sizetopoll > pp->bufsize) {
			pp->bufsize = pp->sizetopoll;
			pp->screenbuffer = MALLOC_OR_REALLOC(pp->screenbuffer, pp->bufsize, float);
			pp->sendbuffer = MALLOC_OR_REALLOC(pp->sendbuffer, pp->bufsize, float);
			pp->corrected_sendbuffer = MALLOC_OR_REALLOC(pp->corrected_sendbuffer, pp->bufsize, float);
			int i;
			for (i = 0; i < pp->bufsize; i++) pp->screenbuffer[i] = 0.0f;
		}

		// 宽高变化时重新分配collapse缓冲区
		if (pp->width != oldwidth) pp->widthcollapsebuffer = MALLOC_OR_REALLOC(pp->widthcollapsebuffer, pp->width, float);
		if (pp->height != oldheight) pp->heightcollapsebuffer = MALLOC_OR_REALLOC(pp->heightcollapsebuffer, pp->height, float);

	}

	// 切换低通滤波模式时清空缓冲区
	if (pp->lowpass_before_sync != lowpass_before_sync) {
		pp->lowpass_before_sync = lowpass_before_sync;
		int i;
		for (i = 0; i < pp->sizetopoll; i++) {
			pp->screenbuffer[i] = 0.0;
			pp->sendbuffer[i] = 0.0;
			pp->corrected_sendbuffer[i] = 0.0;
		}
	}

	// 主后处理流程
	float * input = buffer;
	if (!autogain_after_proc) {
		// 先做自动增益
		dsp_autogain_run(&pp->dsp_autogain, pp->sizetopoll, input, pp->sendbuffer, lowpasscoeff);
		input = pp->sendbuffer;
	}

	float * result;

	if (lowpass_before_sync) {
		// 先低通再同步
		dsp_timelowpass_run(motionblur, pp->sizetopoll, input, pp->screenbuffer);
		dsp_average_v_h(pp->width, pp->height, pp->screenbuffer, pp->widthcollapsebuffer, pp->heightcollapsebuffer);

		float * syncresult = syncdetector_run(&pp->sync, tsdr, pp->screenbuffer, pp->corrected_sendbuffer, pp->width, pp->height, pp->widthcollapsebuffer, pp->heightcollapsebuffer, !tsdr->params_int[PARAM_AUTOCORR_SUPERRESOLUTION], 0);

		if (autogain_after_proc) {
			// 同步后再自动增益
			dsp_autogain_run(&pp->dsp_autogain, pp->sizetopoll, syncresult, pp->sendbuffer, lowpasscoeff);
			result = pp->sendbuffer;
		} else
			result = syncresult;

	} else {
		// 先同步再低通
		dsp_average_v_h(pp->width, pp->height, input, pp->widthcollapsebuffer, pp->heightcollapsebuffer);

		float * syncresult = syncdetector_run(&pp->sync, tsdr, input, pp->corrected_sendbuffer, pp->width, pp->height, pp->widthcollapsebuffer, pp->heightcollapsebuffer, (motionblur == 0.0f) && (!tsdr->params_int[PARAM_AUTOCORR_SUPERRESOLUTION]), 1);
		dsp_timelowpass_run(motionblur, pp->sizetopoll, syncresult, pp->screenbuffer);

		if (autogain_after_proc) {
			// 最后自动增益
			dsp_autogain_run(&pp->dsp_autogain, pp->sizetopoll, pp->screenbuffer, pp->sendbuffer, lowpasscoeff);
			result = pp->sendbuffer;
		} else
			result = pp->screenbuffer;
	}

	// 定期上报自动增益参数
	if (pp->runs++ > AUTOGAIN_REPORT_EVERY_FRAMES) {
		pp->runs = 0;
		announce_callback_changed(tsdr, VALUE_ID_AUTOGAIN_VALUES, pp->dsp_autogain.lastmin, pp->dsp_autogain.lastmax);
		// TO ENABLE SNR UNCOMMENT THIS announce_callback_changed(tsdr, VALUE_ID_SNR, pp->dsp_autogain.snr, 0);
	}

	return result;
}

/*
 * 释放后处理结构体相关的内存。
 */
void dsp_post_process_free(dsp_postprocess_t * pp) {
	if (pp->screenbuffer != NULL) free(pp->screenbuffer);
	if (pp->sendbuffer != NULL) free(pp->sendbuffer);
	if (pp->corrected_sendbuffer != NULL) free(pp->corrected_sendbuffer);

	if (pp->widthcollapsebuffer != NULL) free(pp->widthcollapsebuffer);
	if (pp->heightcollapsebuffer != NULL) free(pp->heightcollapsebuffer);
}

/*
 * 初始化重采样结构体。
 */
void dsp_resample_init(dsp_resample_t * res) {

	res->contrib = 0;
	res->offset = 0;
}

/*
 * 对输入数据进行重采样，支持最近邻和线性插值两种方式。
 * upsample_by为上采样倍数，downsample_by为下采样倍数。
 */
void dsp_resample_process(dsp_resample_t * res, extbuffer_t * in, extbuffer_t * out, const double upsample_by, const double downsample_by, int nearest_neighbour_sampling) {

	const double sampletimeoverpixel = upsample_by / downsample_by;
	const double pixeloversampletme = downsample_by / upsample_by;

	const uint32_t size = in->size_valid_elements;
	const uint32_t output_samples = (int) ((size - res->offset) * sampletimeoverpixel);

	// 调整输出缓冲区大小
	extbuffer_preparetohandle(out, output_samples);

	uint32_t id;

	float * resbuff = out->buffer;
	float * buffer = in->buffer;

	const double offset_sample = -res->offset * sampletimeoverpixel;

	if (nearest_neighbour_sampling) {
		// 最近邻采样
		for (id = 0; id < output_samples; id++)
			// 直接取最近的输入样本
			*(resbuff++) = buffer[((uint64_t) size * id) / output_samples];
	} else {
		// 线性插值采样
		uint32_t pid = 0;
		for (id = 0; id < size; id++) {
			const double idcheck = id*sampletimeoverpixel + offset_sample;
			const double idcheck3 = idcheck + sampletimeoverpixel;
			const double idcheck2 = idcheck + sampletimeoverpixel - 1.0;

			const double val = *(buffer++);

			// 处理跨越区间的采样点
			if (pid < idcheck && pid < idcheck2) {
				*(resbuff++) = res->contrib + val*(1.0 - idcheck+pid);
				res->contrib = 0;
				pid++;
			}

			// 填充区间内的采样点
			while (pid < idcheck2) {
				*(resbuff++) = val;
				pid++;
			}

			// 处理剩余部分
			if (pid < idcheck3 && pid > idcheck)
				res->contrib += (idcheck3-pid) * val;
			else
				res->contrib += sampletimeoverpixel * val;
		}
	}

	// 更新offset，保证连续性
	res->offset += output_samples*pixeloversampletme-size;
}

/*
 * 释放重采样结构体（当前无实际操作）。
 */
void dsp_resample_free(dsp_resample_t * res) {

}

/*
 * 初始化丢帧补偿结构体。
 */
void dsp_dropped_compensation_init(dsp_dropped_compensation_t * res) {
	// difference为需要丢弃的采样点数
	res->difference = 0;
}

/*
 * 计算丢帧补偿量，使丢弃的采样点数为block的整数倍。
 */
static inline uint64_t dsp_dropped_cal_compensation(const int block, const int dropped) {
	const uint64_t frames = dropped / block;
	return ((frames + 1) * block - dropped) % block;
}

/*
 * 向环形缓冲区添加数据，自动丢弃difference个采样点以实现丢帧补偿。
 */
void dsp_dropped_compensation_add(dsp_dropped_compensation_t * res, CircBuff_t * cb, float * buff, const uint32_t size, uint32_t block) {
	assert(res->difference >= 0);

	// 如果本次数据全部需要丢弃
	if (size <= res->difference)
		res->difference -= size;
	// 可以正常写入环形缓冲区
	else if (cb_add(cb, &buff[res->difference], size-res->difference) == CB_OK)
		res->difference = 0;
	else {
		// 如果写入失败，调整difference为block对齐
		res->difference -= size % block;
		if (res->difference < 0) res->difference = dsp_dropped_cal_compensation(block, -res->difference);
	}
}

/*
 * 判断本次数据是否全部需要丢弃。
 */
int dsp_dropped_compensation_will_drop_all(dsp_dropped_compensation_t * res, uint32_t size, uint32_t block) {
	assert(res->difference >= 0);

	return size <= res->difference;
}

/*
 * 根据同步偏移调整丢帧补偿量。
 */
void dsp_dropped_compensation_shift_with(dsp_dropped_compensation_t * res, uint32_t block, int64_t syncoffset) {
	if (syncoffset >= 0)
		res->difference -= syncoffset % block;
	else
		res->difference -= block + syncoffset % block;

	// 保证difference为正且block对齐
	if (res->difference < 0) res->difference = dsp_dropped_cal_compensation(block, -res->difference);
}
