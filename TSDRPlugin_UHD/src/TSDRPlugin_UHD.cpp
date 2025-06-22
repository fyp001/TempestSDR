/*
#-------------------------------------------------------------------------------
# Copyright (c) 2014 Martin Marinov.
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the GNU Public License v3.0
# which accompanies this distribution, and is available at
# http://www.gnu.org/licenses/gpl.html
# 
# 贡献者:
#     Martin Marinov - 初始API和实现
#-------------------------------------------------------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uhd/utils/thread.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <uhd/exception.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>

#include <iostream>
#include <complex>

#include "TSDRPlugin.h"

#include "TSDRCodes.h"

#include <stdint.h>
#include <boost/algorithm/string.hpp>

#include "errors.hpp"

// 回调函数调用的频率（秒）
#define HOW_OFTEN_TO_CALL_CALLBACK_SEC (0.06)
// 可容忍的丢包率
#define FRACT_DROPPED_TO_TOLERATE (0)

// UHD USRP设备智能指针
uhd::usrp::multi_usrp::sptr usrp;
namespace po = boost::program_options;

// 请求的中心频率
uint32_t req_freq = 105e6;
// 请求的增益
float req_gain = 1;
// 请求的采样率
double req_rate = 25e6;
// 运行状态标志
volatile int is_running = 0;

/**
 * @brief 获取插件的名称。
 * @param name [out] 用于存储插件名称的字符数组。
 */
EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_getName(char * name) {
	strcpy(name, "TSDR UHD USRP Compatible Plugin");
}

/**
 * @brief 将0-1范围的归一化增益转换为USRP设备特定的增益值。
 * @param gain [in] 归一化的增益值 (0.0f - 1.0f)。
 * @return USRP硬件实际使用的增益值。
 */
double tousrpgain(float gain) {
	try {
		uhd::gain_range_t range = usrp->get_rx_gain_range();
		return gain * (range.stop() - range.start()) + range.start();
	}
	catch (std::exception const&  ex)
	{
		// 如果获取失败，返回一个默认范围计算值
		return gain * 60;
	}
}

/**
 * @brief 初始化UHD插件和USRP设备。
 * @param params [in] 包含设备参数的字符串，格式类似命令行参数。
 * @return 成功返回TSDR_OK，失败返回错误码。
 */
EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_init(const char * params) {
	// 模拟argc和argv，用于boost::program_options解析
	std::string sparams(params);

	typedef std::vector< std::string > split_vector_type;

	split_vector_type argscounter;
	boost::split( argscounter, sparams, boost::is_any_of(" "), boost::token_compress_on );

	const int argc = argscounter.size()+1;
	char ** argv = (char **) malloc(argc*sizeof(char *));
	char zerothtarg[] = "TSDRPlugin_UHD";
	argv[0] = (char *) zerothtarg;
	for (int i = 0; i < argc-1; i++)
		argv[i+1] = (char *) argscounter[i].c_str();

	// program_options将设置这些变量
	std::string args, file, ant, subdev, ref, tsrc;
	double bw;

	// 设置程序选项
	po::options_description desc("允许的选项");
	desc.add_options()
			("args", po::value<std::string>(&args)->default_value(""), "multi uhd设备地址参数")
			("ant", po::value<std::string>(&ant), "子板天线选择")
			("rate", po::value<double>(&req_rate)->default_value(req_rate), "输入采样率")
			("subdev", po::value<std::string>(&subdev), "子板规格")
			("bw", po::value<double>(&bw), "子板中频滤波器带宽 (Hz)")
			("ref", po::value<std::string>(&ref)->default_value("internal"), "时钟源 (internal, external, mimo)")
			("tsrc", po::value<std::string>(&tsrc)->default_value("external"), "时间源 (none, external, _external_, mimo)") ;

	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	} catch (std::exception const&  ex)
	{
		std::string msg(boost::str(boost::format("错误: %s\n\nTSDRPlugin_UHD %s") % ex.what() % desc));
		RETURN_EXCEPTION(msg.c_str(), TSDR_PLUGIN_PARAMETERS_WRONG);
	}

	try {
		// 创建一个usrp设备
		usrp = uhd::usrp::multi_usrp::make(args);

		// 锁定主板时钟
		usrp->set_clock_source(ref);
		if (vm.count("tsrc")) usrp->set_time_source(tsrc);

		usrp->set_rx_rate(req_rate);
		req_rate = usrp->get_rx_rate();

		// 设置接收中心频率
		usrp->set_rx_freq(req_freq);

		// 设置接收射频增益
		usrp->set_rx_gain(tousrpgain(req_gain));
		// 设置天线
		if (vm.count("ant")) usrp->set_rx_antenna(ant);

		// 设置中频滤波器带宽
		if (vm.count("bw"))
			usrp->set_rx_bandwidth(bw);

		boost::this_thread::sleep(boost::posix_time::seconds(1)); // 留出一些设置时间

		// 检查参考时钟和本振锁定检测
		std::vector<std::string> rx_sensor_names;
		rx_sensor_names = usrp->get_rx_sensor_names(0);
		if (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "lo_locked") != rx_sensor_names.end()) {
			uhd::sensor_value_t lo_locked = usrp->get_rx_sensor("lo_locked",0);
			UHD_ASSERT_THROW(lo_locked.to_bool());
		}
		std::vector<std::string>  sensor_names = usrp->get_mboard_sensor_names(0);
		if ((ref == "mimo") and (std::find(sensor_names.begin(), sensor_names.end(), "mimo_locked") != sensor_names.end())) {
			uhd::sensor_value_t mimo_locked = usrp->get_mboard_sensor("mimo_locked",0);
			UHD_ASSERT_THROW(mimo_locked.to_bool());
		}
		if ((ref == "external") and (std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end())) {
			uhd::sensor_value_t ref_locked = usrp->get_mboard_sensor("ref_locked",0);
			UHD_ASSERT_THROW(ref_locked.to_bool());
		}
	} catch (std::exception const&  ex)
	{
		free(argv);
		RETURN_EXCEPTION(ex.what(), TSDR_CANNOT_OPEN_DEVICE);
	}

	free(argv);
	RETURN_OK();

	return 0; // 避免Eclipse报警告
}

/**
 * @brief 设置USRP设备的采样率。
 * @param rate [in] 请求的采样率。
 * @return 硬件实际设置的采样率。
 */
EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_setsamplerate(uint32_t rate) {
	if (is_running)
		return tsdrplugin_getsamplerate();

	req_rate = rate;

	try {
		usrp->set_rx_rate(req_rate);
		double real_rate = usrp->get_rx_rate();
		req_rate = real_rate;
	}
	catch (std::exception const&  ex)
	{
	}

	return req_rate;
}

/**
 * @brief 获取USRP设备当前的采样率。
 * @return 当前的采样率。
 */
EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_getsamplerate() {

	try {
		req_rate = usrp->get_rx_rate();
	}
	catch (std::exception const&  ex)
	{
	}

	return req_rate;
}

/**
 * @brief 设置USRP设备的中心频率。
 * @param freq [in] 请求的中心频率。
 * @return 成功返回TSDR_OK。
 */
EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setbasefreq(uint32_t freq) {
	req_freq = freq;

	try {
		usrp->set_rx_freq(req_freq);
	}
	catch (std::exception const&  ex)
	{
	}

	RETURN_OK();

	return 0; // 避免Eclipse报警告
}

/**
 * @brief 停止异步读取循环。
 * @return 成功返回TSDR_OK。
 */
EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_stop(void) {
	is_running = 0;
	RETURN_OK();

	return 0; // 避免Eclipse报警告
}

/**
 * @brief 设置USRP设备的接收增益。
 * @param gain [in] 归一化的增益值 (0.0f - 1.0f)。
 * @return 成功返回TSDR_OK。
 */
EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setgain(float gain) {
	req_gain = gain;
	try {
		usrp->set_rx_gain(tousrpgain(req_gain));
	}
	catch (std::exception const&  ex)
	{
	}
	RETURN_OK();

	return 0; // 避免Eclipse报警告
}

/**
 * @brief 异步读取USRP数据。
 * @details 这是一个阻塞函数，它会启动一个循环来从USRP接收数据，
 *          并将数据存入缓冲区。当缓冲区满时，通过回调函数cb将数据传出。
 * @param cb [in] 数据回调函数指针。
 * @param ctx [in] 传递给回调函数的用户上下文指针。
 * @return 成功返回TSDR_OK，失败返回错误码。
 */
EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_readasync(tsdrplugin_readasync_function cb, void *ctx) {
	uhd::set_thread_priority_safe();

	is_running = 1;

	float * buff = NULL;

	try {
		// 设置接收采样率
		usrp->set_rx_rate(req_rate);

		// 设置接收中心频率
		usrp->set_rx_freq(req_freq);

		// 设置接收射频增益
		usrp->set_rx_gain(tousrpgain(req_gain));

		// 创建一个接收流
		uhd::stream_args_t stream_args("fc32"); // 复数浮点数
		uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

		// 循环直到达到总样本数
		// 1个样本 = 2个item (I和Q)
		uhd::rx_metadata_t md;
		md.has_time_spec = false;

		size_t buff_size = HOW_OFTEN_TO_CALL_CALLBACK_SEC * req_rate * 2;
		const size_t samples_per_api_read = rx_stream->get_max_num_samps();
		if (buff_size < samples_per_api_read * 2) buff_size = samples_per_api_read * 2;
		buff = (float *) malloc(sizeof(float) * buff_size);
		const size_t items_per_api_read = samples_per_api_read*2;

		// 初始化计数器
		size_t items_in_buffer = 0;

		const uint64_t samp_rate_uint = req_rate;
		const double samp_rate_fract = req_rate - (double) samp_rate_uint;
		// 设置流
		usrp->set_time_now(uhd::time_spec_t(0.0));
		rx_stream->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);

		uint64_t last_firstsample = 0;

		while(is_running){
			// 如果下一次读取将溢出我们的缓冲区，则调用回调函数
			if (items_per_api_read + items_in_buffer > buff_size) {
				int64_t dropped_samples = 0;
				const uint64_t samples_in_buffer = items_in_buffer >> 1;

				// 估算丢弃的样本数
				if (md.has_time_spec) {
					const uint64_t roundsecs = (uint64_t) md.time_spec.get_full_secs();
					uint64_t first_sample_id = roundsecs * samp_rate_uint;
					first_sample_id += roundsecs * samp_rate_fract + 0.5;
					first_sample_id += md.time_spec.get_frac_secs() * req_rate + 0.5;

					// 我们应该在first_sample_id中得到第一个样本的ID
					const uint64_t expected_first_sample_id = last_firstsample + samples_in_buffer;
					const int64_t dropped_samples_now = first_sample_id - expected_first_sample_id;

					dropped_samples = dropped_samples_now;

					// 估算下一帧
					last_firstsample = first_sample_id;
				}

				if (dropped_samples <= 0)
					cb(buff, items_in_buffer, ctx, 0); // 没有丢包，很好
				else if ((dropped_samples / ((float) samples_in_buffer)) < FRACT_DROPPED_TO_TOLERATE)
					cb(buff, items_in_buffer, ctx, dropped_samples); // 部分数据丢失，但可接受
				else
					cb(buff, 0, ctx, dropped_samples + samples_in_buffer); // 丢失过多，中止

				// 重置计数器，原生缓冲区已空
				items_in_buffer = 0;
			}

			size_t num_rx_samps = rx_stream->recv(
					&buff[items_in_buffer], samples_per_api_read, md
			);

			// 可视化间隙
			items_in_buffer+=(num_rx_samps << 1);

			// 处理错误码
			switch(md.error_code){
			case uhd::rx_metadata_t::ERROR_CODE_NONE:
				break;

			case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
				std::cout << boost::format(
						"在接收所有样本前超时，可能发生丢包。"
				) << std::endl;
				break;
			case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
				//printf("Overflow!\n"); fflush(stdout);
				break;

			default:
				std::cout << boost::format(
						"收到错误码 0x%x, 退出循环..."
				) % md.error_code << std::endl;
				goto done_loop;
			}

		} done_loop:

		usrp->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);

		// 清空USRP缓冲区
	    while(rx_stream->recv(
	        buff, samples_per_api_read, md
	    )){
	        /* 无操作 */
	    };
	}
	catch (std::exception const&  ex)
	{
		is_running = 0;
		if (buff!=NULL) free(buff);
		RETURN_EXCEPTION(ex.what(), TSDR_CANNOT_OPEN_DEVICE);
	}
	if (buff!=NULL) free(buff);
	RETURN_OK();

	return 0; // 避免Eclipse报警告
}

/**
 * @brief 清理并释放插件资源。
 */
EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_cleanup(void) {

	try {
		usrp.reset();
		boost::this_thread::sleep(boost::posix_time::seconds(1));
	} catch (std::exception const&  ex) {

	}

	is_running = 0;
}
