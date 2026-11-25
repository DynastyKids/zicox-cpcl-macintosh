# Zicox CC4 CPCL Driver for macOS

[English](#english) | [中文](#中文)

### Branches / 分支

| Branch / 分支 | Printers / 打印机 | Status / 状态 |
|---|---|---|
| [**`main`**](https://github.com/DynastyKids/zicox-cpcl-macintosh/tree/main) ← you are here / 当前分支 | CC4 (4″) | Tested on a real printer / 已真机测试 |
| [`untested`](https://github.com/DynastyKids/zicox-cpcl-macintosh/tree/untested) | CC4 (4″), CC2 (2″), CC3 (3″), M35 (3″), HDT334 (4″) | Same driver approach, **not tested on real printers** / 相同驱动方案，**未经真机测试** |

To use the `untested` branch / 使用 `untested` 分支：
```bash
git clone -b untested https://github.com/DynastyKids/zicox-cpcl-macintosh.git
# or, in an existing clone / 或在已有的克隆中：
git checkout untested
```

---

## English

A CUPS printer driver for the **Zicox CC4** 104 mm thermal printer (CPCL command set) on macOS,
with **USB**, **Bluetooth** and **Wi-Fi** support. The protocol was implemented from the vendor's
Android SDK and verified on a real CC4.

> Unofficial project, not affiliated with Zicox.

### Components

| File | Purpose |
|---|---|
| `src/rastertozicox.c` | CUPS filter: converts raster pages into CPCL `CG` bitmaps, optionally wrapped in the vendor's GZIP frame |
| `tools/gen_ppd.py` | Generates the printer description `ppd/Zicox_CC4.ppd` (paper sizes, media tracking, darkness, speed …, with a Simplified Chinese UI) when you run `make` |
| `src/zicox-btrelay.m` | Bluetooth relay (`ZicoxBluetooth.app`): `127.0.0.1:9101/9102` ⇄ IOBluetooth RFCOMM (SPP) |
| `src/zicoxctl.c` | Command-line tool: status, serial number, self-test, feed, raw send, text with built-in printer fonts |
| `tools/cpcl_preview.py` | Renders a CPCL job to PNG so output can be checked without printing |

### Requirements

* macOS 11 or later (Apple silicon or Intel)
* Xcode Command Line Tools (provides `cc`, `make`, `python3`): `xcode-select --install`

### Install

```bash
git clone https://github.com/DynastyKids/zicox-cpcl-macintosh.git
cd zicox-cpcl-macintosh
make
./install.sh                 # asks for the administrator password
```

Options: `./install.sh --wifi <IP>` also adds a Wi-Fi queue; `--no-usb` / `--no-bt` skip those queues.
Uninstall with `./uninstall.sh`.

Installed files: filter in `/Library/Printers/Zicox/Filter/`, PPD at
`/Library/Printers/PPDs/Contents/Resources/Zicox_CC4.ppd`, `zicoxctl` in `/usr/local/bin/`, and the
Bluetooth relay `/Library/Printers/Zicox/ZicoxBluetooth.app`, which runs as the user LaunchAgent
`com.zicox.btrelay` (log: `/tmp/zicox-btrelay.log`).

Queues created:

* `Zicox_CC4_USB` — USB (`usb://Zicox/CC4?serial=…`, detected automatically)
* `Zicox_CC4_BT` — Bluetooth (`socket://127.0.0.1:9101`, through the relay)
* `Zicox_CC4_WiFi` — only with `--wifi <IP>` (raw TCP port 9100)

### Bluetooth

1. Pair the CC4 in System Settings → Bluetooth.
2. On current macOS, opening the `/dev/cu.CC4_xxxx` serial port created by pairing does **not** bring up
   the Bluetooth link, so the driver opens an RFCOMM channel through IOBluetooth instead. Bluetooth is
   privacy-protected: the first time, macOS asks whether "Zicox CC4 蓝牙打印" may use Bluetooth. Click
   **Allow** (you can change it later in System Settings → Privacy & Security → Bluetooth).
3. Test the link (printer switched on):
   ```bash
   zicoxctl status        # OK / PAPER OUT / COVER OPEN (via 127.0.0.1:9102)
   zicoxctl sn            # serial number
   zicoxctl selftest      # print the self-test page
   ```
4. Print to `Zicox CC4 (Bluetooth)`. Each job opens a Bluetooth connection (about 2–3 s) and closes it
   afterwards, so a phone can still use the printer between jobs.
5. The printer status is checked before each job. If paper is out or the cover is open, the job is refused
   and CUPS retries it. The same happens when the printer is off, out of range or connected to another
   device, so no job is lost.
6. The relay runs in the logged-in user's session; Bluetooth jobs wait until the next login after logging out.

The Bluetooth and Wi-Fi queues use **compressed transfer (GZIP)** by default (the vendor's frame format,
tested over Bluetooth): a 70×50 mm label shrinks from 24 KB to 4.4 KB. The USB queue does not compress
by default; to turn it on: `sudo lpadmin -p Zicox_CC4_USB -o zcCompress=On`

All queues default to **continuous paper**. For die-cut labels, set "Media Tracking" to "Label Gap" in the
print dialog, or change the queue default: `sudo lpadmin -p Zicox_CC4_BT -o zcMediaTracking=Gap`

### Print options (Print dialog → Printer Features)

| Option | Description |
|---|---|
| Media Size | See [Paper sizes](#paper-sizes); custom sizes 20–104 mm wide are also available |
| Media Tracking | Continuous (default) / Label Gap / Black Mark Left / Black Mark Right → `GAP-SENSE` / `BAR-SENSE` + `FORM`. **Do not choose Label Gap or Black Mark with continuous paper**: the printer keeps feeding while it looks for a gap or mark |
| Darkness | `CONTRAST 0–3` (default: printer setting) |
| Print Speed | `SPEED 0–5` |
| Dithering | Threshold (sharpest text and barcodes, default) / Error Diffusion (photos) / Ordered |
| Black Level | Lighter or darker overall |
| Horizontal Position / Offset | Left-aligned or centred on the 104 mm head, plus 0–20 mm fine offset |
| Feed After Print | Default **3 mm** (`POSTFEED 24`) so the print clears the tear/cut edge; 0–30 mm |
| Rotate 180 Degrees, Trim Blank Tail, Compressed Transfer | |

#### Paper sizes

Paper sits against the left side of the paper bay. The driver left-aligns from dot 0 of the print head,
so no offset is needed. All paper is treated as continuous by default.

Special sizes (width × height, at the top of the list):

| Name | Size |
|---|---|
| `50x25mm` | 50 × 25 mm |
| `50x30mm` | 50 × 30 mm |
| `50x80mm` | 50 × 80 mm |
| `50x100mm` | 50 × 100 mm (2″×4″) |
| `100x50mm` | 100 × 50 mm (4″×2″) |
| `100x100mm` | 100 × 100 mm (4″×4″) |
| `100x150mm` | 100 × 150 mm (4″×6″) |

Continuous rolls, by width only, 50–100 mm in 5 mm steps: `Roll50mm` (2″), `Roll55mm`, `Roll60mm` …
`Roll75mm` (3″) … `Roll100mm` (4″). **The default is `Roll100mm`.** The page is 200 mm long; the blank
top and bottom are trimmed, so the printout is only as long as its content (plus the 3 mm feed).
Special sizes always print at their full length.

Examples:
```bash
lp -d Zicox_CC4_BT -o PageSize=Roll50mm -o zcDarkness=Dark label.pdf
lpoptions -p Zicox_CC4_USB -l      # list all options
```

### Built-in printer fonts

When printing through CUPS, the whole page is rendered on the Mac as a bitmap, so any Mac font works and
the printer's fonts are not used. The built-in fonts (listed under FONTS on the self-test page, e.g.
Source Han Sans / 思源黑体, Source Han Serif / 思源宋体, SysFont) can be used directly with `zicoxctl text`,
which is faster:

```bash
zicoxctl text "Line 1\nLine 2"                    # font 24 (GBK 24×24 bitmap font)
zicoxctl text -f 思源黑体 -s 32 "思源黑体 32"       # as in the vendor sample: T 思源黑体 32 x y text
zicoxctl text -m gap -f 55 "small text"           # label paper (default -m cont, continuous)
echo "several lines" | zicoxctl text -
```
Text is sent as GBK by default (as in the vendor sample); use `-e UTF-8` for UTF-8.
The 00/09/10/80… numbers on the self-test page are font slots; how they map to the font argument of the
`T` command has not been verified.

### Other commands

```bash
zicoxctl feed            # feed to the next label (label paper only! continuous paper keeps feeding)
zicoxctl feed mark       # black-mark paper
zicoxctl send job.cpcl   # send a raw CPCL / ESC-POS file
zicoxctl -n 192.168.1.50 status    # Wi-Fi printer (without -n: the Bluetooth relay, 127.0.0.1:9102)
```
Send raw data over USB through CUPS: `lp -d Zicox_CC4_USB -o raw job.cpcl`.

### Debugging

```bash
# Inspect driver output without printing
cupsfilter -p ppd/Zicox_CC4.ppd -m application/vnd.cups-raster -o PageSize=100x50mm label.pdf > /tmp/l.ras
build/rastertozicox 1 me t 1 "" /tmp/l.ras > /tmp/l.cpcl
python3 tools/cpcl_preview.py /tmp/l.cpcl      # writes /tmp/l-p1.png …

tail -f /tmp/zicox-btrelay.log                 # Bluetooth relay log
cupsctl --debug-logging; tail -f /var/log/cups/error_log
```

### Protocol notes (from the vendor SDK)

* Page header `! 0 200 200 <height dots> <copies>`, `PAGE-WIDTH <dots>`; 8 dots/mm, 832-dot (104 mm) head.
* Bitmap `CG <bytes per row> <rows> <x> <y> <binary>`, 1 = black, MSB first.
* Compressed frame: standard gzip with bytes 4–7 replaced by the frame length and the last 4 bytes by the
  CRC32 of bytes `[8, len-4)`.
* Status `1D 99 00 00` → `1D 99 <s> FF` (bit 0 paper out, bit 1 cover open); serial number `1D 49 44 00`;
  self-test `1F 52 …ui_prn_selftest`.
* USB VID 0x0483 / PID 0x5743 (USB printer class), Bluetooth SPP, Wi-Fi TCP 9100.

---

## 中文

适用于 **Zicox CC4** 104mm 热敏打印机（CPCL 指令集）的 macOS CUPS 驱动，支持 **USB / 蓝牙 / Wi-Fi**。
协议依据厂商 Android SDK 实现，并已在 CC4 真机上验证。

> 非官方项目，与 Zicox 无关联。

### 组成

| 文件 | 作用 |
|---|---|
| `src/rastertozicox.c` | CUPS 过滤器：把系统光栅页转为 CPCL `CG` 位图（可选厂商 GZIP 压缩帧） |
| `tools/gen_ppd.py` | 运行 `make` 时生成打印机描述 `ppd/Zicox_CC4.ppd`（纸张尺寸、定位方式、浓度、速度等选项，含中文界面） |
| `src/zicox-btrelay.m` | 蓝牙中继（`ZicoxBluetooth.app`）：`127.0.0.1:9101/9102` ⇄ IOBluetooth RFCOMM（SPP） |
| `src/zicoxctl.c` | 命令行工具：查状态、序列号、自检、走纸、发送原始指令、用打印机内置字库打字 |
| `tools/cpcl_preview.py` | 把 CPCL 作业渲染成 PNG，不耗纸即可检查输出 |

### 系统要求

* macOS 11 或更新版本（Apple 芯片或 Intel）
* Xcode 命令行工具（提供 `cc`、`make`、`python3`）：`xcode-select --install`

### 安装

```bash
git clone https://github.com/DynastyKids/zicox-cpcl-macintosh.git
cd zicox-cpcl-macintosh
make
./install.sh                 # 会请求管理员密码
```

可选参数：`./install.sh --wifi <IP>` 额外创建 Wi-Fi 队列；`--no-usb` / `--no-bt` 跳过对应队列。
卸载：`./uninstall.sh`

安装位置：过滤器 `/Library/Printers/Zicox/Filter/`，PPD `/Library/Printers/PPDs/Contents/Resources/Zicox_CC4.ppd`，
`zicoxctl` 在 `/usr/local/bin/`，蓝牙中继 `/Library/Printers/Zicox/ZicoxBluetooth.app`，
以用户级 LaunchAgent `com.zicox.btrelay` 运行（日志 `/tmp/zicox-btrelay.log`）。

会创建的队列：

* `Zicox_CC4_USB` — USB（自动识别 `usb://Zicox/CC4?serial=…`）
* `Zicox_CC4_BT` — 蓝牙（`socket://127.0.0.1:9101`，经中继转发）
* `Zicox_CC4_WiFi` — 仅在 `--wifi <IP>` 时创建（裸 TCP 9100）

### 蓝牙

1. 系统设置 → 蓝牙，配对 CC4。
2. 在当前的 macOS 上，打开配对后生成的 `/dev/cu.CC4_xxxx` 串口**不会**建立蓝牙连接，
   所以驱动通过 IOBluetooth 直接打开 RFCOMM 通道。蓝牙属于隐私权限，首次运行时 macOS 会询问
   “Zicox CC4 蓝牙打印”是否可以使用蓝牙，请点**允许**（之后可在 系统设置 → 隐私与安全性 → 蓝牙 中修改）。
3. 测试链路（打印机需开机）：
   ```bash
   zicoxctl status        # OK / PAPER OUT / COVER OPEN（经 127.0.0.1:9102）
   zicoxctl sn            # 序列号
   zicoxctl selftest      # 打印自检页
   ```
4. 选择 `Zicox CC4 (Bluetooth)` 打印即可。每个作业建立一次蓝牙连接（约 2–3 秒）、发完断开，
   手机等设备在两次作业之间仍可连接。
5. 作业前会查询状态：缺纸/开盖时拒绝作业，CUPS 会自动重试。
   打印机关机、超出范围或正被其他设备连接时同样会重试，作业不会丢失。
6. 中继运行在登录用户的会话中，注销后蓝牙作业会等到下次登录再打印。

蓝牙和 Wi-Fi 队列默认开启 **压缩传输(GZIP)**（厂商 SDK 的压缩帧格式，已经蓝牙实测）：
70×50mm 样张从 24 KB 降到 4.4 KB。USB 队列默认不压缩，需要时可开启：
`sudo lpadmin -p Zicox_CC4_USB -o zcCompress=On`

所有队列默认按 **连续纸** 打印。换成标签纸时，在打印对话框里把“纸张定位”改为“标签间隙”，或者修改队列默认值：
`sudo lpadmin -p Zicox_CC4_BT -o zcMediaTracking=Gap`

### 打印选项（打印对话框 → 打印机功能）

| 选项 | 说明 |
|---|---|
| 纸张尺寸 | 见下方[纸张尺寸](#纸张尺寸)，另有自定义尺寸（宽 20–104mm） |
| 纸张定位 | 连续纸（默认）/ 标签间隙 / 左黑标 / 右黑标 → `GAP-SENSE` / `BAR-SENSE` + `FORM`。**装连续纸时不要选“标签间隙”或黑标**：打印机找不到间隙或黑标，会一直出纸 |
| 打印浓度 | `CONTRAST 0–3`（默认用打印机设置） |
| 打印速度 | `SPEED 0–5` |
| 图像处理 | 阈值（文字和条码最清晰，默认）/ 误差扩散（照片）/ 有序抖动 |
| 黑白阈值 | 整体偏淡或偏黑 |
| 横向位置 / 偏移 | 窄纸靠左或居中于 104mm 打印头，另可微调 0–20mm |
| 打印后走纸 | 默认 **3 mm**（`POSTFEED 24`），让打印内容越过切纸刀口；可选 0–30 mm |
| 旋转180度、连续纸裁掉尾部空白、压缩传输 | |

#### 纸张尺寸

纸张靠纸仓最左侧放置，驱动从打印头第 0 点开始左对齐，不需要偏移。默认所有纸都按连续纸打印。

特殊尺寸（宽 × 高，列表开头）：

| 名称 | 尺寸 |
|---|---|
| `50x25mm` | 50 × 25 mm |
| `50x30mm` | 50 × 30 mm |
| `50x80mm` | 50 × 80 mm |
| `50x100mm` | 50 × 100 mm (2″×4″) |
| `100x50mm` | 100 × 50 mm (4″×2″) |
| `100x100mm` | 100 × 100 mm (4″×4″) |
| `100x150mm` | 100 × 150 mm (4″×6″) |

连续纸（只按宽度区分，50–100mm 每 5mm 一档）：`Roll50mm` (2″)、`Roll55mm`、`Roll60mm` … `Roll75mm` (3″) …
`Roll100mm` (4″)，**默认 `Roll100mm`**。页面长 200mm，打印时自动裁掉顶部和底部空白，
内容多长就出多长的纸（再加默认 3mm 走纸）；特殊尺寸则按固定长度出纸。

命令行示例：
```bash
lp -d Zicox_CC4_BT -o PageSize=Roll50mm -o zcDarkness=Dark label.pdf
lpoptions -p Zicox_CC4_USB -l      # 查看全部选项
```

### 内置字库

经 CUPS 打印时，整页都由 Mac 渲染成位图，因此能使用 Mac 上的任何字体，不依赖打印机字库。
打印机内置字库（自检页 FONTS 部分：思源黑体、思源宋体、SysFont 等）可以通过 `zicoxctl text` 直接使用，速度更快：

```bash
zicoxctl text "第一行\n第二行"                     # 字体 24（GBK 24×24 点阵）
zicoxctl text -f 思源黑体 -s 32 "思源黑体 32 点"    # 厂商示例用法：T 思源黑体 32 x y 文本
zicoxctl text -m gap -f 55 "小号字"                # 标签纸（默认 -m cont 连续纸）
echo "多行文本" | zicoxctl text -
```
文本默认按 GBK 编码发送（与厂商示例一致），可用 `-e UTF-8` 改为 UTF-8。
自检页中的 00/09/10/80… 是字库槽号；它们与 `T` 指令中字体参数的对应关系尚未经过验证。

### 其他命令

```bash
zicoxctl feed            # 走到下一张标签（仅标签纸！连续纸会一直出纸）
zicoxctl feed mark       # 黑标纸
zicoxctl send job.cpcl   # 发送原始 CPCL/ESC-POS 文件
zicoxctl -n 192.168.1.50 status    # Wi-Fi 打印机（不加 -n 时默认经蓝牙中继 127.0.0.1:9102）
```
USB 原始数据请经 CUPS 发送：`lp -d Zicox_CC4_USB -o raw job.cpcl`。

### 调试

```bash
# 不打印，只看驱动输出
cupsfilter -p ppd/Zicox_CC4.ppd -m application/vnd.cups-raster -o PageSize=100x50mm label.pdf > /tmp/l.ras
build/rastertozicox 1 me t 1 "" /tmp/l.ras > /tmp/l.cpcl
python3 tools/cpcl_preview.py /tmp/l.cpcl      # 生成 /tmp/l-p1.png …

tail -f /tmp/zicox-btrelay.log                 # 蓝牙中继日志
cupsctl --debug-logging; tail -f /var/log/cups/error_log
```

### 协议要点（来自厂商 SDK）

* 页头 `! 0 200 200 <高度点数> <份数>`，`PAGE-WIDTH <点数>`；8 点/mm，打印头 832 点（104mm）。
* 位图 `CG <每行字节数> <行数> <x> <y> <二进制>`，1=黑，高位在左。
* 压缩帧：标准 gzip，第 4–7 字节改为帧总长，最后 4 字节改为 `[8, len-4)` 区间的 CRC32。
* 状态 `1D 99 00 00` → `1D 99 <s> FF`（bit0 缺纸，bit1 开盖）；序列号 `1D 49 44 00`；自检 `1F 52 …ui_prn_selftest`。
* USB VID 0x0483 / PID 0x5743（USB 打印机类），蓝牙 SPP，Wi-Fi TCP 9100。
