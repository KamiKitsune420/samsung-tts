// Samsung TTS voice centre: a small window that lists Samsung's voice packs, downloads the chosen one straight
// from Samsung's store, unpacks it for the SAPI voice and registers it, removes it again, and plays a sample.
//
// It lives in the same folder as samsungtts_sapi.dll and puts voices in data\voice beside it. Nothing of
// Samsung's is in this program: a pack is fetched from Samsung only when the user asks for it.
//
// Plain Win32 controls throughout (a list box, buttons, a status line, a progress bar), so screen readers read it
// without help. Network and file work happens on a worker thread; the window only shows its progress.
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <sapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- the catalogue: packs the translated engine has been seen to speak with (tools/try_voices.py)

struct Entry { const wchar_t *code, *language, *name, *sample; };
static const wchar_t kEnglish[] = L"Hello. This is a sample of this voice, speaking at its normal rate.";
static const Entry kCatalogue[] = {
    {L"en_us_l03", L"US English", L"Stephanie", kEnglish},
    {L"en_us_g02", L"US English", L"John Lano", kEnglish},
    {L"en_us_l04", L"US English", L"Julia", kEnglish},
    {L"en_us_l05", L"US English", L"Lisa", kEnglish},
    {L"en_gb_l02", L"UK English", L"Amy Green", kEnglish},
#include "catalogue_more.inc"
};
static const int kCount = (int)(sizeof kCatalogue / sizeof kCatalogue[0]);

enum { ID_LIST = 100, ID_INSTALL, ID_REMOVE, ID_SAMPLE, ID_CLOSE, ID_STATUS, ID_PROGRESS };
enum { MSG_STATUS = WM_APP + 1, MSG_PROGRESS, MSG_DONE };

static HWND g_wnd, g_list, g_status, g_progress, g_install, g_remove, g_sample;
static std::wstring g_dir;          // where this program and the SAPI DLL are
static bool g_busy;
static ISpVoice *g_voice;

static std::wstring voice_folder(const Entry &e) {      // en_us_l03 -> ...\data\voice\en_US_l03
    std::wstring c = e.code;
    c[3] = (wchar_t)towupper(c[3]); c[4] = (wchar_t)towupper(c[4]);
    return g_dir + L"\\data\\voice\\" + c;
}
static bool installed(const Entry &e) {
    return GetFileAttributesW((voice_folder(e) + L"\\cfg").c_str()) != INVALID_FILE_ATTRIBUTES;
}
// A recorded sample of each voice ships beside this program (samples\<code>.wav, made by tools/make_samples.py),
// so a voice can be heard before it is downloaded.
static std::wstring sample_file(const Entry &e) { return g_dir + L"\\samples\\" + e.code + L".wav"; }
static bool has_sample(const Entry &e) { return GetFileAttributesW(sample_file(e).c_str()) != INVALID_FILE_ATTRIBUTES; }

static std::wstring label(const Entry &e) {
    std::wstring s = std::wstring(e.language) + L": " + e.name;
    return s + (installed(e) ? L" - installed" : L" - not installed");
}

static void post_status(const std::wstring &s) { PostMessageW(g_wnd, MSG_STATUS, 0, (LPARAM) new std::wstring(s)); }

// ---- helpers for the worker

static bool run_wait(const std::wstring &exe, const std::wstring &args, bool elevate, DWORD *code) {
    SHELLEXECUTEINFOW si = {sizeof si};
    si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    si.lpVerb = elevate ? L"runas" : L"open";
    si.lpFile = exe.c_str(); si.lpParameters = args.c_str(); si.nShow = SW_HIDE;
    if (!ShellExecuteExW(&si) || !si.hProcess) return false;
    WaitForSingleObject(si.hProcess, INFINITE);
    if (code) GetExitCodeProcess(si.hProcess, code);
    CloseHandle(si.hProcess);
    return true;
}

static void remove_tree(const std::wstring &dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p); else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// The SAPI DLL makes one voice token per pack it finds; registering it again brings the tokens up to date.
// Machine-wide registration is what puts a voice in every program's list, and that needs elevation.
static bool register_voices() {
    DWORD code = 1;
    std::wstring dll = L"/s \"" + g_dir + L"\\samsungtts_sapi.dll\"";
    post_status(L"Registering the voices with Windows. Please accept the administrator prompt.");
    return run_wait(L"regsvr32.exe", dll, true, &code) && code == 0;
}

// GET over HTTPS. With `file` the body goes to that file and progress is posted; otherwise it is returned.
static bool https_get(const std::wstring &url, std::string *body, const std::wstring *file, std::wstring *error) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { *error = L"bad address"; return false; }
    // voice packs come from Samsung's own hosts and nowhere else
    std::wstring h = host;
    static const wchar_t suffix[] = L".samsungapps.com";
    size_t n = wcslen(suffix);
    if (h.size() < n || _wcsicmp(h.c_str() + h.size() - n, suffix) != 0) { *error = L"the download address is not a Samsung host"; return false; }
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"SamsungTtsVoiceCentre/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    HANDLE out = INVALID_HANDLE_VALUE;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        wchar_t cl[32] = L"0"; len = sizeof cl;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, cl, &len, nullptr);
        unsigned long long total = _wcstoui64(cl, nullptr, 10), done = 0;
        if (status != 200) { wchar_t m[64]; swprintf(m, 64, L"the server answered %lu", status); *error = m; }
        else {
            if (file) out = CreateFileW(file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (file && out == INVALID_HANDLE_VALUE) *error = L"cannot write the downloaded file";
            else {
                std::vector<char> buf(1 << 16);
                int last = -1;
                ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) { ok = false; *error = L"the connection was lost"; break; }
                    if (!got) break;
                    if (file) { DWORD w; WriteFile(out, buf.data(), got, &w, nullptr); } else body->append(buf.data(), got);
                    done += got;
                    int pct = total ? (int)(done * 100 / total) : 0;
                    if (file && pct != last) { last = pct; PostMessageW(g_wnd, MSG_PROGRESS, (WPARAM)pct, 0); }
                }
                if (ok && total && done != total) { ok = false; *error = L"the download was incomplete"; }
            }
        }
    } else *error = L"cannot reach Samsung's server";
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static std::string between(const std::string &xml, const char *tag) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos) return "";
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos) return "";
    std::string v = xml.substr(a, b - a);
    if (v.compare(0, 9, "<![CDATA[") == 0 && v.size() >= 12) v = v.substr(9, v.size() - 12);
    return v;
}

static std::wstring widen(const std::string &u) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n - 1 : 0), 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    return w;
}
static std::string narrow(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u((size_t)(n > 0 ? n - 1 : 0), 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], n, nullptr, nullptr);
    return u;
}

// ---- the jobs

struct Job { int index; int kind; };     // kind: 0 install, 1 remove

static DWORD WINAPI worker(LPVOID arg) {
    Job job = *(Job *)arg; delete (Job *)arg;
    const Entry &e = kCatalogue[job.index];
    std::wstring who = std::wstring(e.name) + L", " + e.language;
    std::wstring result;
    if (job.kind == 1) {
        post_status(L"Removing " + who + L".");
        remove_tree(voice_folder(e));
        result = register_voices() ? L"Removed " + who + L"." : L"Removed " + who + L", but the registration was not updated.";
    } else {
        // the store answers per device model and Android level: these are the ones the newest packs are offered to
        std::wstring query = L"https://vas.samsungapps.com/stub/stubDownload.as?appId=com.samsung.SMT.lang_" + std::wstring(e.code) +
            L"&deviceId=SM-S921B&mcc=234&mnc=15&csc=BTU&sdkVer=34&pd=0&systemId=0&callerId=com.sec.android.app.samsungapps&abiType=64&extuk=0000000000000000";
        std::string xml; std::wstring err;
        post_status(L"Asking Samsung's store for " + who + L".");
        if (!https_get(query, &xml, nullptr, &err)) result = L"Could not get " + who + L": " + err + L".";
        else {
            std::string uri = between(xml, "downloadURI"), version = between(xml, "versionName"), product = between(xml, "productName");
            if (uri.empty()) result = L"Samsung's store does not offer " + who + L" at the moment.";
            else {
                wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
                std::wstring work = std::wstring(tmp) + L"SamsungTtsVoice", apk = work + L"\\voice.apk";
                remove_tree(work); CreateDirectoryW(work.c_str(), nullptr);
                post_status(L"Downloading " + who + L" from Samsung.");
                if (!https_get(widen(uri), nullptr, &apk, &err)) result = L"Download of " + who + L" failed: " + err + L".";
                else {
                    post_status(L"Unpacking " + who + L".");
                    // an APK is a zip; Windows' own tar reads those
                    DWORD code = 1;
                    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
                    bool ran = run_wait(std::wstring(sys) + L"\\tar.exe", L"-xf \"" + apk + L"\" -C \"" + work + L"\" assets", false, &code);
                    std::wstring dest = voice_folder(e), assets = work + L"\\assets";
                    if (!ran || code != 0 || GetFileAttributesW((assets + L"\\cfg").c_str()) == INVALID_FILE_ATTRIBUTES)
                        result = L"The downloaded pack for " + who + L" could not be unpacked.";
                    else {
                        CreateDirectoryW((g_dir + L"\\data").c_str(), nullptr);
                        CreateDirectoryW((g_dir + L"\\data\\voice").c_str(), nullptr);
                        remove_tree(dest); CreateDirectoryW(dest.c_str(), nullptr);
                        WIN32_FIND_DATAW fd;
                        HANDLE h = FindFirstFileW((assets + L"\\*").c_str(), &fd);
                        if (h != INVALID_HANDLE_VALUE) {
                            do {
                                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                                MoveFileExW((assets + L"\\" + fd.cFileName).c_str(), (dest + L"\\" + fd.cFileName).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
                            } while (FindNextFileW(h, &fd));
                            FindClose(h);
                        }
                        // what the SAPI registration shows for this pack
                        FILE *f = _wfopen((dest + L"\\info.txt").c_str(), L"wb");
                        if (f) {
                            fprintf(f, "name=%s\nlanguage=%s\nversion=%s\nproduct=%s\n", narrow(e.name).c_str(), narrow(e.language).c_str(), version.c_str(), product.c_str());
                            fclose(f);
                        }
                        result = register_voices() ? L"Installed " + who + L". Restart programs that should see it."
                                                   : L"Downloaded " + who + L", but it was not registered with Windows.";
                    }
                }
                remove_tree(work);
            }
        }
    }
    PostMessageW(g_wnd, MSG_DONE, (WPARAM)job.index, (LPARAM) new std::wstring(result));
    return 0;
}

// ---- the window

static void refresh_list(int keep) {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kCount; i++) SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)label(kCatalogue[i]).c_str());
    SendMessageW(g_list, LB_SETCURSEL, (WPARAM)(keep < 0 ? 0 : keep), 0);
}

static void update_buttons() {
    int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    bool have = i >= 0 && installed(kCatalogue[i]);
    EnableWindow(g_install, !g_busy && i >= 0 && !have);
    EnableWindow(g_remove, !g_busy && have);
    EnableWindow(g_sample, !g_busy && i >= 0 && (have || has_sample(kCatalogue[i])));
}

static void start_job(int kind) {
    int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (i < 0 || g_busy) return;
    if (kind == 1) {
        std::wstring q = std::wstring(L"Remove ") + kCatalogue[i].name + L" (" + kCatalogue[i].language + L")?";
        if (MessageBoxW(g_wnd, q.c_str(), L"Samsung TTS voices", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        if (g_voice) g_voice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    }
    g_busy = true; update_buttons();
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    Job *j = new Job{i, kind};
    CloseHandle(CreateThread(nullptr, 0, worker, j, 0, nullptr));
}

static void play_sample() {
    int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (i < 0) return;
    const Entry &e = kCatalogue[i];
    PlaySoundW(nullptr, nullptr, 0);                                        // stop a recorded sample
    if (g_voice) g_voice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);   // and a spoken one
    if (!installed(e)) {        // not downloaded: the recorded sample
        if (has_sample(e) && PlaySoundW(sample_file(e).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
            SetWindowTextW(g_status, (std::wstring(L"Playing a recorded sample of ") + e.name + L", " + e.language + L".").c_str());
        else
            SetWindowTextW(g_status, L"There is no recorded sample of this voice.");
        return;
    }
    std::wstring c = e.code; c[3] = (wchar_t)towupper(c[3]); c[4] = (wchar_t)towupper(c[4]);
    std::wstring id = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\SamsungTTS_" + c;
    ISpObjectToken *tok = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_INPROC_SERVER, IID_ISpObjectToken, (void **)&tok);
    if (SUCCEEDED(hr)) hr = tok->SetId(nullptr, id.c_str(), FALSE);
    if (SUCCEEDED(hr) && !g_voice) hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_ISpVoice, (void **)&g_voice);
    if (SUCCEEDED(hr)) hr = g_voice->SetVoice(tok);
    if (SUCCEEDED(hr)) hr = g_voice->Speak(e.sample, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    if (tok) tok->Release();
    if (FAILED(hr) && has_sample(e) && PlaySoundW(sample_file(e).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        SetWindowTextW(g_status, L"The installed voice could not be opened, so this is its recorded sample. Registering it with Windows may have been declined.");
        return;
    }
    SetWindowTextW(g_status, SUCCEEDED(hr) ? (std::wstring(L"Speaking with ") + e.name + L", " + e.language + L".").c_str()
                                           : L"The sample could not be played: the voice is not registered with Windows.");
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_LIST: if (HIWORD(wp) == LBN_SELCHANGE) update_buttons(); else if (HIWORD(wp) == LBN_DBLCLK) play_sample(); return 0;
        case ID_INSTALL: start_job(0); return 0;
        case ID_REMOVE: start_job(1); return 0;
        case ID_SAMPLE: play_sample(); return 0;
        case ID_CLOSE: case IDCANCEL: if (!g_busy) DestroyWindow(w); return 0;
        }
        break;
    case MSG_STATUS: { std::wstring *s = (std::wstring *)lp; SetWindowTextW(g_status, s->c_str()); delete s; return 0; }
    case MSG_PROGRESS: {
        SendMessageW(g_progress, PBM_SETPOS, wp, 0);
        if (wp % 10 == 0) { wchar_t t[64]; swprintf(t, 64, L"Downloading: %d percent.", (int)wp); SetWindowTextW(g_status, t); }
        return 0;
    }
    case MSG_DONE: {
        std::wstring *s = (std::wstring *)lp;
        g_busy = false;
        refresh_list((int)wp); update_buttons();
        SendMessageW(g_progress, PBM_SETPOS, 0, 0);
        SetWindowTextW(g_status, s->c_str());
        MessageBoxW(w, s->c_str(), L"Samsung TTS voices", MB_OK | MB_ICONINFORMATION);
        delete s;
        SetFocus(g_list);
        return 0;
    }
    case WM_CLOSE: if (!g_busy) DestroyWindow(w); return 0;
    case WM_DESTROY: PlaySoundW(nullptr, nullptr, 0); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
    g_dir.assign(path, n);
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/"));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"SamsungTtsVoiceCentre";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Samsung TTS voices",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 560, 470,
                            nullptr, nullptr, inst, nullptr);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    auto make = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int cx, int cy, int id) {
        HWND h = CreateWindowExW(!wcscmp(cls, L"LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy,
                                 g_wnd, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        return h;
    };
    make(L"STATIC", L"&Voices:", 0, 12, 10, 520, 18, -1);
    g_list = make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 12, 30, 520, 280, ID_LIST);
    g_install = make(L"BUTTON", L"&Download and install", WS_TABSTOP | BS_DEFPUSHBUTTON, 12, 320, 170, 28, ID_INSTALL);
    g_remove = make(L"BUTTON", L"&Remove", WS_TABSTOP, 190, 320, 100, 28, ID_REMOVE);
    g_sample = make(L"BUTTON", L"&Play sample", WS_TABSTOP, 298, 320, 110, 28, ID_SAMPLE);
    make(L"BUTTON", L"&Close", WS_TABSTOP, 432, 320, 100, 28, ID_CLOSE);
    g_progress = make(PROGRESS_CLASSW, L"", 0, 12, 360, 520, 16, ID_PROGRESS);
    g_status = make(L"STATIC", L"Choose a voice. Play sample works before downloading. Installed voices appear in every 64-bit program that uses SAPI 5 voices.",
                    SS_NOPREFIX, 12, 384, 520, 40, ID_STATUS);
    refresh_list(0); update_buttons();
    ShowWindow(g_wnd, show);
    SetFocus(g_list);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (g_voice) g_voice->Release();
    CoUninitialize();
    return 0;
}
