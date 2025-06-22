#include "fft.h"
#include <math.h>
#include <string.h>

/*
 * 获取小于等于size的最大2的幂次，用于FFT长度。
 */
uint32_t fft_getrealsize(uint32_t size) {
	uint32_t m =0;
	while ((size /= 2) != 0)
		m++;

	return 1 << m;
}

/*
 * 将实数数组转换为复数数组，虚部补零。
 * out长度为2*in_size。
 */
void real_to_complex(float * out, float * in, int in_size) {
	uint32_t i;

	// 实部赋值，虚部补零
	for (i = 0; i < in_size; i++) {
		*(out++) = *(in++);
		*(out++) = 0.0f;
	}
}

/*
 * 将复数数组转换为实数幅值数组，结果为模长。
 * data长度为2*samples，输出samples个幅值。
 */
void complex_to_real(float * data, int samples){
	float * src = data;
	uint32_t i;
	for (i = 0; i < samples; i++) {
		const float I = *(src++);
		const float Q = *(src++);
		*(data++) = sqrtf(I*I+Q*Q); // 计算模长
	}
}

/*
 * 将复数数组转为幅值，虚部清零。
 * data长度为2*samples。
 */
void fft_complex_to_absolute_complex(float * data, int samples) {
	uint32_t fft_size2 = samples * 2;

	uint32_t i;
	for (i = 0; i < fft_size2; i+=2) {
		const int i1 = i+1;
		const float I = data[i];
		const float Q = data[i1];
		data[i] = sqrtf(I*I+Q*Q); // 计算模长
		data[i1] = 0;
	}
}

/*
 * 对实数序列进行自相关，结果写入answer。
 * answer长度为2*size。
 */
void fft_autocorrelation(float * answer, float * real, uint32_t size) {

	// 实数转复数
	real_to_complex(answer, real, size);

	// FFT变换
	uint32_t fft_size = fft_getrealsize(size);

	fft_perform(answer, fft_size, 0);

	// 取幅值
	fft_complex_to_absolute_complex(answer, size);

	// 逆FFT
	fft_perform(answer, fft_size, 1);
}

/*
 * 计算两个复数序列的互相关，结果写入answer_out。
 * answer_out和answer_temp长度均为2*samples。
 */
void fft_crosscorrelation(float * answer_out, float * answer_temp, uint32_t samples) {
	uint32_t i;

	uint32_t fft_size = fft_getrealsize(samples);
	uint32_t fft_size2 = fft_size * 2;

	// 对两个输入做FFT
	fft_perform(answer_out, fft_size, 0);
	fft_perform(answer_temp, fft_size, 0);

	// 复数共轭相乘
	for (i = 0; i < fft_size2; i+=2) {
		const int i1 = i+1;
		const float aI = answer_out[i];
		const float aQ = answer_out[i1];
		const float bI = answer_temp[i];
		const float bQ = answer_temp[i1];

		answer_out[i]  = aI*bI + aQ*bQ;
		answer_out[i1] = aI*bQ - aQ*bI;
	}

	// 逆FFT
	fft_perform(answer_out, fft_size, 1);
}

/*
 * 对复数数组进行FFT或逆FFT。
 * iq为输入输出数组，size为点数，inverse为1时做逆变换。
 */
void fft_perform(float * iq, uint32_t size, int inverse)
{
	int64_t i,i1,j,k,i2,l,l1,l2;
	double c1,c2,tx,ty,t1,t2,u1,u2,z;

	int m =0;
	while ((size /= 2) != 0)
		m++;

	uint32_t nn = 1 << m;
	i2 = nn >> 1;
	j = 0;

	// 位反转置换
	for (i=0;i<nn-1;i++) {
		if (i < j) {
			const uint32_t Ii = i << 1;
			const uint32_t Qi = Ii + 1;

			const uint32_t Ij = j << 1;
			const uint32_t Qj = Ij + 1;

			tx = iq[Ii];
			ty = iq[Qi];
			iq[Ii] = iq[Ij];
			iq[Qi] = iq[Qj];
			iq[Ij] = tx;
			iq[Qj] = ty;
		}
		k = i2;
		while (k <= j) {
			j -= k;
			k >>= 1;
		}
		j += k;
	}

	c1 = -1.0;
	c2 = 0.0;
	l2 = 1;
	for (l=0;l<m;l++) {
		l1 = l2;
		l2 <<= 1;
		u1 = 1.0f;
		u2 = 0.0f;
		for (j=0;j<l1;j++) {
			for (i=j;i<nn;i+=l2) {
				const uint32_t Ii = i << 1;
				const uint32_t Qi = Ii + 1;

				i1 = i + l1;

				const uint32_t Ii1 = i1 << 1;
				const uint32_t Qi1 = Ii1 + 1;

				t1 = u1 * iq[Ii1] - u2 * iq[Qi1];
				t2 = u1 * iq[Qi1] + u2 * iq[Ii1];
				iq[Ii1] = iq[Ii] - t1;
				iq[Qi1] = iq[Qi] - t2;
				iq[Ii] += t1;
				iq[Qi] += t2;
			}
			z =  u1 * c1 - u2 * c2;
			u2 = u1 * c2 + u2 * c1;
			u1 = z;
		}
		c2 = sqrt((1.0 - c1) / 2.0); // 旋转因子递推
		if (!inverse)
			c2 = -c2;
		c1 = sqrt((1.0 + c1) / 2.0);
	}

	if (!inverse) {
		for (i=0;i<nn;i++) {
			const uint32_t Ii = i << 1;
			const uint32_t Qi = Ii + 1;

			iq[Ii] /= (float)nn;
			iq[Qi] /= (float)nn;
		}
	}
}
