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
#include "circbuff.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if ASSERT_ENABLED
#include <assert.h>
#endif

#define CB_SIZE_COEFF_DEFAULT (2)

/*
 * 清空环形缓冲区，重置所有指针和容量。
 */
void cb_purge(CircBuff_t * cb) {
	if (cb->invalid) return;
	critical_enter(&cb->mutex);

    cb->remaining_capacity = cb->buffer_size; // 可写入的元素数量
    cb->pos = 0; // 下一个写入元素的位置
    cb->rempos = 0; // 下一个读取元素的位置

	if (cb->is_waiting) mutex_signal(&cb->locker); // 如果有线程等待，唤醒

	critical_leave(&cb->mutex);
}

/*
 * 初始化环形缓冲区结构体，分配内存并初始化互斥锁。
 * max_size_coeff为最大扩展系数。
 */
void cb_init(CircBuff_t * cb, int max_size_coeff) {
#if ASSERT_ENABLED
	assert(max_size_coeff >= CB_SIZE_COEFF_DEFAULT);
#endif

	cb->max_size_coeff = max_size_coeff;
	cb->size_coeff = CB_SIZE_COEFF_DEFAULT;
    cb->desired_buf_size = cb->size_coeff; // 初始缓冲区大小
    cb->buffer_size = cb->desired_buf_size;
    cb->buffer = (float *) malloc(sizeof(float) * cb->buffer_size); // 分配缓冲区内存
    cb->remaining_capacity = cb->buffer_size; // 可写入的元素数量
    cb->pos = 0; // 下一个写入元素的位置
    cb->rempos = 0; // 下一个读取元素的位置
    cb->invalid = 0;

    cb->is_waiting = 0;
    cb->buffering = 0;

    mutex_init(&cb->mutex); // 初始化互斥锁
    mutex_init(&cb->locker);
}

/*
 * 获取当前缓冲区中已存储的元素数量。
 */
int cb_size(CircBuff_t * cb) {
	return cb->buffer_size - cb->remaining_capacity;
}

/*
 * 向环形缓冲区添加数据。
 * in为输入数据，len为长度。
 * 支持自动扩容和环绕写入。
 */
int cb_add(CircBuff_t * cb, float * in, const size_t len) {
	if (cb->invalid) return CB_ERROR;
    if (len <= 0) return CB_OK; // 边界情况，长度为0直接返回

    critical_enter(&cb->mutex);

#if ASSERT_ENABLED
    assert(((cb->pos + cb->remaining_capacity) % cb->buffer_size) == cb->rempos);
#endif

    // 如果缓冲区不够大，设置扩容需求
    if (len*cb->size_coeff > cb->buffer_size) cb->desired_buf_size = len*cb->size_coeff;

    // 实际扩容
    if (cb->buffer_size < cb->desired_buf_size) {
    	const size_t items_inside = cb->buffer_size - cb->remaining_capacity;
    	const size_t inflation = cb->desired_buf_size - cb->buffer_size;

        // 重新分配内存
        cb->buffer = (float *) realloc((void *) cb->buffer, sizeof(float) * cb->desired_buf_size); // 扩容

        // 如果rempos在pos之后，需移动数据
        if (cb->rempos >= cb->pos) {
        	memmove((void *) &cb->buffer[cb->rempos+inflation], (void *) &cb->buffer[cb->rempos], sizeof(float) * (cb->buffer_size-cb->rempos));
        	if (items_inside != 0) cb->rempos += inflation;
        }

        cb->remaining_capacity += inflation;

        cb->buffer_size = cb->desired_buf_size; // 更新缓冲区大小

#if ASSERT_ENABLED
        assert(cb->buffer_size - cb->remaining_capacity == items_inside);
#endif
    }

    // 判断是否有足够空间写入数据
    if (cb->buffering && cb->remaining_capacity < 2*len) {
    	cb->buffering = 0;
    	critical_leave(&cb->mutex);
    	return CB_FULL; // 空间不足，返回满
    } else if (cb->remaining_capacity < len) {
    	cb->buffering = 1;
    	if (cb->size_coeff < cb->max_size_coeff) cb->size_coeff++;
        critical_leave(&cb->mutex);
        return CB_FULL; // 空间不足，返回满
    }

    // 计算写入位置
    const size_t oldpos = cb->pos;
    cb->pos = (oldpos + len) % cb->buffer_size; // 新的写入位置
    cb->remaining_capacity -= len; // 剩余容量减少

    if (cb->pos <= oldpos) {
        // 写入操作需要环绕
        const size_t remaining = cb->buffer_size - oldpos;
        memcpy((void *) &cb->buffer[oldpos], in, remaining * sizeof(float));
        memcpy((void *) cb->buffer, &in[remaining], cb->pos * sizeof(float));
    } else {
        // 写入操作不需要环绕
        memcpy((void *) &cb->buffer[oldpos], in, len*sizeof(float));
    }

    if (cb->is_waiting) mutex_signal(&cb->locker); // 唤醒等待线程

    critical_leave(&cb->mutex);

    return CB_OK;
}

/*
 * 非阻塞方式从环形缓冲区取出数据。
 * in为输出数据，len为长度。
 * 如果数据不足，立即返回CB_EMPTY。
 */
int cb_rem_nonblocking(CircBuff_t * cb, float * in, const size_t len) {
	if (cb->invalid) return CB_ERROR;
	if (len <= 0) return CB_OK;

	size_t items_inside = cb->buffer_size - cb->remaining_capacity;
    while (items_inside < len) return CB_EMPTY; // 数据不足直接返回

    if (cb->invalid) return CB_ERROR;
    critical_enter(&cb->mutex);

#if ASSERT_ENABLED
    assert(((cb->pos + cb->remaining_capacity) % cb->buffer_size) == cb->rempos);
#endif

    if (cb->buffer_size - cb->remaining_capacity < len) {
        critical_leave(&cb->mutex);
        return CB_EMPTY;
    }

    // 计算读取位置
    const size_t oldrempos = cb->rempos;
    cb->rempos = (oldrempos + len) % cb->buffer_size; // 新的读取位置

    if (cb->rempos <= oldrempos) {
        // 读取操作需要环绕
        const size_t remaining = cb->buffer_size - oldrempos;
        memcpy(in, (void *) &cb->buffer[oldrempos], remaining*sizeof(float));
        memcpy(&in[remaining], (void *) cb->buffer, cb->rempos*sizeof(float));
    } else {
        // 读取操作不需要环绕
        memcpy(in, (void *) &cb->buffer[oldrempos], len*sizeof(float));
    }

    cb->remaining_capacity += len; // 已移除len个元素

    critical_leave(&cb->mutex);

    return CB_OK;
}

/*
 * 阻塞方式从环形缓冲区取出数据。
 * in为输出数据，len为长度。
 * 如果数据不足，会等待直到有足够数据或超时。
 */
int cb_rem_blocking(CircBuff_t * cb, float * in, const size_t len) {
	if (cb->invalid) return CB_ERROR;
	if (len <= 0) return CB_OK;

	size_t items_inside = cb->buffer_size - cb->remaining_capacity;
    while (items_inside < len) {
            // 如果缓冲区不够大，设置扩容需求
            if (len*cb->size_coeff > cb->buffer_size) cb->desired_buf_size = len*cb->size_coeff;

            const size_t before_items_inside = items_inside;
            cb->is_waiting = 1;
            if (mutex_wait(&cb->locker) == THREAD_TIMEOUT) {
            	cb->is_waiting = 0;
            	return CB_EMPTY; // 超时返回
            }

            cb->is_waiting = 0;
            items_inside = cb->buffer_size - cb->remaining_capacity;
            if (before_items_inside == items_inside)
                return CB_EMPTY; // 数据依然不足，返回
    }

    if (cb->invalid) return CB_ERROR;
    critical_enter(&cb->mutex);

#if ASSERT_ENABLED
    assert(((cb->pos + cb->remaining_capacity) % cb->buffer_size) == cb->rempos);
#endif

    if (cb->buffer_size - cb->remaining_capacity < len) {
        critical_leave(&cb->mutex);
        return CB_EMPTY;
    }

    // 计算读取位置
    const size_t oldrempos = cb->rempos;
    cb->rempos = (oldrempos + len) % cb->buffer_size; // 新的读取位置

    if (cb->rempos <= oldrempos) {
        // 读取操作需要环绕
        const size_t remaining = cb->buffer_size - oldrempos;
        memcpy(in, (void *) &cb->buffer[oldrempos], remaining*sizeof(float));
        memcpy(&in[remaining], (void *) cb->buffer, cb->rempos*sizeof(float));
    } else {
        // 读取操作不需要环绕
        memcpy(in, (void *) &cb->buffer[oldrempos], len*sizeof(float));
    }

    cb->remaining_capacity += len; // 已移除len个元素

    critical_leave(&cb->mutex);

    return CB_OK;
}

/*
 * 释放环形缓冲区相关资源，包括内存和互斥锁。
 */
void cb_free(CircBuff_t * cb) {
	if (cb->invalid) return;

	critical_enter(&cb->mutex);
	free((void *) cb->buffer); // 释放内存
	cb->invalid = 1;

	if (cb->is_waiting) mutex_signal(&cb->locker); // 唤醒等待线程

	critical_leave(&cb->mutex);

    mutex_free(&cb->locker); // 释放互斥锁
    mutex_free(&cb->mutex);

}

