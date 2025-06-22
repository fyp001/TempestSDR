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
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "extbuffer.h"

/*
 * 初始化extbuffer结构体为float类型，重置所有成员。
 */
void extbuffer_init(extbuffer_t * container) {
	container->buffer = NULL;
	container->dbuffer = NULL;
	container->buffer_max_size = 0;
	container->size_valid_elements = 0;

	container->valid = 0;
	container->cleartozero = 1;
	container->calls = 0;

	container->calls = 0;
	container->type = EXTBUFFER_TYPE_FLOAT;
}

/*
 * 初始化extbuffer结构体为double类型，重置所有成员。
 */
void extbuffer_init_double(extbuffer_t * container) {
	container->buffer = NULL;
	container->dbuffer = NULL;
	container->buffer_max_size = 0;
	container->size_valid_elements = 0;

	container->valid = 0;
	container->cleartozero = 1;
	container->calls = 0;

	container->calls = 0;
	container->type = EXTBUFFER_TYPE_DOUBLE;
}

/*
 * 为extbuffer分配或调整缓冲区大小，并根据类型初始化内容。
 * size为需要的元素数量。
 */
void extbuffer_preparetohandle(extbuffer_t * container, uint32_t size) {
	assert (size > 0); // 保证size大于0

	// 判断是否需要重新分配内存
	if (container->buffer_max_size < size || container->buffer_max_size > (size << 1)) {
		if (container->type == EXTBUFFER_TYPE_FLOAT) {
			if (container->buffer == NULL) {
				container->buffer = (float *) malloc(sizeof(float) * size); // 分配float缓冲区
				container->valid = 1;
			} else if (container->buffer_max_size != size)
				container->buffer = (float *) realloc((void *) container->buffer, sizeof(float) * size); // 扩容
		} else if (container->type == EXTBUFFER_TYPE_DOUBLE) {
			if (container->dbuffer == NULL) {
				container->dbuffer = (double *) malloc(sizeof(double) * size); // 分配double缓冲区
				container->valid = 1;
			} else if (container->buffer_max_size != size)
				container->dbuffer = (double *) realloc((void *) container->dbuffer, sizeof(double) * size); // 扩容
		}
		container->buffer_max_size = size;
	}

	container->size_valid_elements = size;
	if (container->cleartozero) {
		uint32_t i;
		if (container->type == EXTBUFFER_TYPE_FLOAT) {
			for (i = 0; i < container->size_valid_elements; i++)
				container->buffer[i] = 0.0f; // 清零
		} else if (container->type == EXTBUFFER_TYPE_DOUBLE) {
			for (i = 0; i < container->size_valid_elements; i++)
				container->dbuffer[i] = 0.0f; // 清零
		}
		container->cleartozero = 0;
		container->calls = 0;
	}

	container->calls++;
}

/*
 * 标记extbuffer需要在下次使用时清零。
 */
void extbuffer_cleartozero(extbuffer_t * container) {
	container->cleartozero = 1;
}

/*
 * 释放extbuffer相关的内存资源。
 */
void extbuffer_free(extbuffer_t * container) {

	container->valid = 0;
	if (container->buffer != NULL) {
		float * buff = container->buffer;
		container->buffer = NULL;
		free(buff); // 释放float缓冲区
	}
	if (container->dbuffer != NULL) {
		double * dbuff = container->dbuffer;
		container->dbuffer = NULL;
		free(dbuff); // 释放double缓冲区
	}
}

/*
 * 将extbuffer中的数据导出到文件，支持float和double类型。
 * offset为横坐标起始值，filename为文件名，xname/yname为表头。
 */
void extbuffer_dumptofile(extbuffer_t * container, int offset, char * filename, char * xname, char * yname) {
	assert (container->valid); // 保证数据有效

	FILE *f = NULL;

	f = fopen(filename, "w"); // 打开文件

	fprintf(f, "%s, %s\n", xname, yname); // 写入表头

	int i;
	if (container->type == EXTBUFFER_TYPE_FLOAT) {
		for (i = 0; i < container->size_valid_elements; i++)
			fprintf(f, "%d, %f\n", offset + i, container->buffer[i]); // 写入float数据
	} else if (container->type == EXTBUFFER_TYPE_DOUBLE) {
		for (i = 0; i < container->size_valid_elements; i++)
			fprintf(f, "%d, %f\n", offset + i, container->dbuffer[i]); // 写入double数据
	}

	fclose(f); // 关闭文件
}
