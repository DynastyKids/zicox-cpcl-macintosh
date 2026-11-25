#!/usr/bin/env python3
"""Generate the PPD for one printer model.

Run: python3 tools/gen_ppd.py MODEL > ppd/Zicox_MODEL.ppd   (MODEL defaults to CC4)
"""

import sys

FILTER = "/Library/Printers/Zicox/Filter/rastertozicox"
DPI = 203

ROLL_LEN = 200  # page length (mm) for width-only roll sizes; blank top/bottom are trimmed automatically
INCH = {50: "2", 75: "3", 100: "4", 150: "6"}  # nominal inch marks (50 mm = 2 in, ...)

# Per-model settings. Width = across the print head. Only CC4 has been tested on a real printer;
# head and paper widths of the other models are typical values for their class, not vendor data.
#   head_mm   printable width of the print head (8 dots/mm)
#   paper_mm  widest paper the printer takes (paper wider than the head is assumed centred on it)
#   special   special sizes listed first: (w, h) or (w, h, True) to add the inch mark
#   rolls     widths (mm) of the width-only continuous sizes
#   default   default page size keyword
MODELS = {
    "CC4": dict(
        desc="4-inch", head_mm=104, paper_mm=104,
        special=[(50, 25), (50, 30), (50, 80), (50, 100, True), (100, 50, True), (100, 100, True), (100, 150, True)],
        rolls=list(range(50, 101, 5)), default="Roll100mm"),
    "HDT334": dict(
        desc="4-inch", head_mm=104, paper_mm=104,
        special=[(50, 25), (50, 30), (50, 80), (50, 100, True), (100, 50, True), (100, 100, True), (100, 150, True)],
        rolls=list(range(50, 101, 5)), default="Roll100mm"),
    "CC3": dict(
        desc="3-inch", head_mm=72, paper_mm=80,
        special=[(50, 25), (50, 30), (50, 80), (50, 100, True), (75, 50, True), (75, 75, True), (75, 100, True)],
        rolls=list(range(50, 81, 5)), default="Roll75mm"),
    "M35": dict(
        desc="3-inch", head_mm=72, paper_mm=80,
        special=[(50, 25), (50, 30), (50, 80), (50, 100, True), (75, 50, True), (75, 75, True), (75, 100, True)],
        rolls=list(range(50, 81, 5)), default="Roll75mm"),
    "CC2": dict(
        desc="2-inch", head_mm=48, paper_mm=58,
        special=[(40, 30), (50, 25), (50, 30), (50, 40), (50, 50), (50, 80), (50, 100, True)],
        rolls=list(range(30, 56, 5)) + [58], default="Roll50mm"),
}


def build_sizes(m):
    """[(keyword, width_mm, height_mm, english, chinese, is_roll)]"""
    sizes = []
    for item in m["special"]:
        w, h = item[0], item[1]
        en = zh = "%d x %d mm" % (w, h)
        if len(item) > 2:
            en += " (%sx%s in)" % (INCH[w], INCH[h])
            zh += " (%s\u2033\u00d7%s\u2033)" % (INCH[w], INCH[h])
        sizes.append(("%dx%dmm" % (w, h), w, h, en, zh, False))
    for w in m["rolls"]:
        mark_en = " (%s in)" % INCH[w] if w in INCH else ""
        mark_zh = " (%s\u2033)" % INCH[w] if w in INCH else ""
        sizes.append(("Roll%dmm" % w, w, ROLL_LEN, "%d mm Roll%s" % (w, mark_en), "%d mm%s 连续纸" % (w, mark_zh), True))
    for key, w, *_ in sizes:
        assert w <= m["paper_mm"], "%s wider than the paper path" % key
    assert m["default"] in [k for k, *_ in sizes]
    return sizes


def margin(m, w):
    """Left/right unprintable margin (mm) when the paper is wider than the head (paper centred)."""
    return max(0.0, (w - m["head_mm"]) / 2.0)


def pt(mm):
    return mm * 72 / 25.4


def fmt(v):
    return ("%.2f" % v).rstrip("0").rstrip(".")


def split(text):
    """'中文/English' -> ('English', '中文'); plain text -> (text, text)."""
    if "/" in text and any(ord(c) > 127 for c in text):
        zh, en = text.split("/", 1)
        return en.strip(), zh.strip()
    return text, text


def options():
    """(keyword, ui text, default, [(choice, text, code)])"""
    return [
        ("zcMediaTracking", "纸张定位/Media Tracking", "Continuous", [
            ("Gap", "标签间隙/Label Gap", "<</cupsMediaType 1>>setpagedevice"),
            ("Continuous", "连续纸/Continuous", "<</cupsMediaType 0>>setpagedevice"),
            ("MarkLeft", "左黑标/Black Mark Left", "<</cupsMediaType 2>>setpagedevice"),
            ("MarkRight", "右黑标/Black Mark Right", "<</cupsMediaType 3>>setpagedevice"),
        ]),
        ("zcDarkness", "打印浓度/Darkness", "Default", [
            ("Default", "打印机默认/Printer Default", "<</cupsInteger0 0>>setpagedevice"),
            ("Light", "淡/Light", "<</cupsInteger0 1>>setpagedevice"),
            ("Medium", "中/Medium", "<</cupsInteger0 2>>setpagedevice"),
            ("Dark", "深/Dark", "<</cupsInteger0 3>>setpagedevice"),
            ("VeryDark", "很深/Very Dark", "<</cupsInteger0 4>>setpagedevice"),
        ]),
        ("zcSpeed", "打印速度/Print Speed", "Default", [
            ("Default", "打印机默认/Printer Default", "<</cupsInteger1 0>>setpagedevice"),
        ] + [("S%d" % s, {0: "最慢/0 (slowest)", 5: "最快/5 (fastest)"}.get(s, "%d" % s),
              "<</cupsInteger1 %d>>setpagedevice" % (s + 1)) for s in range(6)]),
        ("zcDither", "图像处理/Dithering", "Threshold", [
            ("Threshold", "阈值(文字条码最清晰)/Threshold", "<</cupsInteger2 0>>setpagedevice"),
            ("Diffusion", "误差扩散(照片)/Error Diffusion", "<</cupsInteger2 1>>setpagedevice"),
            ("Ordered", "有序抖动/Ordered", "<</cupsInteger2 2>>setpagedevice"),
        ]),
        ("zcThreshold", "黑白阈值/Black Level", "Normal", [
            ("Lighter", "更淡/Lighter", "<</cupsInteger3 176>>setpagedevice"),
            ("Light", "偏淡/Light", "<</cupsInteger3 152>>setpagedevice"),
            ("Normal", "标准/Normal", "<</cupsInteger3 128>>setpagedevice"),
            ("Dark", "偏黑/Dark", "<</cupsInteger3 100>>setpagedevice"),
            ("Darker", "更黑/Darker", "<</cupsInteger3 72>>setpagedevice"),
        ]),
        ("zcRotate", "旋转180度/Rotate 180 Degrees", "Off", [
            ("Off", "否/Off", "<</cupsInteger5 0>>setpagedevice"),
            ("On", "是/On", "<</cupsInteger5 1>>setpagedevice"),
        ]),
        ("zcPosition", "横向位置/Horizontal Position", "Left", [
            ("Left", "靠左/Left", "<</cupsInteger8 0>>setpagedevice"),
            ("Center", "居中于打印头/Centered", "<</cupsInteger8 1>>setpagedevice"),
        ]),
        ("zcXOffset", "横向偏移/Horizontal Offset", "0", [
            ("%d" % mm, "%d mm" % mm, "<</cupsInteger9 %d>>setpagedevice" % (mm * 8))
            for mm in range(0, 21)
        ]),
        ("zcPostFeed", "打印后走纸/Feed After Print", "3", [  # 3 mm clears the CC4 tear/cut edge
            ("%d" % mm, "%d mm" % mm, "<</cupsInteger7 %d>>setpagedevice" % (mm * 8))
            for mm in (0, 1, 2, 3, 4, 5, 6, 8, 10, 15, 20, 30)
        ]),
        ("zcTrim", "连续纸裁掉尾部空白/Trim Blank Tail", "Off", [
            ("Off", "否/Off", "<</cupsInteger6 0>>setpagedevice"),
            ("On", "是/On", "<</cupsInteger6 1>>setpagedevice"),
        ]),
        ("zcCompress", "压缩传输(GZIP)/Compressed Transfer", "Off", [
            ("Off", "否/Off", "<</cupsInteger4 0>>setpagedevice"),
            ("On", "是/On", "<</cupsInteger4 1>>setpagedevice"),
        ]),
    ]


def main(model):
    m = MODELS[model]
    SIZES = build_sizes(m)
    out = []
    p = out.append
    p('*PPD-Adobe: "4.3"')
    p('*FormatVersion: "4.3"')
    p('*FileVersion: "1.0"')
    p('*LanguageVersion: English')
    p('*LanguageEncoding: ISOLatin1')
    p('*cupsLanguages: "zh_CN"')
    pcname = ("ZICOX" + model) if len(model) <= 3 else ("ZX" + model[:6])
    p('*PCFileName: "%s.PPD"' % pcname.upper())
    p('*Product: "(%s)"' % model)
    p('*Manufacturer: "Zicox"')
    p('*ModelName: "Zicox %s"' % model)
    p('*ShortNickName: "Zicox %s"' % model)
    p('*NickName: "Zicox %s CPCL %s Thermal Printer"' % (model, m["desc"]))
    p('*PSVersion: "(3010.000) 0"')
    p('*LanguageLevel: "3"')
    p('*ColorDevice: False')
    p('*DefaultColorSpace: Gray')
    p('*FileSystem: False')
    p('*Throughput: "1"')
    p('*LandscapeOrientation: Plus90')
    p('*TTRasterizer: Type42')
    p('*1284DeviceID: "MFG:Zicox;MDL:%s;"' % model)
    p('*APRemoteQueueID: ""')
    p('*cupsVersion: 2.2')
    p('*cupsModelNumber: 4')
    p('*cupsManualCopies: True')
    p('*cupsFilter: "application/vnd.cups-raster 0 %s"' % FILTER)
    p('*cupsBackSide: Normal')
    p('*cupsSNMPSupplies: False')
    p('')
    p('*HWMargins: 0 0 0 0')
    p('*VariablePaperSize: True')
    p('*MaxMediaWidth: "%s"' % fmt(pt(m["paper_mm"])))
    p('*MaxMediaHeight: "%s"' % fmt(pt(1000)))
    p('*NonUIOrderDependency: 10 AnySetup *CustomPageSize')
    p('*CustomPageSize True: "pop pop pop <</PageSize[5 -2 roll]/ImagingBBox null>>setpagedevice"')
    p('*ParamCustomPageSize Width: 1 points %s %s' % (fmt(pt(20)), fmt(pt(m["paper_mm"]))))
    p('*ParamCustomPageSize Height: 2 points %s %s' % (fmt(pt(10)), fmt(pt(1000))))
    p('*ParamCustomPageSize WidthOffset: 3 points 0 0')
    p('*ParamCustomPageSize HeightOffset: 4 points 0 0')
    p('*ParamCustomPageSize Orientation: 5 int 0 0')
    p('')

    dname = m["default"]

    for kw, ui in (("PageSize", "Media Size"), ("PageRegion", "Media Size")):
        p('*OpenUI *%s/%s: PickOne' % (kw, ui))
        p('*OrderDependency: 10 AnySetup *%s' % kw)
        p('*Default%s: %s' % (kw, dname))
        for key, w, h, en, zh_t, roll in SIZES:
            # cupsInteger10 = 1 marks width-only roll sizes: the filter trims the blank tail.
            p('*%s %s/%s: "<</PageSize[%s %s]/ImagingBBox null/cupsInteger10 %d>>setpagedevice"'
              % (kw, key, en, fmt(pt(w)), fmt(pt(h)), 1 if roll else 0))
        p('*CloseUI: *%s' % kw)
        p('')

    p('*DefaultImageableArea: %s' % dname)
    for key, w, h, en, zh_t, roll in SIZES:
        mg = margin(m, w)
        p('*ImageableArea %s/%s: "%s 0 %s %s"' % (key, en, fmt(pt(mg)), fmt(pt(w - mg)), fmt(pt(h))))
    p('*DefaultPaperDimension: %s' % dname)
    for key, w, h, en, zh_t, roll in SIZES:
        p('*PaperDimension %s/%s: "%s %s"' % (key, en, fmt(pt(w)), fmt(pt(h))))
    p('')

    p('*OpenUI *Resolution/Resolution: PickOne')
    p('*OrderDependency: 10 AnySetup *Resolution')
    p('*DefaultResolution: %ddpi' % DPI)
    p('*Resolution %ddpi/%d DPI: "<</HWResolution[%d %d]/cupsBitsPerColor 8/cupsRowCount 0'
      '/cupsRowFeed 0/cupsRowStep 0/cupsColorSpace 18/cupsColorOrder 0/cupsInteger11 %d>>setpagedevice"'
      % (DPI, DPI, DPI, DPI, m["head_mm"] * 8))
    p('*CloseUI: *Resolution')
    p('')

    p('*OpenUI *ColorModel/Color Mode: PickOne')
    p('*OrderDependency: 10 AnySetup *ColorModel')
    p('*DefaultColorModel: Gray')
    p('*ColorModel Gray/Grayscale: "<</cupsColorOrder 0/cupsColorSpace 18/cupsBitsPerColor 8>>setpagedevice"')
    p('*CloseUI: *ColorModel')
    p('')

    zh = []
    for key, w, h, en, zh_t, roll in SIZES:
        for kw in ("PageSize", "PageRegion"):
            zh.append('*zh_CN.%s %s/%s: ""' % (kw, key, zh_t))
    zh.append('*zh_CN.Translation PageSize/纸张尺寸: ""')
    zh.append('*zh_CN.Translation PageRegion/纸张尺寸: ""')
    zh.append('*zh_CN.Translation Resolution/分辨率: ""')
    zh.append('*zh_CN.Translation ColorModel/颜色模式: ""')
    zh.append('*zh_CN.ColorModel Gray/灰度: ""')

    for kw, ui, default, choices in options():
        en_ui, zh_ui = split(ui)
        p('*OpenUI *%s/%s: PickOne' % (kw, en_ui))
        p('*OrderDependency: 20 AnySetup *%s' % kw)
        p('*Default%s: %s' % (kw, default))
        zh.append('*zh_CN.Translation %s/%s: ""' % (kw, zh_ui))
        for name, text, code in choices:
            en_t, zh_t = split(text)
            p('*%s %s/%s: "%s"' % (kw, name, en_t, code))
            zh.append('*zh_CN.%s %s/%s: ""' % (kw, name, zh_t))
        p('*CloseUI: *%s' % kw)
        p('')

    p('*% Simplified Chinese localization (UTF-8)')
    out.extend(zh)
    p('')

    p('*DefaultFont: Courier')
    p('*Font Courier: Standard "(002.004S)" Standard ROM')
    p('*% End of Zicox_CC4.ppd')
    print("\n".join(out))


if __name__ == "__main__":
    model = sys.argv[1] if len(sys.argv) > 1 else "CC4"
    if model not in MODELS:
        sys.exit("unknown model %s; known: %s" % (model, ", ".join(MODELS)))
    main(model)
