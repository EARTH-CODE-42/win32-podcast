/*
 * LiteTune - 纯 C / Win32 播客播放器
 *
 * 功能：
 *   - RSS 订阅保存在同目录 feeds.ini（订阅列表与界面配置同文件）
 *   - 后台线程下载并解析 RSS 2.0（含 itunes 扩展）
 *   - 音频先下载到 cache\ 缓存，再用 Media Foundation 播放，支持离线收听
 *   - 显示码率 / 采样率 / 声道 / 格式 / 进度，支持 seek、暂停、音量
 *   - 亮色/暗色主题、中英文双语、定时关机、字号调节、任务栏缩略图按钮
 *
 * 编译（MinGW）:
 *   windres resource.rc -O coff -o resource.o
 *   gcc -O2 -s -mwindows -municode -o LiteTune.exe main.c resource.o \
 *       -lcomctl32 -lwinhttp -lmf -lmfplat -lmfplay -lmfreadwrite -lole32 -lgdiplus
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WINVER       0x0601
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <propidl.h>
#include <propsys.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <share.h>
#include <time.h>

/* 只需 3 个音频属性键，手工定义以避免引入体积庞大的 propkey.h */
static const PROPERTYKEY PKEY_Title_L =
    { {0xf29f85e0,0x4ff9,0x1068,{0xab,0x91,0x08,0x00,0x2b,0x27,0xb3,0xd9}}, 2 };
static const PROPERTYKEY PKEY_Music_Artist_L =
    { {0x56a3372e,0xce9c,0x11d2,{0x9f,0x0e,0x00,0x60,0x97,0xc6,0x86,0xf6}}, 2 };
static const PROPERTYKEY PKEY_Music_AlbumTitle_L =
    { {0x56a3372e,0xce9c,0x11d2,{0x9f,0x0e,0x00,0x60,0x97,0xc6,0x86,0xf6}}, 4 };
static const PROPERTYKEY PKEY_Media_Duration_L =
    { {0x64440490,0x4c8b,0x11d1,{0x8b,0x70,0x08,0x00,0x36,0xb1,0x1a,0x03}}, 3 };

/* 只用到这 9 个 Media Foundation GUID，手工定义，不链接整个 mfuuid
 * 静态库（那会带入数百个 GUID 和属性名字符串，exe 多约 60KB）。
 * 头文件里的 DEFINE_GUID 在未定义 INITGUID 时只是 extern 声明，
 * 所以这里用自带的“定义型”宏真正分配存储。 */
#define DEFINE_GUID_L(name,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) \
    const GUID DECLSPEC_SELECTANY name = { l, w1, w2, { b1,b2,b3,b4,b5,b6,b7,b8 } }
DEFINE_GUID_L(MFAudioFormat_PCM,  0x00000001,0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);
DEFINE_GUID_L(MFAudioFormat_MP3,  0x00000055,0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);
DEFINE_GUID_L(MFAudioFormat_AAC,  0x00001610,0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);
DEFINE_GUID_L(MFAudioFormat_WMAudioV8,0x00000161,0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);
DEFINE_GUID_L(MFAudioFormat_WMAudioV9,0x00000162,0x0000,0x0010,0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71);
DEFINE_GUID_L(MF_MT_SUBTYPE,      0xf7e34c9a,0x42e8,0x4714,0xb7,0x4b,0xcb,0x29,0xd7,0x2c,0x35,0xe5);
DEFINE_GUID_L(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,0x1aab75c8,0xcfef,0x451c,0xab,0x95,0xac,0x03,0x4b,0x8e,0x17,0x31);
DEFINE_GUID_L(MF_MT_AUDIO_NUM_CHANNELS,0x37e48bf5,0x645e,0x4c5b,0x89,0xde,0xad,0xa9,0xe2,0x9b,0x69,0x6a);
DEFINE_GUID_L(MF_MT_AUDIO_SAMPLES_PER_SECOND,0x5faeeae7,0x0290,0x4c31,0x9e,0x8a,0xc5,0x34,0xf6,0x8d,0x9d,0xba);
/* IID_IPropertyStore：propsys 用，避免链接整个 libuuid（含数百 GUID，约 50KB） */
DEFINE_GUID_L(IID_IPropertyStore_L,0x886d8eeb,0x8cf2,0x4446,0x8d,0x02,0xcd,0xba,0x1d,0xbd,0xcf,0x99);
#define IID_IPropertyStore IID_IPropertyStore_L
/* 任务栏缩略图工具栏（ITaskbarList3）所需 GUID，同样手工定义 */
DEFINE_GUID_L(CLSID_TaskbarList_L,0x56fdf344,0xfd6d,0x11d0,0x95,0x8a,0x00,0x60,0x97,0xc9,0xa0,0x90);
DEFINE_GUID_L(IID_ITaskbarList3_L,0xea1afb91,0x9e28,0x4b86,0x90,0xe9,0x9e,0x9f,0x8a,0x5e,0xef,0xaf);
#define CLSID_TaskbarList CLSID_TaskbarList_L
#define IID_ITaskbarList3 IID_ITaskbarList3_L

/* ================= 常量 ================= */
#define MARGIN    8    /* 区域外边距 */
#define GAP_W     8    /* 区域间距（即可拖动分隔条的宽度） */
#define TOP_H     88   /* 顶部播放区高度（含边框） */
#define MAX_PODCASTS    64
#define MAX_FEED_LINE   2048
#define DL_BUF_SIZE     65536

#define WM_APP_FEED_ADDED   (WM_APP + 1)   /* lParam = Podcast* */
#define WM_APP_FEEDS_DONE   (WM_APP + 2)
#define WM_APP_EP_READY     (WM_APP + 3)   /* lParam = EpReady* */
#define WM_APP_DL_PROGRESS  (WM_APP + 4)   /* wParam = 0..100 */
#define WM_APP_MF_EVENT     (WM_APP + 5)   /* wParam = MFP_EVENT_TYPE, lParam = HRESULT */

/* 滑块几何消息（部分旧版 commctrl.h 未声明） */
#ifndef TBM_GETTHUMBRECT
#define TBM_GETTHUMBRECT    (WM_USER + 25)
#endif
#ifndef TBM_GETCHANNELRECT
#define TBM_GETCHANNELRECT  (WM_USER + 26)
#endif

/* ================= 数据结构 ================= */
typedef struct {
    wchar_t *title;
    wchar_t *desc;
    wchar_t *url;        /* enclosure URL（离线视图里是本地 mp3 完整路径） */
    wchar_t *author;     /* itunes:author（离线视图里是播客名称） */
    wchar_t  dateText[24];
    long long dateKey;   /* YYYYMMDDHHMM 分钟级，0=未知 */
    int      durationSec;
    int      origIdx;    /* RSS 原始顺序，排序 tiebreak */
    int      localFile;  /* 1=离线缓存视图：url 为本地路径，直接播放不联网 */
} Episode;

typedef struct {
    wchar_t *title;
    wchar_t *desc;
    wchar_t *author;     /* itunes:author 频道级 */
    wchar_t *lang;       /* language */
    wchar_t *link;       /* 频道主页 */
    wchar_t *copyright;
    char    *imageUrl;   /* 封面图 URL */
    char    *feedUrl;    /* 该播客的 RSS 订阅源地址 */
    HBITMAP  hImage;     /* 已解码的封面图，NULL=未下载或失败 */
    Episode *eps;
    int      epCount;
    int      epCap;
} Podcast;

typedef struct {
    int pod;
    wchar_t *url;   /* 按 URL 标识，排序导致索引变化也不受影响 */
    int ok;
    long gen;       /* 下载代号，过期任务的完成消息会被丢弃 */
} EpReady;

/* 排序列 */
enum { SORT_TITLE = 0, SORT_DATE = 1, SORT_DUR = 2 };

/* ================= 控件 ID ================= */
enum {
    IDC_ST_TIME = 100, IDC_ST_FORMAT, IDC_ST_VOL,
    IDC_SLD_SEEK, IDC_SLD_VOL,
    IDC_BTN_PREV, IDC_BTN_PLAY, IDC_BTN_NEXT, IDC_BTN_STOP, IDC_BTN_MUTE,
    IDC_BTN_ADD, IDC_BTN_DEL, IDC_BTN_SORT, IDC_BTN_REFRESH, IDC_BTN_PLAYMODE,
    IDC_BTN_DIR,
    IDC_LIST_POD, IDC_LIST_EP, IDC_EDIT_DESC
};

/* ================= 全局状态 ================= */
static Podcast  g_pods[MAX_PODCASTS];
static int      g_podCount = 0;
static int      g_curPod = -1, g_curEp = -1;

/* “已缓存”虚拟播客：聚合当前缓存目录里所有 mp3，断网也可浏览播放。
 * 固定挂在播客列表最后一行，下标哨兵 = g_podCount */
static Podcast  g_cachePod;
static int      g_viewCached = 0;     /* 剧集列表当前是否为缓存视图 */
static int      g_cacheRowHere = 0;   /* 列表框里是否已追加“已缓存”行 */

static HWND     g_hwnd;
static HFONT    g_font;          /* 8pt+级别 常规 */
static HFONT    g_fontBold;      /* 8pt+级别 加粗（播客标题） */
static HFONT    g_fontSmall;     /* 7pt+级别 灰色小字（副标题/格式串） */
static HBRUSH   g_whiteBrush = NULL;
static HWND     g_sldSeek, g_stTime;
static HWND     g_btnPrev, g_btnPlay, g_btnNext, g_btnStop, g_btnMute;
static HWND     g_sldVol, g_stVol, g_stFormat;
static HWND     g_listPod, g_listEp, g_editDesc;
static HWND     g_btnAdd, g_btnDel, g_btnSort, g_btnRefresh, g_btnPlayMode;
static HWND     g_btnDir;

/* 缓存目录：默认程序目录下 cache，可由用户更改，退出时存入 ini */
static wchar_t  g_cacheDir[MAX_PATH] = L"";

/* 工具提示 */
static HWND     g_hwndTip = NULL;        /* 系统 tooltip：按钮 */
static HWND     g_hwndPodTip = NULL;     /* 自定义 tooltip：播客封面+标题+作者 */
static int      g_tipPodIdx = -1;        /* 当前播客提示对应的条目下标 */

/* 顶栏状态文本（自绘跑马灯） */
static wchar_t  g_statusText[512] = L"就绪";   /* 当前显示内容 */
static wchar_t  g_statusBase[512] = L"就绪";   /* 持久内容（如“正在播放：…”），临时提示结束后恢复 */
static int      g_mqOffset = 0;       /* 滚动偏移（像素） */
static int      g_mqTextW = 0;        /* 状态文本像素宽，-1=需重测 */
static int      g_mqActive = 0;       /* 文本超长，正在滚动 */

#define TEMP_STATUS_MS 2600           /* 临时提示持续时间 */
static int      g_statusTempOn = 0;   /* 临时提示正在显示（定时器4在跑） */

/* 播客列表模式：0=普通 1=删除 2=排序（互斥） */
static int      g_podMode = 0;
static int      g_muted = 0;          /* 静音 */
static int      g_playMode = 0;       /* 播放模式：0=顺序 1=随机 2=单曲循环 */

/* feeds.txt 中的订阅源（UTF-8）与排序/布局配置 */
static char   **g_feedUrls = NULL;
static int      g_feedUrlCount = 0;
static int      g_sortCol = SORT_DATE;   /* 默认按日期 */
static int      g_sortDir = -1;          /* -1=降序 1=升序 */
static int      g_w1perm = 323;          /* 播客列宽，千分比 */
static int      g_w2perm = 383;          /* 剧集列宽，千分比 */
static int      g_volume = 80;           /* 音量 0-100 */
static int      g_winW = 480, g_winH = 360;  /* 窗口尺寸（默认=最小 4:3） */
static int      g_colW[3] = { 110, 88, 44 };  /* 剧集列表三列像素宽 */
static int      g_fontLevel = 0;  /* 字号级别 0=小(默认) 1=中 2=大，全局字体随级别+1pt */
static int      g_cfgFromTxt = 0;        /* 配置来自旧 feeds.txt，保存时迁移 */
static int      g_darkMode = 0;          /* 夜间模式（存 ini：dark=1） */
static int      g_lang = 0;              /* 界面语言：0=简体中文 1=English（存 ini：lang） */
static HBRUSH   g_bgBrush = NULL;        /* 主窗口背景刷，随主题重建 */

/* 定时关机：g_shutdownAt=UTC 秒（0=未启用），倒计时显示在标题栏 */
static long long g_shutdownAt = 0;

/* 任务栏缩略图工具栏（上一曲/播放暂停/下一曲/随机下一曲） */
static ITaskbarList3 *g_taskbar = NULL;
static UINT   g_wmTaskbarBtnCreated = 0;
static HICON  g_tbIco[6] = { NULL };   /* 0=prev 1=play 2=pause 3=next 4=shuffle */

static ULONG_PTR g_gdipToken = 0;

static volatile int g_feedsLoading = 0;
static volatile int g_downloading  = 0;
static int      g_fullReload = 0;    /* 本次加载是否为全量刷新（区别于单条添加） */

/* 播放 */
static IMFPMediaPlayer *g_player = NULL;
static int  g_playState = 0;          /* 0=停止 1=播放 2=暂停 */
static int  g_seeking   = 0;
static int  g_playPod = -1;         /* 正在播放的播客下标（哨兵 g_podCount=离线） */
static int  g_playEp  = -1;         /* 正在播放的剧集下标，独立于列表选中行 */
static wchar_t *g_playUrl = NULL;   /* 当前请求播放的音频 URL */
static long long g_dur100ns = 0;

/* ================= 多语言（简体中文 / English） ================= */
enum {
    TR_READY = 0,
    TR_SHUTDOWN_TITLE,          /* 标题栏倒计时格式串 */
    TR_COL_TITLE, TR_COL_DATE, TR_COL_DUR, TR_COL_PODCAST,
    TR_UNTITLED,
    TR_CACHED, TR_CACHE_SUBTITLE, TR_UNKNOWN_POD, TR_CACHED_FMT,
    TR_OFFLINE_LOCAL,
    TR_AUDIO, TR_MONO, TR_STEREO, TR_MULTICH,
    TR_CANT_OPEN, TR_LOADING_FEEDS, TR_PREPARING, TR_DL_THREAD_FAIL,
    TR_PLAYING_OFFLINE_FMT, TR_PLAYING_FMT,
    TR_STOPPED, TR_FINISHED, TR_PLAY_ERROR,
    TR_DL_PROGRESS_FMT, TR_AUDIO_GONE, TR_DL_FAILED, TR_NO_AUDIO_DEV,
    TR_OFFLINE_HINT, TR_NO_FEEDS_HINT,
    TR_F_TITLE, TR_F_DESC, TR_F_AUTHOR, TR_F_LANG, TR_F_HOME,
    TR_F_COPYRIGHT, TR_F_FEEDURL, TR_F_PUBDATE, TR_F_LOCALFILE, TR_F_LINK,
    TR_TIP_PODCAST_FMT, TR_TIP_PUBDATE_FMT, TR_TIP_DUR_FMT,
    TR_CACHE_VIEW_DESC,
    TR_INPUT_TITLE, TR_INPUT_LABEL, TR_OK, TR_CANCEL,
    TR_BAD_URL, TR_DUP_FEED, TR_MAX_FEEDS,
    TR_M_OPEN_CACHE, TR_M_CHANGE_CACHE, TR_M_DEFAULT_CACHE,
    TR_M_NIGHT, TR_M_LANGUAGE, TR_LANG_ZH, TR_LANG_EN,
    TR_M_SHUTDOWN, TR_M_SHUT_CANCEL, TR_SHUT_IN_MINS_FMT,
    TR_M_FONT_FMT, TR_FONT_S, TR_FONT_M, TR_FONT_L,
    TR_M_ABOUT, TR_ABOUT_TEXT,
    TR_NIGHT_ON, TR_NIGHT_OFF, TR_FONT_SWITCHED,
    TR_SHUT_SET, TR_SHUT_CANCELED, TR_SHUT_DENIED,
    TR_CACHE_CHANGED, TR_CACHE_RESET,
    TR_MOVE_Q_NEW_FMT, TR_MOVE_Q_DEF_FMT, TR_MOVE_CACHE, TR_BROWSE_CACHE,
    TR_TB_PREV, TR_TB_PLAY, TR_TB_PAUSE, TR_TB_NEXT, TR_TB_SHUFFLE,
    TR_TIP_PREV, TR_TIP_PLAYPAUSE, TR_TIP_NEXT, TR_TIP_STOP, TR_TIP_MUTE,
    TR_TIP_ADD, TR_TIP_DEL, TR_TIP_SORT, TR_TIP_REFRESH, TR_TIP_SETTINGS,
    TR_PM_ORDER, TR_PM_SHUFFLE, TR_PM_REPEAT, TR_PM_ONCE,
    TR_N
};

static const wchar_t *tr(int id)
{
    static const wchar_t *const zh[TR_N] = {
        L"就绪",
        L"LiteTune ｜ %d:%02d 后关机",
        L"标题", L"日期", L"时长", L"播客",
        L"(无标题)",
        L"已缓存", L"本地缓存音频，断网也可播放", L"未知播客", L"已缓存 (%d)",
        L"离线播放本地音频",
        L"音频", L"单声", L"立体", L"多声",
        L"无法打开音频文件", L"正在加载订阅...", L"准备音频...", L"下载线程创建失败",
        L"正在播放（离线）：%s", L"正在播放：%s",
        L"已停止", L"播放完毕", L"播放出错",
        L"下载音频中... %d%%", L"音频已失效", L"音频下载失败",
        L"无法开始播放：未找到可用的音频输出设备",
        L"没有订阅也能听：正在播放列表最后的「已缓存」，无需联网",
        L"还没有订阅，点左下角「+」按钮添加 RSS 播客",
        L"标题", L"描述", L"作者", L"语言", L"主页",
        L"版权", L"订阅地址", L"发布", L"本地文件", L"链接",
        L"\r\n播客：%s", L"\r\n发布：%s", L"\r\n时长：%s",
        L"【已缓存】\r\n本地缓存音频共 %d 个，无需联网即可播放。\r\n\r\n"
            L"缓存内容来自各播客已播放（自动下载）的音频；更改缓存目录请用右上角设置按钮。",
        L"添加播客订阅",
        L"输入 RSS 订阅地址（http:// 或 https://）：",
        L"确定", L"取消",
        L"地址无效，请输入以 http:// 或 https:// 开头的链接",
        L"该订阅地址已存在", L"订阅数量已达上限",
        L"打开缓存目录", L"更改缓存目录...", L"恢复默认目录",
        L"夜间模式", L"语言", L"简体中文", L"English",
        L"定时关机", L"取消定时关机", L"%d 分钟后关机",
        L"切换字号（当前：%s）", L"小", L"中", L"大",
        L"关于 LiteTune",
        L"LiteTune  版本 1.0\r\n\r\n"
            L"轻量级播客播放器：订阅 RSS、自动缓存、离线收听。\r\n\r\n"
            L"纯 C + Win32 API 编写，单文件绿色便携，\r\n不依赖任何第三方 DLL。",
        L"夜间模式已开启", L"夜间模式已关闭", L"字号已切换",
        L"定时关机已设定（关闭程序即可取消）", L"定时关机已取消",
        L"关机请求被系统拒绝",
        L"缓存目录已更改", L"缓存目录已恢复默认",
        L"当前缓存目录有 %d 个音频文件，是否移动到新目录？",
        L"当前缓存目录有 %d 个音频文件，是否移动到默认目录？",
        L"移动缓存", L"选择缓存目录",
        L"上一曲", L"播放", L"暂停", L"下一曲", L"随机下一曲",
        L"上一首", L"播放 / 暂停", L"下一首", L"停止", L"静音",
        L"添加播客", L"删除播客", L"调整顺序", L"刷新", L"设置",
        L"顺序播放", L"随机播放", L"单曲循环", L"一次性播放"
    };
    static const wchar_t *const en[TR_N] = {
        L"Ready",
        L"LiteTune ｜ Shut down in %d:%02d",
        L"Title", L"Date", L"Duration", L"Podcast",
        L"(Untitled)",
        L"Cached", L"Local cached audio, playable offline", L"Unknown podcast",
        L"Cached (%d)",
        L"Play local audio offline",
        L"Audio", L"Mono", L"Stereo", L"Multi",
        L"Cannot open audio file", L"Loading feeds...", L"Preparing audio...",
        L"Failed to start download thread",
        L"Playing (offline): %s", L"Playing: %s",
        L"Stopped", L"Playback finished", L"Playback error",
        L"Downloading audio... %d%%", L"Audio is no longer available",
        L"Audio download failed",
        L"Cannot play: no available audio output device",
        L"No subscription needed: select the last \"Cached\" entry to listen offline",
        L"No subscriptions yet. Click the \"+\" button at bottom-left to add an RSS feed",
        L"Title", L"Description", L"Author", L"Language", L"Homepage",
        L"Copyright", L"Feed URL", L"Published", L"Local file", L"URL",
        L"\r\nPodcast: %s", L"\r\nPublished: %s", L"\r\nDuration: %s",
        L"[Cached]\r\n%d local audio file(s), playable without a network.\r\n\r\n"
            L"Audio is cached automatically when played. Use the Settings button "
            L"at the top-right to change the cache folder.",
        L"Add Podcast Feed",
        L"Enter an RSS feed URL (http:// or https://):",
        L"OK", L"Cancel",
        L"Invalid URL. Please enter a link starting with http:// or https://",
        L"This feed already exists", L"Maximum number of feeds reached",
        L"Open Cache Folder", L"Change Cache Folder...", L"Restore Default Folder",
        L"Night Mode", L"Language", L"Simplified Chinese", L"English",
        L"Sleep Timer", L"Cancel Sleep Timer", L"Shut down in %d min",
        L"Font Size (Current: %s)", L"Small", L"Medium", L"Large",
        L"About LiteTune",
        L"LiteTune  version 1.0\r\n\r\n"
            L"A lightweight podcast player: subscribe to RSS feeds,\r\n"
            L"auto-cache episodes and listen offline.\r\n\r\n"
            L"Written in pure C with Win32 API. One portable executable,\r\n"
            L"no third-party DLLs required.",
        L"Night mode on", L"Night mode off", L"Font size changed",
        L"Sleep timer set (quit the app to cancel)", L"Sleep timer canceled",
        L"Shutdown request denied by the system",
        L"Cache folder changed", L"Cache folder restored to default",
        L"The current cache folder contains %d audio file(s). Move them to the new folder?",
        L"The current cache folder contains %d audio file(s). Move them to the default folder?",
        L"Move Cache", L"Select Cache Folder",
        L"Previous", L"Play", L"Pause", L"Next", L"Random next",
        L"Previous", L"Play / Pause", L"Next", L"Stop", L"Mute",
        L"Add podcast", L"Delete podcast", L"Reorder", L"Refresh", L"Settings",
        L"Sequential", L"Shuffle", L"Repeat one", L"Play once"
    };
    return (g_lang ? en : zh)[id];
}

/* exe 同目录 */
static void ExeDir(wchar_t *out, int outMax)
{
    wchar_t *p;
    GetModuleFileNameW(NULL, out, outMax);
    p = wcsrchr(out, L'\\');
    if (p) *(p + 1) = L'\0';
}

/* ================= 小工具 ================= */
static void ApplyLayout(HWND hwnd, int live);   /* 前置声明，字号切换后要重排 */
static void UpdateMarquee(void);                /* 前置声明，字号切换后要重测文本宽 */
static void UpdatePodTipFromPoint(POINT pt);    /* 前置声明，播客列表悬停提示 */
static char* WToU8(const wchar_t *w);           /* 前置声明，SaveConfig 保存缓存目录 */
static void UpdateThumbbarPlayPause(void);      /* 前置声明，播放/暂停状态切换时更新任务栏缩略图按钮 */

/* ---- 主题配色（夜间模式） ---- */
static COLORREF ThWinBg(void)   { return g_darkMode ? RGB(32,32,36)   : GetSysColor(COLOR_BTNFACE); }
static COLORREF ThPanel(void)   { return g_darkMode ? RGB(40,40,46)   : RGB(255,255,255); }
static COLORREF ThSel(void)     { return g_darkMode ? RGB(56,74,100)  : RGB(204,232,255); }
static COLORREF ThText(void)    { return g_darkMode ? RGB(230,230,230): RGB(20,20,20); }
static COLORREF ThDim(void)     { return g_darkMode ? RGB(160,160,168): RGB(120,120,120); }
static COLORREF ThGreen(void)   { return g_darkMode ? RGB(90,210,130) : RGB(0,130,60); }
static COLORREF ThGreenBg(void) { return g_darkMode ? RGB(28,56,38)   : RGB(222,244,228); }
static COLORREF ThYellow(void)  { return g_darkMode ? RGB(226,186,70) : RGB(190,155,0); }
static COLORREF ThTrack(void)   { return g_darkMode ? RGB(58,58,64)   : RGB(229,229,232); }  /* 滑块滑槽 */
static COLORREF ThAccent(void)  { return RGB(0,120,215);  }                                 /* 进度/滑块把手主色 */

/* 标题栏：无定时关机时固定 LiteTune，否则附倒计时 */
static void UpdateWindowTitle(void)
{
    wchar_t t[96];
    if (!g_hwnd) return;
    if (g_shutdownAt > 0) {
        int left = (int)(g_shutdownAt - (long long)time(NULL));
        if (left < 0) left = 0;
        swprintf(t, 96, tr(TR_SHUTDOWN_TITLE), left / 60, left % 60);
    } else {
        wcscpy_s(t, 96, L"LiteTune");
    }
    SetWindowTextW(g_hwnd, t);
}

/* Win10 1809+ 暗色标题栏（运行时加载 dwmapi，Win7 上静默失败，无副作用） */
typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
static void ApplyDarkTitlebar(HWND hwnd, int dark)
{
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (m) {
        DwmSetAttrFn p = NULL;
        *(void**)&p = (void*)GetProcAddress(m, "DwmSetWindowAttribute");
        if (p) {
            BOOL on = dark ? TRUE : FALSE;
            /* 20=DWMWA_USE_IMMERSIVE_DARK_MODE（20H1+），旧版 1903-2004 为 19 */
            p(hwnd, 20, &on, sizeof(on));
            p(hwnd, 19, &on, sizeof(on));
        }
        FreeLibrary(m);
    }
}

/* 给剧集表头的 3 列设置/取消自绘格式（选中播客会重建列，必须重新应用） */
static void SubclassHeader(void);
static void ApplyHeaderTheme(void)
{
    HWND hdr;
    int ci;
    if (!g_listEp) return;
    hdr = (HWND)SendMessageW(g_listEp, LVM_GETHEADER, 0, 0);
    if (!hdr) return;
    SubclassHeader();
    for (ci = 0; ci < 3; ci++) {
        HDITEMW hi;
        ZeroMemory(&hi, sizeof(hi));
        hi.mask = HDI_FORMAT;
        if (SendMessageW(hdr, HDM_GETITEMW, ci, (LPARAM)&hi)) {
            if (g_darkMode) hi.fmt |= HDF_OWNERDRAW;
            else            hi.fmt &= ~HDF_OWNERDRAW;
            SendMessageW(hdr, HDM_SETITEMW, ci, (LPARAM)&hi);
        }
    }
    InvalidateRect(hdr, NULL, TRUE);
}

/* 表头自身过程：暗色下补绘最后一列之外的空白区（表头默认背景是白色） */
static WNDPROC g_origHdrProc = NULL;
static LRESULT CALLBACK HeaderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT lr = CallWindowProcW(g_origHdrProc, hwnd, msg, wp, lp);
    if (g_darkMode && msg == WM_PAINT) {
        HDC hdc = GetDC(hwnd);
        RECT rc, last;
        GetClientRect(hwnd, &rc);
        if (hdc && SendMessageW(hwnd, HDM_GETITEMRECT, 2, (LPARAM)&last)
            && last.right < rc.right) {
            RECT gap = { last.right, rc.top, rc.right, rc.bottom };
            HBRUSH b = CreateSolidBrush(RGB(46, 46, 52));
            HPEN pn = CreatePen(PS_SOLID, 1, RGB(28, 28, 32));
            HGDIOBJ ob1 = SelectObject(hdc, b);
            HGDIOBJ ob2 = SelectObject(hdc, pn);
            FillRect(hdc, &gap, b);
            MoveToEx(hdc, rc.left, rc.bottom - 1, NULL);
            LineTo(hdc, rc.right, rc.bottom - 1);
            SelectObject(hdc, ob1);
            SelectObject(hdc, ob2);
            DeleteObject(b);
            DeleteObject(pn);
        }
        if (hdc) ReleaseDC(hwnd, hdc);
    }
    return lr;
}

static void SubclassHeader(void)
{
    HWND hdr = g_listEp ? (HWND)SendMessageW(g_listEp, LVM_GETHEADER, 0, 0) : NULL;
    if (hdr && !g_origHdrProc) {
        g_origHdrProc = (WNDPROC)SetWindowLongPtrW(hdr, GWLP_WNDPROC, (LONG_PTR)HeaderProc);
    }
}

/* 主题切换：重建背景刷、更新控件配色、表头/标题栏/非客户区滚动条一起重绘 */
static LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void LayoutControls(HWND hwnd);
static WNDPROC g_oldSeekProc = NULL, g_oldVolProc = NULL;   /* 滑块原过程（重建滑块也用） */

/* 销毁重建滑块：实测 trackbar 在主题切换后即使 NM_CUSTOMDRAW 正常触发、
 * 绘制代码执行无误，屏幕上仍保留旧亮色外观（comctl v6 显示缓存行为），
 * 而新建控件的首次绘制路径与程序启动时一致、已验证可靠 */
static void RecreateSlider(HWND *ph, WNDPROC *pold, int id, int maxRange, int pos)
{
    if (*ph) { DestroyWindow(*ph); *ph = NULL; }
    if (!g_hwnd) return;
    *ph = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(*ph, TBM_SETRANGE, TRUE, MAKELONG(0, maxRange));
    SendMessageW(*ph, TBM_SETPOS, TRUE, pos);
    *pold = (WNDPROC)SetWindowLongPtrW(*ph, GWLP_WNDPROC, (LONG_PTR)SliderProc);
}

static void ApplyTheme(void)
{
    /* 需要强制重绘的子控件：自绘按钮走 WM_DRAWITEM，滑块走 NM_CUSTOMDRAW，
     * 静态文字走 WM_CTLCOLORSTATIC。实测仅靠 RedrawWindow RDW_ALLCHILDREN
     * 在运行期切换时不会触发这些自绘路径，按钮/滑块会残留亮色。 */
    HWND ctrls[] = {
        g_sldSeek, g_sldVol, g_stTime, g_stVol, g_stFormat,
        g_btnPrev, g_btnPlay, g_btnNext, g_btnStop, g_btnMute,
        g_btnAdd, g_btnDel, g_btnSort, g_btnRefresh, g_btnPlayMode, g_btnDir,
        g_listPod, g_listEp, g_editDesc
    };
    int i;
    if (g_hwnd) {
        HBRUSH nb = CreateSolidBrush(ThWinBg());
        SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)nb);
        if (g_bgBrush) DeleteObject(g_bgBrush);
        g_bgBrush = nb;
    }
    if (g_whiteBrush) DeleteObject(g_whiteBrush);
    g_whiteBrush = CreateSolidBrush(ThPanel());
    if (g_listEp) {
        SendMessageW(g_listEp, LVM_SETBKCOLOR, 0, (LPARAM)ThPanel());
        SendMessageW(g_listEp, LVM_SETTEXTBKCOLOR, 0, (LPARAM)ThPanel());
        SendMessageW(g_listEp, LVM_SETTEXTCOLOR, 0, (LPARAM)ThText());
        ApplyHeaderTheme();
    }
    for (i = 0; i < (int)(sizeof(ctrls)/sizeof(ctrls[0])); i++)
        if (ctrls[i]) InvalidateRect(ctrls[i], NULL, TRUE);
    /* 两个 trackbar 重建（保留当前位置），并用 LayoutControls 重新定位 */
    if (g_hwnd) {
        int seekPos = g_sldSeek ? (int)SendMessageW(g_sldSeek, TBM_GETPOS, 0, 0) : 0;
        int volPos  = g_sldVol  ? (int)SendMessageW(g_sldVol,  TBM_GETPOS, 0, 0) : g_volume;
        RecreateSlider(&g_sldSeek, &g_oldSeekProc, IDC_SLD_SEEK, 1000, seekPos);
        RecreateSlider(&g_sldVol,  &g_oldVolProc,  IDC_SLD_VOL, 100, volPos);
        LayoutControls(g_hwnd);
    }
    ApplyDarkTitlebar(g_hwnd, g_darkMode);
    if (g_hwnd) {
        /* DWM 暗色标题栏属性运行期切换后，需重算非客户区并模拟一次
         * 激活同步，标题栏才会立即按新属性重绘 */
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
            SWP_FRAMECHANGED);
        SendMessageW(g_hwnd, WM_NCACTIVATE,
            (WPARAM)(GetForegroundWindow() == g_hwnd), 0);
        /* RDW_FRAME：让滚动条等非客户区也重绘为暗色 */
        RedrawWindow(g_hwnd, NULL, NULL,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE |
            RDW_FRAME | RDW_UPDATENOW);
    }
}

/* 滑块完全自绘（亮/暗两种主题同一种圆角造型，仅配色不同）：
 * 滑槽/已播放部分/蓝色把手；返回 1 表示已处理 */
static int DrawSliderTrack(LPNMCUSTOMDRAW cd, int isSeek)
{
    HDC hdc = cd->hdc;
    HWND sld = cd->hdr.hwndFrom;
    RECT rc = cd->rc, ch, th;
    HBRUSH bg, tb, acb;
    HPEN edge, oldPen_p;
    HGDIOBJ oldPen, oldBr;
    int rr;

    bg = CreateSolidBrush(ThWinBg());
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SendMessageW(sld, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
    SendMessageW(sld, TBM_GETTHUMBRECT, 0, (LPARAM)&th);

    oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    /* 滑槽（圆角） */
    tb = CreateSolidBrush(ThTrack());
    oldBr = SelectObject(hdc, tb);
    rr = (ch.bottom - ch.top) / 2;
    if (rr < 2) rr = 2;
    RoundRect(hdc, ch.left, ch.top, ch.right, ch.bottom, rr, rr);

    /* 进度滑块：已播放部分填充主色 */
    if (isSeek) {
        int pos = (int)SendMessageW(sld, TBM_GETPOS, 0, 0);
        RECT pf = ch;
        pf.left += 3; pf.top += 3; pf.bottom -= 3;
        pf.right = ch.left + 3 +
            (int)((long)(ch.right - ch.left - 6) * pos / 1000);
        acb = CreateSolidBrush(ThAccent());
        SelectObject(hdc, acb);
        FillRect(hdc, &pf, acb);
        DeleteObject(acb);
    }

    /* 把手：蓝色圆角块 + 浅边，保证在蓝色进度上也能看清 */
    acb = CreateSolidBrush(ThAccent());
    SelectObject(hdc, acb);
    edge = CreatePen(PS_SOLID, 1, RGB(235, 240, 248));
    oldPen_p = SelectObject(hdc, edge);
    RoundRect(hdc, th.left, th.top, th.right, th.bottom, 3, 3);
    SelectObject(hdc, oldPen_p);
    DeleteObject(edge);
    DeleteObject(acb);

    SelectObject(hdc, oldBr);
    DeleteObject(tb);
    SelectObject(hdc, oldPen);
    return 1;
}

/* 在按钮矩形内画一个方向三角形：dir 0=上 1=下 2=左 3=右 */
static void DrawScrollArrow(HDC hdc, const RECT *r, int dir, COLORREF col)
{
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    POINT tri[3];
    HBRUSH b = CreateSolidBrush(col);
    HGDIOBJ ob = SelectObject(hdc, b);
    HPEN p = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ op = SelectObject(hdc, p);
    switch (dir) {
    case 0: tri[0].x=cx-3; tri[0].y=cy+2; tri[1].x=cx+3; tri[1].y=cy+2;
            tri[2].x=cx;   tri[2].y=cy-3; break;           /* ▲ */
    case 1: tri[0].x=cx-3; tri[0].y=cy-2; tri[1].x=cx+3; tri[1].y=cy-2;
            tri[2].x=cx;   tri[2].y=cy+3; break;           /* ▼ */
    case 2: tri[0].x=cx+2; tri[0].y=cy-3; tri[1].x=cx+2; tri[1].y=cy+3;
            tri[2].x=cx-3; tri[2].y=cy; break;             /* ◀ */
    default:tri[0].x=cx-2; tri[0].y=cy-3; tri[1].x=cx-2; tri[1].y=cy+3;
            tri[2].x=cx+3; tri[2].y=cy; break;             /* ▶ */
    }
    Polygon(hdc, tri, 3);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(b); DeleteObject(p);
}

/* 夜间模式：把某控件非客户区的一根滚动条重绘为暗色 */
static void PaintDarkScrollBar(HWND hwnd, int vertical)
{
    SCROLLBARINFO sbi;
    SCROLLINFO si;
    RECT wr, r;
    HDC hdc;
    HBRUSH bg, thb;
    int bar, btn, track, thumbLen, off, total, page, maxPos, x0, y0;
    COLORREF thumbCol = RGB(104, 104, 112);
    COLORREF arrowCol = RGB(175, 175, 183);

    sbi.cbSize = sizeof(sbi);
    if (!GetScrollBarInfo(hwnd, vertical ? OBJID_VSCROLL : OBJID_HSCROLL, &sbi))
        return;
    if (sbi.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE))
        return;

    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(hwnd, vertical ? SB_VERT : SB_HORZ, &si))
        return;

    GetWindowRect(hwnd, &wr);
    r = sbi.rcScrollBar;
    OffsetRect(&r, -wr.left, -wr.top);   /* 屏幕坐标 → 窗口 DC 坐标 */

    hdc = GetDCEx(hwnd, NULL, DCX_WINDOW | DCX_CACHE);
    if (!hdc) return;

    /* 整条滚动条铺面板底色 */
    bg = CreateSolidBrush(ThPanel());
    FillRect(hdc, &r, bg);
    DeleteObject(bg);

    bar   = vertical ? (r.bottom - r.top) : (r.right - r.left);
    btn   = sbi.dxyLineButton;
    if (btn <= 0 || btn >= bar) { ReleaseDC(hwnd, hdc); return; }
    track = bar - 2 * btn;

    /* 两端箭头 */
    if (vertical) {
        RECT up = { r.left, r.top, r.right, r.top + btn };
        RECT dn = { r.left, r.bottom - btn, r.right, r.bottom };
        DrawScrollArrow(hdc, &up, 0, arrowCol);
        DrawScrollArrow(hdc, &dn, 1, arrowCol);
    } else {
        RECT lf = { r.left, r.top, r.left + btn, r.bottom };
        RECT rt = { r.right - btn, r.top, r.right, r.bottom };
        DrawScrollArrow(hdc, &lf, 2, arrowCol);
        DrawScrollArrow(hdc, &rt, 3, arrowCol);
    }

    /* 按 SIF_RANGE/PAGE 计算把手长度与位置（标准映射） */
    total = si.nMax - si.nMin;
    page  = si.nPage ? (int)si.nPage : 0;
    if (total <= 0) { ReleaseDC(hwnd, hdc); return; }
    if (page > 0)
        thumbLen = (int)((long)track * page / (total + 1));
    else
        thumbLen = track;
    if (thumbLen > track) thumbLen = track;
    if (thumbLen < 14 && track >= 14) thumbLen = 14;

    maxPos = total + 1 - page;
    if (maxPos < 1) maxPos = 1;
    off = (track - thumbLen > 0 && maxPos > 0)
        ? (int)((long)(si.nPos - si.nMin) * (track - thumbLen) / maxPos)
        : 0;

    thb = CreateSolidBrush(thumbCol);
    {
        HGDIOBJ ob = SelectObject(hdc, thb);
        HPEN np = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ op = SelectObject(hdc, np);
        if (vertical) {
            x0 = r.left + 2; y0 = r.top + btn + off;
            RoundRect(hdc, x0, y0, r.right - 2, y0 + thumbLen, 4, 4);
        } else {
            x0 = r.left + btn + off; y0 = r.top + 2;
            RoundRect(hdc, x0, y0, x0 + thumbLen, r.bottom - 2, 4, 4);
        }
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
    }
    DeleteObject(thb);
    ReleaseDC(hwnd, hdc);
}

/* 给列表/简介框重绘全部可见滚动条（在默认 WM_NCPAINT 之后调用） */
static void PaintDarkScrollBars(HWND hwnd)
{
    HDC hdc;
    RECT wr;
    if (!g_darkMode) return;
    PaintDarkScrollBar(hwnd, 1);
    PaintDarkScrollBar(hwnd, 0);
    /* WS_EX_CLIENTEDGE 凹陷边框默认是亮色，统一描一道暗边 */
    hdc = GetDCEx(hwnd, NULL, DCX_WINDOW | DCX_CACHE);
    if (hdc) {
        HPEN pn = CreatePen(PS_SOLID, 1, RGB(72, 72, 80));
        HGDIOBJ op = SelectObject(hdc, pn);
        GetWindowRect(hwnd, &wr);
        {
            int w = wr.right - wr.left, h = wr.bottom - wr.top;
            MoveToEx(hdc, 0, 0, NULL);
            LineTo(hdc, w - 1, 0);
            LineTo(hdc, w - 1, h - 1);
            LineTo(hdc, 0, h - 1);
            LineTo(hdc, 0, 0);
        }
        SelectObject(hdc, op);
        DeleteObject(pn);
        ReleaseDC(hwnd, hdc);
    }
}

/* 暗色表头自绘（WM_DRAWITEM/ODT_HEADER 由表头的父窗口 ListView 接收） */
static int DrawHeaderItem(DRAWITEMSTRUCT *di)
{
    wchar_t htext[64] = L"";
    RECT rc = di->rcItem, tr;
    HBRUSH bg = CreateSolidBrush(RGB(46, 46, 52));
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(28, 28, 32));
    HGDIOBJ op;
    /* 列文本直接向表头控件取，普通/缓存两种列序都能正确显示 */
    if (di->itemID < 3) {
        HDITEMW hi;
        ZeroMemory(&hi, sizeof(hi));
        hi.mask = HDI_TEXT;
        hi.pszText = htext;
        hi.cchTextMax = 64;
        SendMessageW(di->hwndItem, HDM_GETITEMW, di->itemID, (LPARAM)&hi);
    }
    FillRect(di->hDC, &rc, bg);
    DeleteObject(bg);
    /* 底分隔线 */
    op = SelectObject(di->hDC, sep);
    MoveToEx(di->hDC, rc.left, rc.bottom - 1, NULL);
    LineTo(di->hDC, rc.right, rc.bottom - 1);
    /* 列间竖分隔线 */
    if (di->itemID > 0) {
        MoveToEx(di->hDC, rc.left, 3, NULL);
        LineTo(di->hDC, rc.left, rc.bottom - 4);
    }
    SelectObject(di->hDC, op);
    DeleteObject(sep);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, RGB(210, 210, 214));
    SelectObject(di->hDC, g_font);
    tr = rc; tr.left += 8; tr.right -= 14;
    if (htext[0])
        DrawTextW(di->hDC, htext, -1, &tr,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    /* 当前排序列右侧画升降序小三角 */
    if ((int)di->itemID == g_sortCol) {
        int cx = rc.right - 9, cy = rc.top + (rc.bottom - rc.top) / 2;
        HBRUSH ab = CreateSolidBrush(RGB(210, 210, 214));
        HGDIOBJ ob = SelectObject(di->hDC, ab);
        POINT tri[3];
        if (g_sortDir < 0) {  /* 降序 ▼ */
            tri[0].x=cx-3; tri[0].y=cy-1;
            tri[1].x=cx+3; tri[1].y=cy-1;
            tri[2].x=cx;   tri[2].y=cy+4;
        } else {             /* 升序 ▲ */
            tri[0].x=cx-3; tri[0].y=cy+2;
            tri[1].x=cx+3; tri[1].y=cy+2;
            tri[2].x=cx;   tri[2].y=cy-3;
        }
        Polygon(di->hDC, tri, 3);
        SelectObject(di->hDC, ob);
        DeleteObject(ab);
    }
    return 1;
}

/* 创建指定字号/字重的微软雅黑字体 */
static HFONT CreateUiFont(int pt, int weight)
{
    HDC hdc = GetDC(NULL);
    int h = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

/* 把当前 g_font 应用到所有控件（改字号级别后也要重发 WM_SETFONT） */
static void ApplyFontsToControls(void)
{
    HWND ctrls[] = {
        g_sldSeek, g_stTime,
        g_btnPrev, g_btnPlay, g_btnNext, g_btnStop, g_btnMute,
        g_sldVol, g_stVol, g_stFormat,
        g_btnAdd, g_btnDel, g_btnSort, g_btnRefresh, g_btnPlayMode, g_btnDir,
        g_listPod, g_listEp, g_editDesc
    };
    int i;
    for (i = 0; i < (int)(sizeof(ctrls)/sizeof(ctrls[0])); i++)
        if (ctrls[i]) SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_stFormat) SendMessageW(g_stFormat, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
}

/* 直接设定字号级别 0/1/2 并重建三套字体、刷新界面 */
static void ChangeFontLevel(int nv)
{
    if (nv < 0) nv = 0;
    if (nv > 2) nv = 2;
    if (nv == g_fontLevel) return;
    g_fontLevel = nv;
    if (g_font)      DeleteObject(g_font);
    if (g_fontBold)  DeleteObject(g_fontBold);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    /* 级别0：正文8pt/播客标题8pt加粗/副标题7pt，每升一级全部+1pt */
    g_font      = CreateUiFont(8 + nv, FW_NORMAL);
    g_fontBold  = CreateUiFont(8 + nv, FW_BOLD);
    g_fontSmall = CreateUiFont(7 + nv, FW_NORMAL);
    ApplyFontsToControls();
    InvalidateRect(g_listPod, NULL, TRUE);
    InvalidateRect(g_listEp, NULL, TRUE);
    InvalidateRect(g_btnPlay, NULL, TRUE);
    InvalidateRect(g_btnMute, NULL, TRUE);
    ApplyLayout(g_hwnd, IsWindowVisible(g_hwnd));
    UpdateMarquee();
}

static void FmtTime(long long sec, wchar_t *buf, size_t n)
{
    if (sec < 0) sec = 0;
    if (sec >= 3600)
        swprintf(buf, n, L"%02d:%02d:%02d", (int)(sec/3600), (int)((sec%3600)/60), (int)(sec%60));
    else
        swprintf(buf, n, L"%02d:%02d", (int)(sec/60), (int)(sec%60));
}

/* 顶部播放区第一行（状态跑马灯）的客户区矩形，几何与 LayoutControls 保持一致 */
static void StatusRowRect(RECT *r)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    r->left = MARGIN + 8;
    r->right = rc.right - MARGIN - 8;
    r->top = MARGIN + 6;
    r->bottom = r->top + 18;
}

/* 文本变化/字号变化/窗口大小变化后：重测宽度，决定是否开启滚动定时器 */
static void UpdateMarquee(void)
{
    RECT r;
    HDC hdc;
    SIZE sz;
    HFONT old;
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    StatusRowRect(&r);
    hdc = GetDC(g_hwnd);
    old = (HFONT)SelectObject(hdc, g_font);
    sz.cx = 0; sz.cy = 0;
    GetTextExtentPoint32W(hdc, g_statusText, (int)wcslen(g_statusText), &sz);
    SelectObject(hdc, old);
    ReleaseDC(g_hwnd, hdc);
    g_mqTextW = sz.cx;
    if (sz.cx > (r.right - r.left)) {
        if (!g_mqActive) { g_mqActive = 1; SetTimer(g_hwnd, 2, 40, NULL); }
    } else {
        g_mqActive = 0;
        g_mqOffset = 0;
        KillTimer(g_hwnd, 2);
    }
    InvalidateRect(g_hwnd, &r, FALSE);
}

/* 设置持久状态（如“正在播放：…”）；若正在显示临时提示，只更新备份，
 * 等临时提示结束后自然恢复为新内容 */
static void SetStatus(const wchar_t *text)
{
    if (!text) text = L"";
    wcsncpy(g_statusBase, text, 511);
    g_statusBase[511] = 0;
    /* 有临时提示在显示时（定时器4在跑），不抢显示 */
    if (g_statusTempOn) return;
    wcsncpy(g_statusText, g_statusBase, 511);
    g_statusText[511] = 0;
    g_mqOffset = 0;
    UpdateMarquee();
}

/* 临时提示（如“夜间模式已开启”）：显示片刻后自动恢复持久状态 */
static void SetStatusTemp(const wchar_t *text)
{
    if (!text) text = L"";
    g_statusTempOn = 1;
    wcsncpy(g_statusText, text, 511);
    g_statusText[511] = 0;
    g_mqOffset = 0;
    UpdateMarquee();
    SetTimer(g_hwnd, 4, TEMP_STATUS_MS, NULL);   /* 重复调用会自动重置计时 */
}

/* 临时提示到期：恢复显示持久状态 */
static void RestoreBaseStatus(void)
{
    g_statusTempOn = 0;
    KillTimer(g_hwnd, 4);
    wcsncpy(g_statusText, g_statusBase, 511);
    g_statusText[511] = 0;
    g_mqOffset = 0;
    UpdateMarquee();
}

static wchar_t* U8ToW(const char *src)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    wchar_t *dst;
    if (n <= 0) return NULL;
    dst = (wchar_t*)malloc(n * sizeof(wchar_t));
    if (dst) MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, n);
    return dst;
}

/* ================= HTTP 下载 ================= */
/* 下载整个 URL 到内存。progressMsg!=0 时向 g_hwnd 汇报百分比。 */
/* pReqSlot：调用方提供的槽位，函数把请求句柄放进去，外部线程可关闭它来中止下载；
 * 传 NULL 表示不可取消（RSS/封面图）。句柄归还用 Interlocked 交换，避免双重关闭。
 * pProgGen：进度消息 lParam 带上该代号，主线程据此丢弃过期任务的进度。 */
static char* HttpFetch(const wchar_t *url, DWORD *outLen, UINT progressMsg,
                       void * volatile *pReqSlot, volatile long *pProgGen)
{
    URL_COMPONENTS uc;
    wchar_t host[256], path[2048];
    HINTERNET hSes = NULL, hCon = NULL, hReq = NULL;
    char *buf = NULL;
    DWORD cap = 0, total = 0;
    DWORD contentLen = 0;

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return NULL;

    hSes = WinHttpOpen(L"LiteTune/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return NULL;
    WinHttpSetTimeouts(hSes, 30000, 30000, 60000, 60000);

    hCon = WinHttpConnect(hSes, host,
        uc.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT
                                            : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hCon) goto fail;
    hReq = WinHttpOpenRequest(hCon, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) goto fail;

    /* 登记请求句柄到调用方槽位，供 UI 线程关闭以中止下载 */
    if (pReqSlot)
        InterlockedExchangePointer((PVOID volatile*)pReqSlot, (PVOID)hReq);

    /* 允许 gzip/deflate 自动解压 */
    {
        DWORD flags = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof(flags));
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto fail;
    if (!WinHttpReceiveResponse(hReq, NULL)) goto fail;

    {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &sz, NULL);
        if (status != 200) goto fail;
        sz = sizeof(contentLen);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &contentLen, &sz, NULL);
    }

    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) goto fail;
        if (avail == 0) break;
        if (total + avail + 1 > cap) {
            DWORD ncap = cap ? cap * 2 : 262144;
            char *nb;
            while (ncap < total + avail + 1) ncap *= 2;
            nb = (char*)realloc(buf, ncap);
            if (!nb) goto fail;
            buf = nb; cap = ncap;
        }
        if (!WinHttpReadData(hReq, buf + total, avail, &got)) goto fail;
        if (got == 0) break;
        total += got;
        if (progressMsg && contentLen > 0)
            PostMessageW(g_hwnd, progressMsg,
                (WPARAM)((unsigned __int64)total * 100 / contentLen),
                (LPARAM)(pProgGen ? *pProgGen : 0));
    }

    /* 成功：取回句柄自行关闭；若槽位已空说明被外部取消，数据不完整算失败 */
    {
        HINTERNET own = pReqSlot
            ? (HINTERNET)InterlockedExchangePointer((PVOID volatile*)pReqSlot, NULL)
            : hReq;
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        if (pReqSlot && !own) {        /* 被 UI 取消 */
            free(buf);
            return NULL;
        }
        WinHttpCloseHandle(own);
    }
    if (!buf) { buf = (char*)malloc(1); buf[0] = 0; }
    else buf[total] = 0;
    *outLen = total;
    return buf;

fail:
    {
        HINTERNET own = pReqSlot
            ? (HINTERNET)InterlockedExchangePointer((PVOID volatile*)pReqSlot, NULL)
            : hReq;
        if (own) WinHttpCloseHandle(own);   /* 外部已关闭则跳过 */
    }
    if (hCon) WinHttpCloseHandle(hCon);
    if (hSes) WinHttpCloseHandle(hSes);
    free(buf);
    return NULL;
}

/* ================= XML 解析 ================= */
/* 在 xml 中找 <tag ...>，tag 后必须是 > 空格 / 等边界 */
static const char* FindOpenTag(const char *xml, const char *tag)
{
    char needle[96];
    const char *p;
    snprintf(needle, sizeof(needle), "<%s", tag);
    p = xml;
    while ((p = strstr(p, needle)) != NULL) {
        char c = p[strlen(needle)];
        if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n')
            return p;
        p += strlen(needle);
    }
    return NULL;
}

/* 提取 <tag>content</tag> 的原始内容（复制到新缓冲区） */
static char* XmlText(const char *xml, const char *tag)
{
    const char *open = FindOpenTag(xml, tag);
    const char *gt, *close;
    char closeTag[104];
    int len;
    char *buf;
    if (!open) return NULL;
    gt = strchr(open, '>');
    if (!gt) return NULL;
    gt++;
    snprintf(closeTag, sizeof(closeTag), "</%s", tag);
    close = strstr(gt, closeTag);
    if (!close || close <= gt) return NULL;
    len = (int)(close - gt);
    buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, gt, len);
    buf[len] = 0;
    return buf;
}

/* 提取标签属性值（在 [tagStart, tagEnd) 范围内） */
static char* XmlAttrRange(const char *tagStart, const char *tagEnd, const char *attr)
{
    char needle[80];
    const char *p, *q;
    char *buf;
    int len;
    snprintf(needle, sizeof(needle), "%s=\"", attr);
    p = tagStart;
    while ((p = strstr(p, needle)) != NULL && p < tagEnd) {
        p += strlen(needle);
        q = strchr(p, '"');
        if (!q || q > tagEnd) return NULL;
        len = (int)(q - p);
        buf = (char*)malloc(len + 1);
        if (buf) { memcpy(buf, p, len); buf[len] = 0; }
        return buf;
    }
    /* 单引号 */
    snprintf(needle, sizeof(needle), "%s='", attr);
    p = tagStart;
    while ((p = strstr(p, needle)) != NULL && p < tagEnd) {
        p += strlen(needle);
        q = strchr(p, '\'');
        if (!q || q > tagEnd) return NULL;
        len = (int)(q - p);
        buf = (char*)malloc(len + 1);
        if (buf) { memcpy(buf, p, len); buf[len] = 0; }
        return buf;
    }
    return NULL;
}

/* CDATA 解包 + 实体解码 + 去 HTML 标签，原地处理 */
static void CleanXmlText(char *s)
{
    char *dst;
    const char *src;
    size_t n;

    /* CDATA */
    if (strncmp(s, "<![CDATA[", 9) == 0) {
        size_t len = strlen(s);
        if (len >= 12 && strcmp(s + len - 3, "]]>") == 0) {
            memmove(s, s + 9, len - 12);
            s[len - 12] = 0;
        }
    }
    /* 去 HTML 标签 + 解码实体 */
    dst = s; src = s;
    while (*src) {
        if (*src == '<') {
            const char *gt = strchr(src, '>');
            if (gt) {
                /* <br> <p> </p> <div> </div> 转成换行 */
                size_t tagLen = (size_t)(gt - src);
                if (tagLen <= 6 &&
                    (strnicmp(src, "<br", 3) == 0 || strnicmp(src, "</br>", 5) == 0 ||
                     strnicmp(src, "<p>", 3) == 0 || strnicmp(src, "</p>", 4) == 0 ||
                     strnicmp(src, "<div>", 5) == 0 || strnicmp(src, "</div>", 6) == 0)) {
                    *dst++ = '\r'; *dst++ = '\n';
                }
                src = gt + 1; continue;
            }
        }
        if (*src == '&') {
            if (strncmp(src, "&lt;", 4) == 0)        { *dst++ = '<';  src += 4; continue; }
            if (strncmp(src, "&gt;", 4) == 0)        { *dst++ = '>';  src += 4; continue; }
            if (strncmp(src, "&amp;", 5) == 0)       { *dst++ = '&';  src += 5; continue; }
            if (strncmp(src, "&quot;", 6) == 0)      { *dst++ = '"';  src += 6; continue; }
            if (strncmp(src, "&apos;", 6) == 0)      { *dst++ = '\''; src += 6; continue; }
            if (strncmp(src, "&nbsp;", 6) == 0)      { *dst++ = ' ';  src += 6; continue; }
            if (strncmp(src, "&#", 2) == 0) {
                const char *semi = strchr(src, ';');
                long code = 0;
                if (semi && semi - src < 12) {
                    if (src[2] == 'x' || src[2] == 'X') code = strtol(src + 3, NULL, 16);
                    else code = strtol(src + 2, NULL, 10);
                    if (code > 0 && code < 0x110000) {
                        /* 简单转 UTF-8 */
                        if (code < 0x80) *dst++ = (char)code;
                        else if (code < 0x800) {
                            *dst++ = (char)(0xC0 | (code >> 6));
                            *dst++ = (char)(0x80 | (code & 0x3F));
                        } else if (code < 0x10000) {
                            *dst++ = (char)(0xE0 | (code >> 12));
                            *dst++ = (char)(0x80 | ((code >> 6) & 0x3F));
                            *dst++ = (char)(0x80 | (code & 0x3F));
                        } else {
                            *dst++ = (char)(0xF0 | (code >> 18));
                            *dst++ = (char)(0x80 | ((code >> 12) & 0x3F));
                            *dst++ = (char)(0x80 | ((code >> 6) & 0x3F));
                            *dst++ = (char)(0x80 | (code & 0x3F));
                        }
                        src = semi + 1;
                        continue;
                    }
                }
            }
        }
        *dst++ = *src++;
    }
    *dst = 0;

    /* 首尾空白 */
    src = s;
    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') src++;
    if (src != s) memmove(s, src, strlen(src) + 1);
    n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = 0;
    /* \n -> \r\n（编辑框需要）。重新生成到新缓冲区再拷回太长，直接保留 \n 也可读，
       这里简单替换单个 \n 为 \r\n 需要扩容，交给调用方处理。 */
}

/* 把 UTF-8 文本中的 \n 转成 \r\n 并转为 wchar_t */
static wchar_t* U8ToWCrlf(const char *src)
{
    size_t extra = 0, i, len = strlen(src);
    char *tmp, *d;
    wchar_t *w;
    for (i = 0; i < len; i++)
        if (src[i] == '\n' && (i == 0 || src[i-1] != '\r')) extra++;
    tmp = (char*)malloc(len + extra + 1);
    if (!tmp) return NULL;
    d = tmp;
    for (i = 0; i < len; i++) {
        if (src[i] == '\n' && (i == 0 || src[i-1] != '\r')) *d++ = '\r';
        *d++ = src[i];
    }
    *d = 0;
    w = U8ToW(tmp);
    free(tmp);
    return w;
}

/* 取 RSS 频道文本 */
static wchar_t* GetXmlTextW(const char *xml, const char *tag, int crlf)
{
    char *raw = XmlText(xml, tag);
    wchar_t *w;
    if (!raw) return NULL;
    CleanXmlText(raw);
    w = crlf ? U8ToWCrlf(raw) : U8ToW(raw);
    free(raw);
    return w;
}

static int ParseDuration(const char *s)
{
    int h = 0, m = 0, sec = 0;
    if (!s || !*s) return 0;
    if (sscanf(s, "%d:%d:%d", &h, &m, &sec) == 3) return h*3600 + m*60 + sec;
    if (sscanf(s, "%d:%d", &m, &sec) == 2)         return m*60 + sec;
    return atoi(s);
}

/* RFC822（Tue, 03 Feb 2015 12:34:56 ...）/ ISO8601（2015-02-03T12:34:56...）
 * → 分钟数（year*12*31*24*60 近似），同时输出 "YYYY/MM/DD HH:MM" */
static long long ParseDate(const char *s, wchar_t *outText, int outN)
{
    static const char *months[] =
        { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, i;
    const char *mp = NULL;
    long long key;

    if (outText && outN > 0) outText[0] = 0;
    if (!s) return 0;

    /* ISO8601: 2015-02-03T12:34:56... / 2015-02-03 12:34 */
    if (s[0] && isdigit((unsigned char)s[0]) && s[4] == '-' && s[7] == '-') {
        year = atoi(s);
        mon  = atoi(s + 5);
        day  = atoi(s + 8);
        if (s[10] == 'T' || s[10] == ' ') {
            hour = atoi(s + 11);
            if (s[13] == ':') min = atoi(s + 14);
        }
    } else {
        for (i = 0; i < 12; i++) {
            const char *hit = strstr(s, months[i]);
            if (hit) { mp = hit; mon = i + 1; break; }
        }
        if (mp) {
            const char *p;
            /* 年份：月份之后第一个 4 位数 */
            for (p = mp + 3; *p; p++) {
                if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
                    isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3])) {
                    year = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
                    break;
                }
            }
            /* 时分：年份之后形如 "HH:MM" */
            for (p = mp + 3; *p; p++) {
                if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
                    p[2] == ':' && isdigit((unsigned char)p[3]) && isdigit((unsigned char)p[4])) {
                    int h = (p[0]-'0')*10 + (p[1]-'0');
                    int m = (p[3]-'0')*10 + (p[4]-'0');
                    if (h >= 0 && h < 24 && m >= 0 && m < 60) { hour = h; min = m; }
                    break;
                }
            }
            /* 日：月份之前最后一个 1~2 位数 */
            for (p = mp - 1; p >= s; p--) {
                if (isdigit((unsigned char)p[0])) {
                    int d = *p - '0';
                    if (p > s && isdigit((unsigned char)p[-1])) { d += (p[-1]-'0')*10; }
                    day = d;
                    break;
                }
            }
        }
    }

    if (year >= 1900 && mon >= 1 && mon <= 12 && day >= 1 && day <= 31) {
        if (outText) swprintf(outText, outN, L"%04d/%02d/%02d %02d:%02d", year, mon, day, hour, min);
        /* 分钟级排序键：年月日时分压进一个 long long，比较即排序 */
        key = (long long)year * 100000000LL + (long long)mon * 1000000LL
            + (long long)day * 10000LL + (long long)hour * 100LL + min;
        return key;
    }
    return 0;
}

/* 解析 RSS，填充 Podcast */
static int ParseFeed(const char *xml, Podcast *pod)
{
    const char *chanEnd, *item;

    memset(pod, 0, sizeof(*pod));

    /* 频道信息：只取第一个 <item> 之前的部分，避免拿到单集标题 */
    item = FindOpenTag(xml, "item");
    chanEnd = item ? item : xml + strlen(xml);
    {
        size_t clen = (size_t)(chanEnd - xml);
        char *head = (char*)malloc(clen + 1);
        if (!head) return 0;
        memcpy(head, xml, clen); head[clen] = 0;
        pod->title = GetXmlTextW(head, "title", 0);
        if (!pod->title) pod->title = GetXmlTextW(head, "itunes:name", 0);
        pod->desc = GetXmlTextW(head, "description", 1);
        if (!pod->desc) pod->desc = GetXmlTextW(head, "itunes:summary", 1);
        pod->author = GetXmlTextW(head, "itunes:author", 0);
        pod->lang = GetXmlTextW(head, "language", 0);
        pod->link = GetXmlTextW(head, "link", 0);
        pod->copyright = GetXmlTextW(head, "copyright", 0);

        /* 封面图：itunes:image 属性 或 image/url 子节点 */
        {
            const char *im = strstr(head, "<itunes:image");
            if (im) {
                const char *gt = strchr(im, '>');
                if (gt) pod->imageUrl = XmlAttrRange(im, gt + 1, "href");
            }
            if (!pod->imageUrl) {
                char *u = XmlText(head, "url");
                if (u) { pod->imageUrl = u; }
            }
            if (pod->imageUrl) CleanXmlText(pod->imageUrl);
        }
        free(head);
    }
    if (!pod->title) pod->title = _wcsdup(tr(TR_UNTITLED));
    if (!pod->desc)  pod->desc  = _wcsdup(L"");

    /* 遍历 <item> */
    while (item) {
        const char *itemEnd = strstr(item, "</item>");
        const char *enc, *encClose;
        char *seg;
        size_t segLen;
        Episode *e;
        char *encUrl = NULL;

        if (!itemEnd) break;
        segLen = (size_t)(itemEnd - item) + 7;
        seg = (char*)malloc(segLen + 1);
        if (!seg) break;
        memcpy(seg, item, segLen); seg[segLen] = 0;

        /* enclosure url */
        enc = strstr(seg, "<enclosure");
        if (enc) {
            encClose = strchr(enc, '>');
            if (encClose) encUrl = XmlAttrRange(enc, encClose + 1, "url");
        }
        if (!encUrl) {  /* 部分 Atom 风格 */
            enc = strstr(seg, "<media:content");
            if (enc) {
                encClose = strchr(enc, '>');
                if (encClose) encUrl = XmlAttrRange(enc, encClose + 1, "url");
            }
        }

        if (encUrl && pod->epCount < 10000) {
            char *d;
            if (pod->epCount >= pod->epCap) {
                pod->epCap = pod->epCap ? pod->epCap * 2 : 32;
                pod->eps = (Episode*)realloc(pod->eps, pod->epCap * sizeof(Episode));
                if (!pod->eps) { free(encUrl); free(seg); return 0; }
            }
            e = &pod->eps[pod->epCount];
            memset(e, 0, sizeof(*e));
            e->origIdx = pod->epCount;
            e->title = GetXmlTextW(seg, "title", 0);
            if (!e->title) e->title = _wcsdup(tr(TR_UNTITLED));
            e->desc = GetXmlTextW(seg, "description", 1);
            if (!e->desc) e->desc = GetXmlTextW(seg, "itunes:summary", 1);
            if (!e->desc) e->desc = GetXmlTextW(seg, "content:encoded", 1);
            if (!e->desc) e->desc = _wcsdup(L"");
            e->author = GetXmlTextW(seg, "itunes:author", 0);
            CleanXmlText(encUrl); /* 先解码 XML 转义（如 &amp; -> &），再转宽字符 */
            e->url = U8ToW(encUrl);
            if (!e->url) { free(encUrl); free(seg); item = FindOpenTag(itemEnd, "item"); continue; }
            d = XmlText(seg, "itunes:duration");
            if (!d) d = XmlText(seg, "duration");
            if (d) { e->durationSec = ParseDuration(d); free(d); }
            /* 发布日期：RSS pubDate，Atom published/updated 兜底 */
            d = XmlText(seg, "pubDate");
            if (!d) d = XmlText(seg, "published");
            if (!d) d = XmlText(seg, "updated");
            if (!d) d = XmlText(seg, "date");
            if (d) {
                CleanXmlText(d);
                e->dateKey = ParseDate(d, e->dateText, 24);
                free(d);
            }
            pod->epCount++;
            free(encUrl);
        }
        free(seg);
        item = FindOpenTag(itemEnd, "item");
    }
    return pod->epCount > 0;
}

/* ================= 缓存 ================= */
/* 返回当前缓存目录（末尾带反斜杠）；未设置时默认程序目录下 cache */
static void CacheDir(wchar_t *out, int outMax)
{
    if (g_cacheDir[0]) {
        wcsncpy(out, g_cacheDir, outMax - 1);
        out[outMax - 1] = 0;
    } else {
        ExeDir(out, outMax);
        wcscat_s(out, outMax, L"cache");
    }
    int len = (int)wcslen(out);
    if (len > 0 && out[len - 1] != L'\\' && len < outMax - 1) {
        out[len] = L'\\'; out[len + 1] = 0;
    }
}

static void CachePathFor(const wchar_t *url, wchar_t *out, int outMax)
{
    unsigned long h = 5381;
    const wchar_t *p = url;
    wchar_t dir[MAX_PATH];
    while (*p) h = ((h << 5) + h) ^ (unsigned long)*p++;
    CacheDir(dir, MAX_PATH);
    swprintf(out, outMax, L"%s%08lx.mp3", dir, h);
}

static void EnsureCacheDir(void)
{
    wchar_t dir[MAX_PATH];
    CacheDir(dir, MAX_PATH);
    dir[wcslen(dir) - 1] = 0;   /* 去掉末尾反斜杠 */
    CreateDirectoryW(dir, NULL);
}

/* 剧集是否已缓存到本地 */
static int EpisodeIsCached(Episode *e)
{
    wchar_t path[MAX_PATH];
    if (!e || !e->url) return 0;
    CachePathFor(e->url, path, MAX_PATH);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* ================= 缓存元数据（离线“已缓存”视图用） ================= */
/* 前置声明：离线视图要复用排序/列设置/缓存统计（定义在后面） */
static void SortPodcast(Podcast *p);
static void SetEpisodeColumns(int cached);
static int  CountMp3InDir(const wchar_t *dir);
static void FreePodcast(Podcast *p);
static void ProbeAudioMeta(const wchar_t *path, Episode *e);
static void FreeCachePodcast(void)
{
    FreePodcast(&g_cachePod);
    ZeroMemory(&g_cachePod, sizeof(g_cachePod));
}

/* 扫描缓存目录，用 mp3 重建虚拟播客 */
static void LoadCachePodcast(void)
{
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    g_cachePod.title  = _wcsdup(tr(TR_CACHED));
    g_cachePod.author = _wcsdup(tr(TR_CACHE_SUBTITLE));

    CacheDir(dir, MAX_PATH);
    swprintf(pat, MAX_PATH, L"%s*.mp3", dir);
    hFind = FindFirstFileW(pat, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        Episode *e;
        wchar_t mp[MAX_PATH], base[MAX_PATH];
        int dot;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        if (g_cachePod.epCount >= g_cachePod.epCap) {
            int nc = g_cachePod.epCap ? g_cachePod.epCap * 2 : 16;
            Episode *ne = (Episode*)realloc(g_cachePod.eps, nc * sizeof(Episode));
            if (!ne) break;
            g_cachePod.eps = ne;
            g_cachePod.epCap = nc;
        }
        e = &g_cachePod.eps[g_cachePod.epCount];
        ZeroMemory(e, sizeof(Episode));
        e->localFile = 1;
        e->origIdx = g_cachePod.epCount;

        swprintf(mp, MAX_PATH, L"%s%s", dir, fd.cFileName);
        e->url = _wcsdup(mp);

        /* 默认标题：去掉扩展名的文件名（哈希名），再从 mp3 元数据覆盖 */
        wcsncpy(base, fd.cFileName, MAX_PATH - 1);
        base[MAX_PATH - 1] = 0;
        dot = (int)wcslen(base);
        if (dot >= 4) base[dot - 4] = 0;
        e->title  = _wcsdup(base);
        e->author = _wcsdup(tr(TR_UNKNOWN_POD));

        /* 直接从 mp3 的 ID3 标签补全标题/播客名/时长（无标签则保留默认值） */
        ProbeAudioMeta(mp, e);
        g_cachePod.epCount++;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    SortPodcast(&g_cachePod);
}

/* 缓存目录里的 mp3 数量（供列表行右侧显示，避免每次绘制都扫盘） */
static int g_cacheFileCount = 0;

/* 同步播客列表最后的“已缓存”虚拟行：有缓存才显示 */
static void RefreshCachedEntry(void)
{
    int need, total, has;
    wchar_t row[64];
    if (!g_listPod) return;
    {
        wchar_t cdir[MAX_PATH];
        CacheDir(cdir, MAX_PATH);
        g_cacheFileCount = CountMp3InDir(cdir);
    }
    need = g_cacheFileCount > 0;
    total = (int)SendMessageW(g_listPod, LB_GETCOUNT, 0, 0);
    has = total > g_podCount;
    if (need && !has) {
        int sel = (int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0);
        swprintf(row, 64, tr(TR_CACHED_FMT), g_cacheFileCount);
        SendMessageW(g_listPod, LB_ADDSTRING, 0, (LPARAM)row);
        g_cacheRowHere = 1;
        if (sel >= 0) SendMessageW(g_listPod, LB_SETCURSEL, sel, 0);
    } else if (!need && has) {
        SendMessageW(g_listPod, LB_DELETESTRING, g_podCount, 0);
        g_cacheRowHere = 0;
        if (g_viewCached) {
            /* 缓存被清空/目录变空：退回普通视图 */
            g_viewCached = 0;
            FreeCachePodcast();
            SetEpisodeColumns(0);
            SendMessageW(g_listEp, LVM_DELETEALLITEMS, 0, 0);
            SetWindowTextW(g_editDesc, L"");
        }
    } else if (need && has) {
        /* 数量可能变化（新增下载/移动目录），更新行文本；保留原选中 */
        int sel = (int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0);
        swprintf(row, 64, tr(TR_CACHED_FMT), g_cacheFileCount);
        SendMessageW(g_listPod, LB_DELETESTRING, g_podCount, 0);
        SendMessageW(g_listPod, LB_INSERTSTRING, g_podCount, (LPARAM)row);
        if (sel >= 0) SendMessageW(g_listPod, LB_SETCURSEL, sel, 0);
    }
}

/* ================= 配置（feeds.ini：订阅源 + 排序 + 布局） ================= */
static void AddFeedUrl(const char *s)
{
    char **p = (char**)realloc(g_feedUrls, (g_feedUrlCount + 1) * sizeof(char*));
    if (!p) return;
    g_feedUrls = p;
    g_feedUrls[g_feedUrlCount] = _strdup(s);
    if (g_feedUrls[g_feedUrlCount]) g_feedUrlCount++;
}

static void ParseSortValue(const char *v)
{
    if (strncmp(v, "title", 5) == 0)         g_sortCol = SORT_TITLE;
    else if (strncmp(v, "date", 4) == 0)     g_sortCol = SORT_DATE;
    else if (strncmp(v, "duration", 8) == 0) g_sortCol = SORT_DUR;
    if (strstr(v, ":asc"))  g_sortDir = 1;
    if (strstr(v, ":desc")) g_sortDir = -1;
}

/* feeds.ini 格式：
 *   [settings] 下 sort=title|date|duration:asc|desc, w1/w2=列宽千分比
 *   [feeds] 下一行一个 RSS 地址（裸 URL，直接粘贴）
 *   兼容旧版无 section 的 feeds.txt */
/* 只在程序启动时调用一次（读取订阅与全部界面设置）；
 * 运行期间的增删/排序/刷新都在内存里进行，退出时由 SaveConfig 统一写回 */
static void LoadConfig(void)
{
    wchar_t iniPath[MAX_PATH], txtPath[MAX_PATH], usePath[MAX_PATH];
    FILE *f;
    char line[MAX_FEED_LINE];
    int i;

    for (i = 0; i < g_feedUrlCount; i++) free(g_feedUrls[i]);
    free(g_feedUrls);
    g_feedUrls = NULL; g_feedUrlCount = 0;
    g_sortCol = SORT_DATE; g_sortDir = -1;
    g_w1perm = 323; g_w2perm = 383;
    g_volume = 80; g_winW = 480; g_winH = 360;
    g_colW[0] = 110; g_colW[1] = 88; g_colW[2] = 44;
    g_fontLevel = 0;
    g_darkMode = 0;
    g_lang = 0;
    g_cfgFromTxt = 0;

    ExeDir(iniPath, MAX_PATH);
    wcscat_s(iniPath, MAX_PATH, L"feeds.ini");
    ExeDir(txtPath, MAX_PATH);
    wcscat_s(txtPath, MAX_PATH, L"feeds.txt");

    if (GetFileAttributesW(iniPath) != INVALID_FILE_ATTRIBUTES)
        wcscpy_s(usePath, MAX_PATH, iniPath);
    else if (GetFileAttributesW(txtPath) != INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(usePath, MAX_PATH, txtPath);
        g_cfgFromTxt = 1;   /* 退出保存时迁移成 ini */
    } else return;

    /* _SH_DENYNO：不拒绝其他进程读写，文件被编辑器打开也能读，读完立即关闭 */
    f = _wfsopen(usePath, L"rb", _SH_DENYNO);
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        char *s = line, *eq;
        size_t n;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        while (*s == ' ' || *s == '\t') s++;
        n = strlen(s);
        while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
            s[--n] = 0;
        if (*s == 0 || *s == '#' || *s == ';' || *s == '[') continue;

        eq = strchr(s, '=');
        if (eq) {
            char *val = eq + 1;
            *eq = 0;
            while (eq > s && (eq[-1]==' '||eq[-1]=='\t')) { eq--; *eq = 0; }
            if (strcmp(s, "sort") == 0) ParseSortValue(val);
            else if (strcmp(s, "w1") == 0) g_w1perm = atoi(val);
            else if (strcmp(s, "w2") == 0) g_w2perm = atoi(val);
            else if (strcmp(s, "vol") == 0) { g_volume = atoi(val); if (g_volume < 0) g_volume = 0; if (g_volume > 100) g_volume = 100; }
            else if (strcmp(s, "winw") == 0) g_winW = atoi(val);
            else if (strcmp(s, "winh") == 0) g_winH = atoi(val);
            else if (strcmp(s, "cw0") == 0) g_colW[0] = atoi(val);
            else if (strcmp(s, "cw1") == 0) g_colW[1] = atoi(val);
            else if (strcmp(s, "cw2") == 0) g_colW[2] = atoi(val);
            else if (strcmp(s, "font") == 0) g_fontLevel = atoi(val);
            else if (strcmp(s, "dark") == 0) g_darkMode = (atoi(val) != 0);
            else if (strcmp(s, "lang") == 0) g_lang = (strcmp(val, "en") == 0) ? 1 : 0;
            else if (strcmp(s, "cachedir") == 0) {
                wchar_t *w = U8ToW(val);
                if (w) { wcsncpy(g_cacheDir, w, MAX_PATH - 1); g_cacheDir[MAX_PATH - 1] = 0; free(w); }
            }
        } else if (strstr(s, "://")) {
            AddFeedUrl(s);   /* [feeds] 段或旧 txt 的裸 URL */
        }
    }
    fclose(f);

    if (g_w1perm < 100 || g_w1perm > 800) g_w1perm = 323;
    if (g_w2perm < 100 || g_w2perm > 800) g_w2perm = 383;
    if (g_winW < 480) g_winW = 480;
    if (g_winH < 360) g_winH = 360;
    if (g_winW > 4000) g_winW = 4000;
    if (g_winH > 2400) g_winH = 2400;
    {
        static const int defCw[3] = { 110, 88, 44 };
        int ci;
        for (ci = 0; ci < 3; ci++)
            if (g_colW[ci] < 30 || g_colW[ci] > 2000) g_colW[ci] = defCw[ci];
    }
    if (g_fontLevel < 0 || g_fontLevel > 2) g_fontLevel = 0;
}

static const char *SortColName(void)
{
    return g_sortCol == SORT_TITLE ? "title" : g_sortCol == SORT_DATE ? "date" : "duration";
}

/* 仅在退出时调用一次：覆盖写 feeds.ini（文件被编辑器锁定时静默放弃，不影响退出） */
static void SaveConfig(void)
{
    wchar_t iniPath[MAX_PATH], txtPath[MAX_PATH];
    FILE *f;
    int i;
    ExeDir(iniPath, MAX_PATH);
    wcscat_s(iniPath, MAX_PATH, L"feeds.ini");
    f = _wfsopen(iniPath, L"wb", _SH_DENYNO);
    if (!f) return;

    /* 退出前回收剧集三列的实际宽度 */
    if (g_listEp) {
        int ci;
        for (ci = 0; ci < 3; ci++) {
            LVCOLUMNW col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_WIDTH;
            if (SendMessageW(g_listEp, LVM_GETCOLUMNW, ci, (LPARAM)&col))
                g_colW[ci] = col.cx;
        }
    }

    fprintf(f, "; LiteTune config\n");
    fprintf(f, "; Add one RSS feed URL per line under [feeds]\n");
    fprintf(f, "[settings]\n");
    fprintf(f, "sort=%s:%s\n", SortColName(), g_sortDir > 0 ? "asc" : "desc");
    fprintf(f, "w1=%d\n", g_w1perm);
    fprintf(f, "w2=%d\n", g_w2perm);
    fprintf(f, "cw0=%d\n", g_colW[0]);
    fprintf(f, "cw1=%d\n", g_colW[1]);
    fprintf(f, "cw2=%d\n", g_colW[2]);
    fprintf(f, "vol=%d\n", g_volume);
    fprintf(f, "font=%d\n", g_fontLevel);
    fprintf(f, "dark=%d\n", g_darkMode);
    fprintf(f, "lang=%s\n", g_lang ? "en" : "zh");
    fprintf(f, "winw=%d\n", g_winW);
    fprintf(f, "winh=%d\n", g_winH);
    if (g_cacheDir[0]) {
        char *u8 = WToU8(g_cacheDir);
        if (u8) { fprintf(f, "cachedir=%s\n", u8); free(u8); }
    }
    fprintf(f, "\n[feeds]\n");
    for (i = 0; i < g_feedUrlCount; i++)
        fprintf(f, "%s\n", g_feedUrls[i]);
    fclose(f);

    /* 首次迁移：删除旧 feeds.txt */
    if (g_cfgFromTxt) {
        ExeDir(txtPath, MAX_PATH);
        wcscat_s(txtPath, MAX_PATH, L"feeds.txt");
        DeleteFileW(txtPath);
        g_cfgFromTxt = 0;
    }
}

/* ================= 排序 ================= */
static int EpCmp(const void *pa, const void *pb)
{
    const Episode *a = (const Episode*)pa, *b = (const Episode*)pb;
    int r = 0;
    switch (g_sortCol) {
    case SORT_TITLE: r = lstrcmpiW(a->title, b->title); break;
    case SORT_DATE:  r = a->dateKey - b->dateKey; break;
    default:         r = a->durationSec - b->durationSec; break;
    }
    if (r == 0) r = a->origIdx - b->origIdx;
    return r * g_sortDir;
}

static void SortPodcast(Podcast *p)
{
    qsort(p->eps, p->epCount, sizeof(Episode), EpCmp);
}

/* 下载封面图并解码为 HBITMAP（保留原始分辨率，列表/提示窗按需缩放，不写磁盘） */
static void FreePodcast(Podcast *p);   /* 定义在 UI 段，加载线程失败时也要用 */

static HBITMAP LoadCoverImage(const char *url)
{
    wchar_t *wurl = U8ToW(url);
    DWORD len = 0;
    char *data;
    HBITMAP hbmp = NULL;
    HGLOBAL hMem;
    IStream *stm;
    GpBitmap *bmp = NULL;

    if (!wurl) return NULL;
    data = HttpFetch(wurl, &len, 0, NULL, NULL);
    free(wurl);
    if (!data || len == 0) { free(data); return NULL; }

    hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) { free(data); return NULL; }
    memcpy(GlobalLock(hMem), data, len);
    GlobalUnlock(hMem);
    free(data);

    if (CreateStreamOnHGlobal(hMem, TRUE, &stm) != S_OK) { GlobalFree(hMem); return NULL; }
    if (GdipCreateBitmapFromStream(stm, &bmp) == 0 && bmp) {
        /* 直接转出原始尺寸的 HBITMAP，调用方按需缩放 */
        GdipCreateHBITMAPFromBitmap(bmp, &hbmp, 0);
        GdipDisposeImage((GpImage*)bmp);
    }
    stm->lpVtbl->Release(stm);
    return hbmp;
}

/* ================= 后台线程：加载 feeds ================= */
/* 加载单条订阅：成功返回 Podcast*（已排序、含封面、feedUrl 已设），失败返回 NULL */
static Podcast* LoadOneFeed(const char *url)
{
    wchar_t *wurl;
    DWORD xmlLen = 0;
    char *xml;
    Podcast *pod;

    wurl = U8ToW(url);
    if (!wurl) return NULL;
    xml = HttpFetch(wurl, &xmlLen, 0, NULL, NULL);
    free(wurl);
    if (!xml) return NULL;

    pod = (Podcast*)calloc(1, sizeof(Podcast));
    if (pod && ParseFeed(xml, pod)) {
        SortPodcast(pod);
        pod->feedUrl = _strdup(url);
        if (pod->imageUrl) pod->hImage = LoadCoverImage(pod->imageUrl);
        free(xml);
        return pod;
    }
    if (pod) { FreePodcast(pod); free(pod); }
    free(xml);
    return NULL;
}

static DWORD WINAPI FeedLoaderThread(LPVOID param)
{
    int i, loaded = 0;
    (void)param;

    for (i = 0; i < g_feedUrlCount; i++) {
        Podcast *pod = LoadOneFeed(g_feedUrls[i]);
        if (pod) {
            PostMessageW(g_hwnd, WM_APP_FEED_ADDED, 0, (LPARAM)pod);
            loaded++;
        }
    }
    PostMessageW(g_hwnd, WM_APP_FEEDS_DONE, loaded, 0);
    return 0;
}

/* 单条订阅加载线程参数 */
typedef struct { char *url; } SingleFeedArg;

static DWORD WINAPI SingleFeedLoaderThread(LPVOID param)
{
    SingleFeedArg *a = (SingleFeedArg*)param;
    Podcast *pod = LoadOneFeed(a->url);
    free(a->url);
    free(a);
    if (pod) {
        PostMessageW(g_hwnd, WM_APP_FEED_ADDED, 1, (LPARAM)pod);   /* wParam=1 表示增量添加 */
        PostMessageW(g_hwnd, WM_APP_FEEDS_DONE, 1, 0);
    } else {
        PostMessageW(g_hwnd, WM_APP_FEEDS_DONE, 0, 0);
    }
    return 0;
}

/* 添加单条订阅（不刷新全部） */
static void AddSingleFeed(const char *url)
{
    SingleFeedArg *a = (SingleFeedArg*)malloc(sizeof(SingleFeedArg));
    if (!a) return;
    a->url = _strdup(url);
    if (!a->url) { free(a); return; }
    SetStatus(tr(TR_LOADING_FEEDS));
    g_feedsLoading = 1;
    g_fullReload = 0;
    CreateThread(NULL, 0, SingleFeedLoaderThread, a, 0, NULL);
}

/* ================= 后台线程：下载音频 ================= */
typedef struct {
    int pod;
    wchar_t *url;
    long gen;
    void * volatile reqSlot;   /* HttpFetch 在此登记请求句柄，供取消 */
} DlJob;

static volatile LONG g_dlGen = 0;   /* 下载代号，每次新请求 +1 */
static DlJob *g_curDlJob = NULL;    /* 当前下载任务，仅 UI 线程访问 */

/* 仅断开当前下载连接（切歌用，代号由新请求统一作废旧任务） */
static void AbortDownloadConnection(void)
{
    if (g_curDlJob) {
        HINTERNET h = (HINTERNET)InterlockedExchangePointer(
            (PVOID volatile*)&g_curDlJob->reqSlot, NULL);
        if (h) WinHttpCloseHandle(h);
        g_curDlJob = NULL;
    }
    g_downloading = 0;
}

/* 用户点停止：断开下载并作废代号，过期完成消息将被丢弃 */
static void CancelCurrentDownload(void)
{
    AbortDownloadConnection();
    InterlockedIncrement(&g_dlGen);
}

static DWORD WINAPI EpisodeDlThread(LPVOID param)
{
    DlJob *job = (DlJob*)param;
    EpReady *r;
    wchar_t cache[MAX_PATH];
    DWORD len = 0;
    char *data;
    FILE *f;

    r = (EpReady*)calloc(1, sizeof(EpReady));
    if (!r) { free(job->url); free(job); return 0; }
    r->pod = job->pod;
    r->url = job->url;   /* 转移所有权给 r */
    r->gen = job->gen;

    if (r->url) {
        CachePathFor(r->url, cache, MAX_PATH);
        EnsureCacheDir();
        if (GetFileAttributesW(cache) != INVALID_FILE_ATTRIBUTES) {
            r->ok = 1;  /* 已缓存 */
        } else {
            data = HttpFetch(r->url, &len, WM_APP_DL_PROGRESS, &job->reqSlot, &job->gen);
            if (data && len > 0) {
                f = _wfopen(cache, L"wb");
                if (f) {
                    r->ok = (fwrite(data, 1, len, f) == len);
                    fclose(f);
                }
            }
            free(data);
        }
    }
    free(job);
    PostMessageW(g_hwnd, WM_APP_EP_READY, 0, (LPARAM)r);
    return 0;
}

/* ================= 音频信息探测 ================= */
static void ProbeAudio(const wchar_t *path, wchar_t *out, int outMax, long long *outDur100ns)
{
    IMFSourceReader *reader = NULL;
    IMFMediaType *mt = NULL;
    UINT32 rate = 0, ch = 0, avgBps = 0;
    GUID subtype;
    const wchar_t *fmtName = tr(TR_AUDIO);
    HRESULT hr;

    *outDur100ns = 0;
    wcscpy_s(out, outMax, L"");
    hr = MFCreateSourceReaderFromURL(path, NULL, &reader);
    if (FAILED(hr) || !reader) return;

    /* 时长（100ns）：直接从文件读，比 MFPlay 的 GetDuration 可靠 */
    {
        PROPVARIANT pv;
        memset(&pv, 0, sizeof(pv));
        if (SUCCEEDED(reader->lpVtbl->GetPresentationAttribute(reader,
                MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv))) {
            if (pv.vt == VT_UI8)      *outDur100ns = (long long)pv.uhVal.QuadPart;
            else if (pv.vt == VT_I8)  *outDur100ns = pv.hVal.QuadPart;
            else if (pv.vt == VT_UI4) *outDur100ns = pv.ulVal;
        }
    }

    hr = reader->lpVtbl->GetNativeMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &mt);
    if (SUCCEEDED(hr) && mt) {
        mt->lpVtbl->GetUINT32(mt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        mt->lpVtbl->GetUINT32(mt, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        mt->lpVtbl->GetUINT32(mt, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avgBps);
        if (SUCCEEDED(mt->lpVtbl->GetGUID(mt, &MF_MT_SUBTYPE, &subtype))) {
            if (IsEqualGUID(&subtype, &MFAudioFormat_MP3))       fmtName = L"MP3";
            else if (IsEqualGUID(&subtype, &MFAudioFormat_AAC))  fmtName = L"AAC";
            else if (IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV8) ||
                     IsEqualGUID(&subtype, &MFAudioFormat_WMAudioV9)) fmtName = L"WMA";
            else if (IsEqualGUID(&subtype, &MFAudioFormat_PCM))  fmtName = L"PCM";
        }
        mt->lpVtbl->Release(mt);
    }
    reader->lpVtbl->Release(reader);

    if (rate > 0) {
        int kbps = avgBps > 0 ? (int)(avgBps * 8 / 1000) : 0;
        const wchar_t *chName = ch == 1 ? tr(TR_MONO) : (ch == 2 ? tr(TR_STEREO) : tr(TR_MULTICH));
        if (kbps > 0)
            swprintf(out, outMax, L"%s|%dkbps|%uHz|%s", fmtName, kbps, rate, chName);
        else
            swprintf(out, outMax, L"%s|%uHz|%s", fmtName, rate, chName);
    } else {
        wcscpy_s(out, outMax, fmtName);
    }
}

/* 读一个字符串型属性，返回新分配的宽字符串（调用方 free），无则 NULL */
static wchar_t* PropGetString(IPropertyStore *ps, const PROPERTYKEY *key)
{
    PROPVARIANT pv;
    wchar_t *s = NULL;
    memset(&pv, 0, sizeof(pv));
    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, key, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal)
        s = _wcsdup(pv.pwszVal);
    PropVariantClear(&pv);
    return s;
}

/* 从 mp3 的 ID3 标签补全离线剧集：标题、播客名（优先艺术家，其次专辑）、时长。
 * 直接替换 e 内已有默认值并接管内存，无该字段时保持默认。 */
static void ProbeAudioMeta(const wchar_t *path, Episode *e)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT pv;
    wchar_t *s;
    if (FAILED(SHGetPropertyStoreFromParsingName(path, NULL, GPS_DEFAULT,
            &IID_IPropertyStore, (void**)&ps)) || !ps)
        return;

    s = PropGetString(ps, &PKEY_Title_L);
    if (s) { free(e->title); e->title = s; }

    s = PropGetString(ps, &PKEY_Music_Artist_L);
    if (!s) s = PropGetString(ps, &PKEY_Music_AlbumTitle_L);
    if (s) { free(e->author); e->author = s; }

    memset(&pv, 0, sizeof(pv));
    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &PKEY_Media_Duration_L, &pv))) {
        if (pv.vt == VT_UI8 && pv.uhVal.QuadPart > 0)
            e->durationSec = (int)(pv.uhVal.QuadPart / 10000000);
        PropVariantClear(&pv);
    }

    ps->lpVtbl->Release(ps);
}

/* ================= MFPlay 回调 ================= */
typedef struct {
    IMFPMediaPlayerCallbackVtbl *lpVtbl;
    LONG ref;
} PlayerCallback;

static HRESULT STDMETHODCALLTYPE PC_QueryInterface(IMFPMediaPlayerCallback *This, REFIID riid, void **ppv)
{
    (void)riid;
    *ppv = This;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE PC_AddRef(IMFPMediaPlayerCallback *This)
{
    return (ULONG)InterlockedIncrement(&((PlayerCallback*)This)->ref);
}
static ULONG STDMETHODCALLTYPE PC_Release(IMFPMediaPlayerCallback *This)
{
    return (ULONG)InterlockedDecrement(&((PlayerCallback*)This)->ref);
}
static void STDMETHODCALLTYPE PC_OnMediaPlayerEvent(IMFPMediaPlayerCallback *This, MFP_EVENT_HEADER *hdr)
{
    (void)This;
    PostMessageW(g_hwnd, WM_APP_MF_EVENT, (WPARAM)hdr->eEventType, (LPARAM)hdr->hrEvent);
}

static IMFPMediaPlayerCallbackVtbl g_pcVtbl = {
    PC_QueryInterface, PC_AddRef, PC_Release, PC_OnMediaPlayerEvent
};
static PlayerCallback g_pc = { &g_pcVtbl, 1 };

/* ================= 播放控制 ================= */
static void PlayerPlayFile(const wchar_t *path)
{
    HRESULT hr;
    if (g_player) {
        g_player->lpVtbl->Release(g_player);
        g_player = NULL;
    }
    /* 传 URL + fStartPlayback=TRUE：创建即自动播放，无需 SetMediaItem/Play */
    hr = MFPCreateMediaPlayer(path, TRUE, MFP_OPTION_NONE,
            (IMFPMediaPlayerCallback*)&g_pc, g_hwnd, &g_player);
    if (FAILED(hr) || !g_player) { SetStatus(tr(TR_CANT_OPEN)); return; }
    g_player->lpVtbl->SetVolume(g_player,
        (float)SendMessageW(g_sldVol, TBM_GETPOS, 0, 0) / 100.0f);
    g_player->lpVtbl->SetMute(g_player, g_muted ? TRUE : FALSE);
}

static void PlayerStop(void)
{
    if (g_player) g_player->lpVtbl->Stop(g_player);
    g_playState = 0;
    g_dur100ns = 0;
    g_playPod = -1;         /* 清空“正在播放”标记，避免和后续列表选中混淆 */
    g_playEp = -1;
    if (g_btnPlay) InvalidateRect(g_btnPlay, NULL, TRUE);   /* 图标回到“播放” */
    SetWindowTextW(g_stTime, L"00:00 / 00:00");
    SendMessageW(g_sldSeek, TBM_SETPOS, TRUE, 0);
    if (g_stFormat) SetWindowTextW(g_stFormat, L"");
    if (g_listEp) InvalidateRect(g_listEp, NULL, TRUE);      /* 剧集颜色复位 */
    UpdateThumbbarPlayPause();
}

/* 取当前位置或总时长（wantDur=1），单位 100ns；无播放器/失败返回 -1 */
static long long PlayerGet100ns(int wantDur)
{
    PROPVARIANT pv;
    HRESULT hr;
    if (!g_player) return -1;
    memset(&pv, 0, sizeof(pv));
    hr = wantDur
        ? g_player->lpVtbl->GetDuration(g_player, &MFP_POSITIONTYPE_100NS, &pv)
        : g_player->lpVtbl->GetPosition(g_player, &MFP_POSITIONTYPE_100NS, &pv);
    if (FAILED(hr) || pv.vt != VT_I8) return -1;
    return pv.hVal.QuadPart;
}

static void PlayerSeekToFrac(int permille)
{
    PROPVARIANT pv;
    if (!g_player || g_dur100ns <= 0) return;
    memset(&pv, 0, sizeof(pv));
    pv.vt = VT_I8;
    pv.hVal.QuadPart = g_dur100ns * permille / 1000;
    g_player->lpVtbl->SetPosition(g_player, &MFP_POSITIONTYPE_100NS, &pv);
}

/* ================= UI 逻辑 ================= */
/* 切换剧集列表列头：普通=标题/日期/时长；缓存视图=播客/标题/时长 */
static void SetEpisodeColumns(int cached)
{
    const wchar_t *titlesNormal[3] = { tr(TR_COL_TITLE), tr(TR_COL_DATE), tr(TR_COL_DUR) };
    const wchar_t *titlesCache[3]  = { tr(TR_COL_PODCAST), tr(TR_COL_TITLE), tr(TR_COL_DUR) };
    const wchar_t **titles = cached ? titlesCache : titlesNormal;
    int widths[3], i;
    if (cached) {
        RECT rc;
        GetClientRect(g_listEp, &rc);
        widths[0] = 84;
        widths[2] = 46;
        widths[1] = (rc.right - rc.left) - widths[0] - widths[2] - 24;
        if (widths[1] < 60) widths[1] = 60;
    } else {
        widths[0] = g_colW[0]; widths[1] = g_colW[1]; widths[2] = g_colW[2];
    }
    while (SendMessageW(g_listEp, LVM_DELETECOLUMN, 0, 0)) {}
    for (i = 0; i < 3; i++) {
        LVCOLUMNW col;
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = widths[i];
        col.iSubItem = i;
        col.pszText = (wchar_t*)titles[i];
        SendMessageW(g_listEp, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }
    /* 新建列会丢失 HDF_OWNERDRAW，暗色下要重新设置 */
    ApplyHeaderTheme();
}

/* 当前剧集列表对应的播客（可能是“已缓存”虚拟播客） */
static Podcast* ActivePodcast(void)
{
    if (g_viewCached) return &g_cachePod;
    if (g_curPod >= 0 && g_curPod < g_podCount) return &g_pods[g_curPod];
    return NULL;
}

/* 用当前播客的剧集填充 ListView（调用前数组应已排序） */
static void FillEpisodeList(void)
{
    Podcast *p = ActivePodcast();
    int i;
    if (!p) return;
    SendMessageW(g_listEp, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < p->epCount; i++) {
        wchar_t dur[16];
        LVITEMW lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        /* 缓存视图列序是 播客/标题/时长，普通视图是 标题/日期/时长 */
        lvi.pszText = g_viewCached
            ? (p->eps[i].author ? p->eps[i].author : L"")
            : p->eps[i].title;
        SendMessageW(g_listEp, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

        lvi.iSubItem = 1;
        lvi.pszText = g_viewCached
            ? p->eps[i].title
            : (p->eps[i].dateKey ? p->eps[i].dateText : L"");
        SendMessageW(g_listEp, LVM_SETITEMW, 0, (LPARAM)&lvi);

        FmtTime(p->eps[i].durationSec, dur, 16);
        lvi.iSubItem = 2;
        lvi.pszText = p->eps[i].durationSec > 0 ? dur : L"";
        SendMessageW(g_listEp, LVM_SETITEMW, 0, (LPARAM)&lvi);
    }
}

/* 剧集 tooltip：标题被列宽截断时显示完整标题 + 播客/日期/时长。
 * 走 ListView 原生 LVS_EX_INFOTIP 机制：悬停计时、命中行判断全由控件完成，
 * 这里只在 LVN_GETINFOTIPW 回调里把文本写进控件给的缓冲。 */
static void FillEpisodeTip(LPNMLVGETINFOTIPW gt)
{
    Podcast *p = ActivePodcast();
    Episode *e;
    wchar_t dur[16], line[1024];
    size_t cap, pos = 0;
    int i = (int)gt->iItem;
    if (!p || i < 0 || i >= p->epCount) return;
    e = &p->eps[i];
    if (e->title && *e->title)
        pos += swprintf(line + pos, sizeof(line)/sizeof(line[0]) - pos, L"%s", e->title);
    if (g_viewCached) {
        if (e->author && *e->author)
            pos += swprintf(line + pos, sizeof(line)/sizeof(line[0]) - pos,
                            tr(TR_TIP_PODCAST_FMT), e->author);
    } else if (p->title && *p->title) {
        pos += swprintf(line + pos, sizeof(line)/sizeof(line[0]) - pos,
                        tr(TR_TIP_PODCAST_FMT), p->title);
    }
    if (!g_viewCached && e->dateKey)
        pos += swprintf(line + pos, sizeof(line)/sizeof(line[0]) - pos,
                        tr(TR_TIP_PUBDATE_FMT), e->dateText);
    if (e->durationSec > 0) {
        FmtTime(e->durationSec, dur, 16);
        pos += swprintf(line + pos, sizeof(line)/sizeof(line[0]) - pos,
                        tr(TR_TIP_DUR_FMT), dur);
    }
    cap = gt->cchTextMax;
    if (cap > sizeof(line)/sizeof(line[0]))
        cap = sizeof(line)/sizeof(line[0]);
    wcsncpy(gt->pszText, line, cap - 1);
    gt->pszText[cap - 1] = 0;
}

/* 把字段拼成 "【标题】\r\n内容\r\n\r\n..." 塞进简介框 */
static void ShowDetailField(wchar_t *buf, size_t bufSize, size_t *pos,
                            const wchar_t *label, const wchar_t *value)
{
    if (!value || !*value) return;
    *pos += swprintf(buf + *pos, bufSize - *pos,
        L"【%s】\r\n%s\r\n\r\n", label, value);
}

static void ShowPodcastDetail(int idx)
{
    static wchar_t buf[32768];
    size_t pos = 0;
    Podcast *p;
    wchar_t *feedW = NULL;
    if (idx < 0 || idx >= g_podCount) return;
    p = &g_pods[idx];
    buf[0] = 0;
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_TITLE), p->title);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_DESC), p->desc);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_AUTHOR), p->author);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_LANG), p->lang);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_HOME), p->link);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_COPYRIGHT), p->copyright);
    if (p->feedUrl) {
        feedW = U8ToW(p->feedUrl);
        if (feedW) {
            ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_FEEDURL), feedW);
            free(feedW);
        }
    }
    SetWindowTextW(g_editDesc, buf);
}

static void ShowEpisodeDetail(int podIdx, int epIdx)
{
    static wchar_t buf[32768];
    size_t pos = 0;
    Podcast *p;
    Episode *e;
    wchar_t dur[16];
    if (podIdx == g_podCount) p = &g_cachePod;
    else if (podIdx >= 0 && podIdx < g_podCount) p = &g_pods[podIdx];
    else return;
    if (epIdx < 0 || epIdx >= p->epCount) return;
    e = &p->eps[epIdx];
    buf[0] = 0;
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_TITLE), e->title);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_DESC), e->desc);
    if (g_viewCached)
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_COL_PODCAST), e->author);
    else
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_AUTHOR), e->author);
    if (e->dateKey) ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_F_PUBDATE), e->dateText);
    if (e->durationSec > 0) {
        FmtTime(e->durationSec, dur, 16);
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, tr(TR_COL_DUR), dur);
    }
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos,
                    e->localFile ? tr(TR_F_LOCALFILE) : tr(TR_F_LINK), e->url);
    SetWindowTextW(g_editDesc, buf);
}

/* “已缓存”虚拟行被选中：扫描缓存目录，换成 播客/标题/时长 列 */
static void SelectCachedView(void)
{
    static wchar_t buf[256];
    g_viewCached = 1;
    g_curPod = g_podCount;
    g_curEp = -1;               /* 进入缓存视图时重置剧集选中 */
    FreeCachePodcast();
    LoadCachePodcast();
    SetEpisodeColumns(1);
    FillEpisodeList();
    swprintf(buf, 256, tr(TR_CACHE_VIEW_DESC), g_cachePod.epCount);
    SetWindowTextW(g_editDesc, buf);
}

static void SelectPodcast(int idx)
{
    if (idx == g_podCount) { SelectCachedView(); return; }
    if (idx < 0 || idx >= g_podCount) return;
    if (g_viewCached) {
        /* 从缓存视图切回普通播客：恢复列头并释放扫描数据 */
        g_viewCached = 0;
        FreeCachePodcast();
        SetEpisodeColumns(0);
        g_curPod = -1;          /* 防止旧缓存下标越界 */
        g_curEp = -1;
    }
    g_curPod = idx;
    SortPodcast(&g_pods[idx]);
    FillEpisodeList();
    ShowPodcastDetail(idx);
}

static void SelectEpisode(int idx)
{
    Podcast *p = ActivePodcast();
    if (!p || idx < 0 || idx >= p->epCount) return;
    g_curEp = idx;
    ShowEpisodeDetail(g_curPod, idx);
}

/* 点击列头：同列切换方向，换列用默认方向（日期/时长降序，标题升序） */
static void ChangeSort(int col)
{
    int oldOrig = -1, i, sel;
    Podcast *p;
    int sortCol;

    if (g_viewCached) {
        /* 缓存视图列：0=播客(不支持排序) 1=标题 2=时长 */
        if (col == 1) sortCol = SORT_TITLE;
        else if (col == 2) sortCol = SORT_DUR;
        else return;
        col = sortCol;
    }
    if (g_sortCol == col) g_sortDir = -g_sortDir;
    else {
        g_sortCol = col;
        g_sortDir = (col == SORT_TITLE) ? 1 : -1;
    }
    /* 排序只更新内存，退出时统一保存 */

    if (g_curPod < 0) return;
    p = ActivePodcast();
    if (!p) return;
    sel = (int)SendMessageW(g_listEp, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel >= 0 && sel < p->epCount) oldOrig = p->eps[sel].origIdx;

    SortPodcast(p);
    FillEpisodeList();

    for (i = 0; i < p->epCount; i++) {
        if (p->eps[i].origIdx == oldOrig) {
            SendMessageW(g_listEp, LVM_ENSUREVISIBLE, i, FALSE);
            SendMessageW(g_listEp, LVM_SETITEMSTATE, i,
                (LPARAM)&(LVITEMW){ .state = LVIS_SELECTED|LVIS_FOCUSED,
                                    .stateMask = LVIS_SELECTED|LVIS_FOCUSED });
            break;
        }
    }
}

/* 离线缓存视图：直接播放本地文件，不联网、不起下载线程 */
static void PlayLocalEpisode(int ep)
{
    Episode *e;
    wchar_t info[128], st[512];
    long long pd = 0;
    LVITEMW lvi;
    if (ep < 0 || ep >= g_cachePod.epCount) return;
    e = &g_cachePod.eps[ep];

    if (g_downloading) AbortDownloadConnection();
    PlayerStop();

    g_playPod = g_podCount;
    g_playEp  = ep;
    free(g_playUrl);
    g_playUrl = _wcsdup(e->url);

    /* 只有正在浏览“已缓存”时才同步列表选中/当前下标，避免后台自动连播抢走视图 */
    if (g_viewCached) {
        g_curPod = g_podCount;
        g_curEp = ep;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        lvi.state = 0;
        SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lvi);
        lvi.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)ep, (LPARAM)&lvi);
        SendMessageW(g_listEp, LVM_ENSUREVISIBLE, (WPARAM)ep, FALSE);
        InvalidateRect(g_listEp, NULL, TRUE);
    }

    ShowEpisodeDetail(g_podCount, ep);
    ProbeAudio(e->url, info, 128, &pd);
    g_dur100ns = pd;
    SetWindowTextW(g_stFormat, info);
    swprintf(st, 512, tr(TR_PLAYING_OFFLINE_FMT), e->title ? e->title : L"");
    SetStatus(st);
    PlayerPlayFile(e->url);
}

static void RequestPlayEpisode(int pod, int ep)
{
    DlJob *job;
    Episode *e;
    long myGen;
    if (pod == g_podCount) { PlayLocalEpisode(ep); return; }
    if (pod < 0 || pod >= g_podCount || ep < 0 || ep >= g_pods[pod].epCount) return;
    e = &g_pods[pod].eps[ep];

    /* 改主意了：立即中止旧下载、停止当前播放，转入新任务 */
    if (g_downloading) AbortDownloadConnection();
    PlayerStop();
    g_playPod = pod;
    g_playEp  = ep;
    free(g_playUrl);
    g_playUrl = _wcsdup(e->url);
    /* 播放的正是当前浏览的播客：同步选中行；后台连播到别的播客时不动视图 */
    if (!g_viewCached && pod == g_curPod) {
        LVITEMW lvi;
        g_curEp = ep;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        lvi.state = 0;
        SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lvi);
        lvi.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)ep, (LPARAM)&lvi);
        SendMessageW(g_listEp, LVM_ENSUREVISIBLE, (WPARAM)ep, FALSE);
        InvalidateRect(g_listEp, NULL, TRUE);   /* 播放目标确定，立即标绿 */
    }
    ShowEpisodeDetail(pod, ep);
    SetStatus(tr(TR_PREPARING));

    job = (DlJob*)calloc(1, sizeof(DlJob));
    if (!job) return;
    job->pod = pod;
    job->url = _wcsdup(e->url);
    if (!job->url) { free(job); return; }
    myGen = InterlockedIncrement(&g_dlGen);
    job->gen = myGen;
    g_curDlJob = job;
    g_downloading = 1;
    if (!CreateThread(NULL, 0, EpisodeDlThread, job, 0, NULL)) {
        g_curDlJob = NULL;
        g_downloading = 0;
        free(job->url); free(job);
        SetStatus(tr(TR_DL_THREAD_FAIL));
    }
}

static void PauseResume(void)
{
    if (!g_player) {
        if (g_curPod >= 0 && g_curEp >= 0) RequestPlayEpisode(g_curPod, g_curEp);
        return;
    }
    if (g_playState == 1) {
        g_player->lpVtbl->Pause(g_player);
    } else if (g_playState == 2) {
        g_player->lpVtbl->Play(g_player);
    } else if (g_curPod >= 0 && g_curEp >= 0) {
        RequestPlayEpisode(g_curPod, g_curEp);
    }
    UpdateThumbbarPlayPause();
}

/* 上一曲/下一曲：按正在播放的剧集顺序，越界首尾循环；随机模式下取随机项。
 * 没有正在播放的内容时退化为当前浏览的选中行。 */
static void PlayAdjacent(int delta)
{
    int pod = (g_playPod >= 0) ? g_playPod : g_curPod;
    int ep  = (g_playPod >= 0) ? g_playEp  : g_curEp;
    int n, target;
    if (pod < 0) return;
    n = (pod == g_podCount) ? g_cachePod.epCount : g_pods[pod].epCount;
    if (n <= 0) return;
    if (g_playMode == 1 && n > 1) {
        do { target = rand() % n; } while (target == ep);
    } else {
        target = (ep < 0) ? 0 : ep + delta;
        if (target < 0) target = n - 1;
        if (target >= n) target = 0;
    }
    RequestPlayEpisode(pod, target);
}

/* 喇叭按钮：静音切换（滑块位置不动） */
static void ToggleMute(void)
{
    g_muted = !g_muted;
    if (g_player) g_player->lpVtbl->SetMute(g_player, g_muted ? TRUE : FALSE);
    InvalidateRect(g_btnMute, NULL, TRUE);
}

/* 释放单个播客的全部堆内存与图片（字段允许为 NULL） */
static void FreePodcast(Podcast *p)
{
    int j;
    for (j = 0; j < p->epCount; j++) {
        free(p->eps[j].title);
        free(p->eps[j].desc);
        free(p->eps[j].url);
        free(p->eps[j].author);
    }
    free(p->eps);
    free(p->title);
    free(p->desc);
    free(p->author);
    free(p->lang);
    free(p->link);
    free(p->copyright);
    free(p->imageUrl);
    free(p->feedUrl);
    if (p->hImage) DeleteObject(p->hImage);
}

static void FreePodcasts(void)
{
    int i;
    for (i = 0; i < g_podCount; i++) FreePodcast(&g_pods[i]);
    g_podCount = 0;
    g_curPod = g_curEp = -1;
}

static void ReloadFeeds(void)
{
    if (g_feedsLoading) return;
    if (g_downloading) CancelCurrentDownload();   /* 刷新时中止正在进行的下载 */
    PlayerStop();
    /* 退出可能正处于的离线“已缓存”视图，列头恢复成 标题/日期/时长 */
    if (g_viewCached) {
        g_viewCached = 0;
        SetEpisodeColumns(0);
    }
    FreeCachePodcast();
    g_cacheRowHere = 0;
    g_cacheFileCount = 0;
    FreePodcasts();
    SendMessageW(g_listPod, LB_RESETCONTENT, 0, 0);
    SendMessageW(g_listEp, LVM_DELETEALLITEMS, 0, 0);
    SetWindowTextW(g_editDesc, L"");
    g_podMode = 0;
    SetStatus(tr(TR_LOADING_FEEDS));
    /* 刷新只用内存里的订阅链接；feeds.ini 仅启动读一次、退出写一次，
     * 因此运行中的删除/排序不会被刷新覆盖 */
    g_feedsLoading = 1;
    g_fullReload = 1;
    CreateThread(NULL, 0, FeedLoaderThread, NULL, 0, NULL);
}

/* ================= 播客订阅管理（增删/排序） ================= */

static char* WToU8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *p;
    if (n <= 0) return NULL;
    p = (char*)malloc(n);
    if (p) WideCharToMultiByte(CP_UTF8, 0, w, -1, p, n, NULL, NULL);
    return p;
}

/* 删除一个播客：同时从 g_pods、列表、g_feedUrls 三处移除 */
static void RemovePodcast(int i)
{
    int ui;
    int oldCount = g_podCount;   /* 哨兵 g_playPod/g_curPod 可能等于它（离线播放/缓存视图） */
    if (i < 0 || i >= g_podCount) return;

    if (g_playPod == i) {
        if (g_downloading) CancelCurrentDownload();
        PlayerStop();
        g_playPod = -1;
    } else {
        if (g_downloading && g_curDlJob && g_curDlJob->pod == i)
            CancelCurrentDownload();
        if (g_playPod > i && g_playPod < oldCount) g_playPod--;
        /* g_playPod == oldCount（离线播放中）：哨兵随 g_podCount 自然前移，不动 */
    }

    /* 失败的订阅没有对应 pod，按 feedUrl 精确匹配 URL */
    if (g_pods[i].feedUrl) {
        for (ui = 0; ui < g_feedUrlCount; ui++) {
            if (strcmp(g_feedUrls[ui], g_pods[i].feedUrl) == 0) {
                free(g_feedUrls[ui]);
                memmove(&g_feedUrls[ui], &g_feedUrls[ui + 1],
                        (g_feedUrlCount - ui - 1) * sizeof(char*));
                g_feedUrlCount--;
                break;
            }
        }
    }

    FreePodcast(&g_pods[i]);
    memmove(&g_pods[i], &g_pods[i + 1], (g_podCount - i - 1) * sizeof(Podcast));
    g_podCount--;
    SendMessageW(g_listPod, LB_DELETESTRING, (WPARAM)i, 0);

    if (g_curPod == i) {
        g_curPod = -1; g_curEp = -1;
        SendMessageW(g_listEp, LVM_DELETEALLITEMS, 0, 0);
        SetWindowTextW(g_editDesc, L"");
    } else if (g_curPod > i) {
        g_curPod--;
    }

    if (g_viewCached) {
        /* 正在离线视图里删真实播客：缓存聚合不受影响，留在“已缓存”行 */
        g_curPod = g_podCount;
        SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
    } else if (g_podCount > 0) {
        int sel = (i >= g_podCount) ? g_podCount - 1 : i;
        SendMessageW(g_listPod, LB_SETCURSEL, (WPARAM)sel, 0);
        SelectPodcast(sel);
    }
    InvalidateRect(g_listPod, NULL, TRUE);
}

/* 上移(dir=-1)/下移(dir=+1)一个播客；URL 数组同步移动（可能跨过加载失败的订阅） */
static void MovePodcast(int i, int dir)
{
    int j = i + dir, ui;
    wchar_t *title;
    Podcast tmp;
    if (i < 0 || i >= g_podCount || j < 0 || j >= g_podCount) return;

    if (g_pods[i].feedUrl) {
        for (ui = 0; ui < g_feedUrlCount; ui++) {
            if (strcmp(g_feedUrls[ui], g_pods[i].feedUrl) == 0) {
                if (ui + dir >= 0 && ui + dir < g_feedUrlCount) {
                    char *t = g_feedUrls[ui];
                    g_feedUrls[ui] = g_feedUrls[ui + dir];
                    g_feedUrls[ui + dir] = t;
                }
                break;
            }
        }
    }

    tmp = g_pods[i]; g_pods[i] = g_pods[j]; g_pods[j] = tmp;

    title = g_pods[j].title;   /* 交换后目标位置的标题 */
    SendMessageW(g_listPod, LB_DELETESTRING, (WPARAM)i, 0);
    SendMessageW(g_listPod, LB_INSERTSTRING, (WPARAM)j, (LPARAM)title);
    SendMessageW(g_listPod, LB_SETCURSEL, (WPARAM)j, 0);

    if (g_curPod == i) g_curPod = j;
    else if (g_curPod == j) g_curPod = i;
    if (g_playPod == i) g_playPod = j;
    else if (g_playPod == j) g_playPod = i;
    InvalidateRect(g_listPod, NULL, TRUE);
}

/* ================= 添加订阅输入框（自建模态弹窗） ================= */
#define ID_IB_EDIT   9001
#define ID_IB_OK     9002
#define ID_IB_CANCEL 9003

static wchar_t g_ibBuf[2048];
static int     g_ibResult = 0;

static LRESULT CALLBACK InputBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HWND e, lab;
        e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 32, 356, 24, h, (HMENU)(INT_PTR)ID_IB_EDIT, NULL, NULL);
        lab = CreateWindowExW(0, L"STATIC", tr(TR_INPUT_LABEL),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 10, 356, 18, h, NULL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", tr(TR_OK),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            212, 72, 72, 26, h, (HMENU)(INT_PTR)ID_IB_OK, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", tr(TR_CANCEL),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            296, 72, 72, 26, h, (HMENU)(INT_PTR)ID_IB_CANCEL, NULL, NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(lab, WM_SETFONT, (WPARAM)g_font, TRUE);
        SetFocus(e);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == ID_IB_CANCEL) {
            g_ibResult = 0;
            DestroyWindow(h);
        } else if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == ID_IB_OK) {
            wchar_t raw[2048];
            wchar_t *s, *e;
            int k, dup = 0;
            GetDlgItemTextW(h, ID_IB_EDIT, raw, 2048);
            s = raw;
            while (*s == L' ' || *s == L'\t') s++;
            e = s + wcslen(s);
            while (e > s && (e[-1] == L' ' || e[-1] == L'\t' ||
                             e[-1] == L'\r' || e[-1] == L'\n')) *--e = 0;
            if (!wcsstr(s, L"://")) {
                MessageBoxW(h, tr(TR_BAD_URL),
                            L"LiteTune", MB_ICONWARNING);
                return 0;
            }
            for (k = 0; k < g_feedUrlCount; k++) {
                wchar_t *w = U8ToW(g_feedUrls[k]);
                if (w) { if (_wcsicmp(w, s) == 0) dup = 1; free(w); }
                if (dup) break;
            }
            if (dup) {
                MessageBoxW(h, tr(TR_DUP_FEED), L"LiteTune", MB_ICONINFORMATION);
                return 0;
            }
            if (g_feedUrlCount >= MAX_PODCASTS) {
                MessageBoxW(h, tr(TR_MAX_FEEDS), L"LiteTune", MB_ICONWARNING);
                return 0;
            }
            wcsncpy(g_ibBuf, s, 2047);
            g_ibBuf[2047] = 0;
            g_ibResult = 1;
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        g_ibResult = 0;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 弹出输入框；确认后追加订阅并自动刷新 */
static void AskAddFeed(void)
{
    static const wchar_t *IB_CLASS = L"LpInputBox";
    WNDCLASSW wc;
    HWND d;
    RECT rp, rc;
    MSG m;

    if (!GetClassInfoW(GetModuleHandleW(NULL), IB_CLASS, &wc)) {
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = InputBoxProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = IB_CLASS;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
    }

    g_ibResult = 0;
    g_ibBuf[0] = 0;
    d = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, IB_CLASS,
        tr(TR_INPUT_TITLE),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 384, 138, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!d) return;

    /* 居中到主窗口 */
    GetWindowRect(g_hwnd, &rp);
    GetWindowRect(d, &rc);
    SetWindowPos(d, 0,
        rp.left + ((rp.right - rp.left) - (rc.right - rc.left)) / 2,
        rp.top  + ((rp.bottom - rp.top)  - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(g_hwnd, FALSE);
    ShowWindow(d, SW_SHOW);
    UpdateWindow(d);
    while (IsWindow(d) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(d, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);

    if (g_ibResult && g_ibBuf[0]) {
        char *u8 = WToU8(g_ibBuf);
        if (u8) {
            AddFeedUrl(u8);        /* ini 只在退出时统一写，运行中全部走内存 */
            AddSingleFeed(u8);     /* 只加载新增的这一条，不刷新全部 */
            free(u8);
        }
    }
}

/* 切换播客列表模式：0=普通 1=删除 2=排序（互斥） */
static void SetPodMode(int mode)
{
    if (g_podMode == mode) mode = 0;
    g_podMode = mode;
    InvalidateRect(g_listPod, NULL, TRUE);
    if (g_btnDel) InvalidateRect(g_btnDel, NULL, TRUE);
    if (g_btnSort) InvalidateRect(g_btnSort, NULL, TRUE);
}

/* 删除/排序模式下，播客列表的鼠标按下由这里处理（命中封面区域），返回 1=吞掉 */
static int PodListModeClick(POINT pt)
{
    DWORD v;
    int item;
    RECT rc;
    if (g_podMode == 0) return 0;
    v = (DWORD)SendMessageW(g_listPod, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    if (HIWORD(v)) return 1;                 /* 点在列表空白：吞掉，不改选中 */
    item = (int)LOWORD(v);
    if (item < 0 || item >= g_podCount) return 1;
    if (SendMessageW(g_listPod, LB_GETITEMRECT, (WPARAM)item, (LPARAM)&rc) == LB_ERR)
        return 1;
    /* 封面热区：rc.left+2 .. rc.left+54 */
    if (pt.x < rc.left + 2 || pt.x > rc.left + 54) return 1;
    if (g_podMode == 1) {
        RemovePodcast(item);                 /* 封面整体是一个大 × */
    } else if (g_podMode == 2) {
        /* 封面左右两半：上箭头 / 下箭头 */
        if (pt.x < rc.left + 28) MovePodcast(item, -1);
        else MovePodcast(item, +1);
    }
    return 1;
}

/* ================= 滑块滚轮修正 ================= */
/* trackbar 默认滚轮方向是反的（向上滚反而减小），子类化后自己处理 */

static LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (hwnd == g_sldSeek) ? g_oldSeekProc : g_oldVolProc;
    if (msg == WM_MOUSEWHEEL) {
        /* Ctrl+滚轮：调全局字号（普通滚轮才是调滑块） */
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            ChangeFontLevel(g_fontLevel + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1));
            return 0;
        }
        int isVol = (hwnd == g_sldVol);
        int maxPos = isVol ? 100 : 1000;
        int step  = isVol ? 5 : 20;      /* 音量 5%/格，进度 2%/格 */
        int pos = (int)SendMessageW(hwnd, TBM_GETPOS, 0, 0);
        pos += (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? step : -step;  /* 上滚 = 增大 */
        if (pos < 0) pos = 0;
        if (pos > maxPos) pos = maxPos;
        SendMessageW(hwnd, TBM_SETPOS, TRUE, pos);
        /* 复用主窗口 WM_HSCROLL 里的现有逻辑 */
        SendMessageW(g_hwnd, WM_HSCROLL,
            MAKEWPARAM(isVol ? TB_THUMBTRACK : TB_ENDTRACK, pos), (LPARAM)hwnd);
        return 0;
    }
    return CallWindowProcW(old, hwnd, msg, wp, lp);
}

/* 内容面板与按钮共用：Ctrl+滚轮调字号。
 * 各窗口原始过程存在自己的 GWLP_USERDATA。 */
static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old;
    /* 夜间模式：默认绘制后把非客户区滚动条盖成暗色 */
    if (msg == WM_NCPAINT && g_darkMode) {
        old = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        LRESULT lr = CallWindowProcW(old, hwnd, msg, wp, lp);
        PaintDarkScrollBars(hwnd);
        return lr;
    }
    /* 拖动滚动条后系统未必重发 WM_NCPAINT，主动补绘；
     * 激活切换(WM_NCACTIVATE)和缩放(WM_SIZE)也会把滚动条重画回亮色 */
    if (g_darkMode && (msg == WM_VSCROLL || msg == WM_HSCROLL ||
                       msg == WM_NCACTIVATE || msg == WM_SIZE)) {
        old = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        LRESULT lr = CallWindowProcW(old, hwnd, msg, wp, lp);
        PaintDarkScrollBars(hwnd);
        return lr;
    }
    /* 暗色表头：ODT_HEADER 的 WM_DRAWITEM 发给表头的父窗口 ListView */
    if (g_darkMode && msg == WM_DRAWITEM && hwnd == g_listEp) {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_HEADER)
            return DrawHeaderItem(di) ? TRUE : FALSE;
    }
    /* 删除/排序模式下，播客列表的点击全部自管（不允许改选中项） */
    if (hwnd == g_listPod && msg == WM_LBUTTONDOWN && g_podMode != 0) {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        PodListModeClick(pt);
        return 0;
    }
    /* 播客列表悬停 → 显示封面+标题+作者提示 */
    if (hwnd == g_listPod) {
        if (msg == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_HOVER | TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = HOVER_DEFAULT;
            TrackMouseEvent(&tme);
        } else if (msg == WM_MOUSEHOVER) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            UpdatePodTipFromPoint(pt);
        } else if (msg == WM_MOUSELEAVE) {
            ShowWindow(g_hwndPodTip, SW_HIDE);
        }
    }
    if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)) {
        ChangeFontLevel(g_fontLevel + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1));
        return 0;
    }
    /* 播客列表滚动时，隐藏悬停提示避免显示旧条目 */
    if (hwnd == g_listPod && msg == WM_MOUSEWHEEL)
        ShowWindow(g_hwndPodTip, SW_HIDE);
    /* 滚轮/客户区重绘后，滚动条可能被系统重新画成亮色，补绘 */
    if (g_darkMode && (msg == WM_MOUSEWHEEL || msg == WM_PAINT)
        && (hwnd == g_listEp || hwnd == g_listPod || hwnd == g_editDesc)) {
        old = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        LRESULT lr = CallWindowProcW(old, hwnd, msg, wp, lp);
        PaintDarkScrollBars(hwnd);
        return lr;
    }
    old = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return CallWindowProcW(old, hwnd, msg, wp, lp);
}

/* 给一个控件挂上 PanelProc，并把其原始过程保存在该窗口的 USERDATA */
static void SubclassForFontWheel(HWND w)
{
    SetWindowLongPtrW(w, GWLP_USERDATA, GetWindowLongPtrW(w, GWLP_WNDPROC));
    SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)PanelProc);
}

/* ================= 可拖动分隔条 ================= */
#define SPLIT_MINPX  90    /* 每列最小像素宽 */
static int g_dragSplit = -1;   /* -1=无 0=第一根 1=第二根 */

static void LayoutControls(HWND hwnd);   /* 前置声明 */

/* 返回 (x,y) 命中的分隔条；-1=未命中 */
static int SplitterHitTest(HWND hwnd, int x, int y)
{
    RECT rc;
    int W, H, totalW, w1, w2, by, bh;
    int s1, s2;
    GetClientRect(hwnd, &rc);
    W = rc.right; H = rc.bottom;
    by = MARGIN + TOP_H + GAP_W;
    bh = H - by - MARGIN;
    by += 28;   /* 播客工具栏占去顶部 28px，分隔条只在列表区域生效 */
    if (y < by || y > by + bh - 28) return -1;
    totalW = W - 2*MARGIN - 2*GAP_W;
    w1 = totalW * g_w1perm / 1000;
    w2 = totalW * g_w2perm / 1000;
    s1 = MARGIN + w1 + GAP_W / 2;
    s2 = MARGIN + w1 + GAP_W + w2 + GAP_W / 2;
    if (abs(x - s1) <= 4) return 0;
    if (abs(x - s2) <= 4) return 1;
    return -1;
}

/* 冻结重绘的整体布局，拖动/缩放共用 */
static void ApplyLayout(HWND hwnd, int live)
{
    if (live) {
        SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
        LayoutControls(hwnd);
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(hwnd, NULL, NULL,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE |
            RDW_FRAME | RDW_UPDATENOW);
    } else {
        LayoutControls(hwnd);
    }
}

/* ================= 布局 ================= */
static void LayoutControls(HWND hwnd)
{
    RECT rc;
    int W, H;
    int by, bh, totalW, w1, w2, w3, x, gx, gw, gy, bx;
    int volW = 24, sw = 84, fmtX;

    GetClientRect(hwnd, &rc);
    W = rc.right; H = rc.bottom;

    /* 顶部播放区：边框由 WM_PAINT 绘制，控件向内缩。
     * 第1行状态跑马灯由 WM_PAINT 自绘，这里只管第2、3行。 */
    gx = MARGIN + 8;
    gw = W - 2*MARGIN - 16;

    gy = MARGIN + 6 + 22;
    MoveWindow(g_sldSeek, gx, gy, gw - 108, 22, TRUE);
    MoveWindow(g_stTime,  gx + gw - 104, gy + 2, 104, 18, TRUE);
    gy += 28;

    /* 第3行：上一曲/播放/下一曲/停止/播放模式/静音（间距统一 26，给格式串留空间） */
    bx = gx;
    MoveWindow(g_btnPrev,     bx, gy, 24, 24, TRUE); bx += 26;
    MoveWindow(g_btnPlay,     bx, gy, 24, 24, TRUE); bx += 26;
    MoveWindow(g_btnNext,     bx, gy, 24, 24, TRUE); bx += 26;
    MoveWindow(g_btnStop,     bx, gy, 24, 24, TRUE); bx += 26;
    MoveWindow(g_btnPlayMode, bx, gy, 24, 24, TRUE); bx += 26;
    MoveWindow(g_btnMute,     bx, gy, 24, 24, TRUE); bx += 26;
    /* 音量滑块固定宽 + 数字；格式串右靠齐，占满剩余空间 */
    fmtX = bx + sw + 4 + volW + 8;
    MoveWindow(g_sldVol,   bx, gy, sw, 24, TRUE);
    MoveWindow(g_stVol,    bx + sw + 4, gy + 4, volW, 16, TRUE);
    MoveWindow(g_stFormat, fmtX, gy + 4, gx + gw - fmtX, 16, TRUE);

    by = MARGIN + TOP_H + GAP_W;
    bh = H - by - MARGIN;
    if (bh < 40) bh = 40;
    totalW = W - 2*MARGIN - 2*GAP_W;
    w1 = totalW * g_w1perm / 1000;
    w2 = totalW * g_w2perm / 1000;
    w3 = totalW - w1 - w2;

    /* 播客列顶部工具栏（+/−/排序/刷新/目录），列表向下让出 28px */
    x = MARGIN;
    MoveWindow(g_btnAdd,     x,      by, 24, 24, TRUE);
    MoveWindow(g_btnDel,     x + 27, by, 24, 24, TRUE);
    MoveWindow(g_btnSort,    x + 54, by, 24, 24, TRUE);
    MoveWindow(g_btnRefresh, x + 81, by, 24, 24, TRUE);
    MoveWindow(g_btnDir,     x + 108, by, 24, 24, TRUE);
    MoveWindow(g_listPod, x, by + 28, w1, bh - 28, TRUE);
    x += w1 + GAP_W;
    MoveWindow(g_listEp,   x, by, w2, bh, TRUE);
    x += w2 + GAP_W;
    MoveWindow(g_editDesc, x, by, w3, bh, TRUE);
}

/* ================= 方形图标按钮（owner-draw） ================= */
enum {
    ICO_PREV, ICO_PLAY, ICO_PAUSE, ICO_NEXT, ICO_STOP,
    ICO_MUTE, ICO_MUTED, ICO_PLUS, ICO_CROSS, ICO_SORT, ICO_REFRESH,
    ICO_ORDER, ICO_SHUFFLE, ICO_REPEATONE, ICO_PLAYONCE, ICO_GEAR
};

/* 在按钮矩形内绘制单个矢量图标 */
static void DrawGlyph(HDC hdc, const RECT *rc, int kind)
{
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int s = (w < h ? w : h) - 8;                 /* 24px 按钮 → 16px 图标 */
    int x0 = rc->left + (w - s) / 2;
    int y0 = rc->top + (h - s) / 2;
    int cy = y0 + s / 2;
    COLORREF col = ThDim();
    HPEN pen2 = CreatePen(PS_SOLID, 2, col);
    HPEN pen3 = CreatePen(PS_SOLID, 3, col);
    HPEN pen1 = CreatePen(PS_SOLID, 1, col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(hdc, pen1);
    HGDIOBJ ob = SelectObject(hdc, br);

    switch (kind) {
    case ICO_PLAY: {
        POINT t[3] = { {x0+3,y0+1},{x0+3,y0+s-1},{x0+s-2,cy} };
        Polygon(hdc, t, 3);
        break;
    }
    case ICO_PAUSE:
        Rectangle(hdc, x0+3, y0+2, x0+7, y0+s-2);
        Rectangle(hdc, x0+9, y0+2, x0+13, y0+s-2);
        break;
    case ICO_STOP:
        Rectangle(hdc, x0+3, y0+3, x0+s-3, y0+s-3);
        break;
    case ICO_PREV: {
        POINT t[3] = { {x0+13,y0+2},{x0+5,cy},{x0+13,y0+s-2} };
        Rectangle(hdc, x0+2, y0+2, x0+5, y0+s-2);
        Polygon(hdc, t, 3);
        break;
    }
    case ICO_NEXT: {
        POINT t[3] = { {x0+3,y0+2},{x0+11,cy},{x0+3,y0+s-2} };
        Polygon(hdc, t, 3);
        Rectangle(hdc, x0+11, y0+2, x0+14, y0+s-2);
        break;
    }
    case ICO_MUTE:
    case ICO_MUTED: {
        /* 喇叭整体左移，音波弧收在 16px 图标盒内，避免被按钮边缘裁掉 */
        POINT cone[6] = {
            {x0+3,y0+5},{x0+7,y0+5},{x0+12,y0+1},
            {x0+12,y0+s-1},{x0+7,y0+s-5},{x0+3,y0+s-5} };
        Rectangle(hdc, x0+1, y0+5, x0+4, y0+s-5);
        Polygon(hdc, cone, 6);
        if (kind == ICO_MUTE) {
            /* 两道声波弧（开口朝右） */
            SelectObject(hdc, pen2);
            {
                POINT a1[5] = { {x0+12,y0+5},{x0+14,y0+7},{x0+14,cy},{x0+14,y0+s-7},{x0+12,y0+s-5} };
                POINT a2[5] = { {x0+14,y0+3},{x0+15,y0+6},{x0+15,cy},{x0+15,y0+s-6},{x0+14,y0+s-3} };
                Polyline(hdc, a1, 5);
                Polyline(hdc, a2, 5);
            }
        } else {
            /* 静音：喇叭右侧红叉 */
            HPEN rp = CreatePen(PS_SOLID, 2, RGB(200, 40, 40));
            SelectObject(hdc, rp);
            MoveToEx(hdc, x0+11, y0+3, NULL); LineTo(hdc, x0+16, y0+s-3);
            MoveToEx(hdc, x0+16, y0+3, NULL); LineTo(hdc, x0+11, y0+s-3);
            DeleteObject(rp);
        }
        break;
    }
    case ICO_PLUS:
        SelectObject(hdc, pen3);
        MoveToEx(hdc, x0+2, cy, NULL); LineTo(hdc, x0+s-2, cy);
        MoveToEx(hdc, x0+s/2, y0+2, NULL); LineTo(hdc, x0+s/2, y0+s-2);
        break;
    case ICO_CROSS:
        /* 删除按钮：× */
        SelectObject(hdc, pen3);
        MoveToEx(hdc, x0+3, y0+3, NULL); LineTo(hdc, x0+s-3, y0+s-3);
        MoveToEx(hdc, x0+s-3, y0+3, NULL); LineTo(hdc, x0+3, y0+s-3);
        break;
    case ICO_SORT: {
        /* 上下两个箭头（背靠背）：与“调整播客次序”用途一致 */
        int cx = x0 + s / 2;
        POINT up[3]   = { {cx-5,y0+9},{cx+5,y0+9},{cx,y0+2} };
        POINT down[3] = { {cx-5,y0+s-9},{cx+5,y0+s-9},{cx,y0+s-2} };
        Polygon(hdc, up, 3);
        Polygon(hdc, down, 3);
        break;
    }
    case ICO_REFRESH: {
        /* 标准刷新图标：两段圆弧各带一个箭头，首尾相追围成圆环 */
        double cxx = x0 + s / 2.0 + 0.5, cyy = y0 + s / 2.0 + 0.5, r = 5.6;
        int k;
        /* arcPts: 沿角度增大方向采样一段弧（屏幕上呈逆时针走笔） */
        #define LP_ARC(pts, a0, a1, n) do { \
            for (k = 0; k < (n); k++) { \
                double deg = (a0) + ((a1) - (a0)) * k / ((n) - 1); \
                double rad = deg * 3.14159265358979 / 180.0; \
                (pts)[k].x = (int)(cxx + r * cos(rad) + 0.5); \
                (pts)[k].y = (int)(cyy - r * sin(rad) + 0.5); \
            } } while (0)
        /* 在角度 a 处画一个沿“角度减小”（顺时针）方向的箭头 */
        #define LP_ARROW(a) do { \
            double rad = (a) * 3.14159265358979 / 180.0; \
            double dx = sin(rad), dy = cos(rad);          /* 顺时针切向 */ \
            double px = -cos(rad), py = sin(rad);         /* 法向 */ \
            int hx = (int)(cxx + r * cos(rad) + 0.5); \
            int hy = (int)(cyy - r * sin(rad) + 0.5); \
            POINT ah2[3]; \
            ah2[0].x = hx; ah2[0].y = hy; \
            ah2[1].x = (int)(hx - dx * 5 + px * 3 + 0.5); \
            ah2[1].y = (int)(hy - dy * 5 + py * 3 + 0.5); \
            ah2[2].x = (int)(hx - dx * 5 - px * 3 + 0.5); \
            ah2[2].y = (int)(hy - dy * 5 - py * 3 + 0.5); \
            Polygon(hdc, ah2, 3); } while (0)
        POINT arc[16];
        SelectObject(hdc, pen2);
        LP_ARC(arc, 60, 210, 16);     /* 上段弧：从右上绕左侧到左下 */
        Polyline(hdc, arc, 16);
        LP_ARC(arc, 240, 390, 16);    /* 下段弧：从左下绕右侧回右上 */
        Polyline(hdc, arc, 16);
        SelectObject(hdc, pen1);
        LP_ARROW(60);                 /* 上端箭头朝右 */
        LP_ARROW(240);                /* 下端箭头朝左 */
        #undef LP_ARC
        #undef LP_ARROW
        break;
    }
    case ICO_ORDER: {
        /* 顺序播放：三条横线 + 右下播放三角 */
        SelectObject(hdc, pen2);
        MoveToEx(hdc, x0+2, y0+4, NULL);  LineTo(hdc, x0+11, y0+4);
        MoveToEx(hdc, x0+2, y0+8, NULL);  LineTo(hdc, x0+11, y0+8);
        MoveToEx(hdc, x0+2, y0+12, NULL); LineTo(hdc, x0+8,  y0+12);
        {
            POINT t[3] = { {x0+10,y0+9},{x0+10,y0+s-1},{x0+s-2,cy+1} };
            Polygon(hdc, t, 3);
        }
        break;
    }
    case ICO_SHUFFLE: {
        /* 随机播放：两条交叉的折线箭头 */
        SelectObject(hdc, pen2);
        MoveToEx(hdc, x0+2, y0+3, NULL);
        LineTo(hdc, x0+7, y0+3); LineTo(hdc, x0+11, cy); LineTo(hdc, x0+7, y0+s-3); LineTo(hdc, x0+2, y0+s-3);
        MoveToEx(hdc, x0+9, y0+3, NULL);
        LineTo(hdc, x0+13, y0+3); LineTo(hdc, x0+9, cy); LineTo(hdc, x0+13, y0+s-3);
        /* 右箭头 */
        MoveToEx(hdc, x0+11, y0+s-3, NULL); LineTo(hdc, x0+s-2, y0+s-3);
        {
            POINT ah[3] = { {x0+s-5,y0+s-6},{x0+s-2,y0+s-3},{x0+s-5,y0+s} };
            SelectObject(hdc, pen1);
            Polygon(hdc, ah, 3);
        }
        break;
    }
    case ICO_REPEATONE: {
        /* 单曲循环：上下回环箭头 + 中间数字 1 */
        SelectObject(hdc, pen2);
        MoveToEx(hdc, x0+5, y0+4, NULL);
        LineTo(hdc, x0+s-4, y0+4); LineTo(hdc, x0+s-4, y0+8);
        LineTo(hdc, x0+s-7, y0+8);
        {
            POINT ah[3] = { {x0+s-10,y0+5},{x0+s-7,y0+8},{x0+s-10,y0+11} };
            Polygon(hdc, ah, 3);
        }
        MoveToEx(hdc, x0+s-5, y0+s-4, NULL);
        LineTo(hdc, x0+4, y0+s-4); LineTo(hdc, x0+4, y0+s-8);
        LineTo(hdc, x0+7, y0+s-8);
        {
            POINT ah2[3] = { {x0+10,y0+s-5},{x0+7,y0+s-8},{x0+10,y0+s-11} };
            Polygon(hdc, ah2, 3);
        }
        /* 中间 1 */
        SelectObject(hdc, pen3);
        MoveToEx(hdc, x0+s/2, y0+6, NULL); LineTo(hdc, x0+s/2, y0+s-6);
        break;
    }
    case ICO_PLAYONCE: {
        /* 一次性播放：播放三角 + 右侧数字 1（区别于“下一曲”的竖杠） */
        POINT t[3] = { {x0+1,y0+2},{x0+1,y0+s-2},{x0+9,cy} };
        Polygon(hdc, t, 3);
        SelectObject(hdc, pen2);
        /* 小旗 */
        MoveToEx(hdc, x0+10, y0+5, NULL); LineTo(hdc, x0+13, y0+3);
        /* 竖杆 */
        MoveToEx(hdc, x0+13, y0+3, NULL); LineTo(hdc, x0+13, y0+s-3);
        /* 底座 */
        MoveToEx(hdc, x0+10, y0+s-3, NULL); LineTo(hdc, x0+15, y0+s-3);
        break;
    }
    case ICO_GEAR: {
        /* 设置齿轮：8 根齿 + 外圈 + 中心孔 */
        double cx2 = x0 + s / 2.0 + 0.5, cy2 = y0 + s / 2.0 + 0.5;
        int k;
        SelectObject(hdc, pen2);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        for (k = 0; k < 8; k++) {   /* 齿：沿半径方向的短辐条 */
            double rad = k * 3.14159265358979 / 4.0;
            double c = cos(rad), sn = sin(rad);
            MoveToEx(hdc, (int)(cx2 + c * 4.5 + 0.5), (int)(cy2 - sn * 4.5 + 0.5), NULL);
            LineTo(hdc,  (int)(cx2 + c * 7.5 + 0.5), (int)(cy2 - sn * 7.5 + 0.5));
        }
        Ellipse(hdc, (int)(cx2 - 4.5 + 0.5), (int)(cy2 - 4.5 + 0.5),
                     (int)(cx2 + 4.5 + 0.5), (int)(cy2 + 4.5 + 0.5));
        SelectObject(hdc, pen1);
        Ellipse(hdc, (int)(cx2 - 1.5 + 0.5), (int)(cy2 - 1.5 + 0.5),
                     (int)(cx2 + 1.5 + 0.5), (int)(cy2 + 1.5 + 0.5));
        break;
    }
    }

    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen1);
    DeleteObject(pen2);
    DeleteObject(pen3);
    DeleteObject(br);
}

/* on=1：模式按钮已按下（浅蓝底 + 凹陷边） */
static void DrawIconButton(DRAWITEMSTRUCT *di, int kind, int on)
{
    RECT rc = di->rcItem;
    HBRUSH bg;
    if (on) bg = CreateSolidBrush(g_darkMode ? RGB(56,74,100) : RGB(180, 215, 255));
    else bg = CreateSolidBrush(ThWinBg());
    FillRect(di->hDC, &rc, bg);
    DeleteObject(bg);
    if (on || (di->itemState & ODS_SELECTED))
        DrawEdge(di->hDC, &rc, EDGE_SUNKEN, BF_RECT);
    else
        DrawEdge(di->hDC, &rc, EDGE_RAISED, BF_RECT);
    DrawGlyph(di->hDC, &rc, kind);
}

/* 控件 ID → 图标种类；*on 返回是否按下态，不认识返回 -1 */
static int IconKindOf(int id, int *on)
{
    *on = 0;
    switch (id) {
    case IDC_BTN_PREV:  return ICO_PREV;
    case IDC_BTN_PLAY:  return g_playState == 1 ? ICO_PAUSE : ICO_PLAY;
    case IDC_BTN_NEXT:  return ICO_NEXT;
    case IDC_BTN_STOP:  return ICO_STOP;
    case IDC_BTN_MUTE:  *on = g_muted; return g_muted ? ICO_MUTED : ICO_MUTE;
    case IDC_BTN_ADD:   return ICO_PLUS;
    case IDC_BTN_DEL:   *on = (g_podMode == 1); return ICO_CROSS;
    case IDC_BTN_SORT:  *on = (g_podMode == 2); return ICO_SORT;
    case IDC_BTN_REFRESH: return ICO_REFRESH;
    case IDC_BTN_PLAYMODE:
        return (g_playMode == 1) ? ICO_SHUFFLE :
               (g_playMode == 2) ? ICO_REPEATONE :
               (g_playMode == 3) ? ICO_PLAYONCE : ICO_ORDER;
    case IDC_BTN_DIR:   return ICO_GEAR;
    }
    return -1;
}

/* ================= 缓存目录管理 ================= */
/* 统计某目录下 .mp3 文件数量 */
static int CountMp3InDir(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    swprintf(pat, MAX_PATH, L"%s*.mp3", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++; }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* 把旧目录里所有缓存文件（.mp3）移动到新目录 */
static void MoveCacheFiles(const wchar_t *oldDir, const wchar_t *newDir,
                           const wchar_t *ext)
{
    wchar_t pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, MAX_PATH, L"%s*.%s", oldDir, ext);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        swprintf(src, MAX_PATH, L"%s%s", oldDir, fd.cFileName);
        swprintf(dst, MAX_PATH, L"%s%s", newDir, fd.cFileName);
        MoveFileW(src, dst);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 启动/取消定时关机：minutes>0 设定，0 取消；倒计时显示在窗口标题栏 */
static void SetShutdownTimer(int minutes)
{
    if (minutes > 0) {
        g_shutdownAt = (long long)time(NULL) + minutes * 60;
        SetTimer(g_hwnd, 3, 1000, NULL);
        SetStatusTemp(tr(TR_SHUT_SET));
    } else {
        g_shutdownAt = 0;
        KillTimer(g_hwnd, 3);
        SetStatusTemp(tr(TR_SHUT_CANCELED));
    }
    UpdateWindowTitle();
}

/* 到点执行关机：先提权 SE_SHUTDOWN_NAME，失败则仅提示 */
static void DoSystemShutdown(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    g_shutdownAt = 0;
    KillTimer(g_hwnd, 3);
    UpdateWindowTitle();
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    if (!ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG,
                       SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER))
        SetStatus(tr(TR_SHUT_DENIED));
}

/* 设置按钮（原目录按钮）：缓存目录 3 项 + 夜间模式 + 语言 + 定时关机 + 字号 + 关于 */
static void SetEpisodeColumns(int cached);
static void FillEpisodeList(void);
static void RefreshCachedEntry(void);
static void ShowPodcastDetail(int idx);
static void ShowEpisodeDetail(int podIdx, int epIdx);
static const wchar_t *PlayModeTipText(void);

/* 更新某个按钮的系统 tooltip 文本（语言切换时用） */
static void UpdateTipText(HWND btn, const wchar_t *text)
{
    TOOLINFOW ti;
    if (!g_hwndTip || !btn) return;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND;
    ti.hwnd = g_hwnd;
    ti.uId = (UINT_PTR)btn;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(g_hwndTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

/* 只换任务栏 4 个按钮的 tooltip，图标复用 */
static void UpdateThumbbarLanguage(void)
{
    THUMBBUTTON tb[4];
    const wchar_t *tips[4] = {
        tr(TR_TB_PREV),
        g_playState == 1 ? tr(TR_TB_PAUSE) : tr(TR_TB_PLAY),
        tr(TR_TB_NEXT), tr(TR_TB_SHUFFLE)
    };
    int i;
    if (!g_taskbar) return;
    for (i = 0; i < 4; i++) {
        ZeroMemory(&tb[i], sizeof(tb[i]));
        tb[i].dwMask = THB_TOOLTIP;
        tb[i].iId = i;
        wcscpy_s(tb[i].szTip, 260, tips[i]);
    }
    g_taskbar->lpVtbl->ThumbBarUpdateButtons(g_taskbar, g_hwnd, 4, tb);
}

/* 切换语言后刷新所有已创建的界面文本 */
static void ApplyLanguage(void)
{
    wchar_t buf[256];

    /* “已缓存”虚拟播客的固定标题/副标题 */
    if (g_cachePod.title)  { free(g_cachePod.title);  g_cachePod.title  = _wcsdup(tr(TR_CACHED)); }
    if (g_cachePod.author) { free(g_cachePod.author); g_cachePod.author = _wcsdup(tr(TR_CACHE_SUBTITLE)); }

    if (g_listEp) { SetEpisodeColumns(g_viewCached); FillEpisodeList(); }
    RefreshCachedEntry();
    if (g_listPod) InvalidateRect(g_listPod, NULL, TRUE);

    /* 详情框：按当前选中重显 */
    if (g_viewCached) {
        if (g_curEp >= 0) ShowEpisodeDetail(g_podCount, g_curEp);
        else { swprintf(buf, 256, tr(TR_CACHE_VIEW_DESC), g_cachePod.epCount);
               SetWindowTextW(g_editDesc, buf); }
    } else if (g_curPod >= 0 && g_curPod < g_podCount) {
        if (g_curEp >= 0) ShowEpisodeDetail(g_curPod, g_curEp);
        else ShowPodcastDetail(g_curPod);
    }

    UpdateTipText(g_btnPrev,     tr(TR_TIP_PREV));
    UpdateTipText(g_btnPlay,     tr(TR_TIP_PLAYPAUSE));
    UpdateTipText(g_btnNext,     tr(TR_TIP_NEXT));
    UpdateTipText(g_btnStop,     tr(TR_TIP_STOP));
    UpdateTipText(g_btnMute,     tr(TR_TIP_MUTE));
    UpdateTipText(g_btnAdd,      tr(TR_TIP_ADD));
    UpdateTipText(g_btnDel,      tr(TR_TIP_DEL));
    UpdateTipText(g_btnSort,     tr(TR_TIP_SORT));
    UpdateTipText(g_btnRefresh,  tr(TR_TIP_REFRESH));
    UpdateTipText(g_btnPlayMode, PlayModeTipText());
    UpdateTipText(g_btnDir,      tr(TR_TIP_SETTINGS));
    UpdateThumbbarLanguage();
    UpdateWindowTitle();

    /* 状态行：下载中不打断；播放中重拼“正在播放”，其余回到就绪 */
    if (!g_downloading) {
        if (g_playPod >= 0 && g_playEp >= 0 &&
            (g_playState == 1 || g_playState == 2)) {
            Podcast *pp = (g_playPod == g_podCount) ? &g_cachePod : &g_pods[g_playPod];
            if (g_playEp < pp->epCount) {
                wchar_t st[512];
                swprintf(st, 512,
                    g_playPod == g_podCount ? tr(TR_PLAYING_OFFLINE_FMT) : tr(TR_PLAYING_FMT),
                    pp->eps[g_playEp].title ? pp->eps[g_playEp].title : L"");
                SetStatus(st);
            }
        } else {
            SetStatus(tr(TR_READY));
        }
    }
}

static void OnSettingsButton(void)
{
    HMENU menu = CreatePopupMenu();
    HMENU sub  = CreatePopupMenu();
    HMENU lang = CreatePopupMenu();
    POINT pt;
    int cmd;
    wchar_t curDir[MAX_PATH];
    wchar_t fontItem[32];

    static const int mins[] = { 15, 30, 45, 60, 90, 120 };
    static const int nMins = sizeof(mins) / sizeof(mins[0]);
    int i;

    AppendMenuW(menu, MF_STRING, 1, tr(TR_M_OPEN_CACHE));
    AppendMenuW(menu, MF_STRING, 2, tr(TR_M_CHANGE_CACHE));
    AppendMenuW(menu, MF_STRING, 3, tr(TR_M_DEFAULT_CACHE));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_darkMode ? MF_CHECKED : 0), 4, tr(TR_M_NIGHT));
    AppendMenuW(lang, MF_STRING | (g_lang == 0 ? MF_CHECKED : 0), 20, tr(TR_LANG_ZH));
    AppendMenuW(lang, MF_STRING | (g_lang == 1 ? MF_CHECKED : 0), 21, tr(TR_LANG_EN));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)lang, tr(TR_M_LANGUAGE));
    for (i = 0; i < nMins; i++) {
        wchar_t t[32];
        swprintf(t, 32, tr(TR_SHUT_IN_MINS_FMT), mins[i]);
        AppendMenuW(sub, MF_STRING, 100 + mins[i], t);
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING | (g_shutdownAt ? 0 : MF_GRAYED), 5, tr(TR_M_SHUT_CANCEL));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, tr(TR_M_SHUTDOWN));
    swprintf(fontItem, 32, tr(TR_M_FONT_FMT),
        g_fontLevel == 0 ? tr(TR_FONT_S) : g_fontLevel == 1 ? tr(TR_FONT_M) : tr(TR_FONT_L));
    AppendMenuW(menu, MF_STRING, 6, fontItem);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 7, tr(TR_M_ABOUT));

    GetCursorPos(&pt);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);   /* 级联销毁子菜单 */
    if (cmd == 0) return;

    /* 外观/语言/定时类命令先处理，不涉及缓存目录 */
    if (cmd == 4) {
        g_darkMode = !g_darkMode;
        ApplyTheme();
        SetStatusTemp(g_darkMode ? tr(TR_NIGHT_ON) : tr(TR_NIGHT_OFF));
        return;
    }
    if (cmd == 20 || cmd == 21) {
        int nl = (cmd == 21) ? 1 : 0;
        if (nl != g_lang) { g_lang = nl; ApplyLanguage(); }
        return;
    }
    if (cmd == 7) {
        MessageBoxW(g_hwnd, tr(TR_ABOUT_TEXT), tr(TR_M_ABOUT),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (cmd == 5) { SetShutdownTimer(0); return; }
    if (cmd >= 100) { SetShutdownTimer(cmd - 100); return; }
    if (cmd == 6) {
        ChangeFontLevel((g_fontLevel + 1) % 3);
        SetStatusTemp(tr(TR_FONT_SWITCHED));
        return;
    }

    CacheDir(curDir, MAX_PATH);   /* 保留末尾反斜杠：CountMp3InDir/移动都需要它 */

    if (cmd == 1) {
        wchar_t openDir[MAX_PATH];
        wcsncpy(openDir, curDir, MAX_PATH);
        openDir[MAX_PATH - 1] = 0;
        openDir[wcslen(openDir) - 1] = 0;   /* 浏览时去掉末尾反斜杠 */
        ShellExecuteW(g_hwnd, L"open", openDir, NULL, NULL, SW_SHOWNORMAL);
    } else if (cmd == 2) {
        BROWSEINFOW bi;
        LPITEMIDLIST pidl;
        wchar_t newPath[MAX_PATH] = L"";
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = g_hwnd;
        bi.lpszTitle = tr(TR_BROWSE_CACHE);
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderW(&bi);
        if (pidl && SHGetPathFromIDListW(pidl, newPath)) {
            int oldHasFiles = CountMp3InDir(curDir);   /* curDir 带反斜杠 */
            wchar_t newDir[MAX_PATH];
            swprintf(newDir, MAX_PATH, L"%s\\", newPath);
            /* 新旧相同就不必移动 */
            if (oldHasFiles > 0 &&
                _wcsicmp(curDir, newDir) != 0) {
                wchar_t msg[256];
                swprintf(msg, 256, tr(TR_MOVE_Q_NEW_FMT), oldHasFiles);
                if (MessageBoxW(g_hwnd, msg, tr(TR_MOVE_CACHE), MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    CreateDirectoryW(newPath, NULL);
                    MoveCacheFiles(curDir, newDir, L"mp3");
                }
            }
            wcsncpy(g_cacheDir, newPath, MAX_PATH - 1);
            g_cacheDir[MAX_PATH - 1] = 0;
            EnsureCacheDir();
            SetStatusTemp(tr(TR_CACHE_CHANGED));
        }
        if (pidl) CoTaskMemFree(pidl);
    } else if (cmd == 3) {
        /* 恢复默认目录：与"更改目录"一样，旧目录有文件时询问是否移动 */
        wchar_t defDir[MAX_PATH];
        wchar_t defCreate[MAX_PATH];
        ExeDir(defDir, MAX_PATH);
        wcscat_s(defDir, MAX_PATH, L"cache\\");
        if (CountMp3InDir(curDir) > 0 && _wcsicmp(curDir, defDir) != 0) {
            wchar_t msg[256];
            swprintf(msg, 256, tr(TR_MOVE_Q_DEF_FMT), CountMp3InDir(curDir));
            if (MessageBoxW(g_hwnd, msg, tr(TR_MOVE_CACHE), MB_YESNO | MB_ICONQUESTION) == IDYES) {
                wcsncpy(defCreate, defDir, MAX_PATH);
                defCreate[MAX_PATH - 1] = 0;
                defCreate[wcslen(defCreate) - 1] = 0;   /* CreateDirectory 不带末尾反斜杠 */
                CreateDirectoryW(defCreate, NULL);
                MoveCacheFiles(curDir, defDir, L"mp3");
            }
        }
        g_cacheDir[0] = 0;
        EnsureCacheDir();
        SetStatusTemp(tr(TR_CACHE_RESET));
    }

    /* 目录变了：刷新“已缓存”虚拟行；正在看离线列表则重建内容并保持选中 */
    if (cmd == 2 || cmd == 3) {
        RefreshCachedEntry();
        if (g_viewCached) {
            int sel = (int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0);
            FreeCachePodcast();
            LoadCachePodcast();
            FillEpisodeList();
            if (sel < 0) sel = g_podCount;
            SendMessageW(g_listPod, LB_SETCURSEL, sel, 0);
        }
    }
}

/* ---- 任务栏缩略图工具栏（ITaskbarList3） ---- */
/* 在 16x16 画布上画满幅的简洁白色线性图标（图形区约 14x14） */
static void DrawTbGlyph(HDC hdc, int kind)
{
    HBRUSH wh = CreateSolidBrush(RGB(255, 255, 255));
    HPEN p2 = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(hdc, wh);
    HGDIOBJ op = SelectObject(hdc, p2);

    switch (kind) {
    case ICO_PLAY: {
        POINT t[3] = { {3,2},{3,14},{13,8} };
        Polygon(hdc, t, 3);
        break;
    }
    case ICO_PAUSE:
        Rectangle(hdc, 3, 2, 7, 14);
        Rectangle(hdc, 9, 2, 13, 14);
        break;
    case ICO_PREV: {
        POINT t[3] = { {12,2},{4,8},{12,14} };
        Rectangle(hdc, 1, 3, 4, 13);
        Polygon(hdc, t, 3);
        break;
    }
    case ICO_NEXT: {
        POINT t[3] = { {4,2},{12,8},{4,14} };
        Polygon(hdc, t, 3);
        Rectangle(hdc, 12, 3, 15, 13);
        break;
    }
    case ICO_SHUFFLE: {
        /* 两条交叉折线 + 右向箭头 */
        POINT ah[3] = { {11,9},{11,15},{15,12} };
        MoveToEx(hdc, 2, 4, NULL);
        LineTo(hdc, 7, 4); LineTo(hdc, 12, 12);
        MoveToEx(hdc, 2, 12, NULL);
        LineTo(hdc, 7, 12); LineTo(hdc, 12, 4);
        MoveToEx(hdc, 9, 12, NULL); LineTo(hdc, 14, 12);
        SelectObject(hdc, wh);
        Polygon(hdc, ah, 3);
        break;
    }
    }
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(wh);
    DeleteObject(p2);
}

/* 创建 16x16 真彩带 alpha 的图标：背景全透明，图形不透明白色。
 * GDI 不写 alpha 通道，画完后扫描像素，把有色像素的 A 补成 255。 */
static HICON CreateTbIcon(int kind)
{
    HDC sc = GetDC(NULL), cdc, mdc;
    HBITMAP cb, mb, ob1, ob2;
    HICON ico;
    BITMAPINFO bi;
    unsigned char *bits;
    RECT rc = { 0, 0, 16, 16 };
    ICONINFO ii;
    int i;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 16;
    bi.bmiHeader.biHeight = -16;          /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    cb = CreateDIBSection(sc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    cdc = CreateCompatibleDC(sc);
    ob1 = (HBITMAP)SelectObject(cdc, cb);
    DrawTbGlyph(cdc, kind);
    /* 补 alpha：BGRA 中任一通道非 0 即图形像素 */
    for (i = 0; i < 16 * 16; i++) {
        if (bits[i*4] | bits[i*4+1] | bits[i*4+2])
            bits[i*4+3] = 255;
    }
    SelectObject(cdc, ob1);

    /* AND 遮罩全 0：整块都用彩色位图（含 alpha 混合） */
    mb = CreateBitmap(16, 16, 1, 1, NULL);
    mdc = CreateCompatibleDC(sc);
    ob2 = (HBITMAP)SelectObject(mdc, mb);
    FillRect(mdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(mdc, ob2);

    ii.fIcon = TRUE; ii.xHotspot = 0; ii.yHotspot = 0;
    ii.hbmMask = mb; ii.hbmColor = cb;
    ico = CreateIconIndirect(&ii);
    DeleteObject(mb); DeleteObject(cb);
    DeleteDC(cdc); DeleteDC(mdc);
    ReleaseDC(NULL, sc);
    return ico;
}

/* 响应 WM_TASKBARBUTTONCREATED：创建并挂接缩略图按钮 */
static void InitThumbbar(void)
{
    THUMBBUTTON tb[4];
    HRESULT hr;
    if (!g_wmTaskbarBtnCreated) return;
    hr = CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ITaskbarList3, (void**)&g_taskbar);
    if (FAILED(hr) || !g_taskbar) return;
    hr = g_taskbar->lpVtbl->HrInit(g_taskbar);
    if (FAILED(hr)) { g_taskbar->lpVtbl->Release(g_taskbar); g_taskbar = NULL; return; }

    g_tbIco[0] = CreateTbIcon(ICO_PREV);
    g_tbIco[1] = CreateTbIcon(ICO_PLAY);
    g_tbIco[2] = CreateTbIcon(ICO_PAUSE);
    g_tbIco[3] = CreateTbIcon(ICO_NEXT);
    g_tbIco[4] = CreateTbIcon(ICO_SHUFFLE);

    ZeroMemory(tb, sizeof(tb));
    tb[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    tb[0].iId = 0; tb[0].hIcon = g_tbIco[0];
    wcscpy_s(tb[0].szTip, 260, tr(TR_TB_PREV));
    tb[0].dwFlags = THBF_ENABLED;

    tb[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    tb[1].iId = 1; tb[1].hIcon = g_tbIco[1];
    wcscpy_s(tb[1].szTip, 260, tr(TR_TB_PAUSE));
    tb[1].dwFlags = THBF_ENABLED;

    tb[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    tb[2].iId = 2; tb[2].hIcon = g_tbIco[3];
    wcscpy_s(tb[2].szTip, 260, tr(TR_TB_NEXT));
    tb[2].dwFlags = THBF_ENABLED;

    tb[3].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    tb[3].iId = 3; tb[3].hIcon = g_tbIco[4];
    wcscpy_s(tb[3].szTip, 260, tr(TR_TB_SHUFFLE));
    tb[3].dwFlags = THBF_ENABLED;

    g_taskbar->lpVtbl->ThumbBarAddButtons(g_taskbar, g_hwnd, 4, tb);
}

/* 播放/暂停状态切换时更新缩略图按钮图标 */
static void UpdateThumbbarPlayPause(void)
{
    if (!g_taskbar) return;
    if (g_playState == 1) {
        THUMBBUTTON tb;
        ZeroMemory(&tb, sizeof(tb));
        tb.dwMask = THB_ICON | THB_TOOLTIP;
        tb.iId = 1;
        tb.hIcon = g_tbIco[2];   /* 暂停图标 */
        wcscpy_s(tb.szTip, 260, tr(TR_TB_PAUSE));
        g_taskbar->lpVtbl->ThumbBarUpdateButtons(g_taskbar, g_hwnd, 1, &tb);
    } else {
        THUMBBUTTON tb;
        ZeroMemory(&tb, sizeof(tb));
        tb.dwMask = THB_ICON | THB_TOOLTIP;
        tb.iId = 1;
        tb.hIcon = g_tbIco[1];   /* 播放图标 */
        wcscpy_s(tb.szTip, 260, tr(TR_TB_PLAY));
        g_taskbar->lpVtbl->ThumbBarUpdateButtons(g_taskbar, g_hwnd, 1, &tb);
    }
}

/* ================= 工具提示 ================= */
/* 给按钮加一条固定文本的系统 tooltip。
 * TTF_SUBCLASS 让 tooltip 自己子类化控件窗口拦截鼠标消息；
 * 本程序的 PanelProc 是更早安装的外层子类，二者通过
 * SetWindowSubclass/CallWindowProc 链式共存，互不影响。 */
static void AddButtonTip(HWND btn, const wchar_t *text)
{
    TOOLINFOW ti;
    if (!g_hwndTip || !btn) return;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(btn);
    ti.uId = (UINT_PTR)btn;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(g_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static const wchar_t* PlayModeTipText(void)
{
    return g_playMode == 1 ? tr(TR_PM_SHUFFLE) :
           g_playMode == 2 ? tr(TR_PM_REPEAT) :
           g_playMode == 3 ? tr(TR_PM_ONCE) : tr(TR_PM_ORDER);
}

/* 切换播放模式并刷新按钮图标 + 按钮 tooltip（不改动状态行） */
static void CyclePlayMode(void)
{
    g_playMode = (g_playMode + 1) % 4;
    InvalidateRect(g_btnPlayMode, NULL, TRUE);
    if (g_hwndTip) {
        TOOLINFOW ti; ZeroMemory(&ti, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND;
        ti.hwnd = g_hwnd;
        ti.uId = (UINT_PTR)g_btnPlayMode;
        ti.lpszText = (LPWSTR)PlayModeTipText();
        SendMessageW(g_hwndTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

/* 播客自定义提示窗过程：左侧封面，右侧标题+作者 */
static LRESULT CALLBACK PodTipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_INFOBK));
        DrawEdge(hdc, &rc, EDGE_RAISED, BF_RECT);
        if (g_tipPodIdx >= 0 && g_tipPodIdx < g_podCount) {
            Podcast *p = &g_pods[g_tipPodIdx];
            int pad = 8;
            RECT rci = { pad, pad, pad + 96, pad + 96 };
            RECT rct = { pad + 96 + 12, pad + 4, rc.right - pad, pad + 28 };
            RECT rca = { pad + 96 + 12, pad + 30, rc.right - pad, pad + 56 };
            RECT rcd = { pad + 96 + 12, pad + 58, rc.right - pad, rc.bottom - pad };
            /* 封面：保持比例绘制，黑底占位 */
            if (p->hImage) {
                BITMAP bm;
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP old = (HBITMAP)SelectObject(mem, p->hImage);
                GetObject(p->hImage, sizeof(bm), &bm);
                if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                    double r1 = 96.0 / bm.bmWidth, r2 = 96.0 / bm.bmHeight;
                    double r = r1 < r2 ? r1 : r2;
                    int dw = (int)(bm.bmWidth * r + 0.5);
                    int dh = (int)(bm.bmHeight * r + 0.5);
                    int dx = rci.left + (96 - dw) / 2;
                    int dy = rci.top + (96 - dh) / 2;
                    HBRUSH bk = CreateSolidBrush(ThPanel());
                    FillRect(hdc, &rci, bk);
                    DeleteObject(bk);
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                }
                SelectObject(mem, old);
                DeleteDC(mem);
            } else {
                HBRUSH bk = CreateSolidBrush(ThPanel());
                FillRect(hdc, &rci, bk);
                DeleteObject(bk);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_INFOTEXT));
            SelectObject(hdc, g_fontBold);
            DrawTextW(hdc, p->title ? p->title : tr(TR_UNTITLED), -1, &rct,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hdc, g_fontSmall);
            DrawTextW(hdc, p->author ? p->author : L"", -1, &rca,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            if (p->desc)
                DrawTextW(hdc, p->desc, -1, &rcd,
                    DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 根据鼠标在播客列表中的位置，更新并显示自定义播客提示 */
static void UpdatePodTipFromPoint(POINT pt)
{
    int idx;
    RECT irc;
    POINT screen;
    if (g_podMode != 0) { ShowWindow(g_hwndPodTip, SW_HIDE); return; }
    idx = (int)SendMessageW(g_listPod, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    if (idx < 0 || idx >= g_podCount) { ShowWindow(g_hwndPodTip, SW_HIDE); return; }
    if (idx != g_tipPodIdx) {
        g_tipPodIdx = idx;
        InvalidateRect(g_hwndPodTip, NULL, TRUE);
    }
    /* 定位到条目右下方，避免遮挡文字 */
    SendMessageW(g_listPod, LB_GETITEMRECT, idx, (LPARAM)&irc);
    screen.x = irc.right; screen.y = irc.top;
    ClientToScreen(g_listPod, &screen);
    SetWindowPos(g_hwndPodTip, HWND_TOPMOST, screen.x + 6, screen.y, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_hwndPodTip, SW_SHOWNA);
}

/* ================= 窗口过程 ================= */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* 任务栏缩略图工具栏：Explorer 广播此消息后初始化 */
    if (msg == g_wmTaskbarBtnCreated) {
        InitThumbbar();
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        DWORD lbStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP
                      | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS;

        g_font = CreateUiFont(8 + g_fontLevel, FW_NORMAL);
        g_fontBold = CreateUiFont(8 + g_fontLevel, FW_BOLD);
        g_fontSmall = CreateUiFont(7 + g_fontLevel, FW_NORMAL);
        g_whiteBrush = CreateSolidBrush(ThPanel());

        /* 第一行状态文本由 WM_PAINT 自绘跑马灯，这里不建 STATIC */
        g_sldSeek = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0,0,0,0, hwnd, (HMENU)IDC_SLD_SEEK, NULL, NULL);
        SendMessageW(g_sldSeek, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        g_stTime = CreateWindowExW(0, L"STATIC", L"00:00 / 00:00",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0,0,0,0, hwnd, (HMENU)IDC_ST_TIME, NULL, NULL);

        /* 全部方形图标按钮 */
        {
            const int ids[11] = {
                IDC_BTN_PREV, IDC_BTN_PLAY, IDC_BTN_NEXT, IDC_BTN_STOP, IDC_BTN_MUTE,
                IDC_BTN_ADD, IDC_BTN_DEL, IDC_BTN_SORT, IDC_BTN_REFRESH, IDC_BTN_PLAYMODE,
                IDC_BTN_DIR };
            HWND *p[11] = {
                &g_btnPrev, &g_btnPlay, &g_btnNext, &g_btnStop, &g_btnMute,
                &g_btnAdd, &g_btnDel, &g_btnSort, &g_btnRefresh, &g_btnPlayMode,
                &g_btnDir };
            int bi;
            for (bi = 0; bi < 11; bi++)
                *p[bi] = CreateWindowExW(0, L"BUTTON", NULL,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    0,0,0,0, hwnd, (HMENU)(INT_PTR)ids[bi], NULL, NULL);
        }

        g_sldVol = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0,0,0,0, hwnd, (HMENU)IDC_SLD_VOL, NULL, NULL);
        SendMessageW(g_sldVol, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(g_sldVol, TBM_SETPOS, TRUE, g_volume);
        {
            wchar_t vt[8];
            swprintf(vt, 8, L"%d", g_volume);
            g_stVol = CreateWindowExW(0, L"STATIC", vt,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0,0,0,0, hwnd, (HMENU)IDC_ST_VOL, NULL, NULL);
        }
        g_stFormat = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0,0,0,0, hwnd, (HMENU)IDC_ST_FORMAT, NULL, NULL);

        g_listPod = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
            lbStyle | LBS_OWNERDRAWFIXED,
            0,0,0,0, hwnd, (HMENU)IDC_LIST_POD, NULL, NULL);
        /* 行高 54px 由 WM_MEASUREITEM 统一设置 */

        /* 剧集：ListView 报表视图（标题/日期/时长，横向可滚动，列头可排序） */
        g_listEp = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0,0,0,0, hwnd, (HMENU)IDC_LIST_EP, NULL, NULL);
        {
            const wchar_t *colTitles[3] = { tr(TR_COL_TITLE), tr(TR_COL_DATE), tr(TR_COL_DUR) };
            int i;
            for (i = 0; i < 3; i++) {
                LVCOLUMNW col;
                ZeroMemory(&col, sizeof(col));
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                col.cx = g_colW[i];
                col.iSubItem = i;
                col.pszText = (wchar_t*)colTitles[i];
                SendMessageW(g_listEp, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
            }
            ListView_SetExtendedListViewStyle(g_listEp,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
            SendMessageW(g_listEp, LVM_SETBKCOLOR, 0, (LPARAM)ThPanel());
            SendMessageW(g_listEp, LVM_SETTEXTBKCOLOR, 0, (LPARAM)ThPanel());
        }

        g_editDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_EDIT_DESC, NULL, NULL);

        ApplyFontsToControls();

        /* 滑块子类化：修复滚轮方向 */
        g_oldSeekProc = (WNDPROC)SetWindowLongPtrW(g_sldSeek, GWLP_WNDPROC, (LONG_PTR)SliderProc);
        g_oldVolProc  = (WNDPROC)SetWindowLongPtrW(g_sldVol,  GWLP_WNDPROC, (LONG_PTR)SliderProc);
        /* 内容面板与按钮子类化：Ctrl+滚轮调字号 */
        SubclassForFontWheel(g_listPod);
        SubclassForFontWheel(g_listEp);
        SubclassForFontWheel(g_editDesc);
        SubclassForFontWheel(g_btnPrev);
        SubclassForFontWheel(g_btnPlay);
        SubclassForFontWheel(g_btnNext);
        SubclassForFontWheel(g_btnStop);
        SubclassForFontWheel(g_btnMute);
        SubclassForFontWheel(g_btnAdd);
        SubclassForFontWheel(g_btnDel);
        SubclassForFontWheel(g_btnSort);
        SubclassForFontWheel(g_btnRefresh);
        SubclassForFontWheel(g_btnPlayMode);
        SubclassForFontWheel(g_btnDir);

        SetTimer(hwnd, 1, 500, NULL);

        /* 系统工具提示：按钮 */
        g_hwndTip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, NULL, GetModuleHandleW(NULL), NULL);
        if (g_hwndTip) {
            SetWindowPos(g_hwndTip, HWND_TOPMOST, 0,0,0,0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            AddButtonTip(g_btnPrev,   tr(TR_TIP_PREV));
            AddButtonTip(g_btnPlay,   tr(TR_TIP_PLAYPAUSE));
            AddButtonTip(g_btnNext,   tr(TR_TIP_NEXT));
            AddButtonTip(g_btnStop,   tr(TR_TIP_STOP));
            AddButtonTip(g_btnMute,   tr(TR_TIP_MUTE));
            AddButtonTip(g_btnAdd,    tr(TR_TIP_ADD));
            AddButtonTip(g_btnDel,    tr(TR_TIP_DEL));
            AddButtonTip(g_btnSort,   tr(TR_TIP_SORT));
            AddButtonTip(g_btnRefresh,tr(TR_TIP_REFRESH));
            AddButtonTip(g_btnPlayMode, PlayModeTipText());
            AddButtonTip(g_btnDir,    tr(TR_TIP_SETTINGS));
        }
        /* 剧集列表 tooltip 不用自建：LVS_EX_INFOTIP 让 ListView 在悬停时
         * 自动发 LVN_GETINFOTIPW，由 FillEpisodeTip 回填文本 */

        /* 自定义播客提示窗（封面 + 标题 + 作者） */
        {
            WNDCLASSEXW wc; ZeroMemory(&wc, sizeof(wc));
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = PodTipProc;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.lpszClassName = L"LpPodTip";
            wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_INFOBK + 1);
            RegisterClassExW(&wc);
            g_hwndPodTip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                L"LpPodTip", NULL, WS_POPUP, 0,0, 320, 140, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        }
        return 0;
    }

    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT*)lParam)->CtlID == IDC_LIST_POD) {
            ((MEASUREITEMSTRUCT*)lParam)->itemHeight = 54;
            return TRUE;
        }
        break;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT*)lParam;
        if (di->CtlType == ODT_HEADER) {
            /* 正常路径在 ListView 的子类 PanelProc 中；此处兜底 */
            return DrawHeaderItem(di) ? TRUE : FALSE;
        }
        if (di->CtlType == ODT_BUTTON) {
            int on, kind = IconKindOf(di->CtlID, &on);
            if (kind >= 0) { DrawIconButton(di, kind, on); return TRUE; }
            break;
        }
        if (di->CtlID != IDC_LIST_POD || di->itemID == (UINT)-1) break;
        if ((int)di->itemID > g_podCount) break;
        {
            int isCache = ((int)di->itemID == g_podCount);
            Podcast *p = isCache ? &g_cachePod : &g_pods[di->itemID];
            int isSel = (di->itemState & ODS_SELECTED) != 0;
            int isPlaying = (g_playPod == (int)di->itemID && g_playState == 1);
            HBRUSH bg = CreateSolidBrush(isSel ? ThSel() : ThPanel());
            RECT rc = di->rcItem;
            int textLeft;

            FillRect(di->hDC, &rc, bg);
            DeleteObject(bg);

            /* 封面区：48x48，左留 2px、上留 3px。
             * 删除模式 → 大红叉按钮；排序模式 → 左右两个上/下箭头按钮
             * “已缓存”虚拟行不参与删除/排序，画一个下载缓存图标 */
            if (isCache) {
                RECT ib = { rc.left + 2, rc.top + 3, rc.left + 50, rc.top + 51 };
                HBRUSH fb = CreateSolidBrush(g_darkMode ? RGB(50,56,66) : RGB(238,242,248));
                HPEN gp = CreatePen(PS_SOLID, 2, g_darkMode ? RGB(130,150,180) : RGB(120,140,170));
                HBRUSH ab = CreateSolidBrush(g_darkMode ? RGB(130,150,180) : RGB(120,140,170));
                HGDIOBJ oldPen, oldBrush;
                FillRect(di->hDC, &ib, fb);
                DeleteObject(fb);
                oldPen = SelectObject(di->hDC, gp);
                oldBrush = SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
                RoundRect(di->hDC, ib.left+3, ib.top+3, ib.right-3, ib.bottom-3, 8, 8);
                /* 向下箭头（表示缓存到本地） */
                SelectObject(di->hDC, ab);
                {
                    POINT tri[3] = {
                        {rc.left+17, rc.top+22}, {rc.left+33, rc.top+22},
                        {rc.left+25, rc.top+33} };
                    Polygon(di->hDC, tri, 3);
                }
                SelectObject(di->hDC, GetStockObject(NULL_PEN));
                {
                    RECT tb = { rc.left+22, rc.top+12, rc.left+29, rc.top+24 };
                    Rectangle(di->hDC, tb.left, tb.top, tb.right, tb.bottom);
                }
                SelectObject(di->hDC, oldPen);
                SelectObject(di->hDC, oldBrush);
                DeleteObject(ab);
                DeleteObject(gp);
                textLeft = rc.left + 56;
            } else if (g_podMode == 1) {
                RECT xb = { rc.left + 2, rc.top + 3, rc.left + 50, rc.top + 51 };
                HPEN rp = CreatePen(PS_SOLID, 5, RGB(205, 40, 40));
                HGDIOBJ oldp = SelectObject(di->hDC, rp);
                FillRect(di->hDC, &xb, (HBRUSH)GetStockObject(WHITE_BRUSH));
                DrawEdge(di->hDC, &xb, EDGE_RAISED, BF_RECT);
                MoveToEx(di->hDC, xb.left + 8, xb.top + 8, NULL);
                LineTo(di->hDC, xb.right - 8, xb.bottom - 8);
                MoveToEx(di->hDC, xb.right - 8, xb.top + 8, NULL);
                LineTo(di->hDC, xb.left + 8, xb.bottom - 8);
                SelectObject(di->hDC, oldp);
                DeleteObject(rp);
                textLeft = rc.left + 56;
            } else if (g_podMode == 2) {
                RECT rL = { rc.left + 2, rc.top + 3, rc.left + 25, rc.top + 51 };
                RECT rR = { rc.left + 25, rc.top + 3, rc.left + 50, rc.top + 51 };
                HBRUSH fb = CreateSolidBrush(ThPanel());
                HPEN dp = CreatePen(PS_SOLID, 1, ThDim());
                HBRUSH db = CreateSolidBrush(ThDim());
                HGDIOBJ op = SelectObject(di->hDC, dp);
                HGDIOBJ ob = SelectObject(di->hDC, db);
                POINT up[3]   = { {rc.left + 7, rc.top + 33}, {rc.left + 20, rc.top + 33}, {rc.left + 13, rc.top + 19} };
                POINT down[3] = { {rc.left + 30, rc.top + 21}, {rc.left + 43, rc.top + 21}, {rc.left + 36, rc.top + 35} };
                FillRect(di->hDC, &rL, fb);
                FillRect(di->hDC, &rR, fb);
                DrawEdge(di->hDC, &rL, EDGE_RAISED, BF_RECT);
                DrawEdge(di->hDC, &rR, EDGE_RAISED, BF_RECT);
                Polygon(di->hDC, up, 3);
                Polygon(di->hDC, down, 3);
                SelectObject(di->hDC, op);
                SelectObject(di->hDC, ob);
                DeleteObject(dp);
                DeleteObject(db);
                DeleteObject(fb);
                textLeft = rc.left + 56;
            } else if (p->hImage) {
                BITMAP bm;
                HDC mem = CreateCompatibleDC(di->hDC);
                HBITMAP old = (HBITMAP)SelectObject(mem, p->hImage);
                GetObject(p->hImage, sizeof(bm), &bm);
                SetStretchBltMode(di->hDC, HALFTONE);
                StretchBlt(di->hDC, rc.left + 2, rc.top + 3, 48, 48,
                           mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
                textLeft = rc.left + 56;
            } else {
                textLeft = rc.left + 6;
            }

            SetBkMode(di->hDC, TRANSPARENT);

            /* 标题：加粗，播放中绿色 */
            {
                wchar_t title[64];
                const wchar_t *tp;
                RECT rct = rc;
                rct.left = textLeft;
                rct.right = rc.right - 28;
                rct.top += 3;
                rct.bottom = rct.top + 22;
                if (isCache) {
                    swprintf(title, 64, tr(TR_CACHED_FMT), g_cacheFileCount);
                    tp = title;
                } else tp = p->title;
                SetTextColor(di->hDC, isPlaying ? ThGreen() : ThText());
                SelectObject(di->hDC, g_fontBold);
                DrawTextW(di->hDC, tp, -1, &rct,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            /* 副标题：copyright（空则 author），小字号灰色，标题下方；
             * 虚拟行固定提示离线播放 */
            {
                const wchar_t *sub = isCache
                    ? tr(TR_OFFLINE_LOCAL)
                    : (p->copyright ? p->copyright : p->author);
                if (sub && *sub) {
                    RECT rcs = rc;
                    rcs.left = textLeft;
                    rcs.right = rc.right - 28;
                    rcs.top += 26;
                    rcs.bottom = rcs.top + 24;
                    SetTextColor(di->hDC, ThDim());
                    SelectObject(di->hDC, g_fontSmall);
                    DrawTextW(di->hDC, sub, -1, &rcs,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }

            /* 右侧：剧集数（小一号字体）；虚拟行显示缓存文件数 */
            {
                wchar_t cnt[16];
                RECT rc2 = di->rcItem;
                int n = isCache ? g_cacheFileCount : p->epCount;
                swprintf(cnt, 16, L"%d", n);
                SetTextColor(di->hDC, isSel ? ThText() : ThDim());
                SelectObject(di->hDC, g_fontSmall);
                rc2.left = rc2.right - 26;
                rc2.right -= 4;
                DrawTextW(di->hDC, cnt, -1, &rc2,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }

            /* 焦点虚线框 */
            if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &di->rcItem);
        }
        return TRUE;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_winW = LOWORD(lParam);
            g_winH = HIWORD(lParam);
        }
        ApplyLayout(hwnd, IsWindowVisible(hwnd));
        UpdateMarquee();   /* 可用宽度变化，重算跑马灯 */
        return 0;

    case WM_MOUSEWHEEL:
        /* 在播放区/按钮等空白处 Ctrl+滚轮也能调字号 */
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            ChangeFontLevel(g_fontLevel + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1));
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            DWORD pos = GetMessagePos();
            POINT pt = { GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
            ScreenToClient(hwnd, &pt);
            if (g_dragSplit >= 0 || SplitterHitTest(hwnd, pt.x, pt.y) >= 0) {
                SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int s = SplitterHitTest(hwnd, pt.x, pt.y);
        if (s >= 0) {
            g_dragSplit = s;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (g_dragSplit >= 0) {
            RECT rc;
            int totalW, w1px, w2px;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            GetClientRect(hwnd, &rc);
            totalW = rc.right - 2*MARGIN - 2*GAP_W;
            w1px = totalW * g_w1perm / 1000;
            w2px = totalW * g_w2perm / 1000;
            if (g_dragSplit == 0) {
                w1px = pt.x - MARGIN - GAP_W/2;
                if (w1px < SPLIT_MINPX) w1px = SPLIT_MINPX;
                if (w1px > totalW - w2px - SPLIT_MINPX)
                    w1px = totalW - w2px - SPLIT_MINPX;
                g_w1perm = w1px * 1000 / totalW;
            } else {
                w2px = pt.x - MARGIN - w1px - 3*GAP_W/2;
                if (w2px < SPLIT_MINPX) w2px = SPLIT_MINPX;
                if (w2px > totalW - w1px - SPLIT_MINPX)
                    w2px = totalW - w1px - SPLIT_MINPX;
                g_w2perm = w2px * 1000 / totalW;
            }
            ApplyLayout(hwnd, 1);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_dragSplit >= 0) {
            g_dragSplit = -1;
            ReleaseCapture();
            /* 列宽退出时统一保存 */
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        r.left = MARGIN; r.top = MARGIN;
        r.right -= MARGIN; r.bottom = MARGIN + TOP_H;
        /* 顶部播放区：与下方三个区域同款凹陷边框 */
        DrawEdge(hdc, &r, EDGE_SUNKEN, BF_RECT);

        /* 第一行状态文本：超长时跑马灯循环，用双缓冲避免闪烁 */
        {
            RECT sr;
            int sw, sh;
            HDC mem;
            HBITMAP bmp, oldBmp;
            HFONT oldF;
            StatusRowRect(&sr);
            sw = sr.right - sr.left;
            sh = sr.bottom - sr.top;
            mem = CreateCompatibleDC(hdc);
            bmp = CreateCompatibleBitmap(hdc, sw, sh);
            oldBmp = (HBITMAP)SelectObject(mem, bmp);
            FillRect(mem, &(RECT){0, 0, sw, sh}, g_bgBrush ? g_bgBrush : GetSysColorBrush(COLOR_BTNFACE));
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, ThText());
            oldF = (HFONT)SelectObject(mem, g_font);
            if (!g_mqActive) {
                RECT tr = { 0, 0, sw, sh };
                DrawTextW(mem, g_statusText, -1, &tr,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            } else {
                /* 首尾相接的两份文本，间隔 28px */
                int x0 = -g_mqOffset, i;
                for (i = 0; i < 2; i++) {
                    RECT tr = { x0 + i * (g_mqTextW + 28), 0,
                                x0 + i * (g_mqTextW + 28) + g_mqTextW + 2, sh };
                    DrawTextW(mem, g_statusText, -1, &tr,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
            SelectObject(mem, oldF);
            BitBlt(hdc, sr.left, sr.top, sw, sh, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    /* 播客列表底色随主题 */
    case WM_CTLCOLORLISTBOX:
        if ((HWND)lParam == g_listPod) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, ThPanel());
            SetTextColor(hdc, ThText());
            return (LRESULT)g_whiteBrush;
        }
        break;

    case WM_CTLCOLORSTATIC:
        /* 只读简介框：面板底色 */
        if ((HWND)lParam == g_editDesc) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, ThPanel());
            SetTextColor(hdc, ThText());
            return (LRESULT)g_whiteBrush;
        }
        /* 其它静态文字（时间/音量/格式串）：透明背景 + 主题文字色 */
        {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ThText());
            return (LRESULT)(g_bgBrush
                ? g_bgBrush : GetSysColorBrush(COLOR_BTNFACE));
        }

    case WM_GETMINMAXINFO: {
        /* 限制的是客户区最小 480x360，换算成含标题栏/可调边框的外框尺寸
         * （GetWindowRect 含每侧约 7-8px 不可见调宽边，直接写 480 会导致客户区被缩小） */
        MINMAXINFO *mm = (MINMAXINFO*)lParam;
        RECT wr = { 0, 0, 480, 360 };
        AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMinTrackSize.x = wr.right - wr.left;
        mm->ptMinTrackSize.y = wr.bottom - wr.top;
        return 0;
    }

    case WM_NCCALCSIZE: {
        /* 窄边框：让 DefWindowProc 算出默认非客户区后，把客户区向外扩几像素，
         * 压薄左右下三边的可见边框（标题栏保留）。尺寸拖拽仍由原命中区负责。 */
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS *ncp = (NCCALCSIZE_PARAMS*)lParam;
            DefWindowProcW(hwnd, WM_NCCALCSIZE, wParam, lParam);
            ncp->rgrc[0].left   -= 4;
            ncp->rgrc[0].right  += 4;
            ncp->rgrc[0].bottom += 4;
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        /* 任务栏缩略图工具栏按钮（THBN_CLICKED） */
        if (HIWORD(wParam) == THBN_CLICKED) {
            int tid = LOWORD(wParam);
            if (tid == 0) PlayAdjacent(-1);                    /* 上一曲 */
            else if (tid == 1) PauseResume();                  /* 播放/暂停 */
            else if (tid == 2) PlayAdjacent(+1);               /* 下一曲 */
            else if (tid == 3) {                               /* 随机下一曲 */
                int pod = (g_playPod >= 0) ? g_playPod : g_curPod;
                int n = (pod == g_podCount) ? g_cachePod.epCount : g_pods[pod].epCount;
                if (pod >= 0 && n > 0) {
                    int target = rand() % n;
                    RequestPlayEpisode(pod, target);
                }
            }
            return 0;
        }
        if (HIWORD(wParam) == LBN_SELCHANGE) {
            if (LOWORD(wParam) == IDC_LIST_POD)
                SelectPodcast((int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0));
        } else if (HIWORD(wParam) == BN_CLICKED) {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_PLAY) PauseResume();
            else if (id == IDC_BTN_PREV) PlayAdjacent(-1);
            else if (id == IDC_BTN_NEXT) PlayAdjacent(+1);
            else if (id == IDC_BTN_STOP) {
                if (g_downloading) CancelCurrentDownload();   /* 下载中点停止：中止下载 */
                PlayerStop();
                SetStatus(tr(TR_STOPPED));
            }
            else if (id == IDC_BTN_MUTE) ToggleMute();
            else if (id == IDC_BTN_ADD) AskAddFeed();
            else if (id == IDC_BTN_DEL) SetPodMode(1);
            else if (id == IDC_BTN_SORT) SetPodMode(2);
            else if (id == IDC_BTN_REFRESH) ReloadFeeds();
            else if (id == IDC_BTN_PLAYMODE) CyclePlayMode();
            else if (id == IDC_BTN_DIR) OnSettingsButton();
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR*)lParam;

        /* 滑块始终自绘：亮/暗主题统一圆角造型 */
        if ((nm->idFrom == IDC_SLD_SEEK || nm->idFrom == IDC_SLD_VOL) &&
            nm->code == NM_CUSTOMDRAW) {
            LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)lParam;
            if (cd->dwDrawStage == CDDS_PREPAINT &&
                DrawSliderTrack(cd, nm->idFrom == IDC_SLD_SEEK))
                return CDRF_SKIPDEFAULT;
            return CDRF_DODEFAULT;
        }

        if (nm->code == HDN_ITEMCLICKW) {   /* Unicode 窗口只会收到 W 版通知 */
            LPNMHEADERW hdr = (LPNMHEADERW)lParam;
            HWND hv = (HWND)SendMessageW(g_listEp, LVM_GETHEADER, 0, 0);
            if (nm->hwndFrom == hv && hdr->iItem >= 0 && hdr->iItem <= 2)
                ChangeSort(hdr->iItem);
        } else if (nm->idFrom == IDC_LIST_EP) {
            if (nm->code == LVN_ITEMCHANGED) {
                LPNMLISTVIEW lv = (LPNMLISTVIEW)lParam;
                if ((lv->uNewState & LVIS_SELECTED) && lv->iItem >= 0)
                    SelectEpisode(lv->iItem);
            } else if (nm->code == NM_DBLCLK) {
                int sel = (int)SendMessageW(g_listEp, LVM_GETNEXTITEM,
                                            (WPARAM)-1, LVNI_SELECTED);
                if (sel >= 0) RequestPlayEpisode(g_curPod, sel);
            } else if (nm->code == LVN_GETINFOTIPW) {
                /* ListView 原生悬停提示（LVS_EX_INFOTIP）：控件自己管计时/命中 */
                FillEpisodeTip((LPNMLVGETINFOTIPW)lParam);
            } else if (nm->code == NM_CUSTOMDRAW) {
                /* 正在播放 → 绿；已缓存（非播放）→ 黄 */
                LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;   /* 必须，否则收不到 ITEMPREPAINT */
                } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int i = (int)cd->nmcd.dwItemSpec;
                    Podcast *p = ActivePodcast();
                    int playing = 0;
                    /* 正在播放目标（下载准备中/播放中/暂停中均算）标绿。
                     * 用独立的 g_playEp，用户点选其他行不会让绿色乱跑；
                     * g_playPod 在 PlayerStop 或播放失败时清空。 */
                    if (g_playPod >= 0 && i >= 0 && i == g_playEp) {
                        if (g_viewCached)
                            playing = (g_playPod == g_podCount);
                        else if (g_curPod >= 0 && g_curPod < g_podCount)
                            playing = (g_playPod == g_curPod);
                    }
                    int cached = 0;
                    int sel = (ListView_GetItemState(g_listEp, i, LVIS_SELECTED)
                               & LVIS_SELECTED) != 0;
                    if (p && i >= 0 && i < p->epCount)
                        cached = p->eps[i].localFile || EpisodeIsCached(&p->eps[i]);
                    if (playing) {
                        /* 播放行同时是选中行（默认蓝底白字会盖住绿字），
                         * 自己铺一层浅绿底再用深绿字 */
                        RECT rr = cd->nmcd.rc;
                        HBRUSH bg = CreateSolidBrush(ThGreenBg());
                        FillRect(cd->nmcd.hdc, &rr, bg);
                        DeleteObject(bg);
                        cd->clrText = ThGreen();
                    } else {
                        /* 选中行自己铺主题高亮色（暗色下系统高亮仍是浅色） */
                        if (sel) {
                            RECT rr = cd->nmcd.rc;
                            HBRUSH bg = CreateSolidBrush(ThSel());
                            FillRect(cd->nmcd.hdc, &rr, bg);
                            DeleteObject(bg);
                        }
                        cd->clrText = cached ? ThYellow()
                            : (sel ? (g_darkMode ? RGB(236, 242, 250)
                                                 : RGB(0, 30, 80))
                                   : ThText());
                    }
                    return CDRF_NEWFONT;
                }
            }
        }
        return 0;
    }

    case WM_HSCROLL:
        if ((HWND)lParam == g_sldVol) {
            wchar_t buf[16];
            int v = (int)SendMessageW(g_sldVol, TBM_GETPOS, 0, 0);
            g_volume = v;
            swprintf(buf, 16, L"%d", v);
            SetWindowTextW(g_stVol, buf);
            if (g_player) g_player->lpVtbl->SetVolume(g_player, v / 100.0f);
            /* 手动调音量时自动解除静音 */
            if (g_muted) {
                g_muted = 0;
                if (g_player) g_player->lpVtbl->SetMute(g_player, FALSE);
                InvalidateRect(g_btnMute, NULL, TRUE);
            }
            /* 音量退出时统一保存 */
        } else if ((HWND)lParam == g_sldSeek) {
            int code = LOWORD(wParam);
            if (code == TB_THUMBTRACK || code == TB_THUMBPOSITION) {
                g_seeking = 1;
                if (g_dur100ns > 0) {
                    wchar_t buf[64], a[16], b[16];
                    int pm = (int)SendMessageW(g_sldSeek, TBM_GETPOS, 0, 0);
                    FmtTime(g_dur100ns / 10000000 * pm / 1000, a, 16);
                    FmtTime(g_dur100ns / 10000000, b, 16);
                    swprintf(buf, 64, L"%s / %s", a, b);
                    SetWindowTextW(g_stTime, buf);
                }
            } else if (code == TB_ENDTRACK) {
                int pm = (int)SendMessageW(g_sldSeek, TBM_GETPOS, 0, 0);
                PlayerSeekToFrac(pm);
                g_seeking = 0;
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == 2) {
            /* 跑马灯：每次左移 2px，整条滚完后从头再来 */
            if (g_mqActive) {
                RECT sr;
                g_mqOffset += 2;
                if (g_mqOffset > g_mqTextW + 28) g_mqOffset = 0;
                StatusRowRect(&sr);
                InvalidateRect(hwnd, &sr, FALSE);
            }
            return 0;
        }
        if (wParam == 3) {
            /* 定时关机倒计时：每秒刷新标题栏，到点执行关机 */
            if (g_shutdownAt > 0) {
                if ((long long)time(NULL) >= g_shutdownAt) {
                    DoSystemShutdown();
                } else {
                    UpdateWindowTitle();
                }
            }
            return 0;
        }
        if (wParam == 4) {
            /* 临时状态提示到期：恢复“正在播放：…”等持久内容 */
            RestoreBaseStatus();
            return 0;
        }
        if (g_playState == 1 && !g_seeking && g_player) {
            long long pos = PlayerGet100ns(0);
            long long dur = PlayerGet100ns(1);
            if (dur > 0) g_dur100ns = dur;
            if (pos >= 0) {
                wchar_t buf[64], a[16], b[16];
                FmtTime(pos / 10000000, a, 16);
                if (g_dur100ns > 0) FmtTime(g_dur100ns / 10000000, b, 16);
                else wcscpy_s(b, 16, L"--:--");
                swprintf(buf, 64, L"%s / %s", a, b);
                SetWindowTextW(g_stTime, buf);
                if (g_dur100ns > 0) {
                    SendMessageW(g_sldSeek, TBM_SETPOS, TRUE,
                        (LPARAM)(pos * 1000 / g_dur100ns));
                    if (pos >= g_dur100ns - 5000000 && pos > 0) {
                        /* 播放结束 */
                        PlayerStop();
                        SetStatus(tr(TR_FINISHED));
                    }
                }
            }
        }
        return 0;

    case WM_APP_FEED_ADDED: {
        Podcast *pod = (Podcast*)lParam;
        int incremental = (int)wParam;   /* 1=单条添加，0=全量刷新 */
        if (pod) {
            if (g_podCount < MAX_PODCASTS) {
                int newIdx = g_podCount;
                g_pods[g_podCount] = *pod;
                /* 插入到“已缓存”虚拟行之前（无虚拟行时等同末尾追加） */
                SendMessageW(g_listPod, LB_INSERTSTRING, newIdx, (LPARAM)pod->title);
                g_podCount++;
                if (incremental) {
                    if (g_viewCached) {
                        /* 正在看离线列表：保持虚拟行选中，只让新播客排在前面 */
                        SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
                    } else {
                        /* 单条添加：自动选中新条目，让用户立即看到剧集 */
                        SendMessageW(g_listPod, LB_SETCURSEL, newIdx, 0);
                        SelectPodcast(newIdx);
                    }
                } else if (g_viewCached) {
                    /* 启动时预置了“已缓存”选中：插入真实播客后选中跟随虚拟行 */
                    SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
                }
            } else {
                FreePodcast(pod);   /* 超过上限：丢弃并释放 */
            }
            free(pod);
        }
        return 0;
    }

    case WM_APP_FEEDS_DONE:
        g_feedsLoading = 0;
        RefreshCachedEntry();   /* 末尾补“已缓存”行（有本地音频时） */
        if (g_podCount > 0) {
            int cursel = (int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0);
            SetStatus(tr(TR_READY));
            /* 单条添加时新条目已在 FEED_ADDED 中选中；全量刷新默认第一条。
             * 启动时为离线预置了“已缓存”选中，订阅加载成功后切到第一条 */
            if (cursel == LB_ERR || (g_fullReload && g_viewCached)) {
                SendMessageW(g_listPod, LB_SETCURSEL, 0, 0);
                SelectPodcast(0);
            }
        } else if (g_cacheRowHere) {
            SetStatus(tr(TR_OFFLINE_HINT));
            /* 直接选中“已缓存”行，离线音频一目了然 */
            SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
            SelectPodcast(g_podCount);
        } else {
            SetStatus(tr(TR_NO_FEEDS_HINT));
        }
        g_fullReload = 0;
        return 0;

    case WM_APP_EP_READY: {
        EpReady *r = (EpReady*)lParam;
        if (r) {
            int k, epIdx = -1;
            /* 已被切歌/停止作废的任务：直接丢弃，不动当前状态 */
            if (r->gen != g_dlGen) {
                free(r->url);
                free(r);
                return 0;
            }
            g_curDlJob = NULL;
            g_downloading = 0;
            if (r->ok && r->url && g_playUrl && wcscmp(r->url, g_playUrl) == 0 &&
                r->pod >= 0 && r->pod < g_podCount) {
                /* 按 URL 在（可能已重排的）数组里找回当前位置 */
                for (k = 0; k < g_pods[r->pod].epCount; k++)
                    if (wcscmp(g_pods[r->pod].eps[k].url, r->url) == 0) { epIdx = k; break; }
            }
            if (epIdx >= 0) {
                wchar_t cache[MAX_PATH], info[128], st[512];
                long long pd = 0;
                Episode *e = &g_pods[r->pod].eps[epIdx];
                CachePathFor(e->url, cache, MAX_PATH);
                ProbeAudio(cache, info, 128, &pd);
                g_dur100ns = pd;
                SetWindowTextW(g_stFormat, info);
                /* 探测到时长：补进剧集列表 */
                if (pd > 0 && e->durationSec <= 0) {
                    e->durationSec = (int)(pd / 10000000);
                    {
                        wchar_t dur[16];
                        LVITEMW li;
                        FmtTime(e->durationSec, dur, 16);
                        ZeroMemory(&li, sizeof(li));
                        li.iItem = epIdx;
                        li.iSubItem = 2;
                        li.pszText = dur;
                        SendMessageW(g_listEp, LVM_SETITEMW, 0, (LPARAM)&li);
                    }
                }
                swprintf(st, 512, tr(TR_PLAYING_FMT), e->title);
                SetStatus(st);
                PlayerPlayFile(cache);
            } else {
                /* 下载失败或条目已消失：撤销绿色播放标记 */
                g_playPod = -1;
                g_playEp = -1;
                SetStatus(r->ok ? tr(TR_AUDIO_GONE) : tr(TR_DL_FAILED));
            }
            free(r->url);
            free(r);
        }
        RefreshCachedEntry();   /* 首次下载完成时让“已缓存”行出现/更新计数 */
        InvalidateRect(g_listEp, NULL, TRUE);   /* 下载完成 → 缓存标记变黄 */
        return 0;
    }

    case WM_APP_DL_PROGRESS: {
        wchar_t buf[64];
        if ((long)lParam != g_dlGen) return 0;   /* 旧任务进度，丢弃 */
        swprintf(buf, 64, tr(TR_DL_PROGRESS_FMT), (int)wParam);
        SetStatus(buf);
        return 0;
    }

    case WM_APP_MF_EVENT:
        switch ((MFP_EVENT_TYPE)wParam) {
        case MFP_EVENT_TYPE_PLAY:
            if (SUCCEEDED((HRESULT)lParam)) {
                long long d;
                g_playState = 1;
                InvalidateRect(g_btnPlay, NULL, TRUE);   /* 图标变成“暂停” */
                UpdateThumbbarPlayPause();
                d = PlayerGet100ns(1);
                if (d > 0) g_dur100ns = d;
                if (g_listPod) InvalidateRect(g_listPod, NULL, TRUE);   /* 标题变绿 */
                if (g_listEp) InvalidateRect(g_listEp, NULL, TRUE);    /* 剧集变绿 */
            } else {
                /* 如无可用音频输出设备（0xC00D11BA）：明确提示，绿色标记撤销 */
                g_playState = 0;
                g_playPod = -1;
                g_playEp = -1;
                SetStatus(tr(TR_NO_AUDIO_DEV));
                InvalidateRect(g_btnPlay, NULL, TRUE);
                if (g_listEp) InvalidateRect(g_listEp, NULL, TRUE);
            }
            break;
        case MFP_EVENT_TYPE_PAUSE:
            if (SUCCEEDED((HRESULT)lParam)) {
                g_playState = 2;
                InvalidateRect(g_btnPlay, NULL, TRUE);   /* 图标变回“播放” */
                UpdateThumbbarPlayPause();
                InvalidateRect(g_listPod, NULL, TRUE);
                InvalidateRect(g_listEp, NULL, TRUE);
            }
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            if (g_playMode == 2) {
                /* 单曲循环：重新播放当前集 */
                if (g_playPod >= 0 && g_playEp >= 0)
                    RequestPlayEpisode(g_playPod, g_playEp);
            } else if (g_playMode == 3) {
                /* 一次性播放：播放完毕即停止 */
                PlayerStop();
                SetStatus(tr(TR_FINISHED));
            } else if (g_playPod >= 0 && g_playEp >= 0) {
                int epN = (g_playPod == g_podCount)
                    ? g_cachePod.epCount : g_pods[g_playPod].epCount;
                if (epN > 1) PlayAdjacent(+1);   /* 顺序/随机：自动下一曲 */
                else { PlayerStop(); SetStatus(tr(TR_FINISHED)); }
            } else {
                PlayerStop();
                SetStatus(tr(TR_FINISHED));
            }
            InvalidateRect(g_listPod, NULL, TRUE);
            InvalidateRect(g_listEp, NULL, TRUE);
            break;
        case MFP_EVENT_TYPE_ERROR:
            SetStatus(tr(TR_PLAY_ERROR));
            break;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        KillTimer(hwnd, 4);
        SaveConfig();   /* 退出时统一写盘：排序/列宽/音量/字号/窗口尺寸/订阅顺序 */
        if (g_player) { g_player->lpVtbl->Release(g_player); g_player = NULL; }
        if (g_taskbar) { g_taskbar->lpVtbl->Release(g_taskbar); g_taskbar = NULL; }
        {
            int i;
            for (i = 0; i < 5; i++)
                if (g_tbIco[i]) { DestroyIcon(g_tbIco[i]); g_tbIco[i] = NULL; }
        }

        FreePodcasts();
        FreeCachePodcast();
        free(g_playUrl);
        if (g_whiteBrush) DeleteObject(g_whiteBrush);
        if (g_bgBrush) DeleteObject(g_bgBrush);
        if (g_font) DeleteObject(g_font);
        if (g_fontBold) DeleteObject(g_fontBold);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        MFShutdown();
        CoUninitialize();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ================= 入口 ================= */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow)
{
    const wchar_t *CLASS_NAME = L"LiteTuneWnd";
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev; (void)lpCmdLine;

    /* 单实例：重复启动时把已运行的窗口带到前台后退出。
     * 双开会各自在退出时写 feeds.ini 互相覆盖，丢失订阅/设置 */
    {
        HANDLE mu = CreateMutexW(NULL, FALSE, L"Local\\LiteTune_SingleInstance");
        if (mu && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND prev = FindWindowW(CLASS_NAME, NULL);
            if (prev) {
                ShowWindow(prev, SW_RESTORE);
                SetForegroundWindow(prev);
            }
            CloseHandle(mu);
            return 0;
        }
        /* 首个实例持有句柄直到进程退出，以维持单实例标志 */
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    {
        GdiplusStartupInput gsi;
        gsi.GdiplusVersion = 1;
        gsi.DebugEventCallback = NULL;
        gsi.SuppressBackgroundThread = FALSE;
        gsi.SuppressExternalCodecs = FALSE;
        GdiplusStartup(&g_gdipToken, &gsi, NULL);
    }

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);
    srand((unsigned)GetTickCount());

    /* 建窗前读取配置：排序/列宽/音量/窗口尺寸 */
    LoadConfig();
    EnsureCacheDir();   /* 配置里可能有自定义缓存目录，先确保存在 */

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    if (g_darkMode) {
        g_bgBrush = CreateSolidBrush(ThWinBg());
        wc.hbrBackground = g_bgBrush;
    } else {
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    }
    if (!RegisterClassExW(&wc)) return 1;

    /* 注册任务栏缩略图工具栏消息（Windows 7+） */
    g_wmTaskbarBtnCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");

    /* 保存的是客户区尺寸，换算成含边框标题栏的窗口外尺寸 */
    {
        RECT wr = { 0, 0, g_winW, g_winH };
        AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        hwnd = CreateWindowExW(0, CLASS_NAME, L"LiteTune",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, hInst, NULL);
    }
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetStatus(tr(TR_READY));   /* 初始状态文本跟随语言（静态初值固定为中文） */
    ApplyTheme();    /* 若 ini 里 dark=1：暗色标题栏/表头/滚动条一次到位 */
    ReloadFeeds();
    /* 离线启动时订阅可能加载很慢/失败：先立即挂上“已缓存”行，不等订阅 */
    RefreshCachedEntry();
    if (g_podCount == 0 && g_cacheRowHere) {
        SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
        SelectPodcast(g_podCount);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* ESC 退出（叉叉也是同一条退出路径，都会触发 WM_DESTROY 保存）。
         * 仅在焦点属于主窗口时生效：添加订阅弹窗打开时 ESC 只关弹窗 */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd))) {
            DestroyWindow(hwnd);
            continue;
        }
        /* 空格键暂停/恢复（仅当主窗口或其子控件有焦点时） */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE) {
            HWND focus = GetFocus();
            if (focus == hwnd || IsChild(hwnd, focus)) {
                SendMessageW(hwnd, WM_COMMAND,
                    MAKEWPARAM(IDC_BTN_PLAY, BN_CLICKED), (LPARAM)g_btnPlay);
                continue;
            }
        }
        /* tooltip 采用 TTF_SUBCLASS 自行拦截鼠标消息，无需在此转发 */
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_gdipToken) GdiplusShutdown(g_gdipToken);
    return (int)msg.wParam;
}

