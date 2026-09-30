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
#include <commctrl.h>
#include <winhttp.h>
#include <propidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ================= 常量 ================= */
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
    int      durationSec;
} Episode;

typedef struct {
    wchar_t *title;
    wchar_t *desc;
    Episode *eps;
    int      epCount;
    int      epCap;
} Podcast;

typedef struct {
    int pod, ep;
    int ok;
} EpReady;

/* ================= 控件 ID ================= */
enum {
    IDC_GRP_PLAY = 100, IDC_ST_STATUS, IDC_ST_INFO,
    IDC_SLD_SEEK, IDC_ST_TIME,
    IDC_BTN_PLAY, IDC_BTN_STOP,
    IDC_ST_VOLLABEL, IDC_SLD_VOL, IDC_ST_VOL,
    IDC_GRP_POD, IDC_LIST_POD,
    IDC_GRP_EP, IDC_LIST_EP,
    IDC_GRP_DESC, IDC_EDIT_DESC,
    IDC_BTN_REFRESH
};

/* ================= 全局状态 ================= */
static Podcast  g_pods[MAX_PODCASTS];
static int      g_podCount = 0;
static int      g_curPod = -1, g_curEp = -1;

static HWND     g_hwnd;
static HFONT    g_font;
static HWND     g_grpPlay, g_stStatus, g_stInfo, g_sldSeek, g_stTime;
static HWND     g_btnPlay, g_btnStop, g_stVolLabel, g_sldVol, g_stVol;
static HWND     g_grpPod, g_listPod, g_grpEp, g_listEp, g_grpDesc, g_editDesc;
static HWND     g_btnRefresh;

static volatile int g_feedsLoading = 0;
static volatile int g_downloading  = 0;

/* 播放 */
static IMFPMediaPlayer *g_player = NULL;
static int  g_playState = 0;          /* 0=停止 1=播放 2=暂停 */
static int  g_seeking   = 0;
static int  g_playPod = -1, g_playEp = -1;
static long long g_dur100ns = 0;

/* exe 同目录 */
static void ExeDir(wchar_t *out, int outMax);

/* ================= 调试日志（临时） ================= */
static void DbgLog(const char *fmt, ...)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    FILE *f;
    va_list ap;
    SYSTEMTIME st;
    ExeDir(dir, MAX_PATH);
    swprintf(path, MAX_PATH, L"%sdebug.log", dir);
    f = _wfopen(path, L"a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
    DbgLog("CRASH code=0x%08lx addr=%p", ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ================= 小工具 ================= */
static HFONT CreateUiFont(void)
{
    HDC hdc = GetDC(NULL);
    int h = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

/* exe 同目录 */
static void ExeDir(wchar_t *out, int outMax)
{
    wchar_t *p;
    GetModuleFileNameW(NULL, out, outMax);
    p = wcsrchr(out, L'\\');
    if (p) *(p + 1) = L'\0';
}

/* ================= HTTP 下载 ================= */
/* 下载整个 URL 到内存。progressMsg!=0 时向 g_hwnd 汇报百分比。 */
static char* HttpFetch(const wchar_t *url, DWORD *outLen, UINT progressMsg)
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
            PostMessageW(g_hwnd, progressMsg, (WPARAM)((unsigned __int64)total * 100 / contentLen), 0);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);
    if (!buf) { buf = (char*)malloc(1); buf[0] = 0; }
    else buf[total] = 0;
    *outLen = total;
    return buf;

fail:
    if (hReq) WinHttpCloseHandle(hReq);
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
            if (gt) { src = gt + 1; continue; }
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
            e->title = GetXmlTextW(seg, "title", 0);
            if (!e->title) e->title = _wcsdup(L"(无标题)");
            e->desc = GetXmlTextW(seg, "description", 1);
            if (!e->desc) e->desc = GetXmlTextW(seg, "itunes:summary", 1);
            if (!e->desc) e->desc = GetXmlTextW(seg, "content:encoded", 1);
            if (!e->desc) e->desc = _wcsdup(L"");
            e->url = U8ToW(encUrl);
            if (!e->url) { free(encUrl); free(seg); item = FindOpenTag(itemEnd, "item"); continue; }
            CleanXmlText(encUrl); /* no-op for url but safe */
            d = XmlText(seg, "itunes:duration");
            if (!d) d = XmlText(seg, "duration");
            if (d) { e->durationSec = ParseDuration(d); free(d); }
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

/* ================= 后台线程：加载 feeds ================= */
static DWORD WINAPI FeedLoaderThread(LPVOID param)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char line[MAX_FEED_LINE];
    int loaded = 0;
    (void)param;

    DbgLog("loader: start");
    ExeDir(path, MAX_PATH);
    wcscat_s(path, MAX_PATH, L"feeds.txt");
    f = _wfopen(path, L"rb");
    if (!f) { DbgLog("loader: no feeds.txt"); PostMessageW(g_hwnd, WM_APP_FEEDS_DONE, 0, 0); return 0; }

    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        size_t n;
        wchar_t *wurl;
        DWORD xmlLen = 0;
        char *xml;
        Podcast *pod;

        /* 去 BOM */
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        while (*s == ' ' || *s == '\t') s++;
        n = strlen(s);
        while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
            s[--n] = 0;
        if (*s == 0 || *s == '#') continue;

        wurl = U8ToW(s);
        if (!wurl) continue;
        DbgLog("loader: fetching %s", s);
        xml = HttpFetch(wurl, &xmlLen, 0);
        free(wurl);
        if (!xml) { DbgLog("loader: fetch failed"); continue; }
        DbgLog("loader: fetched %lu bytes", xmlLen);

        pod = (Podcast*)calloc(1, sizeof(Podcast));
        if (pod && ParseFeed(xml, pod)) {
            DbgLog("loader: parsed %d episodes", pod->epCount);
            PostMessageW(g_hwnd, WM_APP_FEED_ADDED, 0, (LPARAM)pod);
            loaded++;
        } else if (pod) {
            DbgLog("loader: parse failed");
            free(pod->title); free(pod->desc); free(pod->eps); free(pod);
        }
        free(xml);
    }
    fclose(f);
    DbgLog("loader: done, %d feeds", loaded);
    PostMessageW(g_hwnd, WM_APP_FEEDS_DONE, loaded, 0);
    return 0;
}

/* ================= 后台线程：下载音频 ================= */
typedef struct { int pod, ep; } DlJob;

static DWORD WINAPI EpisodeDlThread(LPVOID param)
{
    DlJob *job = (DlJob*)param;
    EpReady *r;
    wchar_t cache[MAX_PATH];
    DWORD len = 0;
    char *data;
    FILE *f;

    r = (EpReady*)calloc(1, sizeof(EpReady));
    if (!r) { free(job); g_downloading = 0; return 0; }
    r->pod = job->pod; r->ep = job->ep;

    if (job->pod >= 0 && job->pod < g_podCount &&
        job->ep >= 0 && job->ep < g_pods[job->pod].epCount) {
        Episode *e = &g_pods[job->pod].eps[job->ep];
        CachePathFor(e->url, cache, MAX_PATH);
        EnsureCacheDir();
        if (GetFileAttributesW(cache) != INVALID_FILE_ATTRIBUTES) {
            r->ok = 1;  /* 已缓存 */
        } else {
            data = HttpFetch(e->url, &len, WM_APP_DL_PROGRESS);
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
    g_downloading = 0;
    PostMessageW(g_hwnd, WM_APP_EP_READY, 0, (LPARAM)r);
    return 0;
}

/* ================= 音频信息探测 ================= */
static void ProbeAudio(const wchar_t *path, wchar_t *out, int outMax)
{
    IMFSourceReader *reader = NULL;
    IMFMediaType *mt = NULL;
    UINT32 rate = 0, ch = 0, avgBps = 0;
    GUID subtype;
    const wchar_t *fmtName = L"音频";
    HRESULT hr;

    wcscpy_s(out, outMax, L"");
    hr = MFCreateSourceReaderFromURL(path, NULL, &reader);
    if (FAILED(hr) || !reader) return;
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
    g_dur100ns = 0;
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

static long long PlayerGetPos100ns(void)
{
    PROPVARIANT pv;
    HRESULT hr;
    if (!g_player) return -1;
    memset(&pv, 0, sizeof(pv));
    hr = g_player->lpVtbl->GetPosition(g_player, &MFP_POSITIONTYPE_100NS, &pv);
    if (FAILED(hr) || pv.vt != VT_I8) return -1;
    return pv.hVal.QuadPart;
}

static long long PlayerGetDur100ns(void)
{
    PROPVARIANT pv;
    HRESULT hr;
    if (!g_player) return -1;
    memset(&pv, 0, sizeof(pv));
    hr = g_player->lpVtbl->GetDuration(g_player, &MFP_POSITIONTYPE_100NS, &pv);
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
static void SelectPodcast(int idx)
{
    Podcast *p;
    int i;
    if (idx < 0 || idx >= g_podCount) return;
    g_curPod = idx;
    p = &g_pods[idx];
    SendMessageW(g_listEp, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < p->epCount; i++)
        SendMessageW(g_listEp, LB_ADDSTRING, 0, (LPARAM)p->eps[i].title);
    SetWindowTextW(g_editDesc, p->desc);
}

static void SelectEpisode(int idx)
{
    if (g_curPod < 0 || idx < 0 || idx >= g_pods[g_curPod].epCount) return;
    g_curEp = idx;
    SetWindowTextW(g_editDesc, g_pods[g_curPod].eps[idx].desc);
}

static void RequestPlayEpisode(int pod, int ep)
{
    DlJob *job;
    Episode *e;
    if (pod < 0 || pod >= g_podCount || ep < 0 || ep >= g_pods[pod].epCount) return;
    if (g_downloading) { SetStatus(L"正在下载中，请稍候..."); return; }
    e = &g_pods[pod].eps[ep];

    g_playPod = pod; g_playEp = ep;
    SetWindowTextW(g_editDesc, e->desc);
    SetStatus(L"准备音频...");

    job = (DlJob*)calloc(1, sizeof(DlJob));
    if (!job) return;
    job->pod = pod; job->ep = ep;
    g_downloading = 1;
    if (!CreateThread(NULL, 0, EpisodeDlThread, job, 0, NULL)) {
        g_downloading = 0;
        free(job);
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

static void FreePodcasts(void)
{
    int i, j;
    for (i = 0; i < g_podCount; i++) {
        Podcast *p = &g_pods[i];
        for (j = 0; j < p->epCount; j++) {
            free(p->eps[j].title);
            free(p->eps[j].desc);
            free(p->eps[j].url);
        }
        free(p->eps);
        free(p->title);
        free(p->desc);
    }
    g_podCount = 0;
    g_curPod = g_curEp = -1;
}

static void ReloadFeeds(void)
{
    if (g_feedsLoading) return;
    PlayerStop();
    FreePodcasts();
    SendMessageW(g_listPod, LB_RESETCONTENT, 0, 0);
    SendMessageW(g_listEp, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(g_editDesc, L"");
    SetStatus(L"正在加载订阅...");
    g_feedsLoading = 1;
    CreateThread(NULL, 0, FeedLoaderThread, NULL, 0, NULL);
}

/* ================= 布局 ================= */
static void LayoutControls(HWND hwnd)
{
    RECT rc;
    int W, H;
    const int M = 8, GAP = 8, TOPH = 140;
    int by, bh, totalW, w1, w2, w3, x, gx, gw, gy;

    GetClientRect(hwnd, &rc);
    W = rc.right; H = rc.bottom;

    MoveWindow(g_grpPlay, M, M, W - 2*M, TOPH, TRUE);
    gx = M + 12; gw = W - 2*M - 24; gy = M + 20;

    MoveWindow(g_stStatus, gx, gy, gw - 220, 18, TRUE);
    MoveWindow(g_stInfo,   gx + gw - 215, gy, 215, 18, TRUE);
    gy += 24;
    MoveWindow(g_sldSeek, gx, gy - 2, gw - 112, 24, TRUE);
    MoveWindow(g_stTime,  gx + gw - 104, gy, 104, 18, TRUE);
    gy += 32;
    MoveWindow(g_btnPlay,    gx,       gy, 60, 24, TRUE);
    MoveWindow(g_btnStop,    gx + 68,  gy, 60, 24, TRUE);
    MoveWindow(g_stVolLabel, gx + 150, gy + 4, 32, 16, TRUE);
    MoveWindow(g_sldVol,     gx + 184, gy, 130, 24, TRUE);
    MoveWindow(g_stVol,      gx + 320, gy + 4, 44, 16, TRUE);
    MoveWindow(g_btnRefresh, gx + 380, gy, 70, 24, TRUE);

    by = M + TOPH + GAP;
    bh = H - by - M;
    if (bh < 40) bh = 40;
    totalW = W - 2*M - 2*GAP;
    w1 = totalW * 28 / 100;
    w2 = totalW * 36 / 100;
    w3 = totalW - w1 - w2;

    x = M;
    MoveWindow(g_grpPod, x, by, w1, bh, TRUE);
    MoveWindow(g_listPod, x + 10, by + 20, w1 - 20, bh - 30, TRUE);
    x += w1 + GAP;
    MoveWindow(g_grpEp, x, by, w2, bh, TRUE);
    MoveWindow(g_listEp, x + 10, by + 20, w2 - 20, bh - 30, TRUE);
    x += w2 + GAP;
    MoveWindow(g_grpDesc, x, by, w3, bh, TRUE);
    MoveWindow(g_editDesc, x + 10, by + 20, w3 - 20, bh - 30, TRUE);
}

/* ================= 窗口过程 ================= */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        DWORD ls = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP;

        g_font = CreateUiFont();

        g_grpPlay = CreateWindowExW(0, L"BUTTON", L"播放",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0,0,0,0, hwnd, (HMENU)IDC_GRP_PLAY, NULL, NULL);
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
        SendMessageW(g_sldVol, TBM_SETPOS, TRUE, 80);
        g_stVol = CreateWindowExW(0, L"STATIC", L"80%",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,0,0,0, hwnd, (HMENU)IDC_ST_VOL, NULL, NULL);
        g_btnRefresh = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,0,0,0, hwnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);

        g_grpPod = CreateWindowExW(0, L"BUTTON", L"播客",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0,0,0,0, hwnd, (HMENU)IDC_GRP_POD, NULL, NULL);
        g_listPod = CreateWindowExW(0, L"LISTBOX", NULL,
            ls | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
            0,0,0,0, hwnd, (HMENU)IDC_LIST_POD, NULL, NULL);
        g_grpEp = CreateWindowExW(0, L"BUTTON", L"剧集",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0,0,0,0, hwnd, (HMENU)IDC_GRP_EP, NULL, NULL);
        g_listEp = CreateWindowExW(0, L"LISTBOX", NULL,
            ls | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
            0,0,0,0, hwnd, (HMENU)IDC_LIST_EP, NULL, NULL);
        g_grpDesc = CreateWindowExW(0, L"BUTTON", L"简介",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0,0,0,0, hwnd, (HMENU)IDC_GRP_DESC, NULL, NULL);
        g_editDesc = CreateWindowExW(0, L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_EDIT_DESC, NULL, NULL);

        {
            HWND ctrls[] = {
                g_grpPlay, g_stStatus, g_stInfo, g_sldSeek, g_stTime,
                g_btnPlay, g_btnStop, g_stVolLabel, g_sldVol, g_stVol, g_btnRefresh,
                g_grpPod, g_listPod, g_grpEp, g_listEp, g_grpDesc, g_editDesc
            };
            SetFontAll(ctrls, (int)(sizeof(ctrls)/sizeof(ctrls[0])));
        }

        SetTimer(hwnd, 1, 500, NULL);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = 600;
        mm->ptMinTrackSize.y = 440;
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == LBN_SELCHANGE) {
            int id = LOWORD(wParam);
            if (id == IDC_LIST_POD) {
                SelectPodcast((int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0));
            } else if (id == IDC_LIST_EP) {
                SelectEpisode((int)SendMessageW(g_listEp, LB_GETCURSEL, 0, 0));
            }
        } else if (HIWORD(wParam) == LBN_DBLCLK) {
            if (LOWORD(wParam) == IDC_LIST_EP) {
                int sel = (int)SendMessageW(g_listEp, LB_GETCURSEL, 0, 0);
                if (sel >= 0) RequestPlayEpisode(g_curPod, sel);
            }
        } else if (HIWORD(wParam) == BN_CLICKED) {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_PLAY) PauseResume();
            else if (id == IDC_BTN_STOP) PlayerStop(), SetStatus(L"已停止");
            else if (id == IDC_BTN_REFRESH) ReloadFeeds();
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)lParam == g_sldVol) {
            wchar_t buf[16];
            int v = (int)SendMessageW(g_sldVol, TBM_GETPOS, 0, 0);
            swprintf(buf, 16, L"%d%%", v);
            SetWindowTextW(g_stVol, buf);
            if (g_player) g_player->lpVtbl->SetVolume(g_player, v / 100.0f);
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

    case WM_TIMER: {
        static int tick = 0;
        tick++;
        if (tick % 20 == 0) DbgLog("heartbeat %d", tick / 2);  /* 每 10 秒 */
        if (g_playState == 1 && !g_seeking && g_player) {
            long long pos = PlayerGetPos100ns();
            long long dur = PlayerGetDur100ns();
            if (dur > 0) g_dur100ns = dur;
            if (pos >= 0 && g_dur100ns > 0) {
                wchar_t buf[64], a[16], b[16];
                FmtTime(pos / 10000000, a, 16);
                FmtTime(g_dur100ns / 10000000, b, 16);
                swprintf(buf, 64, L"%s / %s", a, b);
                SetWindowTextW(g_stTime, buf);
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
        if (pod && g_podCount < MAX_PODCASTS) {
            g_pods[g_podCount] = *pod;
            SendMessageW(g_listPod, LB_ADDSTRING, 0, (LPARAM)pod->title);
            g_podCount++;
            free(pod);
        }
        return 0;
    }

    case WM_APP_FEEDS_DONE:
        DbgLog("feeds done, %d podcasts", g_podCount);
        g_feedsLoading = 0;
        if (g_podCount > 0) {
            SetStatus(L"就绪");
            SendMessageW(g_listPod, LB_SETCURSEL, 0, 0);
            SelectPodcast(0);
        } else {
            SetStatus(L"未加载到订阅，请检查 feeds.txt");
        }
        return 0;

    case WM_APP_EP_READY: {
        EpReady *r = (EpReady*)lParam;
        if (r) {
            if (r->ok && r->pod == g_playPod && r->ep == g_playEp &&
                r->pod >= 0 && r->pod < g_podCount && r->ep < g_pods[r->pod].epCount) {
                wchar_t cache[MAX_PATH], info[128], st[512];
                Episode *e = &g_pods[r->pod].eps[r->ep];
                CachePathFor(e->url, cache, MAX_PATH);
                ProbeAudio(cache, info, 128);
                SetWindowTextW(g_stInfo, info);
                swprintf(st, 512, L"正在播放：%s", e->title);
                SetStatus(st);
                PlayerPlayFile(cache);
            } else if (!r->ok) {
                SetStatus(L"音频下载失败");
            }
            free(r);
        }
        return 0;
    }

    case WM_APP_DL_PROGRESS: {
        wchar_t buf[64];
        swprintf(buf, 64, L"下载音频中... %d%%", (int)wParam);
        SetStatus(buf);
        return 0;
    }

    case WM_APP_MF_EVENT:
        switch ((MFP_EVENT_TYPE)wParam) {
        case MFP_EVENT_TYPE_PLAY:
            if (SUCCEEDED((HRESULT)lParam)) {
                g_playState = 1;
                SetWindowTextW(g_btnPlay, L"暂停");
                g_dur100ns = PlayerGetDur100ns();
            }
            break;
        case MFP_EVENT_TYPE_PAUSE:
            if (SUCCEEDED((HRESULT)lParam)) {
                g_playState = 2;
                SetWindowTextW(g_btnPlay, L"播放");
            }
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            PlayerStop();
            SetStatus(L"播放完毕");
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
        if (g_player) { g_player->lpVtbl->Release(g_player); g_player = NULL; }
        FreePodcasts();
        if (g_font) DeleteObject(g_font);
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

    SetUnhandledExceptionFilter(CrashHandler);
    DbgLog("startup");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) return 1;

    hwnd = CreateWindowExW(0, CLASS_NAME, L"LightPodcast",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 560,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    DbgLog("window created, loading feeds");
    ReloadFeeds();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
