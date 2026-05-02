#include <Windows.h>
#include "lazyusf2/usf/usf.h"
#include "psflib/psflib.h"
#include <winamp/in2.h>
#include <loader/loader/utils.h>
#include <loader/loader/paths.h>
#include <loader/loader/delay_load_helper.h>
#include <loader/hook/squash.h>
#include <map>
#include <vector>
#include "resource.h"

#define PLUGIN_VER L"0.9.5"

// wasabi based services for localisation support
SETUP_API_LNG_VARS;

// TODO add to lang.h
// {4B09007F-F09C-4B50-914B-154892794420}
static const GUID InUSF2LangGUID =
{ 0x4b09007f, 0xf09c, 0x4b50, { 0x91, 0x4b, 0x15, 0x48, 0x92, 0x79, 0x44, 0x20 } };

class usf_player
{
public:
    usf_player(const bool _convert_to_mono) : player(nullptr), seek_needed(-1), kill_thread(false),
                                              convert_to_mono(_convert_to_mono), file_buf(nullptr)
    {
        callbacks.path_separators = "\\/";
        callbacks.context = this;
        callbacks.fopen = usf_fopen;
        callbacks.fread = usf_fread;
        callbacks.fseek = usf_fseek;
        callbacks.fclose = usf_fclose;
        callbacks.ftell = usf_ftell;
    }

    ~usf_player()
    {
        if (player != nullptr)
        {
            usf_shutdown(player);
            SafeFree(player);
        }

        if (file_buf != nullptr)
        {
            SafeFree(file_buf);
            file_buf = nullptr;
        }
    }

    static void* usf_fopen(void* ctx, const char* path)
    {
        FILE* fp = fopen(path, "rb");
        if (fp && ctx && !((usf_player*)ctx)->file_buf)
        {
            ((usf_player*)ctx)->file_buf = (char*)SafeMalloc(USHRT_MAX);
            if (((usf_player*)ctx)->file_buf)
            {
                setvbuf(fp, ((usf_player*)ctx)->file_buf, _IOFBF, USHRT_MAX);
            }
        }
        return (void*)fp;
    }

    static size_t usf_fread(void* b, size_t s, size_t c, void* h)
    {
        return fread(b, s, c, (FILE*)h);
    }

    static int usf_fseek(void* h, int64_t o, int w)
    {
        return fseek((FILE*)h, (long)o, w);
    }

    static int usf_fclose(void* h)
    {
        return fclose((FILE*)h);
    }

    static long usf_ftell(void* h)
    {
        return ftell((FILE*)h);
    }

    void* player;
    int seek_needed;
    bool kill_thread;
    bool convert_to_mono;   // memory hole after this
    char* file_buf;
    psf_file_callbacks callbacks;
    std::map<std::string, std::string> tags;
};

extern In_Module plugin;
usf_player* g_player = nullptr;
HANDLE decode_thread = nullptr;
int /*g_decode_pos = 0,*/ g_length = -1, g_seek_needed = -1;
bool paused = false;
CRITICAL_SECTION g_player_cs = {}/*, g_info_cs = {}*/;

static int usf_load_cb(void* ctx, const uint8_t* exe, size_t sz,
                            const uint8_t* reserved, size_t rsz)
{
    usf_player* player = (usf_player*)ctx;
    if (rsz > 0)
    {
        usf_upload_section(player->player, reserved, rsz);
    }

    if (sz > 0)
    {
        usf_upload_section(player->player, exe, sz);
    }
    return 0;
}

static int usf_info_cb(void* ctx, const char* name, const char* value)
{
    ((usf_player*)ctx)->tags[name] = value;
    return 0;
}

usf_player* create_usf_player(const wchar_t* filename, const bool info_only, const bool convert_to_mono)
{
    usf_player* player = new usf_player(convert_to_mono);
    if (player != nullptr)
    {
        player->player = SafeMalloc(usf_get_state_size());
        if (player->player)
        {
            usf_clear(player->player);

            char fn[MAX_PATH];
            if (psf_load(ConvertUnicodeFn(fn, ARRAYSIZE(fn), filename, CP_ACP),
                         &player->callbacks, 0x21, (!info_only ? usf_load_cb :
                         nullptr), (!info_only ? player : nullptr), usf_info_cb,
                         player, 1, nullptr, nullptr) >= 0)
            {
                return player;
            }
        }

        delete player;
    }
    return nullptr;
}

DWORD WINAPI DecodeThread(void* param)
{
    usf_player* this_player = (usf_player*)param;
    if (this_player != nullptr)
    {
        const int nch = (!this_player->convert_to_mono ? 2 : 1);
        bool done = false;
        int16_t sample_buffer[576 * 2];
        while (!this_player->kill_thread)
        {
            if (g_seek_needed != -1)
            {
                plugin.SetInfo(-1, -1, -1, 0);

                const int wanted_pos = g_seek_needed;
                int current_pos = plugin.outMod->GetOutputTime();

                plugin.outMod->Flush(wanted_pos);

                if (wanted_pos < current_pos)
                {
                    usf_restart(this_player->player);
                    current_pos = 0;
                }

                const int ms_to_skip = (wanted_pos - current_pos);

                if (ms_to_skip > 0)
                {
                    usf_set_trimming_mode(this_player->player, 0);
                    //usf_set_hle_audio(this_player->player, 1);

                    int ms_done = 0;
                    while (ms_done < ms_to_skip)
                    {
                        const int diff = (ms_to_skip - ms_done),
                                  chunk = ((diff > 100) ? 100 : diff);
                        const size_t count = (size_t)(chunk * 44.1f);

                        usf_render_resampled(this_player->player, nullptr, count, 44100);

                        ms_done += chunk;
                    }

                    //usf_set_hle_audio(this_player->player, 0);
                    usf_set_trimming_mode(this_player->player, 1);
                }

                plugin.SetInfo(-1, -1, -1, 1);

                g_seek_needed = -1;
            }

            size_t count = 576;
            const char* err = usf_render_resampled(this_player->player, sample_buffer, count, 44100);
            if (err || ((g_length > 0) && (plugin.outMod->GetOutputTime() > g_length)))
            {
                //wait for output to be finished
                plugin.outMod->Write(nullptr, 0);

                /*while (plugin.outMod->IsPlaying())
                {
                    SleepEx(10, TRUE);
                }*/

                PostEOF();
                break;
            }

            if (this_player->convert_to_mono)
            {
                // try to deal with the playback option to force mono playback
                short* output_buffer = (short*)sample_buffer;
                for (int i = 0, index = 0; i < (int)count; i++, index++)
                {
                    const int pos = (i * 2);
                    output_buffer[index] = (short)((output_buffer[pos] + output_buffer[(pos + 1)]) / 2.f);
                }
            }

            const int bytes_to_write = (int)(count * (!this_player->convert_to_mono ? 4 : 2));
            while (((plugin.outMod->CanWrite() < bytes_to_write) << (plugin.dsp_isactive() ? 1 : 0)) && !this_player->kill_thread)
            {
                SleepEx(10, TRUE);
            }

            if (!this_player->kill_thread)
            {
                // TODO
                int l = (int)count;
                if (plugin.dsp_isactive()) l = plugin.dsp_dosamples((short int*)sample_buffer, l / nch / (16 / 8), 16, nch, 44100) * (nch * (16 / 8));
                //if (plugin.SAAddPCMData) plugin.SAAddPCMData((char*)sample_buffer, nch, 16, decode_pos_ms);

                plugin.SAAddPCMData((char*)sample_buffer, nch, 16, /*g_decode_pos/*/plugin.outMod->GetWrittenTime()/**/);
                /*if (plugin.VSAAddPCMData) plugin.VSAAddPCMData((char *)sample_buffer,nch,16,decode_pos_ms);*/

                //g_decode_pos += (bytes_to_write / nch / (16 / 8));

                plugin.outMod->Write((char*)sample_buffer, bytes_to_write);
            }
            else
            {
                SleepEx(10, TRUE);
            }
        }
    }
    else
    {
        PostEOF();
    }

    if (this_player != nullptr)
    {
        delete this_player;
    }

    if (decode_thread != nullptr)
    {
        CloseHandle(decode_thread);
        decode_thread = nullptr;
    }
    return 0;
}

static int init(void)
{
    InitializeCriticalSectionEx(&g_player_cs, 400, CRITICAL_SECTION_NO_DEBUG_INFO);
    //InitializeCriticalSectionEx(&g_info_cs, 400, CRITICAL_SECTION_NO_DEBUG_INFO);

	StartPluginLangWithDesc(plugin.hDllInstance, InUSF2LangGUID,
			  IDS_PLUGIN_NAME, PLUGIN_VER, &plugin.description);
	return IN_INIT_SUCCESS;
}

void quit(void)
{
    //DeleteCriticalSection(&g_info_cs);
    DeleteCriticalSection(&g_player_cs);
}

//void config(HWND hwndParent) {}

void about(HWND hwndParent)
{
    wchar_t message[512]/* = { 0 }*/;
    const unsigned char* output = DecompressResourceText(WASABI_API_LNG_HINST,
							      WASABI_API_ORIG_HINST, IDR_ABOUT_MESSAGE_GZ);
    PrintfCch(message, ARRAYSIZE(message), (LPWSTR)output, (LPCWSTR)
              plugin.description, WACUP_Author(), WACUP_Copyright(), __DATE__);
    DecompressResourceFree(output);
    AboutMessageBox(hwndParent, message, LangString(IDS_ABOUT_TITLE));
}

void stop()
{
    EnterCriticalSection(&g_player_cs);
    if (g_player != nullptr)
    {
        g_player->kill_thread = true;
        g_player = nullptr;
    }
    LeaveCriticalSection(&g_player_cs);

    if (decode_thread != nullptr)
    {
        WaitForThreadToClose(&decode_thread, INFINITE);

        if (decode_thread != nullptr)
        {
            CloseHandle(decode_thread);
            decode_thread = nullptr;
        }
    }

    paused = false;

    if (plugin.outMod && plugin.outMod->Close)
    {
        plugin.outMod->Close();
    }

    if (plugin.SAVSADeInit)
    {
        plugin.SAVSADeInit();
    }
}

int parse_usf_time(const char* time_str)
{
    if (time_str && *time_str)
    {
        int minutes = 0;
        float seconds = 0.f;

        // Handle MM:SS.m or MM:SS
        if (sscanf(time_str, "%d:%f", &minutes, &seconds) == 2)
        {
            return (minutes * 60000) + (int)(seconds * 1000.0f);
        }
        // Handle SS.m or just seconds
        else if (sscanf(time_str, "%f", &seconds) == 1)
        {
            return (int)(seconds * 1000.0f);
        }
    }
    return 0;
}

int get_player_length(usf_player* player)
{
    const int length = parse_usf_time(player->tags["length"].c_str());
    return ((length > 0) ? (length + parse_usf_time(player->
             tags["fade"].c_str())) : (180 * 1000)/*TODO*/);
}

int play(const in_char* filename)
{
    //g_decode_pos = 0;
    g_length = -1;

    const bool convert_to_mono = PlaybackIsMono();
    usf_player* player = create_usf_player(filename, false, convert_to_mono);
    if (player != nullptr)
    {
        const int nch = (!convert_to_mono ? 2 : 1),
                  maxlatency = plugin.outMod->Open(44100, nch, 16, -1, -1);
        if (maxlatency < 0)
        {
            delete player;
            return 1;
        }

        plugin.SetInfo((44100 * 16 * nch) / 1000, 44100 / 1000, nch, 1);

#ifndef _WIN64
        plugin.SAVSAInit(maxlatency, 44100);
        plugin.VSASetInfo(44100, nch);
#else
        plugin.VisInitInfo(maxlatency, 44100, nch);
#endif
        plugin.outMod->SetVolume(-666);

        usf_set_compare(player->player, player->tags["_enablecompare"] == "1");
        usf_set_fifo_full(player->player, player->tags["_enablefifofull"] == "1");

        g_length = get_player_length(player);

        if (StartPlaybackThread(DecodeThread, player, 0, nullptr) == nullptr)
        {
            delete player;
            return 1;
        }

        EnterCriticalSection(&g_player_cs);
        g_player = player;
        LeaveCriticalSection(&g_player_cs);
        return 0;
    }
    return 1;
}

static void pause(void)
{
    paused = true;

    if (plugin.outMod)
    {
        plugin.outMod->Pause(1);
    }
}

static void unpause(void)
{
    paused = false;

    if (plugin.outMod)
    {
        plugin.outMod->Pause(0);
    }
}

static int ispaused(void)
{
    return paused;
}

int getlength()
{
    return g_length;
}

int getoutputtime()
{
    return ((g_seek_needed != -1) ? g_seek_needed : (plugin.outMod ?
                                plugin.outMod->GetOutputTime() : 0));
}

static void setoutputtime(int time_in_ms)
{
    g_seek_needed = time_in_ms;
}

static void setvolume(int volume)
{
    if (plugin.outMod && plugin.outMod->SetVolume)
    {
        plugin.outMod->SetVolume(volume);
    }
}

static void setpan(int pan)
{
    if (plugin.outMod && plugin.outMod->SetPan)
    {
        plugin.outMod->SetPan(pan);
    }
}

void getfileinfo(const wchar_t* filename, wchar_t* title, int* len_ms)
{
    usf_player* player = create_usf_player(filename, true, false);
    if (player != nullptr)
    {
        if (title)
        {
            const std::string& t = player->tags["title"];
            if (!t.empty())
            {
                ConvertANSI(t.c_str(), (const int)t.size(), CP_ACP, title,
                                       GETFILEINFO_TITLE_LENGTH, nullptr);
            }
            else
            {
                const wchar_t* fn = wcsrchr(filename, L'\\');
                ConvertANSI((fn ? ((const char*)fn + 1) : "Unknown USF"), (fn ? -1 :
                             11), CP_ACP, title, GETFILEINFO_TITLE_LENGTH, nullptr);
            }
        }

        if (len_ms)
        {
            *len_ms = get_player_length(player);
        }

        delete player;
    }
}

void __cdecl GetFileExtensions(void)
{
	if (!plugin.FileExtensions)
	{
		wchar_t buffer[64];
		size_t description_len = 0;
		LPCWSTR description = LngStringCopyGetLen(IDS_USF_FILES, buffer,
								   ARRAYSIZE(buffer), &description_len);
		plugin.FileExtensions = BuildInputFileListString(L"USF;MINIUSF",
                                      11, description, description_len);
	}
}

const wchar_t wacup_plugin_id[] = { L'w', L'a', L'c', L'u', L'p', L'(', L'i', L'n', L'_', L'u',
                                    L's', L'f', L'2', L'.', L'd', L'l', L'l', L')', 0 };

#define OUR_INPUT_PLUG_IN_FEATURES INPUT_USES_UNIFIED_ALT3 | INPUT_HAS_FORMAT_CONVERSION_UNICODE | \
								   /*INPUT_HAS_FORMAT_CONVERSION_SET_TIME_MODE |*/ INPUT_HAS_READ_META

In_Module plugin =
{
    IN_VER_WACUP,
    IN_INIT_PRE_FEATURES
    (char*)wacup_plugin_id,
    nullptr,
    nullptr,
    nullptr,
    1,
    1,
    /*config*/nullptr,
    about,
    init,
    quit,
    getfileinfo,
    nullptr,
    nullptr,
    play,
    pause,
    unpause,
    ispaused,
    stop,
    getlength,
    getoutputtime,
    setoutputtime,
    setvolume,
    setpan,
    IN_INIT_VIS_RELATED_CALLS,
    0,
    0,
    IN_INIT_WACUP_EQSET_EMPTY
    0,
    0,
    nullptr,      // api_service
    IN_INIT_POST_FEATURES
    GetFileExtensions,	// loading optimisation
    IN_INIT_WACUP_END_STRUCT/**/
};

extern "C" __declspec(dllexport) In_Module* __cdecl winampGetInModule2(void)
{
    return &plugin;
}

// return 1 if you want winamp to show it's own file info dialogue, 0 if you want to show your own (via In_Module.InfoBox)
// if returning 1, remember to implement winampGetExtendedFileInfo("formatinformation")!
extern "C" __declspec(dllexport) int winampUseUnifiedFileInfoDlg(const wchar_t* fn)
{
    return 1;
}

// should return a child window of 513x271 pixels (341x164 in msvc dlg units), or return NULL for no tab.
// Fill in name (a buffer of namelen characters), this is the title of the tab (defaults to "Advanced").
// filename will be valid for the life of your window. n is the tab number. This function will first be 
// called with n == 0, then n == 1 and so on until you return NULL (so you can add as many tabs as you like).
// The window you return will recieve WM_COMMAND, IDOK/IDCANCEL messages when the user clicks OK or Cancel.
// when the user edits a field which is duplicated in another pane, do a SendMessage(GetParent(hwnd),WM_USER,(WPARAM)L"fieldname",(LPARAM)L"newvalue");
// this will be broadcast to all panes (including yours) as a WM_USER.
extern "C" __declspec(dllexport) HWND winampAddUnifiedFileInfoPane(int n, const wchar_t* filename, HWND parent, wchar_t* name, size_t namelen)
{
    return NULL;
}

extern "C" __declspec(dllexport) int winampGetExtendedFileInfoW(const wchar_t* fn, const char* data, wchar_t* dest, size_t destlen)
{
    if (SameStrA(data, "type") ||
        SameStrA(data, "streammetadata") ||
        SameStrA(data, "lossless"))
    {
        dest[0] = L'0';
        dest[1] = L'\0';
        return 1;
    }
    else if (SameStrNA(data, "stream", 6) &&
            (SameStrA((data + 6), "type") ||
            SameStrA((data + 6), "genre") ||
            SameStrA((data + 6), "url") ||
            SameStrA((data + 6), "name") ||
            SameStrA((data + 6), "title")))
    {
        return 0;
    }

    if (!fn || !fn[0] || SameStrA(data, "reset"))
    {
        return 0;
    }

    const bool album = SameStrA(data, "album"),
               track = SameStrA(data, "track"),
               publisher = SameStrA(data, "publisher"),
               length_seconds = SameStrA(data, "length_seconds");
    if (SameStrA(data, "family"))
    {
        LPCWSTR e = FindPathExtension(fn);
        if ((e != NULL) && (SameStr(e, L"USF") || SameStr(e, L"MINIUSF")))
        {
            size_t copied = 0;
            LngStringCopyGetLen(IDS_FAMILY_STRING, dest, destlen, &copied);
            return (int)copied;
        }
    }
    else if (SameStrA(data, "title") || SameStrA(data, "artist") ||
             album || track || publisher || SameStrA(data, "genre")
             || SameStrA(data, "year") || SameStrA(data, "comment"))
    {
        usf_player* player = create_usf_player(fn, true, false);
        if (player != nullptr)
        {
            size_t copied = 0;
            const std::string& t = player->tags[(album ? "game" : (publisher ? "copyright" : data))];
            if (!t.empty())
            {
                ConvertANSI(t.c_str(), (const int)t.size(), CP_ACP, dest, destlen, nullptr);
            }

            delete player;
            return (int)copied;
        }
    }
    else if (SameStrA(data, "length") || length_seconds)
    {
        usf_player* player = create_usf_player(fn, true, false);
        if (player != nullptr)
        {
            int ret = 0;
            const int length = get_player_length(player);
            I2WStrLen(((length > 0) ? (length / (!length_seconds ? 1 :
                       1000)) : (!length_seconds ? (180 * 1000) : 180)
                                      /*TODO*/), dest, destlen, &ret);

            delete player;
            return ret;
        }
    }
    else if (SameStrA(data, "formatinformation"))
    {
        usf_player* player = create_usf_player(fn, true, false);
        if (player != nullptr)
        {
            const auto &usfby = player->tags["usfby"],
                       &tagger = player->tags["tagger"],
                       &length = player->tags["length"],
                       &fade = player->tags["fade"],
                       &lib = player->tags["_lib"];
            const bool has_fade = !fade.empty();
            // TODO localise
            size_t copied = PrintfCch(dest, destlen, L"Length: %hs\nFade: %hs%s\nRipped by: %hs\nTagged by: %hs\n"
                                      L"Libfile: %hs", (!length.empty() ? length.c_str() : "<not set>"), (has_fade ?
                                      fade.c_str() : "<not set>"), (has_fade ? L" seconds" : L""), (!usfby.empty() ?
                                      usfby.c_str() : "<not provided>"), (!tagger.empty() ? tagger.c_str() :
                                      "<not provided>"), (!lib.empty() ? lib.c_str() : "<not provided>"));

            delete player;
            return (int)copied;
        }
    }
    else if (SameStrA(data, "bitrate"))
    {
        int ret = 0;
        I2WStrLen(((2 * 44100 * 16) / 1000), dest, destlen, &ret);
        return ret;
    }
    else if (SameStrA(data, "samplerate"))
    {
        int ret = 0;
        I2WStrLen(44100, dest, destlen, &ret);
        return ret;
    }
    else if (SameStrA(data, "bitdepth"))
    {
        // TODO is this correct though as it's been
        //      hard-coded to be 16-bit it should
        dest[0] = L'1';
        dest[1] = L'6';
        dest[2] = 0;
        return 2;
    }

    return 0;
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW(const wchar_t* filename, int* size,
																	      int* bps, int* nch, int* srate)
{
    usf_player* this_player = create_usf_player(filename, false, false);
    if (this_player != nullptr)
    {
        if (bps)
        {
            *bps = 16;
        }

        if (nch)
        {
            *nch = 2;
        }

        if (srate)
        {
            *srate = 44100;
        }

        if (size)
        {
            // TODO
            *size = -1;
        }
        return (intptr_t)this_player;
    }
    return 0;
}

extern "C" __declspec (dllexport) intptr_t winampGetExtendedRead_getData(intptr_t handle, char* dest, int len, int* killswitch)
{
    if (handle)
    {
        const char* err = usf_render_resampled(((usf_player*)handle)->player, (int16_t*)dest, (len / 4), 44100);
        if (!err)
        {
            return len;
        }
    }
    return 0;
}

extern "C" __declspec (dllexport) int winampGetExtendedRead_setTime(intptr_t handle, int millisecs)
{
    // TODO
    return 0;
}

extern "C" __declspec (dllexport) void winampGetExtendedRead_close(intptr_t handle)
{
    if (handle)
    {
        delete ((usf_player*)handle);
    }
}

DLL_DELAY_LOAD_HANDLER