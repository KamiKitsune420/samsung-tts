// Stress test of the SAPI voice (sapi/samsung_sapi.cpp) without registering it: the DLL is loaded directly, its
// class factory makes the engine objects, and a stand-in token and site take SAPI's place. So it can run beside an
// installed copy that a screen reader is using, and it controls exactly when an abort arrives.
//
//   sapi_stress <samsungtts_sapi.dll> <voice root> <corpus.txt> [options]
//
//   --voices A,B,..   voice folders to use (default en_US_l03)
//   --threads N       callers speaking at once, each with its own engine object, as separate SpVoice objects do (default 2)
//   --minutes M       how long (default 5)
//   --aborts P        percentage of Speak calls aborted, after 0 to 400 ms (default 70)
//   --seed S
//
// A caller behaves like a screen reader: mostly short calls cut off early, now and then a long one left to finish,
// with rate changes, bookmarks, silences and several fragments in one call. Checked: a call spoken in full at the
// default rate gives the same audio every time for that voice and text; word events stay inside the text; an
// aborted call returns promptly; nothing hangs (a minute with no audio for anyone and no call returning ends the test).
// Memory and handles are printed every minute. Exit status 0 if nothing was wrong.
//
// Build:  see tests\build.ps1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")

static double now() {
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static std::wstring widen(const std::string &u) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
    return w;
}

// ---- a token that only answers what the engine asks: the two strings of a Samsung voice token

class Token : public ISpObjectToken {
    std::map<std::wstring, std::wstring> values_;
public:
    Token(const std::wstring &root, const std::wstring &voice) {
        values_[L"VoiceRoot"] = root; values_[L"Voice"] = voice;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **out) override {
        if (riid == IID_IUnknown || riid == IID_ISpDataKey || riid == IID_ISpObjectToken) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetStringValue(LPCWSTR name, LPWSTR *out) override {
        auto it = values_.find(name ? name : L"");
        if (it == values_.end()) return SPERR_NOT_FOUND;
        size_t bytes = (it->second.size() + 1) * sizeof(wchar_t);
        *out = (LPWSTR)CoTaskMemAlloc(bytes);
        memcpy(*out, it->second.c_str(), bytes);
        return S_OK;
    }
    STDMETHODIMP SetData(LPCWSTR, ULONG, const BYTE *) override { return E_NOTIMPL; }
    STDMETHODIMP GetData(LPCWSTR, ULONG *, BYTE *) override { return E_NOTIMPL; }
    STDMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP SetDWORD(LPCWSTR, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetDWORD(LPCWSTR, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP OpenKey(LPCWSTR, ISpDataKey **) override { return E_NOTIMPL; }
    STDMETHODIMP CreateKey(LPCWSTR, ISpDataKey **) override { return E_NOTIMPL; }
    STDMETHODIMP DeleteKey(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP DeleteValue(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP EnumKeys(ULONG, LPWSTR *) override { return E_NOTIMPL; }
    STDMETHODIMP EnumValues(ULONG, LPWSTR *) override { return E_NOTIMPL; }
    STDMETHODIMP SetId(LPCWSTR, LPCWSTR, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP GetId(LPWSTR *) override { return E_NOTIMPL; }
    STDMETHODIMP GetCategory(ISpObjectTokenCategory **) override { return E_NOTIMPL; }
    STDMETHODIMP CreateInstance(IUnknown *, DWORD, REFIID, void **) override { return E_NOTIMPL; }
    STDMETHODIMP GetStorageFileName(REFCLSID, LPCWSTR, LPCWSTR, ULONG, LPWSTR *) override { return E_NOTIMPL; }
    STDMETHODIMP RemoveStorageFileName(REFCLSID, LPCWSTR, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP Remove(const CLSID *) override { return E_NOTIMPL; }
    STDMETHODIMP IsUISupported(LPCWSTR, void *, ULONG, IUnknown *, BOOL *) override { return E_NOTIMPL; }
    STDMETHODIMP DisplayUI(HWND, LPCWSTR, LPCWSTR, void *, ULONG, IUnknown *) override { return E_NOTIMPL; }
    STDMETHODIMP MatchesAttributes(LPCWSTR, BOOL *) override { return E_NOTIMPL; }
};

static std::atomic<double> g_last_progress{0};      // when audio was last delivered or a call returned

// ---- the site: takes the audio and the events, and says "abort" from a given moment on

class Site : public ISpTTSEngineSite {
public:
    double abort_at = 0;            // 0: never
    long rate = 0;
    unsigned long long bytes = 0, hash = 1469598103934665603ULL;
    long words = 0, bad_words = 0, bookmarks = 0;
    size_t text_chars = 0;          // all word events must lie inside this many characters
    double first_audio = 0, started = 0;

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override {
        if (riid == IID_IUnknown || riid == IID_ISpEventSink || riid == IID_ISpTTSEngineSite) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP AddEvents(const SPEVENT *ev, ULONG n) override {
        for (ULONG i = 0; i < n; i++) {
            if (ev[i].eEventId == SPEI_WORD_BOUNDARY) {
                words++;
                if ((size_t)ev[i].lParam + (size_t)ev[i].wParam > text_chars || ev[i].wParam == 0 || ev[i].ullAudioStreamOffset > bytes) bad_words++;
            } else if (ev[i].eEventId == SPEI_TTS_BOOKMARK) bookmarks++;
        }
        return S_OK;
    }
    STDMETHODIMP GetEventInterest(ULONGLONG *interest) override { *interest = SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_TTS_BOOKMARK); return S_OK; }
    STDMETHODIMP_(DWORD) GetActions() override { return (abort_at && now() >= abort_at) ? SPVES_ABORT : 0; }
    STDMETHODIMP Write(const void *data, ULONG n, ULONG *written) override {
        g_last_progress = now();
        if (!bytes) first_audio = now() - started;
        const unsigned char *p = (const unsigned char *)data;
        for (ULONG i = 0; i < n; i++) hash = (hash ^ p[i]) * 1099511628211ULL;
        bytes += n;
        if (written) *written = n;
        return S_OK;
    }
    STDMETHODIMP GetRate(long *r) override { *r = rate; return S_OK; }
    STDMETHODIMP GetVolume(USHORT *v) override { *v = 100; return S_OK; }
    STDMETHODIMP GetSkipInfo(SPVSKIPTYPE *type, long *count) override { *type = SPVST_SENTENCE; *count = 0; return S_OK; }
    STDMETHODIMP CompleteSkip(long) override { return S_OK; }
};

// ---- shared state

static std::vector<std::wstring> g_lines;
static std::vector<std::string> g_voices;
static std::mutex g_m;                                          // the reference table and the printing
static std::map<std::pair<int, int>, std::pair<unsigned long long, unsigned long long>> g_ref;   // (voice, line) -> hash, bytes
static std::atomic<long> g_calls{0}, g_aborted{0}, g_wrong{0}, g_changed{0}, g_silent{0}, g_bad_words{0}, g_slow_aborts{0}, g_failed{0};
static std::atomic<long long> g_audio_bytes{0};
static double g_worst_abort, g_worst_early_abort, g_worst_first;
static std::atomic<bool> g_quit{false};
static std::atomic<double> g_call_started[16];                  // per thread, 0 when not in a call
static IClassFactory *g_factory;
static std::wstring g_root;
static int g_abort_pct = 70, g_threads = 2;

static bool speakable(const std::wstring &s) {
    for (wchar_t c : s) if ((c >= L'0' && c <= L'9') || ((c | 32) >= L'a' && (c | 32) <= L'z')) return true;
    return false;
}

static SPVTEXTFRAG frag(const std::wstring &text, ULONG offset, SPVACTIONS action = SPVA_Speak) {
    SPVTEXTFRAG f; memset(&f, 0, sizeof f);
    f.State.eAction = action; f.State.Volume = 100; f.State.EmphAdj = 0;
    f.pTextStart = text.c_str(); f.ulTextLen = (ULONG)text.size(); f.ulTextSrcOffset = offset;
    return f;
}

struct Caller { int index; unsigned long long seed; };

static DWORD WINAPI caller(LPVOID arg) {
    Caller *me = (Caller *)arg;
    unsigned long long rng = me->seed;
    auto rnd = [&rng]() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(rng >> 33); };
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // one engine object per voice, as SAPI keeps one per SpVoice and voice
    std::vector<ISpTTSEngine *> engines;
    std::vector<Token *> tokens;
    for (auto &v : g_voices) {
        ISpTTSEngine *e = nullptr;
        if (FAILED(g_factory->CreateInstance(nullptr, IID_ISpTTSEngine, (void **)&e))) { printf("cannot create the engine object\n"); exit(2); }
        Token *t = new Token(g_root, widen(v));
        ISpObjectWithToken *owt = nullptr;
        e->QueryInterface(IID_ISpObjectWithToken, (void **)&owt);
        owt->SetObjectToken(t); owt->Release();
        engines.push_back(e); tokens.push_back(t);
    }
    int v = (int)(rnd() % g_voices.size());
    for (long n = 0; !g_quit; n++) {
        if (rnd() % 6 == 0) v = (int)(rnd() % g_voices.size());       // a screen reader stays with a voice for a while
        int l = (int)(rnd() % g_lines.size());
        const std::wstring &text = g_lines[l];
        Site site;
        bool abort = (int)(rnd() % 100) < g_abort_pct;
        bool plain = rnd() % 4 != 0;        // otherwise: a bookmark, a silence and the text in two fragments
        if (rnd() % 5 == 0) site.rate = (long)(rnd() % 21) - 10;
        std::wstring mark = L"7", first = text.substr(0, text.size() / 2), second = text.substr(text.size() / 2);
        SPVTEXTFRAG f[4];
        if (plain) f[0] = frag(text, 0);
        else {
            f[0] = frag(mark, 0, SPVA_Bookmark);
            f[1] = frag(first, 0); f[2] = frag(mark, 0, SPVA_Silence); f[2].State.SilenceMSecs = 50;
            f[3] = frag(second, (ULONG)first.size());
            f[0].pNext = &f[1]; f[1].pNext = &f[2]; f[2].pNext = &f[3];
        }
        site.text_chars = text.size();
        double t0 = now();
        site.started = t0;
        if (abort) site.abort_at = t0 + (rnd() % 5 == 0 ? 0.0 : (double)(rnd() % 400) / 1000.0) + 1e-9;
        g_call_started[me->index] = t0;
        WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};
        HRESULT hr = engines[(size_t)v]->Speak(0, SPDFID_WaveFormatEx, &wf, f, &site);
        double t1 = now();
        g_call_started[me->index] = 0;
        g_last_progress = t1;
        g_calls++; g_audio_bytes += (long long)site.bytes;
        bool was_aborted = abort && t1 >= site.abort_at;      // otherwise it finished before the abort was due
        std::lock_guard<std::mutex> lock(g_m);
        if (FAILED(hr)) { printf("caller %d #%ld %s line %d: Speak failed %08lx\n", me->index, n, g_voices[(size_t)v].c_str(), l, (unsigned long)hr); g_failed++; g_wrong++; }
        if (site.bad_words) { printf("caller %d #%ld %s line %d: %ld of %ld word events outside the text or ahead of the audio\n", me->index, n, g_voices[(size_t)v].c_str(), l, site.bad_words, site.words); g_bad_words++; g_wrong++; }
        if (was_aborted) {
            g_aborted++;
            // An abort that comes once audio is flowing must be quick. One that comes before the first audio waits
            // for the engine to get there (it only looks for a stop between pieces of audio), which for a very long
            // sentence is seconds: that is reported, with one caller only, since with more the time includes
            // waiting for the others, and not counted as wrong.
            double took = t1 - site.abort_at;
            if (site.bytes) {
                if (took > g_worst_abort) g_worst_abort = took;
                if (took > 1.0) { printf("caller %d #%ld %s line %d: abort took %.2f s\n", me->index, n, g_voices[(size_t)v].c_str(), l, took); g_slow_aborts++; g_wrong++; }
            } else if (g_threads == 1 && took > g_worst_early_abort) g_worst_early_abort = took;
        } else if (SUCCEEDED(hr)) {
            if (site.first_audio > g_worst_first) g_worst_first = site.first_audio;
            unsigned long long silence = plain ? 0 : 2400;     // the 50 ms silence fragment
            if (site.bytes <= silence + 4800 && speakable(text)) { printf("caller %d #%ld %s line %d: no audio (%llu bytes)\n", me->index, n, g_voices[(size_t)v].c_str(), l, site.bytes); g_silent++; g_wrong++; }
            else if (plain && site.rate == 0) {
                auto key = std::make_pair(v, l);
                auto it = g_ref.find(key);
                if (it == g_ref.end()) g_ref[key] = std::make_pair(site.hash, site.bytes);
                else if (it->second.first != site.hash) {
                    printf("caller %d #%ld %s line %d: the audio CHANGED: %llu bytes, was %llu the first time\n", me->index, n, g_voices[(size_t)v].c_str(), l, site.bytes, it->second.second);
                    g_changed++; g_wrong++;
                }
            }
        }
    }
    for (auto e : engines) e->Release();
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 4) { fprintf(stderr, "usage: sapi_stress samsungtts_sapi.dll voiceroot corpus [options]\n"); return 2; }
    double minutes = 5; int threads = 2; unsigned long long seed = 1;
    std::wstring voices = L"en_US_l03";
    for (int i = 4; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--voices" && i + 1 < argc) voices = argv[++i];
        else if (a == L"--threads" && i + 1 < argc) threads = _wtoi(argv[++i]);
        else if (a == L"--minutes" && i + 1 < argc) minutes = _wtof(argv[++i]);
        else if (a == L"--aborts" && i + 1 < argc) g_abort_pct = _wtoi(argv[++i]);
        else if (a == L"--seed" && i + 1 < argc) seed = _wcstoui64(argv[++i], nullptr, 10);
        else { fwprintf(stderr, L"unknown option %s\n", a.c_str()); return 2; }
    }
    if (threads < 1) threads = 1; if (threads > 16) threads = 16;
    g_threads = threads;
    for (size_t p = 0; p <= voices.size();) {
        size_t q = voices.find(L',', p); if (q == std::wstring::npos) q = voices.size();
        std::wstring one = voices.substr(p, q - p);
        if (!one.empty()) { int n = WideCharToMultiByte(CP_UTF8, 0, one.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string u((size_t)n - 1, 0); WideCharToMultiByte(CP_UTF8, 0, one.c_str(), -1, &u[0], n, nullptr, nullptr); g_voices.push_back(u); }
        p = q + 1;
    }
    wchar_t full[2][MAX_PATH * 2];
    for (int i = 0; i < 2; i++) GetFullPathNameW(argv[1 + i], MAX_PATH * 2, full[i], nullptr);
    g_root = full[1];
    FILE *in = _wfopen(argv[3], L"rb"); if (!in) { fprintf(stderr, "cannot open the corpus\n"); return 2; }
    static char line[16384];
    while (fgets(line, sizeof line, in)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) g_lines.push_back(widen(s));
    }
    fclose(in);
    if (g_lines.empty() || g_voices.empty()) { fprintf(stderr, "nothing to speak\n"); return 2; }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(full[0]);
    if (!dll) { fprintf(stderr, "cannot load the DLL (error %lu)\n", GetLastError()); return 2; }
    typedef HRESULT(__stdcall *get_class_t)(REFCLSID, REFIID, void **);
    get_class_t get_class = (get_class_t)GetProcAddress(dll, "DllGetClassObject");
    static const CLSID clsid = {0x6b0f3a52, 0x7c1e, 0x4e0b, {0x9d, 0x5a, 0x53, 0xa3, 0xc0, 0xe2, 0xf7, 0x71}};
    if (!get_class || FAILED(get_class(clsid, IID_IClassFactory, (void **)&g_factory))) { fprintf(stderr, "the DLL has no Samsung TTS class\n"); return 2; }

    auto resources = [](double *mb, DWORD *handles) {
        PROCESS_MEMORY_COUNTERS_EX pmc; memset(&pmc, 0, sizeof pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
        *mb = (double)pmc.PrivateUsage / 1048576.0;
        GetProcessHandleCount(GetCurrentProcess(), handles);
    };
    double mb; DWORD handles = 0;
    resources(&mb, &handles);
    printf("start: %.0f MB private, %lu handles, %zu lines, %zu voices, %d callers\n", mb, handles, g_lines.size(), g_voices.size(), threads);
    fflush(stdout);
    std::vector<HANDLE> hs;
    for (int i = 0; i < threads; i++) hs.push_back(CreateThread(nullptr, 0, caller, new Caller{i, seed * 1000003ULL + (unsigned long long)i * 7919ULL + 1}, 0, nullptr));
    double t0 = now(), next_report = t0 + 60;
    int status = 0;
    g_last_progress = t0;
    while (now() - t0 < minutes * 60) {
        Sleep(250);
        // Callers wait for one another, and a long text takes a while, so a long call is not a hang. It is one when
        // nothing at all moves: no audio delivered to anyone and no call returning.
        if (now() - g_last_progress > 60) {
            printf("HANG: no audio and no call returning for %.0f s;", now() - g_last_progress);
            for (int i = 0; i < threads; i++) {
                double s = g_call_started[i];
                if (s != 0) printf(" caller %d has been inside Speak for %.0f s;", i, now() - s);
            }
            printf("\n");
            fflush(stdout);
            _exit(3);       // the threads cannot be stopped
        }
        if (now() >= next_report) {
            resources(&mb, &handles);
            std::lock_guard<std::mutex> lock(g_m);
            printf("%4.0f min: %ld calls (%ld aborted), %.0f s of audio, %.0f MB private, %lu handles, worst abort %.0f ms, %ld wrong\n",
                   (now() - t0) / 60, g_calls.load(), g_aborted.load(), (double)g_audio_bytes.load() / 48000.0, mb, handles, g_worst_abort * 1000, g_wrong.load());
            fflush(stdout);
            next_report += 60;
        }
    }
    g_quit = true;
    if (WaitForMultipleObjects((DWORD)hs.size(), hs.data(), TRUE, 180000) == WAIT_TIMEOUT) { printf("HANG: the callers did not finish\n"); status = 3; }
    resources(&mb, &handles);
    printf("end: %ld calls (%ld aborted) in %.1f min, %.0f s of audio, %.0f MB private, %lu handles\n", g_calls.load(), g_aborted.load(),
           (now() - t0) / 60, (double)g_audio_bytes.load() / 48000.0, mb, handles);
    printf("worst abort %.0f ms, worst first audio %.0f ms (includes waiting for another caller and loading a voice)\n", g_worst_abort * 1000, g_worst_first * 1000);
    if (g_threads == 1) printf("worst abort that came before the first audio: %.0f ms\n", g_worst_early_abort * 1000);
    printf("wrong: %ld (Speak failed %ld, audio changed %ld, no audio %ld, bad word events %ld, slow aborts %ld)\n", g_wrong.load(), g_failed.load(),
           g_changed.load(), g_silent.load(), g_bad_words.load(), g_slow_aborts.load());
    fflush(stdout);
    _exit(status ? status : g_wrong ? 1 : 0);       // the engine thread never ends, so leave without unloading
}
