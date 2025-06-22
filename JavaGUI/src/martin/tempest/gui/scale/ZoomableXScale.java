package martin.tempest.gui.scale;

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


/**
 * 实现一个可缩放的X轴刻度，将[min_val, max_val]区间的值映射到0到max_value像素区间。
 * 支持最大缩放限制。
 * 
 * @author martinmarinov
 *
 */
public class ZoomableXScale {

	private int max_pixels = 800;
	private double min_value, max_value, offset_val = 0.0;
	private int offset_px = 0;
	private double scale = 1.0; // one raw pixels = scale * screen pixel
	private double max_zoom_val = 1.0;
	private double one_val_in_pixels_relative = max_pixels / (max_value - min_value);
	private double one_px_in_values_relative = (max_value - min_value) / max_pixels;
	
	private boolean autofixzoomandoffset = true;

	private final Object locker = new Object();

	/**
	 * 构造函数，指定最小值、最大值和最大缩放值。
	 * @param min_value X轴最小值
	 * @param max_value X轴最大值
	 * @param max_zoom_val 最大可缩放区间
	 */
	public ZoomableXScale(final double min_value, final double max_value, final double max_zoom_val) {
		setMinMaxValue(min_value, max_value, max_zoom_val);
	}
	
	/**
	 * 构造函数，指定最小值和最大值，最大缩放为1.0。
	 * @param min_value X轴最小值
	 * @param max_value X轴最大值
	 */
	public ZoomableXScale(final double min_value, final double max_value) {
		this(min_value, max_value, 1.0);
	}
	
	/**
	 * 构造函数，指定最大缩放值，默认区间[0,100]。
	 * @param max_zoom_val 最大可缩放区间
	 */
	public ZoomableXScale(final double max_zoom_val) {
		this(0, 100, max_zoom_val);
	}
	
	/**
	 * 默认构造函数，区间[0,100]，最大缩放为1.0。
	 */
	public ZoomableXScale() {
		this(0, 100, 1.0);
	}
	
	/**
	 * 设置是否自动修正缩放和偏移。
	 * @param enforce 是否启用自动修正
	 */
	public void autofixZoomAndOffsetEnabled(final boolean enforce) {
		synchronized (locker) {
			this.autofixzoomandoffset = enforce;
		}
	}

	/**
	 * 设置最大像素宽度。
	 * @param max_pixels 最大像素数
	 */
	public void setMaxPixels(final int max_pixels) {
		synchronized (locker) {
			this.max_pixels = max_pixels;

			calculateValues_unsafe();
		}
	}

	/**
	 * 设置最小值、最大值和最大缩放值。
	 * @param min_value X轴最小值
	 * @param max_value X轴最大值
	 * @param max_zoom_val 最大可缩放区间
	 */
	public void setMinMaxValue(final double min_value, final double max_value, final double max_zoom_val) {
		synchronized (locker) {
			this.max_zoom_val = max_zoom_val;
			setMinMaxValue(min_value, max_value);
		}
	}
	
	/**
	 * 设置最小值和最大值。
	 * @param min_value X轴最小值
	 * @param max_value X轴最大值
	 */
	public void setMinMaxValue(final double min_value, final double max_value) {
		synchronized (locker) {
			this.min_value = min_value;
			this.max_value = max_value;

			calculateValues_unsafe();
		}
	}

	/**
	 * 按像素平移X轴偏移量。
	 * @param offset 偏移像素数
	 */
	public void moveOffsetWithPixels(final int offset) {
		synchronized (locker) {
			setPxOffset_unsafe(offset_px - offset);
			
			if (autofixzoomandoffset)
				autoFixOffset_unsafe();
		}
	}

	/**
	 * 按值平移X轴偏移量。
	 * @param value 偏移的实际值
	 */
	public void moveOffsetWithValue(final double value) {
		synchronized (locker) {
			setValOffset_unsafe(offset_val - value);
			
			if (autofixzoomandoffset)
				autoFixOffset_unsafe();
		}
	}

	/**
	 * 以某像素为中心进行缩放。
	 * @param px 缩放中心像素
	 * @param coeff 缩放系数
	 */
	public void zoomAround(final int px, final double coeff) {
		synchronized (locker) {
			final double val = pixels_to_value_absolute(px);
			scale *= coeff;
			calculateValues_unsafe();

			final double newval = pixels_to_value_absolute(px);

			setValOffset_unsafe(offset_val - newval + val);
			
			if (autofixzoomandoffset)
				autoFixOffset_unsafe();
		}
	}
	
	/**
	 * 自动修正偏移。
	 */
	public void fixOffset() {
		synchronized (locker) {
			autoFixOffset_unsafe();
		}
	}
	
	/**
	 * 重置缩放和偏移到初始状态。
	 */
	public void reset() {
		synchronized (locker) {
			reset_unsafe();
		}
	}

	/**
	 * 将像素坐标转换为绝对值。
	 * @param pixels 像素坐标
	 * @return 对应的实际值
	 */
	public double pixels_to_value_absolute(final int pixels) {
		synchronized (locker) {
			return pixels * one_px_in_values_relative + offset_val + min_value;
		}
	}

	/**
	 * 将像素坐标转换为相对值（不含偏移）。
	 * @param pixels 像素坐标
	 * @return 相对值
	 */
	public double pixels_to_value_relative(final int pixels) {
		synchronized (locker) {
			return pixels * one_px_in_values_relative;
		}
	}

	/**
	 * 将实际值转换为像素坐标（绝对）。
	 * @param val 实际值
	 * @return 对应的像素坐标
	 */
	public int value_to_pixel_absolute(final double val) {
		synchronized (locker) {
			return (int) ((val - min_value) * one_val_in_pixels_relative) - offset_px;
		}
	}

	/**
	 * 将实际值转换为像素坐标（相对）。
	 * @param val 实际值
	 * @return 相对像素坐标
	 */
	public int value_to_pixel_relative(final double val) {
		synchronized (locker) {
			return (int) (val * one_val_in_pixels_relative);
		}
	}
	
	/**
	 * 设置像素偏移（线程不安全，仅限内部调用）。
	 * @param offset_px 像素偏移
	 */
	private void setPxOffset_unsafe(final int offset_px) {
		this.offset_px = offset_px;
		offset_val = pixels_to_value_relative(offset_px);
	}
	
	/**
	 * 设置值偏移（线程不安全，仅限内部调用）。
	 * @param offset_val 值偏移
	 */
	private void setValOffset_unsafe(final double offset_val) {
		this.offset_val = offset_val;
		offset_px = value_to_pixel_relative(offset_val);
	}
	
	/**
	 * 重新计算像素与值的映射关系（线程不安全，仅限内部调用）。
	 */
	private void calculateValues_unsafe() {

		one_val_in_pixels_relative = max_pixels / ((max_value - min_value)*scale);
		one_px_in_values_relative = ((max_value - min_value)*scale) / max_pixels;
		
		final double values_in_screen = pixels_to_value_relative(max_pixels);
		if (values_in_screen < max_zoom_val) {
			scale = max_zoom_val / (max_value - min_value);
			one_val_in_pixels_relative = max_pixels / ((max_value - min_value)*scale);
			one_px_in_values_relative = ((max_value - min_value)*scale) / max_pixels;
		}
	}
	
	/**
	 * 重置缩放和偏移到初始状态（线程不安全，仅限内部调用）。
	 */
	private void reset_unsafe() {
		scale = 1;
		offset_val = 0;
		offset_px = 0;

		calculateValues_unsafe();
	}
	
	/**
	 * 自动修正偏移，防止超出边界（线程不安全，仅限内部调用）。
	 */
	private void autoFixOffset_unsafe() {
		
		if (offset_px < 0)
			setPxOffset_unsafe(0);
		
		final double max_val = pixels_to_value_absolute(max_pixels);
		if (max_val > this.max_value)
				 setValOffset_unsafe(this.max_value - pixels_to_value_relative(max_pixels) - this.min_value);

		if (offset_px < 0)
			reset_unsafe();
	}
}
