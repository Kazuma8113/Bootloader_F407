# -*- coding: utf-8 -*-
"""给 LCD 24x24 汉字字库 (Hardware/LCD/lcd_fonts.c 的 Chinese_2424 表) 追加汉字。

用法:
    python gen_lcd_font.py                  # 追加脚本内置的常用字列表
    python gen_lcd_font.py 你 好 世 界      # 追加指定的字（可带引号，也可连写 "你好世界"）

特性:
    * 幂等：字库里已存在的字会自动跳过，可反复运行
    * 与厂商风格一致：宋体 48px 渲染后按 2x2 子像素块降采样（4 取 2）
    * 自动更新数组维度 Chinese_2424[N][72]  (Table_Rows 用 sizeof 自动跟随)
    * 文件统一以 UTF-8 保存（驱动已改为按 UTF-8 三字节查字）

依赖: pillow  (pip install pillow)
"""
import re
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS_C = os.path.normpath(os.path.join(HERE, "..", "Hardware", "LCD", "lcd_fonts.c"))

W = H = 24
SZ = 72                                  # 24*24/8
FONT_TTF = r"C:\Windows\Fonts\simsun.ttc"
SS, SIZE, THR = 2, 24, 2                 # 2x 超采样；4 个子像素里 >=2 亮则该点画

# 默认追加的界面常用字（可自行修改）
DEFAULT_CHARS = ("℃上下降限阈值报警常异常过高低"
                 "加热风扇电机开启关闭运行停止"
                 "前状态模式自动手设置保护"
                 "系统初始化错误故障数据传收发信号"
                 "连接断蓝牙备机"
                 "菜单页面返回确定择"
                 "秒钟间日期版本"
                 "智能家居环境冷暖干燥舒适安全超")


def gen_bytes(ch):
    """生成 24x24 汉字点阵，72 字节，LSB-first 连续位流（每行 3 字节）。"""
    f = ImageFont.truetype(FONT_TTF, SIZE * SS)
    im = Image.new('L', (W * SS, H * SS), 0)
    ImageDraw.Draw(im).text((0, 0), ch, font=f, fill=255)
    px = im.load()
    out = Image.new('1', (W, H), 0)
    op = out.load()
    for y in range(H):
        for x in range(W):
            c = sum(1 for dy in range(SS) for dx in range(SS)
                    if px[x * SS + dx, y * SS + dy] >= 128)
            if c >= THR:
                op[x, y] = 1
    pp = out.load()
    data = bytearray(SZ)
    for y in range(H):
        for x in range(W):
            if pp[x, y]:
                b = y * W + x
                data[b // 8] |= 1 << (b % 8)
    return bytes(data)


def main():
    chars = "".join(sys.argv[1:]) or DEFAULT_CHARS
    if not os.path.isfile(FONTS_C):
        sys.exit("找不到字库文件: %s" % FONTS_C)

    raw = open(FONTS_C, 'rb').read()
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError:
        text = raw.decode('gbk')
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)

    s = next(i for i, l in enumerate(lines) if 'Chinese_2424[' in l)
    e = next(i for i in range(s + 1, len(lines)) if lines[i].strip().startswith('};'))
    head = lines[s]
    body = nl.join(lines[s + 1:e])

    pairs = []
    recs = re.findall(r'\{[^{}]*\}', body)
    i = 0
    while i < len(recs):
        nums = re.findall(r'0x([0-9A-Fa-f]{2})', recs[i])
        if len(nums) == SZ and i + 1 < len(recs):
            lit = re.findall(r'\{\s*"(.*?)"\s*\}', recs[i + 1], re.S)
            pairs.append([lit[0].strip() if lit else '?',
                          bytes(int(x, 16) for x in nums)])
            i += 2
        else:
            i += 1
    print("字库现有 %d 字" % len(pairs))

    have = {p[0] for p in pairs}
    added = []
    for ch in chars:
        if ch in have:
            continue
        pairs.append([ch, gen_bytes(ch)])
        have.add(ch)
        added.append(ch)

    if not added:
        print("没有需要新增的字，未做改动。")
        return
    print("新增 %d 字: %s" % (len(added), "".join(added)))
    print("合计 %d 字 -> Chinese_2424[%d][72]" % (len(pairs), len(pairs) * 2))

    out = []
    for ch, data in pairs:
        out.append("\t{" + ",".join("0x%02X" % v for v in data) + "},")
        out.append('\t{"' + ch + ' "},')
    newhead = re.sub(r'\[\d+\]\[72\]', '[%d][72]' % (len(pairs) * 2), head)
    newlines = lines[:s] + [newhead, "{"] + out + ["};"] + lines[e + 1:]
    open(FONTS_C, 'wb').write(nl.join(newlines).encode('utf-8'))
    print("已更新: %s" % FONTS_C)


if __name__ == "__main__":
    main()
