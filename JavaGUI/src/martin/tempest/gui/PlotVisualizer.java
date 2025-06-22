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
package martin.tempest.gui;

import java.awt.Color;
import java.awt.Font;
import java.awt.Graphics;
import java.awt.Rectangle;
import java.awt.event.MouseEvent;
import java.awt.event.MouseListener;
import java.awt.event.MouseMotionListener;
import java.awt.event.MouseWheelEvent;
import java.awt.event.MouseWheelListener;

import javax.swing.JPanel;

import martin.tempest.gui.scale.LogScale;
import martin.tempest.gui.scale.ZoomableXScale;

public class PlotVisualizer extends JPanel {

	private static final long serialVersionUID = -6754436015453195809L;

	private static final double FONT_SPACING_COEFF = 1.3;
	private static final double FONT_SIZE_COEFF = 0.8;

	private static final double ZOOM_AMOUNT = 0.9;
	private static final double UNZOOM_AMOUNT = 1.1;

	private final Color default_txt_colour_background = Color.LIGHT_GRAY;

	private Object locker = new Object();
	private double[] data = null;
	private double[] visdata = null;
	private int size = 0;
	private int max_index = 0;
	private boolean enabled;
	private int mouse_xpx = -1, mouse_drag = -1;
	private Integer index_selected = null;
	private Double initial_value = null;

	private long samplerate;
	private int offset;
	private int size_of_val_in_px = 0;

	private int area_around_mouse = 0;

	private final TransformerAndCallback trans;

	private int nwidth = 1, nheight = 1;

	private boolean font_set = false;
	private int half_fontsize;
	private Font font;

	private ZoomableXScale scale_x = new ZoomableXScale(10);
	private final LogScale scale_y = new LogScale(default_txt_colour_background, FONT_SPACING_COEFF);

	public PlotVisualizer(final TransformerAndCallback trans) {
		this.trans = trans;

		addMouseMotionListener(new MouseMotionListener() {
			public void mouseDragged(MouseEvent arg0) {
				if (mouse_drag != -1) {
					final int dx = arg0.getX() - mouse_drag;
					mouse_drag = arg0.getX();
					mouse_xpx = mouse_drag;

					scale_x.moveOffsetWithPixels(dx);

					synchronized (locker) {
						populateData();
					}

					repaint();
				}
			}

			public void mouseMoved(MouseEvent arg0) {
				mouse_xpx = arg0.getX();

				final int m_id = (int) scale_x.pixels_to_value_absolute(mouse_xpx);
				if (m_id >= 0 && m_id < size)
					trans.onMouseMoved(m_id, offset, samplerate);

				repaint();
			}
		});

		addMouseWheelListener(new MouseWheelListener() {

			@Override
			public void mouseWheelMoved(MouseWheelEvent arg0) {
				final double zoom = (arg0.getWheelRotation() > 0) ? ZOOM_AMOUNT : UNZOOM_AMOUNT;
				scale_x.zoomAround(arg0.getX(), zoom);

				synchronized (locker) {
					populateData();
				}

				repaint();
			}
		});

		addMouseListener(new MouseListener() {
			public void mouseReleased(MouseEvent arg0) {
				mouse_drag = -1;
			}

			public void mousePressed(MouseEvent arg0) {
				mouse_drag = arg0.getX();
			}

			public void mouseEntered(MouseEvent arg0) {
			}

			public void mouseClicked(MouseEvent arg0) {
				synchronized (locker) {
					if (arg0.getButton() == MouseEvent.BUTTON1) {
						final int id_selected = getBestIdAround(mouse_xpx, area_around_mouse);
						setSelectedIndex(id_selected);
						trans.executeIdSelected(id_selected, offset, samplerate);
					} else
						scale_x.reset();
				}

				repaint();
			}

			public void mouseExited(MouseEvent arg0) {
				mouse_xpx = -1;
				trans.onMouseExited();
				repaint();
			}
		});
	}

	public void setAreaAroundMouse(int area) {
		if (area >= 0)
			area_around_mouse = area;
	}

	private int getBestIdAround(final int px, final int area_px) {

		int start_id = (int) scale_x.pixels_to_value_absolute(px - area_px / 2);
		if (start_id >= size)
			return -1;
		if (start_id < 0)
			start_id = 0;

		int end_id = (int) scale_x.pixels_to_value_absolute(px + area_px / 2);
		if (end_id < 0)
			return -1;
		if (end_id > size)
			end_id = size;

		int bestid = start_id;
		double max = data[bestid];
		for (int id = start_id + 1; id < end_id; id++)
			if (data[id] > max) {
				max = data[id];
				bestid = id;
			}

		return bestid;
	}

	public void setSelectedValue(double val) {
		if (data == null) {
			synchronized (locker) {
				this.initial_value = val;
			}
		} else
			setSelectedIndex(trans.toIndex(val, offset, samplerate));

		repaint();
	}

	public void setSelectedIndex(int val) {
		synchronized (locker) {
			this.index_selected = val;
		}
		repaint();
	}

	public Double getSelectedValue() {
		if (data == null)
			return initial_value;
		else
			return trans.fromIndex(index_selected, offset, samplerate);
	}

	public Double getValueFromId(int id) {
		return trans.fromIndex(id, offset, samplerate);
	}

	private String getValueAt(int id) {
		final double val = trans.fromIndex(id, offset, samplerate);
		return trans.getDescription(val, id);
	}

	private void populateData() {
		// 检查Y轴刻度是否有效，若无效则不进行数据处理
		if (!scale_y.valuesValid())
			return;

		// 初始化最大值、最小值、最大值索引
		double highest_val = data[0];
		double lowest_val = highest_val;
		max_index = 0;
		double max_val = highest_val;

		// 计算每个数据点在X轴上占据的像素宽度
		size_of_val_in_px = scale_x.value_to_pixel_relative(1) + 1;

		int prev_px = 0;
		// 计算当前可视区域对应的数据索引范围，避免无效数据处理
		final int first_id = (int) Math.min(Math.max(scale_x.pixels_to_value_absolute(0), 0), size);
		final int last_id = (int) Math.min(Math.max(scale_x.pixels_to_value_absolute(nwidth) + 1, 0), size);
		// 初始化当前像素区间的最大值
		double localmax = data[first_id];
		// 主循环：遍历可视区间内的数据，将数据映射到像素，并检测峰值
		for (int id = first_id; id < last_id; id++) {
			final double val = data[id];

			final int px = scale_x.value_to_pixel_absolute(id);
			if (px >= 0 && px < nwidth) {

				if (prev_px != px) {
					// 进入新像素区间，记录上一区间的最大/最小值
					if (localmax > highest_val)
						highest_val = localmax;
					else if (localmax < lowest_val)
						lowest_val = localmax;
					// 填充上一区间所有像素为localmax，保证峰值不丢失
					for (int i = prev_px; i < px; i++)
						visdata[i] = localmax;
					// 更新localmax为当前值，准备下一区间
					localmax = val;
					prev_px = px;

				} else if (val > localmax)
					// 同一像素区间内，持续更新最大值
					localmax = val;
			}

			// 记录全局最大值及其索引
			if (val > max_val) {
				max_val = val;
				max_index = id;
			}
		}

		// 填充最后一个像素区间的剩余像素
		for (int i = prev_px; i < nwidth; i++)
			visdata[i] = localmax;

		// 设置Y轴刻度的最大最小值，便于后续像素映射
		scale_y.setLowestHighestValue(lowest_val, highest_val);

		// 将所有像素的值转换为Y轴像素坐标，便于直接绘图
		for (int i = 0; i < nwidth; i++)
			visdata[i] = scale_y.valtopx(visdata[i]);
	}

	public void plot(double[] incoming_data, int offset, final int size, long samplerate) {
		if (size <= 0)
			return;

		synchronized (locker) {
			if (data == null || data.length < size)
				data = new double[size];

			System.arraycopy(incoming_data, 0, data, 0, size);

			if (visdata == null || visdata.length != nwidth) {
				visdata = new double[nwidth];
				scale_x.reset();
			}

			this.size = size;
			scale_x.setMinMaxValue(0, size);
			this.offset = offset;
			this.samplerate = samplerate;

			populateData();

			// calculate selected initial index
			if (index_selected == null && initial_value != null) {
				index_selected = trans.toIndex(initial_value, offset, samplerate);
				initial_value = null;
			}
		}

		repaint();
	}

	public int getMaxIndex() {
		return max_index;
	}

	public int getOffset() {
		return offset;
	}

	public long getSamplerate() {
		return samplerate;
	}

	@Override
	public void setBounds(int x, int y, int width, int height) {
		this.nwidth = width;
		this.nheight = height;
		scale_y.setDimentions(this.nwidth, this.nheight);
		scale_x.setMaxPixels(this.nwidth);
		enabled = isEnabled();
		super.setBounds(x, y, width, height);
	}

	@Override
	public void setBounds(Rectangle r) {
		this.nwidth = r.width;
		this.nheight = r.height;
		scale_y.setDimentions(this.nwidth, this.nheight);
		scale_x.setMaxPixels(this.nwidth);
		enabled = isEnabled();
		super.setBounds(r);
	}

	@Override
	public void setEnabled(boolean arg0) {
		super.setEnabled(arg0);
		enabled = isEnabled();
	}

	public void reset() {
		synchronized (locker) {
			data = null;
			size = 0;
		}

		repaint();
	}

	private void setFont(Graphics g) {
		if (!font_set) {
			final Font existing = g.getFont();
			font = new Font(existing.getFontName(), Font.PLAIN, (int) (existing.getSize() * FONT_SIZE_COEFF));
			half_fontsize = font.getSize() / 2;
			g.setFont(font);
			font_set = true;
		} else {
			g.setFont(font);
		}
	}

	/**
	 * 绘制频谱图的主要方法，负责将IQ数据可视化显示
	 * @param g 图形上下文，用于绘制操作
	 */
	@Override
	public void paint(Graphics g) {

		// 设置字体，确保文字显示的一致性
		setFont(g);

		// 如果没有数据，绘制空白背景和Y轴刻度
		if (data == null) {
			g.setColor(Color.black);
			g.fillRect(0, 0, nwidth, nheight);
			scale_y.paintScale(g);
			return;
		}

		// 使用同步锁确保数据一致性，防止并发修改
		synchronized (locker) {

			// 检查Y轴刻度是否有效，无效则不绘制
			if (!scale_y.valuesValid())
				return;

			// 绘制背景：启用时用黑色，禁用时用深灰色
			g.setColor(enabled ? Color.black : Color.DARK_GRAY);
			g.fillRect(0, 0, nwidth, nheight);

			// 如果鼠标在组件内，绘制鼠标悬停区域的高亮背景
			if (mouse_xpx != -1) {
				g.setColor(Color.darkGray);

				// 在鼠标周围绘制一个矩形高亮区域，帮助用户定位
				if (area_around_mouse != 0)
					g.fillRect(mouse_xpx - area_around_mouse / 2, 0, area_around_mouse, nheight);
			}

			// 检查可视化数据数组长度是否正确
			if (visdata.length == nwidth) {

				// 设置频谱线的颜色为灰色
				g.setColor(Color.gray);

				// 绘制频谱图：为每个X轴像素位置绘制一条垂直线
				// 线的起点是(visdata[x], x)，终点是(nheight, x)
				// visdata[x]包含了经过Y轴映射的像素坐标
				for (int x = 0; x < nwidth; x++)
					g.drawLine(x, (int) (visdata[x]), x, nheight);

			} else {
				// 如果数据长度不匹配，用蓝色填充整个区域表示错误状态
				g.setColor(Color.blue);
				g.fillRect(0, 0, nwidth, nheight);
				return;
			}

			// 如果鼠标在组件内，绘制鼠标交互相关的元素
			if (mouse_xpx != -1) {

				// 绘制鼠标位置的垂直十字线（浅灰色）
				g.setColor(default_txt_colour_background);
				g.drawLine(mouse_xpx, 0, mouse_xpx, nheight);

				// 找到鼠标附近最佳的数据点索引
				final int id = getBestIdAround(mouse_xpx, area_around_mouse);

				// 如果找到了有效的数据点
				if (id >= 0 && id < size) {
					// 计算该数据点在X轴上的像素位置
					final int x = scale_x.value_to_pixel_absolute(id);
					// 计算该数据点在Y轴上的像素位置
					final int y = scale_y.valtopx(data[id]);
					// 计算该数据点的dB值，用于显示
					final double db = scale_y.valtodb(data[id]);

					// 用黄色高亮显示选中的数据点
					g.setColor(Color.yellow);
					g.fillRect(x, y, size_of_val_in_px, nheight);

					// 绘制数据点的详细信息文本
					g.setColor(default_txt_colour_background);
					// 显示数据点的描述信息（如频率值）
					g.drawString(getValueAt(id), mouse_xpx + nheight / 10, nheight / 5);
					// 显示数据点的dB值
					g.drawString(String.format("%.1f dB", db), mouse_xpx + nheight / 10,
							nheight / 5 + 3 * half_fontsize);
				}
			}

			// 如果有选中的索引，绘制选中标记
			if (index_selected != null) {

				// 检查选中索引是否有效
				if (index_selected >= 0 || index_selected < size) {
					// 设置选中线的颜色为绿色
					g.setColor(Color.green);

					// 计算选中索引在X轴上的像素位置，并居中显示
					final int x = scale_x.value_to_pixel_absolute(index_selected) + size_of_val_in_px / 2;

					// 绘制绿色的选中线，从顶部到底部
					g.drawLine(x, 0, x, nheight);

					// 如果选中索引有效，显示其描述信息
					if (index_selected >= 0 && index_selected < size)
						g.drawString(getValueAt(index_selected), x + nheight / 10, nheight / 9);
				}
			}

			// 最后绘制Y轴刻度，确保刻度显示在最上层
			scale_y.paintScale(g);
		}
	}

	public static abstract class TransformerAndCallback {
		public abstract double fromIndex(final int id, final int offset, final long samplerate);

		public abstract int toIndex(final double val, final int offset, final long samplerate);

		public abstract String getDescription(final double val, final int id);

		public void onMouseMoved(final int m_id, final int offset, final long samplerate) {
		};

		public void onMouseExited() {
		};

		public void executeIdSelected(final int sel_id, final int offset, final long samplerate) {
		};
	}
}
