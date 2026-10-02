# LightPodcast

一个极简、绿色便携的 RSS 播客播放器。纯 C 语言 + 原生 Win32 API，单文件可执行，无需安装、不依赖额外 DLL。

## 功能

- RSS 播客订阅管理（添加 / 删除 / 调整顺序 / 刷新）
- 剧集列表（标题、日期、时长，列头可排序）
- 在线播放与本地缓存（音频自动下载到 `cache` 目录）
- 播放控制：上一曲 / 播放暂停 / 下一曲 / 停止 / 静音 / 音量
- 播放模式：顺序播放 / 随机播放 / 单曲循环 / 一次性播放
- 进度条拖动定位、播放中剧集高亮（绿色）、已缓存剧集标黄
- 三档字号调节（Ctrl + 鼠标滚轮）
- 可自定义缓存目录（支持打开 / 更改 / 移动已有缓存）
- 鼠标悬停提示：按钮说明、剧集完整文本、播客封面 + 标题 + 作者

## 文件说明

| 文件 | 说明 |
|------|------|
| `lightpodcast.exe` | 主程序 |
| `main.c` | 全部源代码 |
| `feeds.ini` | 配置与订阅源（首次运行自动生成） |
| `cache\` | 已下载音频缓存（文件名 = URL 哈希） |
| `resource.rc` / `resource.o` / `app.manifest` | 编译资源（图标、DPI 感知） |

## 配置（feeds.ini）

```ini
[settings]
sort=date:desc        ; 排序列: title/date/duration，方向: asc/desc
w1=323                ; 播客列宽占比（千分比）
w2=383                ; 剧集列宽占比（千分比）
cw0=110               ; 剧集列表三列像素宽
cw1=88
cw2=44
vol=80                ; 音量 0-100
font=0                ; 字号级别 0/1/2
winw=480              ; 窗口尺寸（客户区）
winh=360
cachedir=             ; 缓存目录（留空 = 程序目录下 cache）

[feeds]
https://example.com/feed1.xml
https://example.com/feed2.xml
```

> 配置文件仅在启动时读取、退出时保存，运行中不锁定。

## 编译

需要 MinGW-w64（gcc）：

```bash
windres resource.rc -O coff -o resource.o
gcc -O2 -s -mwindows -municode -o lightpodcast.exe main.c resource.o \
    -lcomctl32 -lwinhttp -lmf -lmfplat -lmfplay -lmfreadwrite -lmfuuid \
    -lole32 -luuid -lgdiplus -lm -lshell32
```

## 系统要求

- Windows 7 及以上
- 最小窗口尺寸 480×360（客户区）
