// A SAPI 5 text-to-speech engine over the translated Samsung engine (engine/sstts.h). 64-bit, in-process.
//
// The engine is single-threaded, not re-entrant and wants a deep stack, so one worker thread owns it for the
// life of the process. Speak() hands the worker one piece of text at a time and stays on SAPI's thread itself:
// it takes the audio from a queue, writes it to the site, raises word events and watches for an abort.
//
// Registration (DllRegisterServer, i.e. regsvr32) writes the COM class and one voice token for every voice pack
// found in data\voice beside this DLL, machine-wide when elevated and otherwise for the current user. Running it
// again after adding or removing a pack brings the tokens up to date. A token says where the voice files are and
// which pack it speaks with; the engine itself, code and data, is in this DLL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <new>
#include "../engine/sstts.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

// {6B0F3A52-7C1E-4E0B-9D5A-53A3C0E2F771}
static const CLSID CLSID_SamsungTts = {0x6b0f3a52, 0x7c1e, 0x4e0b, {0x9d, 0x5a, 0x53, 0xa3, 0xc0, 0xe2, 0xf7, 0x71}};
static const wchar_t kClsid[] = L"{6B0F3A52-7C1E-4E0B-9D5A-53A3C0E2F771}";
static const wchar_t kTokensKey[] = L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens";
static const wchar_t kTokenPrefix[] = L"SamsungTTS_";

static HMODULE g_module;
static long g_objects;

static std::string utf8(const wchar_t *s, int n = -1) {
    if (!s) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, 0);
    WideCharToMultiByte(CP_UTF8, 0, s, n, &out[0], len, nullptr, nullptr);
    if (n < 0 && !out.empty() && out.back() == 0) out.pop_back();
    return out;
}

// ---- the worker that owns the engine

struct Piece {
    std::vector<short> pcm;
    int word_offset, word_length;       // bytes into the job's UTF-8 text, or -1
};

struct Worker {
    std::mutex m;
    std::condition_variable cv;
    HANDLE thread = nullptr;
    bool opened = false, failed = false;
    std::string error, root;
    int end_silence_ms = 100;      // the engine's own default is 500, which drags between sentences
    // the job
    bool has_job = false, job_done = true;
    std::string text, voice;
    int rate = 100, pitch = 100;
    std::deque<Piece> queue;
    size_t queued_samples = 0;
    volatile bool stopping = false;

    static void on_audio(void *user, const short *pcm, int samples, int word_offset, int word_length) {
        Worker *w = (Worker *)user;
        Piece p;
        p.pcm.assign(pcm, pcm + samples);
        p.word_offset = word_offset; p.word_length = word_length;
        std::unique_lock<std::mutex> lock(w->m);
        // do not run more than about two seconds ahead of the consumer
        w->cv.wait(lock, [w] { return w->queued_samples < 48000 || w->stopping; });
        // Asking again here covers a stop that came while the voice was still loading: the engine forgets a stop
        // when it starts on an utterance.
        if (w->stopping) { sstts_stop(); return; }
        w->queued_samples += (size_t)samples;
        w->queue.push_back(std::move(p));
        w->cv.notify_all();
    }

    // What went wrong goes to the debugger output (Sysinternals DebugView shows it): the caller of Speak only
    // gets silence, and without this nobody can tell why.
    static void report(const char *what, const char *detail, const std::string &voice) {
        std::string line = std::string("SamsungTTS: ") + what + " (" + voice + "): " + (detail ? detail : "") + "\n";
        OutputDebugStringA(line.c_str());
    }

    static DWORD WINAPI run(LPVOID arg) {
        Worker *w = (Worker *)arg;
        sstts_prefer_fast_cores();
        const char *err = "";
        int r = sstts_init(&err);
        sstts_fast_denormals(1);
        {
            std::lock_guard<std::mutex> lock(w->m);
            w->opened = r == 0; w->failed = r != 0; w->error = err;
            w->cv.notify_all();
        }
        if (r) return 1;
        for (;;) {
            std::string text, voice; int rate, pitch;
            {
                std::unique_lock<std::mutex> lock(w->m);
                w->cv.wait(lock, [w] { return w->has_job; });
                w->has_job = false;
                text = w->text; voice = w->voice; rate = w->rate; pitch = w->pitch;
            }
            // selecting the voice that is already current costs nothing; a new one loads in a fraction of a second
            if (sstts_select(w->root.c_str(), voice.c_str(), &err) == 0) {
                sstts_ending_silence(w->end_silence_ms);
                sstts_control(pitch, 100, rate);
                if (!w->stopping && sstts_speak(text.c_str(), on_audio, w) == -2)
                    report("the engine faulted and this text was not spoken", sstts_last_fault(), voice);
            } else report("the voice could not be loaded", err, voice);
            {
                std::lock_guard<std::mutex> lock(w->m);
                w->job_done = true;
                w->cv.notify_all();
            }
        }
    }

    // Start the engine if it is not running; false with the reason in `error` if it cannot be.
    bool ensure(const std::string &root_) {
        std::unique_lock<std::mutex> lock(m);
        if (!thread) {
            root = root_;
            thread = CreateThread(nullptr, 64u << 20, run, this, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!thread) { failed = true; error = "cannot start the engine thread"; }
        }
        cv.wait(lock, [this] { return opened || failed; });
        return opened;
    }

    void start(const std::string &t, const std::string &voice_, int rate_, int pitch_) {
        std::lock_guard<std::mutex> lock(m);
        text = t; voice = voice_; rate = rate_; pitch = pitch_;
        queue.clear(); queued_samples = 0;
        stopping = false; job_done = false; has_job = true;
        cv.notify_all();
    }

    // Next piece of audio: 1 with the piece, 0 when the job has finished and the queue is empty, -1 if nothing
    // came within wait_ms (the voice is loading, or the engine is still reading a long text), so that the caller
    // can look for an abort meanwhile.
    int next(Piece &out, int wait_ms) {
        std::unique_lock<std::mutex> lock(m);
        if (!cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [this] { return !queue.empty() || job_done; })) return -1;
        if (queue.empty()) return 0;
        out = std::move(queue.front());
        queue.pop_front();
        queued_samples -= out.pcm.size();
        cv.notify_all();
        return 1;
    }

    // Abandon the job and wait until the engine has let go of it.
    void stop() {
        std::unique_lock<std::mutex> lock(m);
        stopping = true;
        sstts_stop();
        cv.notify_all();
        cv.wait(lock, [this] { return job_done; });
        queue.clear(); queued_samples = 0;
    }
};

static Worker g_worker;
// One Speak at a time across all voice objects, and in the order they came: with a plain mutex a caller that
// speaks again at once takes the engine back before a waiting one has woken, and the waiting one can starve.
struct Turn {
    std::mutex m;
    std::condition_variable cv;
    unsigned long long next = 0, serving = 0;
    void lock() {
        std::unique_lock<std::mutex> hold(m);
        unsigned long long mine = next++;
        cv.wait(hold, [&] { return serving == mine; });
    }
    void unlock() {
        { std::lock_guard<std::mutex> hold(m); serving++; }
        cv.notify_all();
    }
};
static Turn g_speak;

// ---- the COM object

class Engine : public ISpTTSEngine, public ISpObjectWithToken {
    long refs_ = 1;
    ISpObjectToken *token_ = nullptr;
    std::string root_, voice_ = "en_US_l03";

public:
    Engine() { InterlockedIncrement(&g_objects); }
    virtual ~Engine() { if (token_) token_->Release(); InterlockedDecrement(&g_objects); }

    STDMETHODIMP QueryInterface(REFIID riid, void **out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISpTTSEngine) *out = static_cast<ISpTTSEngine *>(this);
        else if (riid == IID_ISpObjectWithToken) *out = static_cast<ISpObjectWithToken *>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        long n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return (ULONG)n;
    }

    // ISpObjectWithToken
    STDMETHODIMP SetObjectToken(ISpObjectToken *token) override {
        if (!token) return E_INVALIDARG;
        if (token_) token_->Release();
        token_ = token; token_->AddRef();
        auto get = [&](const wchar_t *name, std::string &dst) {
            LPWSTR v = nullptr;
            if (SUCCEEDED(token_->GetStringValue(name, &v)) && v) { dst = utf8(v); CoTaskMemFree(v); }
        };
        get(L"VoiceRoot", root_); get(L"Voice", voice_);
        std::string silence;
        get(L"EndingSilenceMs", silence);       // optional, read when the engine starts
        if (!silence.empty()) g_worker.end_silence_ms = atoi(silence.c_str());
        return S_OK;
    }
    STDMETHODIMP GetObjectToken(ISpObjectToken **out) override {
        if (!out) return E_POINTER;
        *out = token_;
        if (token_) token_->AddRef();
        return token_ ? S_OK : S_FALSE;
    }

    // ISpTTSEngine
    STDMETHODIMP GetOutputFormat(const GUID *, const WAVEFORMATEX *, GUID *format_id, WAVEFORMATEX **format) override {
        if (!format_id || !format) return E_POINTER;
        WAVEFORMATEX *wf = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!wf) return E_OUTOFMEMORY;
        wf->wFormatTag = WAVE_FORMAT_PCM; wf->nChannels = 1; wf->nSamplesPerSec = 24000; wf->wBitsPerSample = 16;
        wf->nBlockAlign = 2; wf->nAvgBytesPerSec = 48000; wf->cbSize = 0;
        *format_id = SPDFID_WaveFormatEx;
        *format = wf;
        return S_OK;
    }

    STDMETHODIMP Speak(DWORD, REFGUID, const WAVEFORMATEX *, const SPVTEXTFRAG *frags, ISpTTSEngineSite *site) override {
        if (!site) return E_INVALIDARG;
        std::lock_guard<Turn> one(g_speak);
        if (!g_worker.ensure(root_)) return E_FAIL;

        ULONGLONG written = 0;      // bytes of audio given to the site in this call
        bool aborted = false;
        for (const SPVTEXTFRAG *f = frags; f && !aborted; f = f->pNext) {
            if (site->GetActions() & SPVES_ABORT) break;
            switch (f->State.eAction) {
            case SPVA_Bookmark: {
                std::wstring name(f->pTextStart, f->ulTextLen);
                SPEVENT ev; memset(&ev, 0, sizeof ev);
                ev.eEventId = SPEI_TTS_BOOKMARK; ev.elParamType = SPET_LPARAM_IS_STRING;
                ev.ullAudioStreamOffset = written;
                ev.lParam = (LPARAM)name.c_str(); ev.wParam = (WPARAM)_wtol(name.c_str());
                site->AddEvents(&ev, 1);
                break;
            }
            case SPVA_Silence: {
                std::vector<short> quiet((size_t)(24000 * f->State.SilenceMSecs / 1000), 0);
                ULONG done = 0;
                if (!quiet.empty()) site->Write(quiet.data(), (ULONG)(quiet.size() * 2), &done);
                written += done;
                break;
            }
            case SPVA_Speak:
            case SPVA_SpellOut: {
                if (!f->pTextStart || !f->ulTextLen) break;
                std::wstring w(f->pTextStart, f->ulTextLen);
                if (f->State.eAction == SPVA_SpellOut) {    // letter by letter
                    std::wstring s;
                    for (wchar_t ch : w) { s += ch; s += L' '; }
                    w = s;
                }
                // UTF-8 for the engine, with a map from byte offsets back to characters of the fragment
                std::string text;
                std::vector<ULONG> char_at;     // per UTF-8 byte, the index of its character in w
                for (size_t i = 0; i < w.size();) {
                    int units = (w[i] >= 0xD800 && w[i] <= 0xDBFF && i + 1 < w.size()) ? 2 : 1;
                    std::string one = utf8(w.data() + i, units);
                    ULONG at = f->State.eAction == SPVA_SpellOut ? (ULONG)(i / 2) : (ULONG)i;
                    for (size_t k = 0; k < one.size(); k++) char_at.push_back(at);
                    text += one;
                    i += (size_t)units;
                }
                bool blank = true;
                for (char ch : text) if ((unsigned char)ch > ' ') { blank = false; break; }
                if (blank) break;

                long base = 0; site->GetRate(&base);
                long r = base + f->State.RateAdj;
                r = r < -10 ? -10 : r > 10 ? 10 : r;
                int rate = (int)std::lround(100.0 * std::pow(3.0, r / 10.0));       // SAPI: +10 is three times as fast
                long p = f->State.PitchAdj.MiddleAdj;
                p = p < -10 ? -10 : p > 10 ? 10 : p;
                int pitch = (int)std::lround(100.0 * std::pow(2.0, p / 24.0));
                USHORT site_vol = 100; site->GetVolume(&site_vol);
                double volume = (site_vol / 100.0) * (f->State.Volume / 100.0);

                g_worker.start(text, voice_, rate, pitch);
                Piece piece;
                int last_word = -1;
                for (;;) {
                    int got = g_worker.next(piece, 20);
                    if (site->GetActions() & SPVES_ABORT) { g_worker.stop(); aborted = true; break; }
                    if (got == 0) break;
                    if (got < 0) continue;
                    if (piece.word_offset >= 0 && piece.word_offset != last_word && (size_t)piece.word_offset < char_at.size()) {
                        last_word = piece.word_offset;
                        ULONG first = char_at[(size_t)piece.word_offset];
                        size_t end_byte = (size_t)piece.word_offset + (size_t)(piece.word_length > 0 ? piece.word_length : 1) - 1;
                        ULONG last = char_at[end_byte < char_at.size() ? end_byte : char_at.size() - 1];
                        SPEVENT ev; memset(&ev, 0, sizeof ev);
                        ev.eEventId = SPEI_WORD_BOUNDARY; ev.elParamType = SPET_LPARAM_IS_UNDEFINED;
                        ev.ullAudioStreamOffset = written;
                        ev.lParam = (LPARAM)(f->ulTextSrcOffset + first);
                        ev.wParam = (WPARAM)(last - first + 1);
                        site->AddEvents(&ev, 1);
                    }
                    if (volume != 1.0)
                        for (short &s : piece.pcm) s = (short)std::lround(s * volume);
                    ULONG done = 0;
                    HRESULT hr = site->Write(piece.pcm.data(), (ULONG)(piece.pcm.size() * 2), &done);
                    written += done;
                    if (FAILED(hr)) { g_worker.stop(); aborted = true; break; }
                }
                break;
            }
            default:
                break;      // SPVA_Pronounce, SPVA_Section, SPVA_ParseUnknownTag: nothing to say
            }
        }
        return S_OK;
    }
};

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **out) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *out = static_cast<IClassFactory *>(this); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Engine *e = new (std::nothrow) Engine();
        if (!e) return E_OUTOFMEMORY;
        HRESULT hr = e->QueryInterface(riid, out);
        e->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_objects); else InterlockedDecrement(&g_objects);
        return S_OK;
    }
};
static Factory g_factory;

// ---- registration

static LSTATUS set_string(HKEY root, const std::wstring &key, const wchar_t *name, const std::wstring &value) {
    HKEY h;
    LSTATUS s = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (s != ERROR_SUCCESS) return s;
    s = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return s;
}

static std::wstring module_dir() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH * 4);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_module = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}

__declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
    if (clsid != CLSID_SamsungTts) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, out);
}

// The engine thread lives as long as the process, so the DLL is never unloaded once it has spoken.
__declspec(dllexport) HRESULT __stdcall DllCanUnloadNow(void) { return (g_objects == 0 && !g_worker.thread) ? S_OK : S_FALSE; }

// What is known about a voice pack: its folder name, and what the voice centre wrote in info.txt beside the
// pack's files (name=, language=, gender=), if anything.
struct Pack { std::wstring code, name, language, gender; };

static std::wstring widen(const std::string &u) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n - 1 : 0), 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    return w;
}

static std::vector<Pack> installed_packs(const std::wstring &voice_root) {
    std::vector<Pack> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((voice_root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring code = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || code.size() < 9 || code[2] != L'_' || code[5] != L'_') continue;
        std::wstring dir = voice_root + L"\\" + code;
        if (GetFileAttributesW((dir + L"\\cfg").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        Pack p; p.code = code;
        FILE *f = _wfopen((dir + L"\\info.txt").c_str(), L"rb");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                size_t eq = l.find('=');
                if (eq == std::string::npos) continue;
                std::wstring v = widen(l.substr(eq + 1));
                std::string k = l.substr(0, eq);
                if (k == "name") p.name = v; else if (k == "language") p.language = v; else if (k == "gender") p.gender = v;
            }
            fclose(f);
        }
        out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static void delete_tokens(HKEY root) {
    HKEY h;
    if (RegOpenKeyExW(root, kTokensKey, 0, KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS) return;
    std::vector<std::wstring> mine;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD n = 256;
        if (RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (!wcsncmp(name, kTokenPrefix, wcslen(kTokenPrefix))) mine.push_back(name);
    }
    for (auto &k : mine) RegDeleteTreeW(h, k.c_str());
    RegCloseKey(h);
}

static LSTATUS register_under(HKEY root) {
    wchar_t path[MAX_PATH * 4];
    GetModuleFileNameW(g_module, path, MAX_PATH * 4);
    std::wstring dir = module_dir(), data = dir + L"\\data";
    std::wstring cls = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;
    LSTATUS s = set_string(root, cls, nullptr, L"Samsung TTS (translated engine)");
    if (s == ERROR_SUCCESS) s = set_string(root, cls + L"\\InprocServer32", nullptr, path);
    if (s == ERROR_SUCCESS) s = set_string(root, cls + L"\\InprocServer32", L"ThreadingModel", L"Both");
    if (s != ERROR_SUCCESS) return s;
    delete_tokens(root);
    for (const Pack &p : installed_packs(data + L"\\voice")) {
        // en_GB_l02 -> locale en-GB; its number in hexadecimal is how SAPI names a language
        std::wstring locale = p.code.substr(0, 2) + L"-" + p.code.substr(3, 2);
        LCID lcid = LocaleNameToLCID(locale.c_str(), 0);
        wchar_t lang_hex[16]; swprintf(lang_hex, 16, L"%X", (unsigned)lcid);
        wchar_t lang_name[128] = L"";
        GetLocaleInfoEx(locale.c_str(), LOCALE_SENGLISHDISPLAYNAME, lang_name, 128);
        std::wstring name = p.name.empty() ? p.code : p.name;
        std::wstring label = L"Samsung " + name + L" - " + (p.language.empty() ? std::wstring(lang_name) : p.language);
        std::wstring tok = std::wstring(kTokensKey) + L"\\" + kTokenPrefix + p.code;
        s = set_string(root, tok, nullptr, label);
        if (s == ERROR_SUCCESS) s = set_string(root, tok, lang_hex, label);
        if (s == ERROR_SUCCESS) s = set_string(root, tok, L"CLSID", kClsid);
        if (s == ERROR_SUCCESS) s = set_string(root, tok, L"VoiceRoot", data + L"\\voice");
        if (s == ERROR_SUCCESS) s = set_string(root, tok, L"Voice", p.code);
        std::wstring attr = tok + L"\\Attributes";
        if (s == ERROR_SUCCESS) s = set_string(root, attr, L"Name", L"Samsung " + name);
        if (s == ERROR_SUCCESS && !p.gender.empty()) s = set_string(root, attr, L"Gender", p.gender);
        if (s == ERROR_SUCCESS) s = set_string(root, attr, L"Age", L"Adult");
        if (s == ERROR_SUCCESS) s = set_string(root, attr, L"Language", lang_hex);
        if (s == ERROR_SUCCESS) s = set_string(root, attr, L"Vendor", L"Samsung");
        if (s != ERROR_SUCCESS) return s;
    }
    return ERROR_SUCCESS;
}

// Machine-wide when run elevated, which is what puts the voices in SAPI's list; otherwise for the current user,
// where a program can still open a voice by its token id (sapi\test_sapi.ps1 does).
__declspec(dllexport) HRESULT __stdcall DllRegisterServer(void) {
    LSTATUS s = register_under(HKEY_LOCAL_MACHINE);
    if (s == ERROR_ACCESS_DENIED) s = register_under(HKEY_CURRENT_USER);
    return s == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(s);
}

__declspec(dllexport) HRESULT __stdcall DllUnregisterServer(void) {
    std::wstring cls = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;
    delete_tokens(HKEY_CURRENT_USER);
    RegDeleteTreeW(HKEY_CURRENT_USER, cls.c_str());
    delete_tokens(HKEY_LOCAL_MACHINE);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, cls.c_str());
    return S_OK;
}

}   // extern "C"
