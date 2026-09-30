/*
 * LightPodcast - 纯 C / 原生 Win32 播客播放器（界面原型）
 *
 * 编译（MinGW）:
 *   windres resource.rc -O coff -o resource.o
 *   gcc -Os -s -mwindows -municode -ffunction-sections -fdata-sections \
 *       -Wl,--gc-sections -o lightpodcast.exe main.c resource.o -lcomctl32
 */
#define UNICODE
#define _UNICODE
#define WINVER       0x0601   /* 最低 Windows 7 */
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* 示例数据（后续改为从 feeds.txt + RSS 解析加载）                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const wchar_t *title;
    int durationSec;
    const wchar_t *desc;
} Episode;

typedef struct {
    const wchar_t *title;
    const wchar_t *desc;
    const Episode *eps;
    int epCount;
} Podcast;

static const Episode g_wolf359_eps[] = {
    { L"Episode 1: Succulent Rat-Killing Tarot Cards", 1805,
      L"Our premiere episode. Officer Doug Eiffel, communications officer on board the "
      L"U.S.S. Hephaestus Station, is willing to go to any lengths to procrastinate his "
      L"work. Tasked with a pointless hunt for alien intelligence, Eiffel would much "
      L"rather spend his time complaining about the station's malfunctioning autopilot, "
      L"making mixtapes, and antagonizing his by-the-book mission commander, Minkowski. "
      L"Plus, plant monsters, rat infestations, and the perils of deep space tarot." },
    { L"Episode 2: Little Revolución", 1655,
      L"Eiffel takes a quick trip to the exterior of the Hephaestus in pursuit of a "
      L"cleaner reading of one of the station's mysterious signals. What begins as a "
      L"routine maintenance task takes a turn when he discovers that he is not alone "
      L"outside the station." },
    { L"Episode 3: Discomforts, Pains, and Irregularities", 1581,
      L"Eiffel and Minkowski attempt to diagnose the autopilot's increasingly erratic "
      L"behavior. As tensions rise aboard the Hephaestus, Hera begins to suspect that "
      L"there may be more to the station's malfunctions than meets the eye." },
    { L"Episode 4: Cataracts and Hurricanoes", 1796,
      L"A mysterious illness strikes Eiffel, quickly leaving him out of commission and "
      L"bedridden. It falls on Minkowski to oversee an important rendezvous with a "
      L"supply ship. But when the vessel fails to respond to hails, the crew realizes "
      L"something has gone very, very wrong." },
    { L"Episode 5: Extreme Danger Bug", 1596,
      L"Eiffel and Minkowski must work together to deal with a highly dangerous, "
      L"unexpected stowaway. But as the situation grows more dire, personal grudges "
      L"and simmering tensions threaten to tear the crew apart." },
    { L"Episode 6: Am I Alone Now?", 1882,
      L"The crew of the Hephaestus has a very, very bad day. As Eiffel and Minkowski "
      L"fight to regain control of the station, they are forced to confront the "
      L"possibility that they may be truly alone in deep space." },
    { L"Episode 7: The Sound and the Fury", 1684,
      L"When Minkowski and Hera get into a heated argument over command decisions, "
      L"Eiffel decides to stay out of the way. But neutrality leaves him stranded in "
      L"the station's dark comms room - and he soon discovers something else is in "
      L"there with him." },
    { L"Episode 8: Box 953", 1638,
      L"Eiffel and Minkowski discover a mysterious pod floating in the void of deep "
      L"space. Against their better judgment, they bring it aboard - and immediately "
      L"begin to regret that decision." },
    { L"Episode 9: The Empty Man Cometh", 1604,
      L"The crew's very bad day continues. With the station's systems failing one by "
      L"one and an unseen menace lurking in the shadows, Eiffel is forced to consider "
      L"desperate measures." },
    { L"Episode 10: Danger, Not Fantasy", 1568,
      L"The crew attempts to resume normal operations, but normal is a relative term "
      L"on a station where nothing ever goes according to plan. Eiffel tries to lift "
      L"morale with a talent show; the universe responds with mortal peril." },
    { L"Episode 11: The Devil's Plaything", 1727,
      L"As the crew recovers from recent events, Eiffel finds an unexpected companion "
      L"in the station's storage bay. But some things on the Hephaestus are not what "
      L"they seem." },
    { L"Episode 12: Deep Breaths", 1551,
      L"The Hephaestus crew celebrates Christmas in deep space - or tries to, anyway. "
      L"Between Hera's glitching systems and a mysterious signal that refuses to be "
      L"decoded, holiday cheer is in short supply." },
};

static const Podcast g_podcasts[] = {
    { L"Wolf 359",
      L"Life's not easy for Doug Eiffel and his communications officer... no wait. "
      L"Doug Eiffel IS the communications officer aboard the U.S.S. Hephaestus "
      L"Research Station, currently on day 448 of its mission around red dwarf star "
      L"Wolf 359. He's also the station's slacker-in-chief. But the Hephaestus is a "
      L"weird place, and Eiffel is about to discover that his insignificance might "
      L"be the only thing keeping him alive. Sci-fi radio drama from Kinda Evil "
      L"Genius Productions.",
      g_wolf359_eps, (int)(sizeof(g_wolf359_eps) / sizeof(g_wolf359_eps[0])) },
};
#define PODCAST_COUNT (sizeof(g_podcasts) / sizeof(g_podcasts[0]))

/* ------------------------------------------------------------------ */
/* 控件 ID                                                             */
/* ------------------------------------------------------------------ */
enum {
    IDC_GRP_PLAY = 100,
    IDC_ST_STATUS,
    IDC_ST_INFO,
    IDC_SLD_SEEK,
    IDC_ST_TIME,
    IDC_BTN_PLAY,
    IDC_BTN_STOP,
    IDC_ST_VOLLABEL,
    IDC_SLD_VOL,
    IDC_ST_VOL,
    IDC_GRP_POD,
    IDC_LIST_POD,
    IDC_GRP_EP,
    IDC_LIST_EP,
    IDC_GRP_DESC,
    IDC_EDIT_DESC,
};

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */
static HWND g_grpPlay, g_stStatus, g_stInfo, g_sldSeek, g_stTime;
static HWND g_btnPlay, g_btnStop, g_stVolLabel, g_sldVol, g_stVol;
static HWND g_grpPod, g_listPod, g_grpEp, g_listEp, g_grpDesc, g_editDesc;
static HFONT g_font;
static int  g_curPod = 0;
static int  g_curEp  = -1;
static BOOL g_playing = TRUE;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */
static HFONT CreateUiFont(void)
{
    HDC hdc = GetDC(NULL);
    int h = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei");
}

static void SetFontAll(HWND *ctrls, int n)
{
    int i;
    for (i = 0; i < n; i++)
        SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_font, TRUE);
}

static void FormatTime(int sec, wchar_t *buf, size_t n)
{
    swprintf(buf, n, L"%02d:%02d", sec / 60, sec % 60);
}

static void SetStatus(const wchar_t *prefix, const wchar_t *title)
{
    wchar_t buf[512];
    swprintf(buf, 512, L"%s%s", prefix, title ? title : L"");
    SetWindowTextW(g_stStatus, buf);
}

static void UpdateTimeLabel(void)
{
    wchar_t buf[64], a[16], b[16];
    int pos = (int)SendMessageW(g_sldSeek, TBM_GETPOS, 0, 0);
    int dur = 0;
    if (g_curEp >= 0 && g_curEp < g_podcasts[g_curPod].epCount)
        dur = g_podcasts[g_curPod].eps[g_curEp].durationSec;
    FormatTime(dur * pos / 1000, a, 16);
    FormatTime(dur, b, 16);
    swprintf(buf, 64, L"%s / %s", a, b);
    SetWindowTextW(g_stTime, buf);
}

/* 选中某个播客：刷新剧集列表 + 显示播客简介 */
static void SelectPodcast(int idx)
{
    int i;
    const Podcast *p;
    if (idx < 0 || idx >= (int)PODCAST_COUNT) return;
    g_curPod = idx;
    g_curEp  = -1;
    p = &g_podcasts[idx];

    SendMessageW(g_listEp, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < p->epCount; i++)
        SendMessageW(g_listEp, LB_ADDSTRING, 0, (LPARAM)p->eps[i].title);

    SetWindowTextW(g_editDesc, p->desc);
}

/* 选中某一集：显示简介 + 更新播放状态区 */
static void SelectEpisode(int idx)
{
    const Podcast *p = &g_podcasts[g_curPod];
    if (idx < 0 || idx >= p->epCount) return;
    g_curEp = idx;
    g_playing = TRUE;

    SetWindowTextW(g_editDesc, p->eps[idx].desc);
    SetStatus(L"正在播放：", p->eps[idx].title);
    SetWindowTextW(g_btnPlay, L"暂停");
    SendMessageW(g_sldSeek, TBM_SETPOS, TRUE, 0);
    UpdateTimeLabel();
}

/* ------------------------------------------------------------------ */
/* 布局：四个方框随窗口尺寸变化铺满                                     */
/* ------------------------------------------------------------------ */
static void LayoutControls(HWND hwnd)
{
    RECT rc;
    int W, H;
    const int M = 8;        /* 外边距 */
    const int GAP = 8;      /* 方框间距 */
    const int TOPH = 140;   /* 顶部播放区高度 */
    int by, bh, totalW, w1, w2, w3, x;
    int gx, gw, gy;

    GetClientRect(hwnd, &rc);
    W = rc.right;
    H = rc.bottom;

    /* ---- 顶部：播放状态 ---- */
    MoveWindow(g_grpPlay, M, M, W - 2 * M, TOPH, TRUE);
    gx = M + 12;                    /* 组框内左缘 */
    gw = W - 2 * M - 24;            /* 组框内可用宽 */
    gy = M + 20;                    /* 组框标题之下 */

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

    /* ---- 底部三栏：播客 | 剧集 | 简介 ---- */
    by = M + TOPH + GAP;
    bh = H - by - M;
    if (bh < 40) bh = 40;
    totalW = W - 2 * M - 2 * GAP;
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

/* ------------------------------------------------------------------ */
/* 窗口过程                                                            */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        DWORD ls = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP;
        int i;

        g_font = CreateUiFont();

        /* 顶部播放区 */
        g_grpPlay = CreateWindowExW(0, L"BUTTON", L"播放",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_PLAY, NULL, NULL);
        g_stStatus = CreateWindowExW(0, L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ST_STATUS, NULL, NULL);
        g_stInfo = CreateWindowExW(0, L"STATIC", L"MP3 · 44100 Hz · 128 kbps · 立体声",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ST_INFO, NULL, NULL);
        g_sldSeek = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SLD_SEEK, NULL, NULL);
        SendMessageW(g_sldSeek, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        g_stTime = CreateWindowExW(0, L"STATIC", L"00:00 / 00:00",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ST_TIME, NULL, NULL);
        g_btnPlay = CreateWindowExW(0, L"BUTTON", L"暂停",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_PLAY, NULL, NULL);
        g_btnStop = CreateWindowExW(0, L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)IDC_BTN_STOP, NULL, NULL);
        g_stVolLabel = CreateWindowExW(0, L"STATIC", L"音量",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ST_VOLLABEL, NULL, NULL);
        g_sldVol = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_SLD_VOL, NULL, NULL);
        SendMessageW(g_sldVol, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(g_sldVol, TBM_SETPOS, TRUE, 80);
        g_stVol = CreateWindowExW(0, L"STATIC", L"80%",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, (HMENU)IDC_ST_VOL, NULL, NULL);

        /* 播客列表 */
        g_grpPod = CreateWindowExW(0, L"BUTTON", L"播客",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_POD, NULL, NULL);
        g_listPod = CreateWindowExW(0, L"LISTBOX", NULL,
            ls | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LIST_POD, NULL, NULL);

        /* 剧集列表 */
        g_grpEp = CreateWindowExW(0, L"BUTTON", L"剧集",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_EP, NULL, NULL);
        g_listEp = CreateWindowExW(0, L"LISTBOX", NULL,
            ls | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LIST_EP, NULL, NULL);

        /* 简介 */
        g_grpDesc = CreateWindowExW(0, L"BUTTON", L"简介",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            0, 0, 0, 0, hwnd, (HMENU)IDC_GRP_DESC, NULL, NULL);
        g_editDesc = CreateWindowExW(0, L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)IDC_EDIT_DESC, NULL, NULL);

        {
            HWND ctrls[] = {
                g_grpPlay, g_stStatus, g_stInfo, g_sldSeek, g_stTime,
                g_btnPlay, g_btnStop, g_stVolLabel, g_sldVol, g_stVol,
                g_grpPod, g_listPod, g_grpEp, g_listEp, g_grpDesc, g_editDesc
            };
            SetFontAll(ctrls, (int)(sizeof(ctrls) / sizeof(ctrls[0])));
        }

        /* 填充播客列表并默认选中第一个播客、第一集 */
        for (i = 0; i < (int)PODCAST_COUNT; i++)
            SendMessageW(g_listPod, LB_ADDSTRING, 0, (LPARAM)g_podcasts[i].title);
        SendMessageW(g_listPod, LB_SETCURSEL, 0, 0);
        SelectPodcast(0);
        SendMessageW(g_listEp, LB_SETCURSEL, 0, 0);
        SelectEpisode(0);
        SendMessageW(g_sldSeek, TBM_SETPOS, TRUE, 417);   /* 示例进度 12:34 */
        UpdateTimeLabel();
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lParam;
        mm->ptMinTrackSize.x = 600;
        mm->ptMinTrackSize.y = 440;
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == LBN_SELCHANGE) {
            int id = LOWORD(wParam);
            if (id == IDC_LIST_POD) {
                int sel = (int)SendMessageW(g_listPod, LB_GETCURSEL, 0, 0);
                SelectPodcast(sel);
            } else if (id == IDC_LIST_EP) {
                int sel = (int)SendMessageW(g_listEp, LB_GETCURSEL, 0, 0);
                SelectEpisode(sel);
            }
        } else if (HIWORD(wParam) == BN_CLICKED) {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_PLAY) {
                g_playing = !g_playing;
                SetWindowTextW(g_btnPlay, g_playing ? L"暂停" : L"播放");
                if (g_curEp >= 0)
                    SetStatus(g_playing ? L"正在播放：" : L"已暂停：",
                              g_podcasts[g_curPod].eps[g_curEp].title);
            } else if (id == IDC_BTN_STOP) {
                g_playing = FALSE;
                SetWindowTextW(g_btnPlay, L"播放");
                if (g_curEp >= 0)
                    SetStatus(L"已停止：", g_podcasts[g_curPod].eps[g_curEp].title);
                SendMessageW(g_sldSeek, TBM_SETPOS, TRUE, 0);
                UpdateTimeLabel();
            }
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)lParam == g_sldVol) {
            wchar_t buf[16];
            int v = (int)SendMessageW(g_sldVol, TBM_GETPOS, 0, 0);
            swprintf(buf, 16, L"%d%%", v);
            SetWindowTextW(g_stVol, buf);
        } else if ((HWND)lParam == g_sldSeek) {
            UpdateTimeLabel();
        }
        return 0;

    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow)
{
    const wchar_t *CLASS_NAME = L"LightPodcastWnd";
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev; (void)lpCmdLine;

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
        CW_USEDEFAULT, CW_USEDEFAULT, 680, 520,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {   /* 支持 Tab 键切换控件 */
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
