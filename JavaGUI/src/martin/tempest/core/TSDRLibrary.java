/*******************************************************************************
 * 版权所有 (c) 2014 Martin Marinov.
 * 保留所有权利。本程序及其附带的材料根据 GNU 通用公共许可证 v3.0 发布，
 * 许可证随附于本发行版，也可在 http://www.gnu.org/licenses/gpl.html 获取。
 * 
 * 贡献者：
 *     Martin Marinov - 初始API和实现
 ******************************************************************************/
package martin.tempest.core;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

import martin.tempest.core.exceptions.TSDRAlreadyRunningException;
import martin.tempest.core.exceptions.TSDRException;
import martin.tempest.core.exceptions.TSDRLibraryNotCompatible;
import martin.tempest.gui.VideoMode;
import martin.tempest.sources.TSDRSource;

/**
 * 这是 TSDRLibrary 的 Java 封装库。只要本地库为对应操作系统编译，并且本地dll位于 lib/OSNAME/ARCH 或 LD_LIBRARY_PATH 下，
 * 就可以实现平台无关的调用。它控制本地库并暴露其全部功能。
 * 
 * @author Martin Marinov
 *
 */
public class TSDRLibrary {
	private final static ArrayList<String> files_to_delete_on_shutdown = new ArrayList<String>();
	
	/** NDK将写入像素的图像对象 */
	private BufferedImage bimage;
	/** 指向 {@link #bimage} 像素的指针 */
	private volatile int[] pixels;
	
	/** 手动同步的方向 */
	public enum SYNC_DIRECTION {ANY, UP, DOWN, LEFT, RIGHT};
	
	public enum PARAM {AUTOSHIFT, PLLFRAMERATE, AUTOCORR_PLOTS_RESET, AUTOCORR_PLOTS_OFF, SUPERRESOLUTION, NEAREST_NEIGHBOUR_RESAMPLING, LOW_PASS_BEFORE_SYNC, AUTOGAIN_AFTER_PROCESSING, AUTOCORR_DUMP};
	public enum PARAM_DOUBLE {};
	
	/** 本地是否正在运行 */
	volatile private boolean nativerunning = false;
	
	private final Object float_array_locker = new Object();
	private int float_array_locker_count = 0;
	private double[] double_array;
	
	/** 如果二进制库未加载，将抛出此异常 */
	private static TSDRLibraryNotCompatible m_e = null;
	
	/** 所有注册的帧回调列表 */
	private final List<FrameReadyCallback> callbacks = new ArrayList<FrameReadyCallback>();
	
	private final List<IncomingValueCallback> value_callbacks = new ArrayList<IncomingValueCallback>();
	
	/**
	 * 根据操作系统返回对应的动态库文件名。
	 * @param name 库名（不带扩展名）
	 * @throws TSDRLibraryNotCompatible 如果当前操作系统不支持
	 * @return 完整库文件名
	 */
	public static final String getNativeLibraryFullName(final String name) throws TSDRLibraryNotCompatible {
		final String rawOSNAME = System.getProperty("os.name").toLowerCase();
		String EXT = null, LIBPREFIX = "";

		if (rawOSNAME.contains("win"))
			EXT = ".dll";
		else if (rawOSNAME.contains("nix") || rawOSNAME.contains("nux") || rawOSNAME.contains("aix")) {
			EXT = ".so";
			LIBPREFIX = "lib";
		} else if (rawOSNAME.contains("mac")) {
			EXT = ".so";
		}

		if (EXT == null)
			throw new TSDRLibraryNotCompatible("您的操作系统或CPU暂不支持，抱歉。");
		
		return LIBPREFIX+name+EXT;
	}
	
	/**
	 * 将动态库提取到临时目录，供后续加载。
	 * @param name 库名
	 * @return 临时文件对象
	 * @throws TSDRLibraryNotCompatible 如果不支持的系统或架构
	 */
	public static final File extractLibrary(final String name) throws TSDRLibraryNotCompatible {


		final String rawOSNAME = System.getProperty("os.name").toLowerCase();
		final String rawARCHNAME = System.getProperty("os.arch").toLowerCase();
		String OSNAME = null, ARCHNAME = null;
		final String dllfullfilename = getNativeLibraryFullName(name);

		if (rawOSNAME.contains("win"))
			OSNAME = "WINDOWS";
		else if (rawOSNAME.contains("nix") || rawOSNAME.contains("nux") || rawOSNAME.contains("aix"))
			OSNAME = "LINUX";
		else if (rawOSNAME.contains("mac"))
			OSNAME = "MAC";

		if (rawARCHNAME.contains("arm"))
			ARCHNAME = "ARM";
		else if (rawARCHNAME.contains("64"))
			ARCHNAME = "X64";
		else
			ARCHNAME = "X86";
		
		if (OSNAME == null || ARCHNAME == null)
			throw new TSDRLibraryNotCompatible("您的操作系统或CPU暂不支持，抱歉。");

		final String relative_path = "lib/"+OSNAME+"/"+ARCHNAME+"/"+dllfullfilename;

		InputStream in = TSDRLibrary.class.getClassLoader().getResourceAsStream(relative_path);

		if (in == null)
			try {
				in = new FileInputStream(relative_path);
			} catch (FileNotFoundException e) {}

		if (in == null) throw new TSDRLibraryNotCompatible("该库尚未为您的操作系统/架构编译 ("+OSNAME+"/"+ARCHNAME+")。");

		File temp;
		try {
			byte[] buffer = new byte[in.available()];

			int read = -1;
			temp = new File(System.getProperty("java.io.tmpdir"), dllfullfilename);
			final String fullpath = temp.getAbsolutePath();
			
			// 如果文件已存在且已记录过，不覆盖
			if (temp.exists() && files_to_delete_on_shutdown.contains(fullpath))
				return temp;
			else if (temp.exists()) {
				// 文件存在但未记录，删除旧版本
				try {
					temp.delete();
				} catch (Throwable e) {}
			}
			
			temp.deleteOnExit();
			final FileOutputStream fos = new FileOutputStream(temp);

			while((read = in.read(buffer)) != -1) {
				fos.write(buffer, 0, read);
			}
			fos.close();
			in.close();
			
			if (!temp.exists())
				throw new TSDRLibraryNotCompatible("无法将库文件提取到临时目录。");
			
			
			if (!files_to_delete_on_shutdown.contains(fullpath)) files_to_delete_on_shutdown.add(fullpath);
		} catch (IOException e) {
			throw new TSDRLibraryNotCompatible(e);
		}

		return temp;
	}

	/**
	 * 动态加载指定名称的本地库（不带扩展名）。
	 * @param name 库名
	 * @throws IOException 
	 */
	private static final void loadLibrary(final String name) throws TSDRLibraryNotCompatible {
		try {
			// 先尝试传统方式加载
			System.loadLibrary(name); 
		} catch (Throwable t) {
				final File library = extractLibrary(name);
				System.load(library.getAbsolutePath());
				library.delete();
		}
	}

	/**
	 * 静态代码块，初始化时加载本地库。
	 */
	static {
		try {
			loadLibrary("TSDRLibraryNDK");
		} catch (TSDRLibraryNotCompatible e) {
			m_e = e;
		} 
	}
	
	/**
	 * 构造函数，初始化库并加载本地二进制。
	 * @throws TSDRException
	 */
	public TSDRLibrary() throws TSDRException {
		if (m_e != null) throw m_e;
		init();
	}

	/** 初始化本地缓冲区和变量 */
	private native void init();
	
	/** 设置中心频率 */
	public native void setBaseFreq(long freq) throws TSDRException;
	
	/**
	 * 加载插件，准备后续启动。
	 * @param pluginfilepath 插件路径
	 * @param params 参数
	 * @throws TSDRException
	 */
	private native void loadPlugin(String pluginfilepath, String params) throws TSDRException;
	
	/**
	 * 启动处理流程。阻塞调用，直到结束。
	 * @throws TSDRException
	 */
	private native void nativeStart() throws TSDRException;
	
	/**
	 * 停止处理流程。
	 * @throws TSDRException
	 */
	public native void stop() throws TSDRException;
	
	/**
	 * 卸载并释放已加载的本地插件。
	 * @throws TSDRException
	 */
	public native void unloadPlugin() throws TSDRException;
	
	/**
	 * 设置增益（0~1）。
	 * @param gain 增益
	 * @throws TSDRException
	 */
	public native void setGain(float gain) throws TSDRException;
	
	/**
	 * 检查库是否正在运行。
	 * @return true表示nativeStart仍在运行
	 */
	public native boolean isRunning();
	public native void setInvertedColors(boolean invertedEnabled);
	public native void sync(int pixels, SYNC_DIRECTION dir);
	public native void setParam(PARAM param, long value) throws TSDRException;
	public native void setParamDouble(PARAM_DOUBLE param, double value) throws TSDRException;
	public native void setResolution(int height, double refreshrate) throws TSDRException;
	public native void setMotionBlur(float gain) throws TSDRException;
	
	/**
	 * 释放本地库资源。除非再次调用init，否则后续调用TSDRLibrary方法可能导致崩溃。
	 */
	public native void free();
	
	/**
	 * 加载插件（推荐先unloadPlugin）。
	 * @param plugin 插件对象
	 * @throws TSDRException
	 */
	public void loadPlugin(final TSDRSource plugin) throws TSDRException {
		loadPlugin(plugin.getAbsolutePathToLibrary(), plugin.getParams());
	}
	
	/**
	 * 启动整个系统。调用后，所有注册的帧回调将开始接收视频帧。
	 * 注意：width和height不一定是你期望的分辨率。例如800x600@60Hz实际可能为1056x628。详见VideoMode。
	 * @param width 总宽度
	 * @param height 总高度
	 * @param refreshrate 刷新率
	 * @throws TSDRException
	 */
	public void startAsync(int height, double refreshrate) throws TSDRException {
		if (nativerunning) throw new TSDRAlreadyRunningException("");
		
		setResolution(height, refreshrate);
		
		new Thread() {
			public void run() {
				nativerunning = true;
				try {
				Runtime.getRuntime().removeShutdownHook(unloaderhook);
				} catch (Throwable e) {};
				
				Runtime.getRuntime().addShutdownHook(unloaderhook);
				try {
					nativeStart();
				} catch (TSDRException e) {
					for (final FrameReadyCallback callback : callbacks) callback.onException(TSDRLibrary.this, e);
				}
				
				for (final FrameReadyCallback callback : callbacks) callback.onStopped(TSDRLibrary.this);
				nativerunning = false;
			};
		}.start();
		
	}
	
	/**
	 * JVM关闭前自动卸载本地库。
	 */
	final private Thread unloaderhook = new Thread() {
		@Override
		public void run() {
			try {
				TSDRLibrary.this.unloadPlugin();
			} catch (Throwable e) {}
			try {
				TSDRLibrary.this.stop();
			} catch (Throwable e) {}
			try {
				TSDRLibrary.this.free();
			} catch (Throwable e) {}
			
			// delete all extracted libraries
			for (final String filename : files_to_delete_on_shutdown) {
				try {
					final File file = new File(filename);
					file.delete();
				} catch (Throwable e) {}
			}
		}
	};
	
	@Override
	protected void finalize() throws Throwable {
		try {
		Runtime.getRuntime().removeShutdownHook(unloaderhook);
		} catch (Throwable e) {};
		unloaderhook.run();
		super.finalize();
	}
	
	/**
	 * 注册帧回调。
	 * @param callback
	 * @return true表示注册成功，false表示已注册
	 */
	public boolean registerFrameReadyCallback(final FrameReadyCallback callback) {
		if (callbacks.contains(callback))
			return false;
		else
			return callbacks.add(callback);
	}
	
	/**
	 * 注销帧回调。
	 * @param callback
	 * @return true表示移除成功，false表示未注册
	 */
	public boolean unregisterFrameReadyCallback(final FrameReadyCallback callback) {
		return callbacks.remove(callback);
	}
	
	/**
	 * 注册数值变化回调。
	 */
	public boolean registerValueChangedCallback(final IncomingValueCallback callback) {
		if (value_callbacks.contains(callback))
			return false;
		else
			return value_callbacks.add(callback);
	}
	
	/**
	 * 注销数值变化回调。
	 */
	public boolean unregisterValueChangedCallback(final IncomingValueCallback callback) {
		return callbacks.remove(callback);
	}
	
	/**
	 * 回调接口：异步接收视频帧和异常。
	 */
	public interface FrameReadyCallback {
		
		/**
		 * 新视频帧生成时回调。
		 * @param lib
		 * @param frame
		 */
		void onFrameReady(final TSDRLibrary lib, final BufferedImage frame);
		
		/**
		 * 运行时发生异常时回调。
		 * @param lib
		 * @param e
		 */
		void onException(final TSDRLibrary lib, final Exception e);
		
		/**
		 * 停止时回调。
		 * @param lib
		 */
		void onStopped(final TSDRLibrary lib);
	}
	
	/**
	 * 回调接口：异步接收数值变化和绘图数据。
	 */
	public interface IncomingValueCallback {
		public static enum VALUE_ID {PLL_FRAMERATE, AUTOCORRECT_RESET, FRAMES_COUNT, AUTOGAIN, SNR, AUTOCORRECT_DUMPED};
		public static enum PLOT_ID {FRAME, LINE};
		
		public void onValueChanged(final VALUE_ID id, double arg0, double arg1);
		public void onIncommingPlot(final PLOT_ID id, int offset, double[] data, int size, long samplerate);
	}

	
	/**
	 * 本地代码应调用此方法以初始化或调整像素缓冲区大小。
	 * @param x 帧宽度
	 * @param y 帧高度
	 */
	final private void fixSize(final int x, final int y) {
		if (bimage == null || bimage.getWidth() != x || bimage.getHeight() != y) {
			try {
				bimage = new BufferedImage(x, y, BufferedImage.TYPE_INT_RGB);
				pixels = ((DataBufferInt) bimage.getRaster().getDataBuffer()).getData();
			} catch (Throwable t) {
				t.printStackTrace();
				System.err.flush(); System.out.flush();
			}
		}
	}
	
	final private void onIncomingArray(final int size) {
		synchronized (float_array_locker) {
			if (float_array_locker_count > 0)
				try {
					float_array_locker.wait();
				} catch (InterruptedException e) {}
			float_array_locker_count++;
		}
		
		if (double_array == null || double_array.length < size)
			double_array = new double[size];
	}
	
	final private void onIncomingArrayNotify(final int plot_id, final int offset, final int size, long samplerate) {
		final IncomingValueCallback.PLOT_ID[] values = IncomingValueCallback.PLOT_ID.values();
		
		if (plot_id < 0 || plot_id >= values.length) {
			System.err.println("Warning: Received unrecognized callback plot id "+plot_id+" from JNI!");
			return;
		}
		
		final IncomingValueCallback.PLOT_ID val = values[plot_id];
		
		for (final IncomingValueCallback callback : value_callbacks) callback.onIncommingPlot(val, offset, double_array, size, samplerate);
		
		// unlock so somebody else could send a request
		synchronized (float_array_locker) {
			float_array_locker_count--;
			float_array_locker.notify();
		}
	}
	
	/**
	 * 本地代码写入像素数据后应调用此方法，通知所有帧回调。
	 */
	final private void notifyCallbacks() {
		try {
			for (final FrameReadyCallback callback : callbacks) callback.onFrameReady(this, bimage);
		} catch (Throwable t) {
			t.printStackTrace();
			System.err.flush(); System.out.flush();
		}
	}
	
	private void onValueChanged(final int value_id, final double arg0, final double arg1) {
		final IncomingValueCallback.VALUE_ID[] values = IncomingValueCallback.VALUE_ID.values();
		
		if (value_id < 0 || value_id >= values.length) {
			System.err.println("Warning: Received unrecognized callback value id "+value_id+" with arg0="+arg0+" and arg1="+arg1+" from JNI!");
			return;
		}
		
		final IncomingValueCallback.VALUE_ID val = values[value_id];
		
		for (final IncomingValueCallback callback : value_callbacks) callback.onValueChanged(val, arg0, arg1);
	}
	
}
