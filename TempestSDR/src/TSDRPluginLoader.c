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

#include "TSDRPluginLoader.h"
#include "include/TSDRLibrary.h"
#include "include/TSDRCodes.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// 跨平台动态库加载器

/*
 * 获取动态库中的函数指针。
 * plugin为插件结构体，functname为函数名。
 */
void *tsdrplug_getfunction(pluginsource_t * plugin, char *functname)
{
#if WINHEAD
    // Windows下通过GetProcAddress获取函数地址
    return (void*)GetProcAddress((HINSTANCE)plugin->fd,functname);
#else
    // Linux下通过dlsym获取函数地址
    return dlsym(plugin->fd,functname);
#endif
}

/*
 * 加载插件动态库，并获取所有必要的函数指针。
 * 返回TSDR_OK或错误码。
 */
int tsdrplug_load(pluginsource_t * plugin, const char *dlname)
{
	plugin->tsdrplugin_cleanup = NULL;

    #if WINHEAD // Microsoft编译器
        plugin->fd = (void*)LoadLibrary(dlname);

       if (plugin->fd == NULL) {
        char err_str[512];
        // 获取系统错误信息
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,
            NULL, GetLastError(), 
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            err_str, 512, NULL);
        fprintf(stderr,"Library %s load exception: %s\n", dlname, err_str);
        }
    #else
        plugin->fd = dlopen(dlname,RTLD_NOW);
        if (plugin->fd == NULL)
        	fprintf(stderr,"Library %s load exception: %s\n", dlname, dlerror());
    #endif

    if (plugin->fd == NULL)
    	return TSDR_INCOMPATIBLE_PLUGIN;

    // 获取插件各接口函数指针，任一失败则返回错误
    if ((plugin->tsdrplugin_init = tsdrplug_getfunction(plugin, "tsdrplugin_init")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_getsamplerate = tsdrplug_getfunction(plugin, "tsdrplugin_getsamplerate")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_getName = tsdrplug_getfunction(plugin, "tsdrplugin_getName")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_setsamplerate = tsdrplug_getfunction(plugin, "tsdrplugin_setsamplerate")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_setbasefreq = tsdrplug_getfunction(plugin, "tsdrplugin_setbasefreq")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_stop = tsdrplug_getfunction(plugin, "tsdrplugin_stop")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_setgain = tsdrplug_getfunction(plugin, "tsdrplugin_setgain")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_readasync = tsdrplug_getfunction(plugin, "tsdrplugin_readasync")) == 0) return TSDR_ERR_PLUGIN;
    if ((plugin->tsdrplugin_getlasterrortext = tsdrplug_getfunction(plugin, "tsdrplugin_getlasterrortext")) == 0) return TSDR_ERR_PLUGIN;

    // cleanup函数必须最后获取
    if ((plugin->tsdrplugin_cleanup = tsdrplug_getfunction(plugin, "tsdrplugin_cleanup")) == 0) return TSDR_ERR_PLUGIN;

    plugin->initialized = 1;
    return TSDR_OK;
}

/*
 * 卸载插件，释放动态库资源。
 */
void tsdrplug_close(pluginsource_t * plugin)
{
	if (!plugin->initialized) return;
	if (plugin->fd == NULL) return;
	if (plugin->tsdrplugin_cleanup != NULL) plugin->tsdrplugin_cleanup(); // 卸载前先清理
	plugin->initialized = 0;
#if WINHEAD
    FreeLibrary((HINSTANCE)plugin->fd);
#else
    dlclose(plugin->fd);
#endif

}
