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
#define UNICODE
#define _UNICODE
#define WINVER       0x0601
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
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
    wchar_t *url;        /* enclosure URL */
    wchar_t *author;     /* itunes:author */
    wchar_t  dateText[24];
    long long dateKey;   /* YYYYMMDDHHMM 分钟级，0=未知 */
    int      durationSec;
    int      origIdx;    /* RSS 原始顺序，排序 tiebreak */
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
    IDC_ST_STATUS = 100, IDC_ST_INFO,
    IDC_SLD_SEEK, IDC_ST_TIME,
    IDC_BTN_PLAY, IDC_BTN_STOP,
    IDC_ST_VOLLABEL, IDC_SLD_VOL, IDC_ST_VOL,
    IDC_LIST_POD, IDC_LIST_EP, IDC_EDIT_DESC,
    IDC_BTN_REFRESH
};

/* ================= 全局状态 ================= */
static Podcast  g_pods[MAX_PODCASTS];
static int      g_podCount = 0;
static int      g_curPod = -1, g_curEp = -1;

static HWND     g_hwnd;
static HFONT    g_font;          /* 9pt 常规 */
static HFONT    g_fontBold;      /* 10pt 加粗（播客标题） */
static HFONT    g_fontSmall;     /* 8pt 灰色小字（版权/副标题） */
static HBRUSH   g_whiteBrush = NULL;
static HWND     g_stStatus, g_stInfo, g_sldSeek, g_stTime;
static HWND     g_btnPlay, g_btnStop, g_stVolLabel, g_sldVol, g_stVol;
static HWND     g_listPod, g_listEp, g_editDesc;
static HWND     g_btnRefresh;

/* feeds.txt 中的订阅源（UTF-8）与排序/布局配置 */
static char   **g_feedUrls = NULL;
static int      g_feedUrlCount = 0;
static int      g_sortCol = SORT_DATE;   /* 默认按日期 */
static int      g_sortDir = -1;          /* -1=降序 1=升序 */
static int      g_w1perm = 260;          /* 播客列宽，千分比 */
static int      g_w2perm = 400;          /* 剧集列宽，千分比 */
static int      g_volume = 80;           /* 音量 0-100 */
static int      g_winW = 720, g_winH = 560;  /* 窗口尺寸 */
static int      g_colW[3] = { 300, 122, 64 };  /* 剧集列表三列像素宽 */
static int      g_cfgFromTxt = 0;        /* 配置来自旧 feeds.txt，保存时迁移 */

static ULONG_PTR g_gdipToken = 0;

static volatile int g_feedsLoading = 0;
static volatile int g_downloading  = 0;

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

static void SetFontAll(HWND *ctrls, int n)
{
    int i;
    for (i = 0; i < n; i++)
        SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_font, TRUE);
}

static void FmtTime(long long sec, wchar_t *buf, size_t n)
{
    if (sec < 0) sec = 0;
    if (sec >= 3600)
        swprintf(buf, n, L"%02d:%02d:%02d", (int)(sec/3600), (int)((sec%3600)/60), (int)(sec%60));
    else
        swprintf(buf, n, L"%02d:%02d", (int)(sec/60), (int)(sec%60));
}

static void SetStatus(const wchar_t *text)
{
    SetWindowTextW(g_stStatus, text);
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
static void CachePathFor(const wchar_t *url, wchar_t *out, int outMax)
{
    unsigned long h = 5381;
    const wchar_t *p = url;
    wchar_t dir[MAX_PATH];
    while (*p) h = ((h << 5) + h) ^ (unsigned long)*p++;
    ExeDir(dir, MAX_PATH);
    swprintf(out, outMax, L"%scache\\%08lx.mp3", dir, h);
}

static void EnsureCacheDir(void)
{
    wchar_t dir[MAX_PATH];
    ExeDir(dir, MAX_PATH);
    wcscat_s(dir, MAX_PATH, L"cache");
    CreateDirectoryW(dir, NULL);
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
        g_w1perm = 260; g_w2perm = 400;
        g_volume = 80; g_winW = 720; g_winH = 560;
        g_colW[0] = 300; g_colW[1] = 122; g_colW[2] = 64;
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
            }
        } else if (strstr(s, "://")) {
            AddFeedUrl(s);   /* [feeds] 段或旧 txt 的裸 URL */
        }
    }
    fclose(f);

    if (startup) {
        if (g_w1perm < 100 || g_w1perm > 800) g_w1perm = 260;
        if (g_w2perm < 100 || g_w2perm > 800) g_w2perm = 400;
        if (g_winW < 480) g_winW = 480;
        if (g_winH < 360) g_winH = 360;
        if (g_winW > 4000) g_winW = 4000;
        if (g_winH > 2400) g_winH = 2400;
        {
            int ci;
            for (ci = 0; ci < 3; ci++)
                if (g_colW[ci] < 30 || g_colW[ci] > 2000)
                    g_colW[ci] = (ci == 0) ? 300 : (ci == 1 ? 122 : 64);
        }
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
    fprintf(f, "winw=%d\n", g_winW);
    fprintf(f, "winh=%d\n\n", g_winH);
    fprintf(f, "[feeds]\n");
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

/* 下载封面图并缩放到 50x50 HBITMAP（GDI+ 内存流解码，不写磁盘） */
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
    GpBitmap *thumb = NULL;
    GpGraphics *gfx = NULL;

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
        GdipCreateBitmapFromScan0(50, 50, 0, PixelFormat32bppARGB, NULL, &thumb);
        if (thumb) {
            GdipGetImageGraphicsContext((GpImage*)thumb, &gfx);
            if (gfx) {
                GdipSetInterpolationMode(gfx, InterpolationModeHighQualityBicubic);
                GdipDrawImageRect((GpGraphics*)gfx, (GpImage*)bmp, 0, 0, 50, 50);
                GdipDeleteGraphics(gfx);
            }
            GdipCreateHBITMAPFromBitmap(thumb, &hbmp, 0);
            GdipDisposeImage((GpImage*)thumb);
        }
        GdipDisposeImage((GpImage*)bmp);
    }
    stm->lpVtbl->Release(stm);
    return hbmp;
}

/* ================= 后台线程：加载 feeds ================= */
static DWORD WINAPI FeedLoaderThread(LPVOID param)
{
    int i, loaded = 0;
    (void)param;

    for (i = 0; i < g_feedUrlCount; i++) {
        wchar_t *wurl;
        DWORD xmlLen = 0;
        char *xml;
        Podcast *pod;

        wurl = U8ToW(g_feedUrls[i]);
        if (!wurl) continue;
        xml = HttpFetch(wurl, &xmlLen, 0, NULL, NULL);
        free(wurl);
        if (!xml) continue;

        pod = (Podcast*)calloc(1, sizeof(Podcast));
        if (pod && ParseFeed(xml, pod)) {
            SortPodcast(pod);
            pod->feedUrl = _strdup(g_feedUrls[i]);
            if (pod->imageUrl) pod->hImage = LoadCoverImage(pod->imageUrl);
            PostMessageW(g_hwnd, WM_APP_FEED_ADDED, 0, (LPARAM)pod);
            loaded++;
        } else if (pod) {
            FreePodcast(pod);
            free(pod);
        }
        free(xml);
    }
    PostMessageW(g_hwnd, WM_APP_FEEDS_DONE, loaded, 0);
    return 0;
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
        const wchar_t *chName = ch == 1 ? L"单声道" : (ch == 2 ? L"立体声" : L"多声道");
        if (kbps > 0)
            swprintf(out, outMax, L"%s · %u Hz · %d kbps · %s", fmtName, rate, kbps, chName);
        else
            swprintf(out, outMax, L"%s · %u Hz · %s", fmtName, rate, chName);
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
}

static void PlayerStop(void)
{
    if (g_player) g_player->lpVtbl->Stop(g_player);
    g_playState = 0;
    g_dur100ns = 0;
    SetWindowTextW(g_btnPlay, L"播放");
    SendMessageW(g_sldSeek, TBM_SETPOS, TRUE, 0);
    SetWindowTextW(g_stTime, L"00:00 / 00:00");
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
/* 用当前播客的剧集填充 ListView（调用前数组应已排序） */
static void FillEpisodeList(void)
{
    Podcast *p = &g_pods[g_curPod];
    int i;
    SendMessageW(g_listEp, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < p->epCount; i++) {
        wchar_t dur[16];
        LVITEMW lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.pszText = p->eps[i].title;
        SendMessageW(g_listEp, LVM_INSERTITEMW, 0, (LPARAM)&lvi);

        lvi.iSubItem = 1;
        lvi.pszText = p->eps[i].dateKey ? p->eps[i].dateText : L"";
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
    Episode *e;
    wchar_t dur[16];
    if (podIdx < 0 || podIdx >= g_podCount) return;
    if (epIdx < 0 || epIdx >= g_pods[podIdx].epCount) return;
    e = &g_pods[podIdx].eps[epIdx];
    buf[0] = 0;
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"标题", e->title);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"描述", e->desc);
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"作者", e->author);
    if (e->dateKey) ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"发布", e->dateText);
    if (e->durationSec > 0) {
        FmtTime(e->durationSec, dur, 16);
        ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"时长", dur);
    }
    ShowDetailField(buf, sizeof(buf)/sizeof(buf[0]), &pos, L"链接", e->url);
    SetWindowTextW(g_editDesc, buf);
}

static void SelectPodcast(int idx)
{
    if (idx < 0 || idx >= g_podCount) return;
    g_curPod = idx;
    SortPodcast(&g_pods[idx]);
    FillEpisodeList();
    ShowPodcastDetail(idx);
}

static void SelectEpisode(int idx)
{
    if (g_curPod < 0 || idx < 0 || idx >= g_pods[g_curPod].epCount) return;
    g_curEp = idx;
    ShowEpisodeDetail(g_curPod, idx);
}

/* 点击列头：同列切换方向，换列用默认方向（日期/时长降序，标题升序） */
static void ChangeSort(int col)
{
    int oldOrig = -1, i, sel;
    if (g_sortCol == col) g_sortDir = -g_sortDir;
    else {
        g_sortCol = col;
        g_sortDir = (col == SORT_TITLE) ? 1 : -1;
    }
    /* 排序只更新内存，退出时统一保存 */

    if (g_curPod < 0) return;
    sel = (int)SendMessageW(g_listEp, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel >= 0) oldOrig = g_pods[g_curPod].eps[sel].origIdx;

    SortPodcast(&g_pods[g_curPod]);
    FillEpisodeList();

    for (i = 0; i < g_pods[g_curPod].epCount; i++) {
        if (g_pods[g_curPod].eps[i].origIdx == oldOrig) {
            SendMessageW(g_listEp, LVM_ENSUREVISIBLE, i, FALSE);
            SendMessageW(g_listEp, LVM_SETITEMSTATE, i,
                (LPARAM)&(LVITEMW){ .state = LVIS_SELECTED|LVIS_FOCUSED,
                                    .stateMask = LVIS_SELECTED|LVIS_FOCUSED });
            break;
        }
    }
}

static void RequestPlayEpisode(int pod, int ep)
{
    DlJob *job;
    Episode *e;
    long myGen;
    if (pod < 0 || pod >= g_podCount || ep < 0 || ep >= g_pods[pod].epCount) return;
    e = &g_pods[pod].eps[ep];

    /* 改主意了：立即中止旧下载、停止当前播放，转入新任务 */
    if (g_downloading) AbortDownloadConnection();
    PlayerStop();

    g_playPod = pod;
    free(g_playUrl);
    g_playUrl = _wcsdup(e->url);
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
    FreePodcasts();
    SendMessageW(g_listPod, LB_RESETCONTENT, 0, 0);
    SendMessageW(g_listEp, LVM_DELETEALLITEMS, 0, 0);
    SetWindowTextW(g_editDesc, L"");
    SetStatus(L"正在加载订阅...");
    LoadConfig(0);   /* 只重读订阅列表，界面设置保持当前值 */
    g_feedsLoading = 1;
    CreateThread(NULL, 0, FeedLoaderThread, NULL, 0, NULL);
}

/* ================= 滑块滚轮修正 ================= */
/* trackbar 默认滚轮方向是反的（向上滚反而减小），子类化后自己处理 */
static WNDPROC g_oldSeekProc = NULL, g_oldVolProc = NULL;

static LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (hwnd == g_sldSeek) ? g_oldSeekProc : g_oldVolProc;
    if (msg == WM_MOUSEWHEEL) {
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
    if (y < by || y > by + bh) return -1;
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
    int by, bh, totalW, w1, w2, w3, x, gx, gw, gy;

    GetClientRect(hwnd, &rc);
    W = rc.right; H = rc.bottom;

    /* 顶部播放区：边框由 WM_PAINT 绘制，控件向内缩 */
    gx = MARGIN + 8;
    gw = W - 2*MARGIN - 16;
    gy = MARGIN + 6;

    MoveWindow(g_stStatus, gx, gy, gw - 220, 18, TRUE);
    MoveWindow(g_stInfo,   gx + gw - 215, gy, 215, 18, TRUE);
    gy += 22;
    MoveWindow(g_sldSeek, gx, gy, gw - 112, 22, TRUE);
    MoveWindow(g_stTime,  gx + gw - 104, gy + 2, 104, 18, TRUE);
    gy += 28;
    MoveWindow(g_btnPlay,    gx,       gy, 56, 24, TRUE);
    MoveWindow(g_btnStop,    gx + 62,  gy, 56, 24, TRUE);
    MoveWindow(g_stVolLabel, gx + 134, gy + 4, 32, 16, TRUE);
    MoveWindow(g_sldVol,     gx + 168, gy, 120, 24, TRUE);
    MoveWindow(g_stVol,      gx + 294, gy + 4, 44, 16, TRUE);
    MoveWindow(g_btnRefresh, gx + 348, gy, 64, 24, TRUE);

    by = MARGIN + TOP_H + GAP_W;
    bh = H - by - MARGIN;
    if (bh < 40) bh = 40;
    totalW = W - 2*MARGIN - 2*GAP_W;
    w1 = totalW * g_w1perm / 1000;
    w2 = totalW * g_w2perm / 1000;
    w3 = totalW - w1 - w2;

    x = MARGIN;
    MoveWindow(g_listPod,  x, by, w1, bh, TRUE);
    x += w1 + GAP_W;
    MoveWindow(g_listEp,   x, by, w2, bh, TRUE);
    x += w2 + GAP_W;
    MoveWindow(g_editDesc, x, by, w3, bh, TRUE);
}

/* ================= 窗口过程 ================= */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        DWORD lbStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP
                      | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS;

        g_font = CreateUiFont(9, FW_NORMAL);
        g_fontBold = CreateUiFont(10, FW_BOLD);
        g_fontSmall = CreateUiFont(8, FW_NORMAL);
        g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));

        g_stStatus = CreateWindowExW(0, L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            0,0,0,0, hwnd, (HMENU)IDC_ST_STATUS, NULL, NULL);
        g_stInfo = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0,0,0,0, hwnd, (HMENU)IDC_ST_INFO, NULL, NULL);
        g_sldSeek = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0,0,0,0, hwnd, (HMENU)IDC_SLD_SEEK, NULL, NULL);
        SendMessageW(g_sldSeek, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        g_stTime = CreateWindowExW(0, L"STATIC", L"00:00 / 00:00",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0,0,0,0, hwnd, (HMENU)IDC_ST_TIME, NULL, NULL);
        g_btnPlay = CreateWindowExW(0, L"BUTTON", L"播放",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_PLAY, NULL, NULL);
        g_btnStop = CreateWindowExW(0, L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_STOP, NULL, NULL);
        g_stVolLabel = CreateWindowExW(0, L"STATIC", L"音量",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,0,0,0, hwnd, (HMENU)IDC_ST_VOLLABEL, NULL, NULL);
        g_sldVol = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0,0,0,0, hwnd, (HMENU)IDC_SLD_VOL, NULL, NULL);
        SendMessageW(g_sldVol, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(g_sldVol, TBM_SETPOS, TRUE, g_volume);
        {
            wchar_t vt[8];
            swprintf(vt, 8, L"%d%%", g_volume);
            g_stVol = CreateWindowExW(0, L"STATIC", vt,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0,0,0,0, hwnd, (HMENU)IDC_ST_VOL, NULL, NULL);
        }
        g_btnRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);

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

        {
            HWND ctrls[] = {
                g_stStatus, g_stInfo, g_sldSeek, g_stTime,
                g_btnPlay, g_btnStop, g_stVolLabel, g_sldVol, g_stVol, g_btnRefresh,
                g_listPod, g_listEp, g_editDesc
            };
            SetFontAll(ctrls, (int)(sizeof(ctrls)/sizeof(ctrls[0])));
        }

        /* 滑块子类化：修复滚轮方向 */
        g_oldSeekProc = (WNDPROC)SetWindowLongPtrW(g_sldSeek, GWLP_WNDPROC, (LONG_PTR)SliderProc);
        g_oldVolProc  = (WNDPROC)SetWindowLongPtrW(g_sldVol,  GWLP_WNDPROC, (LONG_PTR)SliderProc);

        SetTimer(hwnd, 1, 500, NULL);
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
        if (di->CtlID != IDC_LIST_POD || di->itemID == (UINT)-1) break;
        {
            Podcast *p = &g_pods[di->itemID];
            int isSel = (di->itemState & ODS_SELECTED) != 0;
            int isPlaying = (g_playPod == (int)di->itemID && g_playState == 1);
            HBRUSH bg = CreateSolidBrush(isSel ? RGB(204,232,255) : RGB(255,255,255));
            RECT rc = di->rcItem;
            int textLeft;

            FillRect(di->hDC, &rc, bg);
            DeleteObject(bg);

            /* 封面图：50x50，左/上各留 2px */
            if (p->hImage) {
                HDC mem = CreateCompatibleDC(di->hDC);
                HBITMAP old = (HBITMAP)SelectObject(mem, p->hImage);
                BitBlt(di->hDC, rc.left + 2, rc.top + 2, 50, 50, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
                textLeft = rc.left + 58;
            } else {
                textLeft = rc.left + 6;
            }

            SetBkMode(di->hDC, TRANSPARENT);

            /* 标题：10pt 加粗，顶部对齐，播放中绿色 */
            {
                RECT rct = rc;
                rct.left = textLeft;
                rct.right = rc.right - 34;
                rct.top += 3;
                rct.bottom = rct.top + 22;
                SetTextColor(di->hDC, isPlaying ? RGB(0,140,60) : RGB(20,20,20));
                SelectObject(di->hDC, g_fontBold);
                DrawTextW(di->hDC, p->title, -1, &rct,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            /* 副标题：copyright（空则 author），8pt 灰色，标题下方 */
            {
                const wchar_t *sub = p->copyright ? p->copyright : p->author;
                if (sub && *sub) {
                    RECT rcs = rc;
                    rcs.left = textLeft;
                    rcs.right = rc.right - 34;
                    rcs.top += 26;
                    rcs.bottom = rcs.top + 24;
                    SetTextColor(di->hDC, RGB(110,110,110));
                    SelectObject(di->hDC, g_fontSmall);
                    DrawTextW(di->hDC, sub, -1, &rcs,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }

            /* 右侧：纯数字（剧集数） */
            {
                wchar_t cnt[16];
                RECT rc2 = di->rcItem;
                swprintf(cnt, 16, L"%d", p->epCount);
                SetTextColor(di->hDC, isSel ? RGB(40,40,40) : RGB(120,120,120));
                SelectObject(di->hDC, g_font);
                rc2.left = rc2.right - 32;
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
        return 0;

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
        MINMAXINFO *mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = 600;
        mm->ptMinTrackSize.y = 440;
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == LBN_SELCHANGE) {
            if (LOWORD(wParam) == IDC_LIST_POD)
                SelectPodcast((int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0));
        } else if (HIWORD(wParam) == BN_CLICKED) {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_PLAY) PauseResume();
            else if (id == IDC_BTN_STOP) {
                if (g_downloading) CancelCurrentDownload();   /* 下载中点停止：中止下载 */
                PlayerStop();
                SetStatus(L"已停止");
            }
            else if (id == IDC_BTN_REFRESH) ReloadFeeds();
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR*)lParam;
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
            }
        }
        return 0;
    }

    case WM_HSCROLL:
        if ((HWND)lParam == g_sldVol) {
            wchar_t buf[16];
            int v = (int)SendMessageW(g_sldVol, TBM_GETPOS, 0, 0);
            g_volume = v;
            swprintf(buf, 16, L"%d%%", v);
            SetWindowTextW(g_stVol, buf);
            if (g_player) g_player->lpVtbl->SetVolume(g_player, v / 100.0f);
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
        if (pod) {
            if (g_podCount < MAX_PODCASTS) {
                g_pods[g_podCount] = *pod;
                SendMessageW(g_listPod, LB_ADDSTRING, 0, (LPARAM)pod->title);
                g_podCount++;
            } else {
                FreePodcast(pod);   /* 超过上限：丢弃并释放 */
            }
            free(pod);
        }
        return 0;
    }

    case WM_APP_FEEDS_DONE:
        g_feedsLoading = 0;
        if (g_podCount > 0) {
            SetStatus(L"就绪");
            SendMessageW(g_listPod, LB_SETCURSEL, 0, 0);
            SelectPodcast(0);
        } else {
            SetStatus(L"未加载到订阅，请检查 feeds.ini");
        }
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
                SetWindowTextW(g_stInfo, info);
                swprintf(st, 512, L"正在播放：%s", e->title);
                SetStatus(st);
                PlayerPlayFile(cache);
            } else if (!r->ok) {
                SetStatus(L"音频下载失败");
            }
            free(r->url);
            free(r);
        }
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
                SetWindowTextW(g_btnPlay, L"暂停");
                d = PlayerGet100ns(1);
                if (d > 0) g_dur100ns = d;
                InvalidateRect(g_listPod, NULL, TRUE);   /* 标题变绿 */
            }
            break;
        case MFP_EVENT_TYPE_PAUSE:
            if (SUCCEEDED((HRESULT)lParam)) {
                g_playState = 2;
                SetWindowTextW(g_btnPlay, L"播放");
                InvalidateRect(g_listPod, NULL, TRUE);
            }
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            PlayerStop();
            SetStatus(L"播放完毕");
            InvalidateRect(g_listPod, NULL, TRUE);
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
        SaveConfig();   /* 退出时唯一一次写盘：排序/列宽/音量/窗口尺寸 */
        if (g_player) { g_player->lpVtbl->Release(g_player); g_player = NULL; }
        FreePodcasts();
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

    /* 建窗前读取配置：排序/列宽/音量/窗口尺寸 */
    LoadConfig(1);

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

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* ESC 退出（叉叉也是同一条退出路径，都会触发 WM_DESTROY 保存） */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_gdipToken) GdiplusShutdown(g_gdipToken);
    return (int)msg.wParam;
}
