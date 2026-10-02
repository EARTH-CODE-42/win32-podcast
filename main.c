/*
 * LightPodcast - 纯 C / Win32 播客播放器
 *
 * 功能：
 *   - 从同目录 feeds.txt 读取 RSS 订阅链接（一行一个，# 开头为注释）
 *   - 后台线程下载并解析 RSS 2.0（含 itunes 扩展）
 *   - 音频先下载到同目录 cache\ 缓存，再用 Media Foundation 播放
 *   - 显示码率 / 采样率 / 声道 / 格式 / 进度，支持 seek、暂停、音量
 *
 * 编译（MinGW）:
 *   windres resource.rc -O coff -o resource.o
 *   gcc -O2 -s -mwindows -municode -o lightpodcast.exe main.c resource.o \
 *       -lcomctl32 -lwinhttp -lmf -lmfplat -lmfplay -lmfreadwrite -lmfuuid -lole32 -luuid
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
#include <shellapi.h>
#include <winhttp.h>
#include <propidl.h>
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
static HWND     g_hwndTip = NULL;        /* 系统 tooltip：按钮 + 剧集条目 */
static HWND     g_hwndPodTip = NULL;     /* 自定义 tooltip：播客封面+标题+作者 */
static int      g_tipPodIdx = -1;        /* 当前播客提示对应的条目下标 */

/* 顶栏状态文本（自绘跑马灯） */
static wchar_t  g_statusText[512] = L"就绪";
static int      g_mqOffset = 0;       /* 滚动偏移（像素） */
static int      g_mqTextW = 0;        /* 状态文本像素宽，-1=需重测 */
static int      g_mqActive = 0;       /* 文本超长，正在滚动 */

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

static ULONG_PTR g_gdipToken = 0;

static volatile int g_feedsLoading = 0;
static volatile int g_downloading  = 0;
static int      g_fullReload = 0;    /* 本次加载是否为全量刷新（区别于单条添加） */

/* 播放 */
static IMFPMediaPlayer *g_player = NULL;
static int  g_playState = 0;          /* 0=停止 1=播放 2=暂停 */
static int  g_seeking   = 0;
static int  g_playPod = -1;
static wchar_t *g_playUrl = NULL;   /* 当前请求播放的音频 URL */
static long long g_dur100ns = 0;

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

static void SetStatus(const wchar_t *text)
{
    if (!text) text = L"";
    wcsncpy(g_statusText, text, 511);
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

    hSes = WinHttpOpen(L"LightPodcast/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return NULL;
    WinHttpSetTimeouts(hSes, 15000, 15000, 30000, 30000);

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
    if (!pod->title) pod->title = _wcsdup(L"(无标题)");
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
            if (!e->title) e->title = _wcsdup(L"(无标题)");
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
/* 每个缓存音频旁边放一个同名 .nfo（UTF-8 文本），记录播客名/标题/时长等，
 * 这样断网、无订阅时也能列出并播放本地缓存。字段：
 * pod= 播客名  title= 标题  url= 源地址  dur= 秒  date= 日期文本  key= 日期键 */

/* 把路径扩展名替换成 nfo（缓冲区与源路径同长） */
static void ReplaceExtNfo(const wchar_t *src, wchar_t *out, int outMax)
{
    int len;
    wcsncpy(out, src, outMax - 1);
    out[outMax - 1] = 0;
    len = (int)wcslen(out);
    if (len >= 4 && out[len - 4] == L'.')
        wcscpy_s(out + len - 3, outMax - (len - 3), L"nfo");
}

/* 下载/探测成功后写元数据。durSec<=0 时保留剧集自带时长 */
static void WriteEpisodeMeta(const wchar_t *podTitle, Episode *e, int durSec)
{
    wchar_t mp[MAX_PATH], nfo[MAX_PATH];
    FILE *f;
    char *cp, *ct, *cu, *cd;
    int dur;
    if (!e || !e->url) return;
    CachePathFor(e->url, mp, MAX_PATH);
    ReplaceExtNfo(mp, nfo, MAX_PATH);
    f = _wfopen(nfo, L"wb");
    if (!f) return;
    cp = WToU8(podTitle ? podTitle : L"");
    ct = WToU8(e->title ? e->title : L"");
    cu = WToU8(e->url ? e->url : L"");
    cd = WToU8(e->dateText[0] ? e->dateText : L"");
    dur = durSec > 0 ? durSec : e->durationSec;
    /* 标题/名称里的换行由解析端按单行处理，这里也替换成空格避免破坏格式 */
    if (cp) { char *q; for (q = cp; *q; q++) if (*q == '\r' || *q == '\n') *q = ' '; }
    if (ct) { char *q; for (q = ct; *q; q++) if (*q == '\r' || *q == '\n') *q = ' '; }
    if (cd) { char *q; for (q = cd; *q; q++) if (*q == '\r' || *q == '\n') *q = ' '; }
    fprintf(f, "pod=%s\ntitle=%s\nurl=%s\ndur=%d\ndate=%s\nkey=%lld\n",
            cp ? cp : "", ct ? ct : "", cu ? cu : "", dur,
            cd ? cd : "", (long long)e->dateKey);
    fclose(f);
    free(cp); free(ct); free(cu); free(cd);
}

/* 从 .nfo 读一个字段（值无换行）；找到则返回新 malloc 的宽字符串，否则 NULL */
static wchar_t* NfoField(const wchar_t *nfoPath, const char *key)
{
    FILE *f = _wfopen(nfoPath, L"rb");
    char line[2048];
    int klen = (int)strlen(key);
    wchar_t *result = NULL;
    if (!f) return NULL;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *v = line + klen + 1;
            int n = (int)strlen(v);
            while (n > 0 && (v[n-1] == '\r' || v[n-1] == '\n')) v[--n] = 0;
            result = U8ToW(v);
            break;
        }
    }
    fclose(f);
    return result;
}

static void FreeCachePodcast(void)
{
    FreePodcast(&g_cachePod);
    ZeroMemory(&g_cachePod, sizeof(g_cachePod));
}

/* 扫描缓存目录，用 mp3+nfo 重建虚拟播客 */
static void LoadCachePodcast(void)
{
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    g_cachePod.title  = _wcsdup(L"已缓存");
    g_cachePod.author = _wcsdup(L"本地缓存音频，断网也可播放");

    CacheDir(dir, MAX_PATH);
    swprintf(pat, MAX_PATH, L"%s*.mp3", dir);
    hFind = FindFirstFileW(pat, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        Episode *e;
        wchar_t mp[MAX_PATH], nfo[MAX_PATH], base[MAX_PATH];
        wchar_t *v;
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

        /* 默认标题：去掉扩展名的文件名（哈希名），有 nfo 再覆盖 */
        wcsncpy(base, fd.cFileName, MAX_PATH - 1);
        base[MAX_PATH - 1] = 0;
        dot = (int)wcslen(base);
        if (dot >= 4) base[dot - 4] = 0;
        e->title  = _wcsdup(base);
        e->author = _wcsdup(L"未知播客");

        ReplaceExtNfo(mp, nfo, MAX_PATH);
        if (GetFileAttributesW(nfo) != INVALID_FILE_ATTRIBUTES) {
            v = NfoField(nfo, "pod");
            if (v) { free(e->author); e->author = v; }
            v = NfoField(nfo, "title");
            if (v) { free(e->title); e->title = v; }
            v = NfoField(nfo, "date");
            if (v && *v) { wcsncpy(e->dateText, v, 23); e->dateText[23] = 0; free(v); }
            else if (v) free(v);
            v = NfoField(nfo, "dur");
            if (v) { e->durationSec = _wtoi(v); free(v); }
            v = NfoField(nfo, "key");
            if (v) { e->dateKey = _wtoi64(v); free(v); }
        }
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
        swprintf(row, 64, L"已缓存 (%d)", g_cacheFileCount);
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
        swprintf(row, 64, L"已缓存 (%d)", g_cacheFileCount);
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
/* startup=1：启动时完整读取（含排序/列宽/音量/窗口）；
 * startup=0：点"刷新"时只重读订阅列表，保留运行中的界面设置 */
static void LoadConfig(int startup)
{
    wchar_t iniPath[MAX_PATH], txtPath[MAX_PATH], usePath[MAX_PATH];
    FILE *f;
    char line[MAX_FEED_LINE];
    int i;

    for (i = 0; i < g_feedUrlCount; i++) free(g_feedUrls[i]);
    free(g_feedUrls);
    g_feedUrls = NULL; g_feedUrlCount = 0;
    if (startup) {
        g_sortCol = SORT_DATE; g_sortDir = -1;
        g_w1perm = 323; g_w2perm = 383;
        g_volume = 80; g_winW = 480; g_winH = 360;
        g_colW[0] = 110; g_colW[1] = 88; g_colW[2] = 44;
        g_fontLevel = 0;
        g_cfgFromTxt = 0;
    }

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
            if (startup) {
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
                else if (strcmp(s, "cachedir") == 0) {
                    wchar_t *w = U8ToW(val);
                    if (w) { wcsncpy(g_cacheDir, w, MAX_PATH - 1); g_cacheDir[MAX_PATH - 1] = 0; free(w); }
                }
            }
        } else if (strstr(s, "://")) {
            AddFeedUrl(s);   /* [feeds] 段或旧 txt 的裸 URL */
        }
    }
    fclose(f);

    if (startup) {
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

    fprintf(f, "; LightPodcast config\n");
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
    SetStatus(L"正在加载订阅...");
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
    const wchar_t *fmtName = L"音频";
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
        const wchar_t *chName = ch == 1 ? L"单声" : (ch == 2 ? L"立体" : L"多声");
        if (kbps > 0)
            swprintf(out, outMax, L"%s|%dkbps|%uHz|%s", fmtName, kbps, rate, chName);
        else
            swprintf(out, outMax, L"%s|%uHz|%s", fmtName, rate, chName);
    } else {
        wcscpy_s(out, outMax, fmtName);
    }
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
    if (FAILED(hr) || !g_player) { SetStatus(L"无法打开音频文件"); return; }
    g_player->lpVtbl->SetVolume(g_player,
        (float)SendMessageW(g_sldVol, TBM_GETPOS, 0, 0) / 100.0f);
    g_player->lpVtbl->SetMute(g_player, g_muted ? TRUE : FALSE);
}

static void PlayerStop(void)
{
    if (g_player) g_player->lpVtbl->Stop(g_player);
    g_playState = 0;
    g_dur100ns = 0;
    if (g_btnPlay) InvalidateRect(g_btnPlay, NULL, TRUE);   /* 图标回到“播放” */
    SetWindowTextW(g_stTime, L"00:00 / 00:00");
    SendMessageW(g_sldSeek, TBM_SETPOS, TRUE, 0);
    if (g_stFormat) SetWindowTextW(g_stFormat, L"");
    if (g_listEp) InvalidateRect(g_listEp, NULL, TRUE);      /* 剧集颜色复位 */
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
    static const wchar_t *titlesNormal[3] = { L"标题", L"日期", L"时长" };
    static const wchar_t *titlesCache[3]  = { L"播客", L"标题", L"时长" };
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
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"标题", p->title);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"描述", p->desc);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"作者", p->author);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"语言", p->lang);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"主页", p->link);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"版权", p->copyright);
    if (p->feedUrl) {
        feedW = U8ToW(p->feedUrl);
        if (feedW) {
            ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"订阅地址", feedW);
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
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"标题", e->title);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"描述", e->desc);
    if (g_viewCached)
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"播客", e->author);
    else
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"作者", e->author);
    if (e->dateKey) ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"发布", e->dateText);
    if (e->durationSec > 0) {
        FmtTime(e->durationSec, dur, 16);
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"时长", dur);
    }
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos,
                    e->localFile ? L"本地文件" : L"链接", e->url);
    SetWindowTextW(g_editDesc, buf);
}

/* “已缓存”虚拟行被选中：扫描缓存目录，换成 播客/标题/时长 列 */
static void SelectCachedView(void)
{
    static wchar_t buf[256];
    g_viewCached = 1;
    g_curPod = g_podCount;
    FreeCachePodcast();
    LoadCachePodcast();
    SetEpisodeColumns(1);
    FillEpisodeList();
    swprintf(buf, 256,
        L"【已缓存】\r\n本地缓存音频共 %d 个，无需联网即可播放。\r\n\r\n"
        L"缓存内容来自各播客已播放（自动下载）的音频；更改缓存目录可通过右上角文件夹按钮。",
        g_cachePod.epCount);
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
    g_curPod = g_podCount;
    g_curEp = ep;
    free(g_playUrl);
    g_playUrl = _wcsdup(e->url);

    ZeroMemory(&lvi, sizeof(lvi));
    lvi.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    lvi.state = 0;
    SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lvi);
    lvi.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)ep, (LPARAM)&lvi);
    SendMessageW(g_listEp, LVM_ENSUREVISIBLE, (WPARAM)ep, FALSE);

    ShowEpisodeDetail(g_podCount, ep);
    ProbeAudio(e->url, info, 128, &pd);
    g_dur100ns = pd;
    SetWindowTextW(g_stFormat, info);
    swprintf(st, 512, L"正在播放（离线）：%s", e->title ? e->title : L"");
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
    g_curPod = pod;
    g_curEp = ep;
    free(g_playUrl);
    g_playUrl = _wcsdup(e->url);
    /* 同步剧集列表选中行（上一曲/下一曲时也要跟着走） */
    {
        LVITEMW lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        lvi.state = 0;
        SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lvi);
        lvi.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_listEp, LVM_SETITEMSTATE, (WPARAM)ep, (LPARAM)&lvi);
        SendMessageW(g_listEp, LVM_ENSUREVISIBLE, (WPARAM)ep, FALSE);
    }
    ShowEpisodeDetail(pod, ep);
    SetStatus(L"准备音频...");

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
        SetStatus(L"下载线程创建失败");
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
}

/* 上一曲/下一曲：按当前剧集列表顺序，越界首尾循环；随机模式下取随机项 */
static void PlayAdjacent(int delta)
{
    int n, target;
    if (g_curPod < 0) return;
    n = (g_curPod == g_podCount) ? g_cachePod.epCount : g_pods[g_curPod].epCount;
    if (n <= 0) return;
    if (g_playMode == 1 && n > 1) {
        do { target = rand() % n; } while (target == g_curEp);
    } else {
        target = (g_curEp < 0) ? 0 : g_curEp + delta;
        if (target < 0) target = n - 1;
        if (target >= n) target = 0;
    }
    RequestPlayEpisode(g_curPod, target);
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
    SetStatus(L"正在加载订阅...");
    LoadConfig(0);   /* 只重读订阅列表，界面设置保持当前值 */
    g_feedsLoading = 1;
    g_fullReload = 1;
    CreateThread(NULL, 0, FeedLoaderThread, NULL, 0, NULL);
}

/* ================= 播客订阅管理（增删/排序） ================= */
/* 把一条 URL 追加到 feeds.ini 的 [feeds] 末尾（文件不存在则建最小骨架）。
 * 只有"添加订阅"时立即写盘；删除/排序仍在退出时由 SaveConfig 统一保存。 */
static void AppendFeedToIni(const char *url)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    int needNl = 0;
    ExeDir(path, MAX_PATH);
    wcscat_s(path, MAX_PATH, L"feeds.ini");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        f = _wfsopen(path, L"wb", _SH_DENYNO);
        if (!f) return;
        fprintf(f, "; LightPodcast config\n[settings]\n\n[feeds]\n");
    } else {
        f = _wfsopen(path, L"ab", _SH_DENYNO);
        if (!f) return;
        /* 手工编辑过的文件末尾可能没换行，先补一个避免粘行 */
        if (fseek(f, -1, SEEK_END) == 0) {
            int c = fgetc(f);
            if (c != '\n') needNl = 1;
        }
    }
    if (needNl) fputc('\n', f);
    fprintf(f, "%s\n", url);
    fclose(f);
}

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
        lab = CreateWindowExW(0, L"STATIC", L"输入 RSS 订阅地址（http:// 或 https://）：",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 10, 356, 18, h, NULL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            212, 72, 72, 26, h, (HMENU)(INT_PTR)ID_IB_OK, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消",
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
                MessageBoxW(h, L"地址无效，请输入以 http:// 或 https:// 开头的链接",
                            L"LightPodcast", MB_ICONWARNING);
                return 0;
            }
            for (k = 0; k < g_feedUrlCount; k++) {
                wchar_t *w = U8ToW(g_feedUrls[k]);
                if (w) { if (_wcsicmp(w, s) == 0) dup = 1; free(w); }
                if (dup) break;
            }
            if (dup) {
                MessageBoxW(h, L"该订阅地址已存在", L"LightPodcast", MB_ICONINFORMATION);
                return 0;
            }
            if (g_feedUrlCount >= MAX_PODCASTS) {
                MessageBoxW(h, L"订阅数量已达上限", L"LightPodcast", MB_ICONWARNING);
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
        L"添加播客订阅",
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
            AddFeedUrl(u8);
            AppendFeedToIni(u8);
            AddSingleFeed(u8);   /* 只加载新增的这一条，不刷新全部 */
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
static WNDPROC g_oldSeekProc = NULL, g_oldVolProc = NULL;

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
 * 各窗口原始过程存在自己的 GWLP_USERDATA。
 * tooltip 的鼠标转发统一在 WinMain 消息循环做（TTM_RELAYEVENT） */
static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old;
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
    ICO_MUTE, ICO_MUTED, ICO_PLUS, ICO_MINUS, ICO_SORT, ICO_REFRESH,
    ICO_ORDER, ICO_SHUFFLE, ICO_REPEATONE, ICO_PLAYONCE, ICO_FOLDER
};

/* 在按钮矩形内绘制单个矢量图标 */
static void DrawGlyph(HDC hdc, const RECT *rc, int kind)
{
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int s = (w < h ? w : h) - 8;                 /* 24px 按钮 → 16px 图标 */
    int x0 = rc->left + (w - s) / 2;
    int y0 = rc->top + (h - s) / 2;
    int cy = y0 + s / 2;
    COLORREF col = RGB(120, 120, 120);
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
    case ICO_MINUS:
        SelectObject(hdc, pen3);
        MoveToEx(hdc, x0+2, cy, NULL); LineTo(hdc, x0+s-2, cy);
        break;
    case ICO_SORT: {
        int cx = x0 + s / 2;
        POINT up[3]   = { {cx-5,y0+3},{cx+5,y0+3},{cx,y0+9} };
        POINT down[3] = { {cx-5,y0+s-3},{cx+5,y0+s-3},{cx,y0+s-9} };
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
    case ICO_FOLDER: {
        /* 目录文件夹 */
        POINT pts[6] = {
            {x0+1,y0+5},{x0+5,y0+5},{x0+7,y0+3},{x0+s-2,y0+3},
            {x0+s-2,y0+s-2},{x0+1,y0+s-2} };
        Polygon(hdc, pts, 6);
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
    if (on) bg = CreateSolidBrush(RGB(180, 215, 255));
    else bg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
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
    case IDC_BTN_DEL:   *on = (g_podMode == 1); return ICO_MINUS;
    case IDC_BTN_SORT:  *on = (g_podMode == 2); return ICO_SORT;
    case IDC_BTN_REFRESH: return ICO_REFRESH;
    case IDC_BTN_PLAYMODE:
        return (g_playMode == 1) ? ICO_SHUFFLE :
               (g_playMode == 2) ? ICO_REPEATONE :
               (g_playMode == 3) ? ICO_PLAYONCE : ICO_ORDER;
    case IDC_BTN_DIR:   return ICO_FOLDER;
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

/* 把旧目录里所有缓存文件（.mp3 及同名 .nfo 元数据）移动到新目录 */
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

/* 目录按钮：弹出菜单，可打开当前缓存目录或更改缓存目录 */
static void OnDirButton(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int cmd;
    wchar_t curDir[MAX_PATH];

    AppendMenuW(menu, MF_STRING, 1, L"打开缓存目录");
    AppendMenuW(menu, MF_STRING, 2, L"更改缓存目录...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 3, L"恢复默认目录");

    GetCursorPos(&pt);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
    if (cmd == 0) return;

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
        bi.lpszTitle = L"选择缓存目录";
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
                swprintf(msg, 256, L"当前缓存目录有 %d 个音频文件，是否移动到新目录？", oldHasFiles);
                if (MessageBoxW(g_hwnd, msg, L"移动缓存", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    CreateDirectoryW(newPath, NULL);
                    MoveCacheFiles(curDir, newDir, L"mp3");
                    MoveCacheFiles(curDir, newDir, L"nfo");
                }
            }
            wcsncpy(g_cacheDir, newPath, MAX_PATH - 1);
            g_cacheDir[MAX_PATH - 1] = 0;
            EnsureCacheDir();
            SetStatus(L"缓存目录已更改");
        }
        if (pidl) CoTaskMemFree(pidl);
    } else if (cmd == 3) {
        g_cacheDir[0] = 0;
        EnsureCacheDir();
        SetStatus(L"缓存目录已恢复默认");
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
    return g_playMode == 1 ? L"随机播放" :
           g_playMode == 2 ? L"单曲循环" :
           g_playMode == 3 ? L"一次性播放" : L"顺序播放";
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
                    HBRUSH bk = CreateSolidBrush(RGB(0,0,0));
                    FillRect(hdc, &rci, bk);
                    DeleteObject(bk);
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchBlt(hdc, dx, dy, dw, dh, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                }
                SelectObject(mem, old);
                DeleteDC(mem);
            } else {
                HBRUSH bk = CreateSolidBrush(RGB(210,210,210));
                FillRect(hdc, &rci, bk);
                DeleteObject(bk);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_INFOTEXT));
            SelectObject(hdc, g_fontBold);
            DrawTextW(hdc, p->title ? p->title : L"(无标题)", -1, &rct,
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
    switch (msg) {
    case WM_CREATE: {
        DWORD lbStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP
                      | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS;

        g_font = CreateUiFont(8 + g_fontLevel, FW_NORMAL);
        g_fontBold = CreateUiFont(8 + g_fontLevel, FW_BOLD);
        g_fontSmall = CreateUiFont(7 + g_fontLevel, FW_NORMAL);
        g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));

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
            static const wchar_t *colTitles[3] = { L"标题", L"日期", L"时长" };
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
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SendMessageW(g_listEp, LVM_SETBKCOLOR, 0, (LPARAM)RGB(255,255,255));
            SendMessageW(g_listEp, LVM_SETTEXTBKCOLOR, 0, (LPARAM)RGB(255,255,255));
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

        /* 系统工具提示：按钮 + 剧集条目 */
        g_hwndTip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, NULL, GetModuleHandleW(NULL), NULL);
        if (g_hwndTip) {
            SetWindowPos(g_hwndTip, HWND_TOPMOST, 0,0,0,0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            AddButtonTip(g_btnPrev,   L"上一首");
            AddButtonTip(g_btnPlay,   L"播放 / 暂停");
            AddButtonTip(g_btnNext,   L"下一首");
            AddButtonTip(g_btnStop,   L"停止");
            AddButtonTip(g_btnMute,   L"静音");
            AddButtonTip(g_btnAdd,    L"添加播客");
            AddButtonTip(g_btnDel,    L"删除播客");
            AddButtonTip(g_btnSort,   L"调整顺序");
            AddButtonTip(g_btnRefresh,L"刷新");
            AddButtonTip(g_btnPlayMode, PlayModeTipText());
            AddButtonTip(g_btnDir,    L"缓存目录");
            /* 剧集列表：悬停时按需返回完整文本 */
            {
                TOOLINFOW ti; ZeroMemory(&ti, sizeof(ti));
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd = hwnd;
                ti.uId = (UINT_PTR)g_listEp;
                ti.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(g_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            }
        }

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
            HBRUSH bg = CreateSolidBrush(isSel ? RGB(204,232,255) : RGB(255,255,255));
            RECT rc = di->rcItem;
            int textLeft;

            FillRect(di->hDC, &rc, bg);
            DeleteObject(bg);

            /* 封面区：48x48，左留 2px、上留 3px。
             * 删除模式 → 大红叉按钮；排序模式 → 左右两个上/下箭头按钮
             * “已缓存”虚拟行不参与删除/排序，画一个下载缓存图标 */
            if (isCache) {
                RECT ib = { rc.left + 2, rc.top + 3, rc.left + 50, rc.top + 51 };
                HBRUSH fb = CreateSolidBrush(RGB(238,242,248));
                HPEN gp = CreatePen(PS_SOLID, 2, RGB(120,140,170));
                HBRUSH ab = CreateSolidBrush(RGB(120,140,170));
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
                HBRUSH fb = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
                HPEN dp = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
                HBRUSH db = CreateSolidBrush(RGB(120, 120, 120));
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
                    swprintf(title, 64, L"已缓存 (%d)", g_cacheFileCount);
                    tp = title;
                } else tp = p->title;
                SetTextColor(di->hDC, isPlaying ? RGB(0,140,60) : RGB(20,20,20));
                SelectObject(di->hDC, g_fontBold);
                DrawTextW(di->hDC, tp, -1, &rct,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            /* 副标题：copyright（空则 author），小字号灰色，标题下方；
             * 虚拟行固定提示离线播放 */
            {
                const wchar_t *sub = isCache
                    ? L"离线播放本地音频"
                    : (p->copyright ? p->copyright : p->author);
                if (sub && *sub) {
                    RECT rcs = rc;
                    rcs.left = textLeft;
                    rcs.right = rc.right - 28;
                    rcs.top += 26;
                    rcs.bottom = rcs.top + 24;
                    SetTextColor(di->hDC, RGB(110,110,110));
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
                SetTextColor(di->hDC, isSel ? RGB(40,40,40) : RGB(120,120,120));
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
            FillRect(mem, &(RECT){0, 0, sw, sh}, GetSysColorBrush(COLOR_BTNFACE));
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(0, 0, 0));
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

    /* 列表框 / 只读简介框统一白底黑字，与中间 ListView 一致 */
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        if ((HWND)lParam == g_listPod || (HWND)lParam == g_editDesc) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, RGB(255,255,255));
            SetTextColor(hdc, RGB(0,0,0));
            return (LRESULT)g_whiteBrush;
        }
        break;

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
                SetStatus(L"已停止");
            }
            else if (id == IDC_BTN_MUTE) ToggleMute();
            else if (id == IDC_BTN_ADD) AskAddFeed();
            else if (id == IDC_BTN_DEL) SetPodMode(1);
            else if (id == IDC_BTN_SORT) SetPodMode(2);
            else if (id == IDC_BTN_REFRESH) ReloadFeeds();
            else if (id == IDC_BTN_PLAYMODE) CyclePlayMode();
            else if (id == IDC_BTN_DIR) OnDirButton();
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR*)lParam;
        if (nm->code == TTN_GETDISPINFOW && nm->hwndFrom == g_hwndTip) {
            /* 剧集条目悬停提示：返回鼠标所在列的完整文本 */
            LPNMTTDISPINFOW nmtd = (LPNMTTDISPINFOW)lParam;
            POINT pt;
            LVHITTESTINFO ht;
            GetCursorPos(&pt);
            ScreenToClient(g_listEp, &pt);
            ht.pt = pt;
            ht.flags = 0;
            if (ListView_HitTest(g_listEp, &ht) >= 0 && ht.iItem >= 0 &&
                ht.iSubItem >= 0 && ht.iSubItem <= 2) {
                wchar_t txt[512];
                LVITEMW lvi;
                ZeroMemory(&lvi, sizeof(lvi));
                lvi.iItem = ht.iItem;
                lvi.iSubItem = ht.iSubItem;
                lvi.pszText = txt;
                lvi.cchTextMax = 512;
                if (ListView_GetItem(g_listEp, &lvi) && txt[0]) {
                    static wchar_t s_buf[512];
                    wcsncpy(s_buf, txt, 511); s_buf[511] = 0;
                    nmtd->lpszText = s_buf;
                    return 0;
                }
            }
            nmtd->lpszText = L"";
            return 0;
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
            } else if (nm->code == NM_CUSTOMDRAW) {
                /* 正在播放 → 绿；已缓存（非播放）→ 黄 */
                LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;   /* 必须，否则收不到 ITEMPREPAINT */
                } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int i = (int)cd->nmcd.dwItemSpec;
                    Podcast *p = ActivePodcast();
                    int playing = (g_playPod == g_curPod && g_curEp == i && g_playState == 1);
                    int cached = 0;
                    if (p && i >= 0 && i < p->epCount)
                        cached = p->eps[i].localFile || EpisodeIsCached(&p->eps[i]);
                    if (playing) {
                        /* 播放行同时是选中行（默认蓝底白字会盖住绿字），
                         * 自己铺一层浅绿底再用深绿字 */
                        RECT rr = cd->nmcd.rc;
                        HBRUSH bg = CreateSolidBrush(RGB(222, 244, 228));
                        FillRect(cd->nmcd.hdc, &rr, bg);
                        DeleteObject(bg);
                        cd->clrText = RGB(0, 130, 60);
                    } else if (cached) {
                        cd->clrText = RGB(190, 155, 0);
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
                        SetStatus(L"播放完毕");
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
            SetStatus(L"就绪");
            /* 单条添加时新条目已在 FEED_ADDED 中选中；全量刷新默认第一条。
             * 启动时为离线预置了“已缓存”选中，订阅加载成功后切到第一条 */
            if (cursel == LB_ERR || (g_fullReload && g_viewCached)) {
                SendMessageW(g_listPod, LB_SETCURSEL, 0, 0);
                SelectPodcast(0);
            }
        } else if (g_cacheRowHere) {
            SetStatus(L"没有订阅也能听：正在播放列表最后的「已缓存」，无需联网");
            /* 直接选中“已缓存”行，离线音频一目了然 */
            SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
            SelectPodcast(g_podCount);
        } else {
            SetStatus(L"还没有订阅，点左下角「+」按钮添加 RSS 播客");
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
                /* 探测到时长：补进剧集并写同名 nfo（供离线“已缓存”列表使用） */
                if (pd > 0) {
                    if (e->durationSec <= 0) {
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
                    WriteEpisodeMeta(g_pods[r->pod].title, e, (int)(pd / 10000000));
                } else {
                    WriteEpisodeMeta(g_pods[r->pod].title, e, e->durationSec);
                }
                swprintf(st, 512, L"正在播放：%s", e->title);
                SetStatus(st);
                PlayerPlayFile(cache);
            } else if (!r->ok) {
                SetStatus(L"音频下载失败");
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
        swprintf(buf, 64, L"下载音频中... %d%%", (int)wParam);
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
                d = PlayerGet100ns(1);
                if (d > 0) g_dur100ns = d;
                InvalidateRect(g_listPod, NULL, TRUE);   /* 标题变绿 */
                InvalidateRect(g_listEp, NULL, TRUE);    /* 剧集变绿 */
            } else {
                /* 如无可用音频输出设备（0xC00D11BA）：明确提示，避免状态一直停在“正在播放” */
                g_playState = 0;
                SetStatus(L"无法开始播放：未找到可用的音频输出设备");
                InvalidateRect(g_btnPlay, NULL, TRUE);
            }
            break;
        case MFP_EVENT_TYPE_PAUSE:
            if (SUCCEEDED((HRESULT)lParam)) {
                g_playState = 2;
                InvalidateRect(g_btnPlay, NULL, TRUE);   /* 图标变回“播放” */
                InvalidateRect(g_listPod, NULL, TRUE);
                InvalidateRect(g_listEp, NULL, TRUE);
            }
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            if (g_playMode == 2) {
                /* 单曲循环：重新播放当前集 */
                if (g_curPod >= 0 && g_curEp >= 0)
                    RequestPlayEpisode(g_curPod, g_curEp);
            } else if (g_playMode == 3) {
                /* 一次性播放：播放完毕即停止 */
                PlayerStop();
                SetStatus(L"播放完毕");
            } else if (g_curPod >= 0 && g_curEp >= 0) {
                int epN = (g_curPod == g_podCount)
                    ? g_cachePod.epCount : g_pods[g_curPod].epCount;
                if (epN > 1) PlayAdjacent(+1);   /* 顺序/随机：自动下一曲 */
                else { PlayerStop(); SetStatus(L"播放完毕"); }
            } else {
                PlayerStop();
                SetStatus(L"播放完毕");
            }
            InvalidateRect(g_listPod, NULL, TRUE);
            InvalidateRect(g_listEp, NULL, TRUE);
            break;
        case MFP_EVENT_TYPE_ERROR:
            SetStatus(L"播放出错");
            break;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        SaveConfig();   /* 退出时统一写盘：排序/列宽/音量/字号/窗口尺寸/订阅顺序 */
        if (g_player) { g_player->lpVtbl->Release(g_player); g_player = NULL; }
        FreePodcasts();
        FreeCachePodcast();
        free(g_playUrl);
        if (g_whiteBrush) DeleteObject(g_whiteBrush);
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
    const wchar_t *CLASS_NAME = L"LightPodcastWnd";
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev; (void)lpCmdLine;

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
    icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    srand((unsigned)GetTickCount());

    /* 建窗前读取配置：排序/列宽/音量/窗口尺寸 */
    LoadConfig(1);
    EnsureCacheDir();   /* 配置里可能有自定义缓存目录，先确保存在 */

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) return 1;

    /* 保存的是客户区尺寸，换算成含边框标题栏的窗口外尺寸 */
    {
        RECT wr = { 0, 0, g_winW, g_winH };
        AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        hwnd = CreateWindowExW(0, CLASS_NAME, L"LightPodcast",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, hInst, NULL);
    }
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    ReloadFeeds();
    /* 离线启动时订阅可能加载很慢/失败：先立即挂上“已缓存”行，不等订阅 */
    RefreshCachedEntry();
    if (g_podCount == 0 && g_cacheRowHere) {
        SendMessageW(g_listPod, LB_SETCURSEL, g_podCount, 0);
        SelectPodcast(g_podCount);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* ESC 退出（叉叉也是同一条退出路径，都会触发 WM_DESTROY 保存） */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            continue;
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
